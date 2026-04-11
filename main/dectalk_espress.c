// SPDX-License-Identifier: MIT
// Copyright (c) 2025 Leland Lucius
// ----------------------------------------------------------------
// DECtalk ESPress Serial Protocol Emulation - Implementation
//
// Emulates the DECtalk ESPress serial host <-> device protocol on ESP32.
// See dectalk_espress.h for protocol documentation.
// ----------------------------------------------------------------

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "sdkconfig.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/i2s_std.h"
#include "usb_cdc_transport.h"
#include "esp_log.h"
#include "esp_pthread.h"
#include "ttsapi.h"
#include "dectalk_espress.h"
#include "diag_mem.h"

static const char *TAG = "DECtalk ESPress";

static esp_log_level_t dectalk_log_level(void)
{
#if CONFIG_DECTALK_LOG_LEVEL_ERROR
    return ESP_LOG_ERROR;
#elif CONFIG_DECTALK_LOG_LEVEL_WARN
    return ESP_LOG_WARN;
#elif CONFIG_DECTALK_LOG_LEVEL_INFO
    return ESP_LOG_INFO;
#elif CONFIG_DECTALK_LOG_LEVEL_DEBUG
    return ESP_LOG_DEBUG;
#else
    return ESP_LOG_VERBOSE;
#endif
}

static void configure_logging(void)
{
    esp_log_level_t log_level = dectalk_log_level();

    esp_log_level_set(TAG, log_level);
    esp_log_level_set("USB-CDC", log_level);
    esp_log_level_set("DIAG", log_level);
}

#define SAMPLE_RATE              CONFIG_DECTALK_I2S_SAMPLE_RATE
#define I2S_BCK_IO               CONFIG_DECTALK_I2S_BCK_GPIO
#define I2S_WS_IO                CONFIG_DECTALK_I2S_WS_GPIO
#define I2S_DO_IO                CONFIG_DECTALK_I2S_DO_GPIO
#define I2S_DMA_DESC_NUM         CONFIG_DECTALK_I2S_DMA_DESC_NUM
#define I2S_DMA_FRAME_NUM        CONFIG_DECTALK_I2S_DMA_FRAME_NUM
#define ESPRESS_SPEECH_TASK_CORE CONFIG_DECTALK_SPEECH_TASK_CORE
#define ESPRESS_MAIN_STACK_SIZE  CONFIG_DECTALK_MAIN_TASK_STACK_SIZE

static i2s_chan_handle_t audio_handle;


// -- Configuration ---------------------------------------------------

#define ESPRESS_TEXT_BUFSIZE  CONFIG_DECTALK_TEXT_BUFFER_SIZE // Text accumulation buffer size
#define ESPRESS_QUEUE_SIZE   CONFIG_DECTALK_SPEECH_QUEUE_SIZE // Speech queue depth
#define ESPRESS_FLUSH_MARKER ((char *)1) // Sentinel for flush signal
#define ESPRESS_RX_TIMEOUT_MS CONFIG_DECTALK_RX_TIMEOUT_MS // CDC read timeout in ms

// Flow control thresholds (fraction of text buffer size)
#define HIWATER_NUM          2 // Send XOFF at 2/3 full
#define HIWATER_DEN          3
#define LOWATER_NUM          1 // Send XON at 1/3 full
#define LOWATER_DEN          3

// Queue-level flow control thresholds (fraction of queue depth)
#define QUEUE_HIWATER_NUM    3 // Send XOFF at 3/4 full queue
#define QUEUE_HIWATER_DEN    4
#define QUEUE_LOWATER_NUM    1 // Send XON at 1/4 full queue
#define QUEUE_LOWATER_DEN    4

// Text flush timeout: if no new chars arrive for this many ms, flush
#define TEXT_IDLE_TIMEOUT_MS CONFIG_DECTALK_TEXT_IDLE_TIMEOUT_MS

// -- ESPress Protocol State ------------------------------------------

typedef struct
{
    // DLE state machine
    int dle_state; // 0=normal, 1-3=collecting DLE bytes
    uint8_t dle_buf[4]; // DLE sequence accumulator

    // Device status
    uint16_t status; // Current device status bits
    uint16_t last_index; // Last index marker value

    // Flow control
    int host_xoff; // Host sent XOFF: stop device TX
    int xoff_sent; // We sent XOFF to host

    // Text accumulation
    char text_buf[ESPRESS_TEXT_BUFSIZE];
    int text_pos;

    // Speech state
    volatile int paused; // Speech output is paused (SO/SI)
    volatile int speaking; // Synthesis task is active
    volatile int flushing; // Flush in progress
} espress_state_t;

static espress_state_t estate;
static QueueHandle_t speech_queue;

// TTS handle for the ttsapi interface
static LPTTS_HANDLE_T espress_tts_handle;

// In-memory buffer management
#define ESPRESS_TTS_NUM_BUFFERS  3
#define ESPRESS_TTS_BUFFER_SIZE  16384 // bytes (8192 16-bit samples)
#define ESPRESS_TTS_MAX_INDEXES 8 // max index marks per buffer

static TTS_BUFFER_T  espress_tts_bufs[ESPRESS_TTS_NUM_BUFFERS] = {};
static char          espress_tts_audio[ESPRESS_TTS_NUM_BUFFERS][ESPRESS_TTS_BUFFER_SIZE];
static TTS_INDEX_T   espress_tts_indexes[ESPRESS_TTS_NUM_BUFFERS][ESPRESS_TTS_MAX_INDEXES];

// -- Protocol Decode Logging -----------------------------------------

// ----------------------------------------------------------------
// Append human-readable status flag names to a buffer.
// Returns the number of characters written (excluding NUL terminator).
// ----------------------------------------------------------------
static int status_bits_to_str(uint16_t status, char *buf, int bufsize)
{
    static const struct { uint16_t bit; const char *name; } flags[] =
    {
        { STAT_int,         "INT"         },
        { STAT_tr_char,     "TR_CHAR"     },
        { STAT_rr_char,     "RR_CHAR"     },
        { STAT_cmd_ready,   "CMD_READY"   },
        { STAT_dma_ready,   "DMA_READY"   },
        { STAT_digitized,   "DIGITIZED"   },
        { STAT_new_index,   "NEW_INDEX"   },
        { STAT_new_status,  "NEW_STATUS"  },
        { STAT_index_valid, "INDEX_VALID" },
        { STAT_flushing,    "FLUSHING"    },
    };
    int pos = 0;

    for (int i = 0; i < (int)(sizeof(flags) / sizeof(flags[0])); i++)
    {
        if (status & flags[i].bit)
        {
            if (pos > 0 && pos < bufsize - 1)
            {
                buf[pos++] = '|';
            }
            int n = snprintf(buf + pos, bufsize - pos, "%s", flags[i].name);
            if (n > 0)
            {
                pos += n;
            }
        }
    }
    if (pos == 0 && bufsize > 1)
    {
        buf[0] = '0';
        buf[1] = '\0';
        pos = 1;
    }
    else if (pos < bufsize)
    {
        buf[pos] = '\0';
    }
    else if (bufsize > 0)
    {
        buf[bufsize - 1] = '\0';
    }
    return pos;
}

// Log a decoded DLE command sequence received from the host.
static void log_rx_dle_command(uint16_t word)
{
    uint16_t cmd_class = word & 0xF000;
    uint16_t cmd_sub   = word & 0x0FFF;

    switch (cmd_class)
    {
    case CMD_null:
        ESP_LOGI(TAG, "RX DLE cmd: NULL (post status) [0x%04X]", word);
        break;
    case CMD_control:
        switch (cmd_sub)
        {
        case CTRL_vol_up:
            ESP_LOGI(TAG, "RX DLE cmd: CONTROL VOLUME_UP [0x%04X]", word);
            break;
        case CTRL_vol_down:
            ESP_LOGI(TAG, "RX DLE cmd: CONTROL VOLUME_DOWN [0x%04X]", word);
            break;
        case CTRL_vol_set:
            ESP_LOGI(TAG, "RX DLE cmd: CONTROL VOLUME_SET [0x%04X]", word);
            break;
        case CTRL_pause:
            ESP_LOGI(TAG, "RX DLE cmd: CONTROL PAUSE [0x%04X]", word);
            break;
        case CTRL_resume:
            ESP_LOGI(TAG, "RX DLE cmd: CONTROL RESUME [0x%04X]", word);
            break;
        case CTRL_flush:
            ESP_LOGI(TAG, "RX DLE cmd: CONTROL FLUSH [0x%04X]", word);
            break;
        default:
            ESP_LOGI(TAG, "RX DLE cmd: CONTROL sub=0x%03X [0x%04X]",
                     cmd_sub, word);
            break;
        }
        break;
    case CMD_test:
        ESP_LOGI(TAG, "RX DLE cmd: TEST [0x%04X]", word);
        break;
    case CMD_id:
        ESP_LOGI(TAG, "RX DLE cmd: ID (request identification) [0x%04X]",
                 word);
        break;
    default:
        ESP_LOGI(TAG, "RX DLE cmd: class=0x%X sub=0x%03X [0x%04X]",
                 cmd_class >> 12, cmd_sub, word);
        break;
    }
}


// -- USB CDC I/O Helpers ---------------------------------------------

// Send raw bytes to the host over the USB CDC interface.
static void espress_send(const uint8_t *data, int len)
{
    usb_cdc_transport_write(data, len);
}

// Send a single byte to the host.
static void espress_send_byte(uint8_t c)
{
    espress_send(&c, 1);
}

// -- DLE Status / Index Transmission ---------------------------------

// ----------------------------------------------------------------
// Send a 4-byte DLE status sequence to the host.
// Called in response to ENQ or when status changes.
// ----------------------------------------------------------------
static void espress_send_status(void)
{
    uint8_t buf[4];
    char flags[128];
    dle_encode_word(DLE_PREFIX_STATUS, estate.status, buf);
    status_bits_to_str(estate.status, flags, sizeof(flags));
    ESP_LOGI(TAG, "TX DLE STATUS 0x%04X [%s]", estate.status, flags);
    espress_send(buf, 4);
}

// ----------------------------------------------------------------
// Send a DLE index marker sequence followed by a status update.
// Index: DLE 5X YY ZZ, then Status: DLE 4X YY ZZ
// ----------------------------------------------------------------
static void espress_send_index(uint16_t index_value)
{
    uint8_t buf[8];
    char flags[128];
    dle_encode_word(DLE_PREFIX_INDEX, index_value, buf);
    estate.status |= STAT_new_index | STAT_index_valid;
    dle_encode_word(DLE_PREFIX_STATUS, estate.status, buf + 4);
    status_bits_to_str(estate.status, flags, sizeof(flags));
    ESP_LOGI(TAG, "TX DLE INDEX %u + STATUS 0x%04X [%s]",
             index_value, estate.status, flags);
    espress_send(buf, 8);
    estate.last_index = index_value;
}

// -- Flow Control ----------------------------------------------------

// ----------------------------------------------------------------
// Check if we need to send XOFF/XON based on text buffer and queue usage.
// Considers both the text accumulation buffer level and the speech queue
// depth so the host is throttled before the queue overflows.
// ----------------------------------------------------------------
static void espress_check_flow_control(void)
{
    int hiwater = (ESPRESS_TEXT_BUFSIZE * HIWATER_NUM) / HIWATER_DEN;
    int lowater = (ESPRESS_TEXT_BUFSIZE * LOWATER_NUM) / LOWATER_DEN;

    int q_used = (int)uxQueueMessagesWaiting(speech_queue);
    int q_hi   = (ESPRESS_QUEUE_SIZE * QUEUE_HIWATER_NUM) / QUEUE_HIWATER_DEN;
    int q_lo   = (ESPRESS_QUEUE_SIZE * QUEUE_LOWATER_NUM) / QUEUE_LOWATER_DEN;

    // XOFF triggers when *either* threshold is exceeded (aggressive),
    // XON requires *both* to be below limits (conservative) to avoid
    // rapid XON/XOFF oscillation.
    int need_xoff = (estate.text_pos >= hiwater) || (q_used >= q_hi);
    int need_xon  = (estate.text_pos <= lowater) && (q_used <= q_lo);

    if (!estate.xoff_sent && need_xoff)
    {
        ESP_LOGI(TAG, "TX XOFF (pause host, buffer %d/%d, queue %d/%d)",
                 estate.text_pos, ESPRESS_TEXT_BUFSIZE,
                 q_used, ESPRESS_QUEUE_SIZE);
        espress_send_byte(XOFF);
        estate.xoff_sent = 1;
    }
    else if (estate.xoff_sent && need_xon)
    {
        ESP_LOGI(TAG, "TX XON (resume host, buffer %d/%d, queue %d/%d)",
                 estate.text_pos, ESPRESS_TEXT_BUFSIZE,
                 q_used, ESPRESS_QUEUE_SIZE);
        espress_send_byte(XON);
        estate.xoff_sent = 0;
    }
}

// -- Text Buffer Management ------------------------------------------

// Send a flush signal to the speech task.
static void espress_send_flush(void)
{
    char *flush_marker = ESPRESS_FLUSH_MARKER;
    xQueueSend(speech_queue, &flush_marker, pdMS_TO_TICKS(50));
}

// ----------------------------------------------------------------
// Flush the text buffer to the speech queue.
// Allocates a copy of the buffered text and queues it for synthesis.
// ----------------------------------------------------------------
static void espress_flush_text_to_queue(void)
{
    if (estate.text_pos == 0)
    {
        return;
    }

    estate.text_buf[estate.text_pos] = '\0';
    ESP_LOGI(TAG, "RX text queued (%d bytes): %.60s%s",
             estate.text_pos, estate.text_buf,
             estate.text_pos > 60 ? "..." : "");
    char *text = strdup(estate.text_buf);
    if (text)
    {
        if (xQueueSend(speech_queue, &text, pdMS_TO_TICKS(500)) != pdTRUE)
        {
            ESP_LOGW(TAG, "Speech queue full, dropped: %.30s%s",
                     text, strlen(text) > 30 ? "..." : "");
            free(text);
        }
    }
    estate.text_pos = 0;
    espress_check_flow_control();
}

// ----------------------------------------------------------------
// Add a text character to the accumulation buffer.
// Triggers speech on clause boundaries (CR, LF) or when buffer is nearly full.
// ----------------------------------------------------------------
static void espress_add_char(uint8_t c)
{
    if (estate.text_pos >= ESPRESS_TEXT_BUFSIZE - 1)
    {
        espress_flush_text_to_queue();
    }

    estate.text_buf[estate.text_pos++] = (char)c;
    espress_check_flow_control();

    // Flush on clause boundaries
    if (c == '\r' || c == '\n')
    {
        espress_flush_text_to_queue();
    }
}

// ----------------------------------------------------------------
// Handle an internal DECtalk synchronisation marker.
// The original ESPress parser treats raw 0xFF as CMD_sync_char, which is not
// spoken text.  Use it to advance buffered text through the pipeline.
// ----------------------------------------------------------------
static void espress_handle_sync_char(const char *source)
{
    ESP_LOGI(TAG, "RX sync marker from %s", source);
    if (estate.text_pos > 0)
    {
        espress_flush_text_to_queue();
        espress_send_byte(XON);
    }
}

// -- DLE Command Processing ------------------------------------------

// Process a completed 4-byte DLE sequence received from the host.
static void espress_process_dle(void)
{
    uint8_t type_byte = estate.dle_buf[1];
    uint16_t word;

    // Validate DLE type byte range (original serial.c rejects < 0x20 or > 0x71)
    if (type_byte < DLE_PREFIX_CMD_LO || type_byte > DLE_PREFIX_FLUSHCH)
    {
        ESP_LOGW(TAG, "RX DLE bad type byte 0x%02X -- discarding sequence", type_byte);
        return;
    }

    if (type_byte == DLE_PREFIX_FLUSHCH)
    {
        // 0x71 'q': Flush current speech, then speak a single character
        uint8_t ch = ((estate.dle_buf[2] & 0x0F) << 4) |
                      (estate.dle_buf[3] & 0x0F);
        ESP_LOGI(TAG, "RX DLE FLUSHCH char=0x%02X '%c'",
                 ch, (ch >= 0x20 && ch < 0x7F) ? (char)ch : '.');
        // Flush current speech
        espress_send_flush();
        estate.text_pos = 0;
        // Queue the single character for speech
        char single[2] = {(char)ch, '\0'};
        char *text = strdup(single);
        if (text)
        {
            xQueueSend(speech_queue, &text, pdMS_TO_TICKS(50));
        }
    }
    else if (type_byte == DLE_PREFIX_SYNC)
    {
        // 0x70 'p': DMA sync - equivalent to internal CMD_sync_char (0xFF)
        espress_handle_sync_char("DLE SYNC");
    }
    else if (type_byte <= DLE_PREFIX_CMD_HI)
    {
        // 0x20-0x2F: Command sequence
        word = dle_decode_word(estate.dle_buf);
        log_rx_dle_command(word);

        uint16_t cmd_class = word & 0xF000;
        uint16_t cmd_sub   = word & 0x0FFF;

        if (cmd_class == CMD_control)
        {
            switch (cmd_sub)
            {
            case CTRL_pause:
                estate.paused = 1;
                estate.status &= ~STAT_tr_char;
                // Real DECtalk updates internal status only; no
                // unsolicited DLE STATUS is sent to the host.
                break;

            case CTRL_resume:
                estate.paused = 0;
                estate.status |= STAT_tr_char;
                // Internal status update only; no DLE STATUS to host.
                break;

            case CTRL_flush:
                estate.text_pos = 0;
                TextToSpeechReset(espress_tts_handle, FALSE);
                espress_send_flush();
                estate.status |= STAT_flushing;
                // Match real DECtalk: XON + SOH, no DLE STATUS
                espress_send_byte(XON);
                estate.xoff_sent = 0;
                espress_send_byte(SOH);
                break;

            case CTRL_vol_up:
            case CTRL_vol_down:
            case CTRL_vol_set:
                // Volume control: not directly supported by the ESP32 TTS
                // engine. The host can use [:volume ...] inline commands
                // instead. Acknowledge silently.
                break;

            default:
                break;
            }
        }
        else if (cmd_class == CMD_null)
        {
            // CMD_null: post current status
            espress_send_status();
        }
    }
    else if (type_byte <= DLE_PREFIX_DATA_HI)
    {
        // 0x30-0x3F: Data sequence from host (e.g., volume level)
        word = dle_decode_word(estate.dle_buf);
        ESP_LOGI(TAG, "RX DLE DATA prefix=0x%02X value=0x%04X",
                 type_byte, word);
        // Store for use by subsequent command. Currently not used.
    }
    else
    {
        ESP_LOGW(TAG, "RX DLE reserved type byte 0x%02X -- discarding sequence",
                 type_byte);
    }
}

// ----------------------------------------------------------------
// Feed a byte into the DLE state machine.
// Called for bytes 1-3 after the initial DLE (byte 0) was detected.
// ----------------------------------------------------------------
static void espress_dle_byte(uint8_t c)
{
    estate.dle_buf[estate.dle_state] = c;
    estate.dle_state++;

    if (estate.dle_state >= 4)
    {
        // Complete 4-byte sequence received
        espress_process_dle();
        estate.dle_state = 0;
    }
}

// -- Control Character Handlers --------------------------------------

// ----------------------------------------------------------------
// Handle ETX (0x03): Cancel/flush all pending speech.
//
// The real DECtalk ESPress responds to ETX with:
//   1. XON  -- flow-control resume (input ring was just flushed)
//   2. SOH  -- flush acknowledge
// No unsolicited DLE STATUS is sent.
//
// TextToSpeechReset() is called immediately from the protocol loop
// to halt the speech pipeline.  The in-progress TextToSpeechSpeak()/
// Sync() on the speech task will return promptly.
// ----------------------------------------------------------------
static void espress_handle_etx(void)
{
    // Discard any buffered text
    estate.text_pos = 0;

    // Immediately interrupt any in-progress synthesis.
    // TextToSpeechReset() halts the speech pipeline, which is safe
    // to call from this task while TextToSpeechSpeak()/Sync() runs
    // on the speech task.  The original DECtalk ESPress hardware
    // uses the same pattern (ISR sets halting while the ISA task is
    // running).
    TextToSpeechReset(espress_tts_handle, FALSE);

    // Signal flush to speech task so it drains stale queue entries
    espress_send_flush();

    estate.status |= STAT_flushing;

    // Match real DECtalk ESPress: send XON then SOH.
    // The real hardware sends XON (flow-control resume because the
    // input ring was emptied by start_flush) followed by SOH (flush
    // acknowledge from the ISA task via p_putc('\001')).  No DLE
    // STATUS is sent -- only ENQ triggers status output.
    espress_send_byte(XON);
    estate.xoff_sent = 0;
    espress_send_byte(SOH);
}

// ----------------------------------------------------------------
// Handle the ']' + ETX + XON flush sequence.
// This is the TSR FLUSH_TEXT sequence.
// espress_handle_etx() already sends XON + SOH, matching the real
// DECtalk ESPress behavior.
// ----------------------------------------------------------------
static void espress_handle_flush_sequence(void)
{
    ESP_LOGI(TAG, "RX ] + ETX + XON flush sequence (TSR FLUSH_TEXT)");
    espress_handle_etx();
}

// -- Audio Callback for ESPress Mode ---------------------------------

// Throttle audio callback logging so it doesn't flood the console
static int audio_cb_call_count = 0;
static int audio_cb_total_samples = 0;

// ----------------------------------------------------------------
// DtCallbackRoutine for the ttsapi interface.
// Handles:
//   TTS_MSG_BUFFER     - synthesized audio data -> I2S output
//   TTS_MSG_INDEX_MARK - index markers -> DLE INDEX sequence to host
// Respects the pause state by silencing audio output while paused.
// ----------------------------------------------------------------
static void espress_tts_callback(LONG lParam1, LONG lParam2,
                                  DWORD dwInstanceParam, UINT uiMsg)
{
    if (uiMsg == TTS_MSG_INDEX_MARK)
    {
        espress_send_index((uint16_t)lParam2);
        return;
    }

    if (uiMsg == TTS_MSG_BUFFER)
    {
        LPTTS_BUFFER_T pBuf = (LPTTS_BUFFER_T)(uintptr_t)lParam2;
        if (pBuf)
        {
            // Extract any index marks embedded in the buffer
            if (pBuf->lpIndexArray && pBuf->dwNumberOfIndexMarks > 0)
            {
                for (DWORD i = 0; i < pBuf->dwNumberOfIndexMarks; i++)
                {
                    espress_send_index(
                        (uint16_t)pBuf->lpIndexArray[i].dwIndexValue);
                }
            }

            if (pBuf->dwBufferLength > 0)
            {
                short *samples = (short *)pBuf->lpData;
                long num_samples = (long)(pBuf->dwBufferLength / sizeof(short));

                audio_cb_call_count++;
                audio_cb_total_samples += (int)num_samples;

                // Log every 50th callback to avoid flooding, plus the first
                if (audio_cb_call_count == 1 ||
                    (audio_cb_call_count % 50) == 0)
                {
                    short s0 = (num_samples > 0) ? samples[0] : 0;
                    short s1 = (num_samples > 1) ? samples[1] : 0;
                    short s2 = (num_samples > 2) ? samples[2] : 0;
                    ESP_LOGI(TAG, "audio_cb #%d: %ld samples, paused=%d, "
                             "first3=[%d,%d,%d], total_samples=%d",
                             audio_cb_call_count, num_samples, estate.paused,
                             s0, s1, s2, audio_cb_total_samples);
                }

                if (estate.paused)
                {
                    memset(samples, 0, pBuf->dwBufferLength);
                }

                // Write audio to I2S
                if (audio_handle)
                {
                    size_t bytes_written;
                    i2s_channel_write(audio_handle, samples,
                                      pBuf->dwBufferLength,
                                      &bytes_written, portMAX_DELAY);
                }
            }

            // Reset and re-queue the buffer for reuse
            pBuf->dwBufferLength = 0;
            pBuf->dwNumberOfPhonemeChanges = 0;
            pBuf->dwNumberOfIndexMarks = 0;
            TextToSpeechAddBuffer(espress_tts_handle, pBuf);
        }
    }
}

// -- Speech Synthesis Task -------------------------------------------

// ----------------------------------------------------------------
// Pthread that dequeues text and performs speech synthesis.
// Runs on a separate thread to keep the protocol handler responsive.
// ----------------------------------------------------------------
static void *speech_task(void *arg)
{
    ESP_LOGI(TAG, "Speech task started");

    // Create the speech queue
    speech_queue = xQueueCreate(ESPRESS_QUEUE_SIZE, sizeof(char *));

    while (1)
    {
        char *text;

        if (xQueueReceive(speech_queue, &text, portMAX_DELAY))
        {
            char chunk_count = 0;

            if (text == ESPRESS_FLUSH_MARKER)
            {
                // Flush: cancel any ongoing synthesis
                ESP_LOGI(TAG, "Speech task: FLUSH marker received, "
                         "resetting TTS and clearing I2S DMA");
                TextToSpeechReset(espress_tts_handle, FALSE);

                // Clear the I2S DMA buffer so that any audio already
                // queued for playback is silenced immediately.  This
                // matches the real DECtalk ESPress behaviour where ETX
                // stops speech output at once.
                i2s_channel_disable(audio_handle);
                i2s_channel_enable(audio_handle);

                // Drain any remaining text entries that were queued
                // before the flush marker.  Without this, stale text
                // would be spoken after the flush completes.
                {
                    char *stale;
                    int drained = 0;
                    while (xQueueReceive(speech_queue, &stale, 0) == pdTRUE)
                    {
                        if (stale != ESPRESS_FLUSH_MARKER)
                        {
                            ESP_LOGI(TAG, "Speech task: drained stale text: %.30s%s",
                                     stale, strlen(stale) > 30 ? "..." : "");
                            free(stale);
                        }
                        drained++;
                    }
                    if (drained > 0)
                    {
                        ESP_LOGI(TAG, "Speech task: drained %d stale entries", drained);
                    }
                }

                estate.speaking = 0;
                estate.flushing = 0;
                estate.status &= ~STAT_flushing;
                estate.status |= STAT_rr_char | STAT_cmd_ready;
                // No unsolicited DLE STATUS -- the real DECtalk ESPress
                // only sends status in response to ENQ from the host.
                // XON + SOH were already sent by the ETX handler.
                espress_check_flow_control();
                ESP_LOGI(TAG, "Speech task: flush complete, ready for new text");
                continue;
            }

            chunk_count++;
            estate.speaking = 1;
            estate.status |= STAT_tr_char;

            ESP_LOGI(TAG, "Speech task: === CHUNK #%d START ===", chunk_count);
            ESP_LOGI(TAG, "Speech task: text (%d bytes): \"%.60s%s\"",
                     (int)strlen(text), text,
                     strlen(text) > 60 ? "..." : "");
            ESP_LOGI(TAG, "Speech task: queue depth before speak: %d/%d",
                     (int)uxQueueMessagesWaiting(speech_queue),
                     ESPRESS_QUEUE_SIZE);

            // Reset audio callback counters for this chunk
            audio_cb_call_count = 0;
            audio_cb_total_samples = 0;

            // Defense-in-depth: strip any non-ASCII bytes (0x80-0xFF)
            // that may have slipped through.  The TTS engine only
            // handles 7-bit ASCII and can hang on high-bit characters.
            {
                int rd = 0, wr = 0;
                while (text[rd])
                {
                    if ((unsigned char)text[rd] <= 0x7F)
                        text[wr++] = text[rd];
                    rd++;
                }
                text[wr] = '\0';
            }

            if (text[0] == '\0')
            {
                ESP_LOGW(TAG, "Speech task: text empty after sanitisation, "
                         "skipping synthesis");
                free(text);
                estate.speaking = 0;
                estate.status &= ~STAT_tr_char;
                estate.status |= STAT_rr_char | STAT_cmd_ready;
                espress_check_flow_control();
                ESP_LOGI(TAG, "Speech task: === CHUNK #%d DONE ===",
                         chunk_count);
                continue;
            }

            // Synthesise this text chunk.  Do NOT reset the I2S channel
            // here -- the DMA buffer may still contain audio from the
            // previous chunk that has not finished playing.  Clearing
            // it would discard up to ~186 ms of speech (8 DMA frames x
            // 256 samples / 11 025 Hz), producing choppy or missing
            // audio.  The I2S auto_clear setting fills the DMA with
            // silence when no new samples arrive, so there is a
            // natural, brief pause between chunks instead of a hard
            // cut-off.
            ESP_LOGI(TAG, "Speech task: calling TextToSpeechSpeak()...");
            TextToSpeechSpeak(espress_tts_handle, text, TTS_FORCE);
            TextToSpeechSync(espress_tts_handle);
            ESP_LOGI(TAG, "Speech task: TextToSpeechSpeak()+Sync() returned, "
                     "audio_callbacks=%d, total_samples=%d",
                     audio_cb_call_count, audio_cb_total_samples);
            free(text);

            estate.speaking = 0;
            estate.status &= ~STAT_tr_char;
            estate.status |= STAT_rr_char | STAT_cmd_ready;
            espress_check_flow_control();
            ESP_LOGI(TAG, "Speech task: === CHUNK #%d DONE ===", chunk_count);
        }
    }

    return NULL; // unreachable - task loops forever
}

// -- Main Protocol Loop ----------------------------------------------

// Process a single byte received from the host.
static void espress_process_byte(uint8_t c)
{
    // If collecting a DLE sequence, feed bytes to state machine
    if (estate.dle_state > 0)
    {
        espress_dle_byte(c);
        return;
    }

    // Control character handling
    switch (c)
    {
    case XON:
        ESP_LOGI(TAG, "RX XON (host resumes device TX)");
        estate.host_xoff = 0;
        return;

    case XOFF:
        ESP_LOGI(TAG, "RX XOFF (host pauses device TX)");
        estate.host_xoff = 1;
        return;

    case DLE:
        estate.dle_state = 1;
        estate.dle_buf[0] = DLE;
        return;

    case ETX:
        ESP_LOGI(TAG, "RX ETX (flush/cancel all speech)");
        espress_handle_etx();
        return;

    case ENQ:
        ESP_LOGI(TAG, "RX ENQ (host requests status)");
        espress_send_status();
        return;

    case SO:
        ESP_LOGI(TAG, "RX SO (pause speech output)");
        estate.paused = 1;
        return;

    case SI:
        ESP_LOGI(TAG, "RX SI (resume speech output)");
        estate.paused = 0;
        return;

    case VT:
        ESP_LOGI(TAG, "RX VT (sync marker)");
        // Sync marker: pass through as regular character
        break;

    case SOH:
        // SOH from host: ignore (this is a device->host byte)
        ESP_LOGI(TAG, "RX SOH (unexpected from host, ignored)");
        return;

    default:
        if (dectalk_is_sync_char(c))
        {
            espress_handle_sync_char("raw 0xFF");
            return;
        }

        // Accept non-ASCII bytes (0x80-0xFF) to match the original
        // DECtalk ESPress hardware, which passes all bytes >= 0x20
        // through to the input ring without filtering.  The speech
        // task strips non-ASCII bytes before calling
        // TextToSpeechSpeak(), so the TTS engine never sees them.
        if (c < 0x20 && c != '\r' && c != '\n' && c != '\t')
        {
            return;
        }
        break;
    }

    // Regular text character (or VT/CR/LF/TAB): add to buffer
    espress_add_char(c);
}

// ----------------------------------------------------------------
// DECtalk ESPress protocol emulation pthread entry point.
// Initialises the TTS engine and USB CDC-ACM transport, then enters
// the main protocol loop that reads host bytes, decodes DLE commands,
// and dispatches text to the speech queue.  Does not return.
// ----------------------------------------------------------------
static void *espress_task(void *arg)
{
    (void)arg;

    // Initialize state
    estate.status = STAT_rr_char | STAT_cmd_ready;

    // Initialize DECtalk engine with the ttsapi interface
    ESP_LOGI(TAG, "Initializing DECtalk TTS engine (ttsapi)...");
    MMRESULT tts_status = TextToSpeechStartup(&espress_tts_handle, WAVE_MAPPER,
                                               DO_NOT_USE_AUDIO_DEVICE,
                                               espress_tts_callback, 0);
    if (tts_status != MMSYSERR_NOERROR)
    {
        ESP_LOGE(TAG, "TextToSpeechStartup failed with code %d", (int)tts_status);
        ESP_LOGE(TAG, "Cannot continue without TTS engine, restarting...");
        esp_restart();
    }

    // Switch TTS to in-memory mode: synthesised PCM is delivered via
    // the audio callback rather than played through a system device.
    TextToSpeechOpenInMemory(espress_tts_handle, WAVE_FORMAT_1M16);

    // Initialise the pre-allocated buffer pool and hand each buffer
    // to the TTS engine so it can begin filling them with audio data.
    for (int i = 0; i < ESPRESS_TTS_NUM_BUFFERS; i++)
    {
        memset(&espress_tts_bufs[i], 0, sizeof(TTS_BUFFER_T));
        espress_tts_bufs[i].lpData = espress_tts_audio[i];
        espress_tts_bufs[i].dwMaximumBufferLength = ESPRESS_TTS_BUFFER_SIZE;
        espress_tts_bufs[i].lpIndexArray = espress_tts_indexes[i];
        espress_tts_bufs[i].dwMaximumNumberOfIndexMarks = ESPRESS_TTS_MAX_INDEXES;
        TextToSpeechAddBuffer(espress_tts_handle, &espress_tts_bufs[i]);
    }
    ESP_LOGI(TAG, "DECtalk TTS engine initialized (handle=%p)", (void *)espress_tts_handle);

    // Install USB CDC-ACM transport for ESPress protocol communication.
    // The host sees the ESP32-S3 as a USB serial (CDC-ACM) COM port.
    // Console / ESP_LOG output remains on UART0.
    ESP_LOGI(TAG, "Initializing USB CDC-ACM transport...");
    ESP_ERROR_CHECK(usb_cdc_transport_init());

    ESP_LOGI(TAG, "ESPress protocol ready. Waiting for host communication "
             "on USB CDC.");

    // Send initial XON to indicate device is ready.
    // The real DECtalk ESPress hardware (serial_task in serial.c) sends
    // only XON at startup -- no unsolicited DLE STATUS.  Match that
    // behaviour so the host driver (e.g. JAWS) does not interpret the
    // extra 4-byte DLE sequence as unexpected/bogus data.
    ESP_LOGI(TAG, "TX XON (device ready)");
    espress_send_byte(XON);

    // Track time since last character for idle flush
    TickType_t last_char_time = xTaskGetTickCount();
    int pending_bracket_etx = 0;
    int rx_byte_count = 0;

    ESP_LOGI(TAG, "Entering main protocol loop...");

    // Main protocol loop
    while (1)
    {
        uint8_t rx_byte;
        int c;
        int cnt;

        // Detect host reconnection.  When the USB host closes and
        // re-opens the CDC port the protocol state may be stale
        // (e.g. mid-DLE-sequence, XOFF sent, flushing flag set).
        // Reset everything so the new session starts cleanly, and
        // send XON so the host knows the device is ready.
        if (usb_cdc_transport_check_reconnected())
        {
            ESP_LOGI(TAG, "Host reconnected -- resetting protocol state");

            // Cancel any in-progress synthesis first
            TextToSpeechReset(espress_tts_handle, FALSE);
            espress_send_flush();

            // Reset DLE state machine
            estate.dle_state = 0;

            // Reset text buffer and flush-sequence tracker
            estate.text_pos = 0;
            pending_bracket_etx = 0;

            // Reset flow control
            estate.host_xoff = 0;
            estate.xoff_sent = 0;

            // Reset speech/flush flags
            estate.paused = 0;

            // Restore clean status
            estate.status = STAT_rr_char | STAT_cmd_ready;

            // Signal readiness to the new host
            ESP_LOGI(TAG, "TX XON (device ready after reconnect)");
            espress_send_byte(XON);
        }

        // Read one byte with timeout -- yields to scheduler, preventing WDT
        cnt = usb_cdc_transport_read(&rx_byte, 1, pdMS_TO_TICKS(ESPRESS_RX_TIMEOUT_MS));
        if (cnt > 0)
        {
            c = (int)rx_byte;
            rx_byte_count++;
        }
        else
        {
            // No data available - check for idle text flush
            if (estate.text_pos > 0)
            {
                TickType_t elapsed = xTaskGetTickCount() - last_char_time;
                if (elapsed >= pdMS_TO_TICKS(TEXT_IDLE_TIMEOUT_MS))
                {
                    ESP_LOGI(TAG, "Idle timeout: flushing %d buffered text bytes to queue",
                             estate.text_pos);
                    espress_flush_text_to_queue();
                }
            }
            continue;
        }

        last_char_time = xTaskGetTickCount();

        // Detect the ']' + ETX + XON flush sequence from TSR FLUSH_TEXT.
        // State machine: track ']' then ETX then XON.
        if (pending_bracket_etx == 1 && c == ETX)
        {
            pending_bracket_etx = 2;
            continue;
        }
        else if (pending_bracket_etx == 2 && c == XON)
        {
            pending_bracket_etx = 0;
            espress_handle_flush_sequence();
            continue;
        }
        else if (pending_bracket_etx > 0)
        {
            // Sequence broken: process the ']' as text, then current byte
            espress_add_char(']');
            if (pending_bracket_etx == 2)
            {
                espress_process_byte(ETX);
            }
            pending_bracket_etx = 0;
            // Fall through to process current byte normally
        }

        if (c == ']')
        {
            pending_bracket_etx = 1;
            continue;
        }

        espress_process_byte((uint8_t)c);
    }
}

// ----------------------------------------------------------------
// Application entry point.
// Configures logging, initialises I2S audio output, and launches
// two pthreads:
//   1. speech_thread (CPU 1) - dequeues text and runs TTS synthesis
//   2. main_thread           - runs the ESPress protocol loop
// ----------------------------------------------------------------
void app_main(void)
{
    configure_logging();

#if CONFIG_DECTALK_ENABLE_DIAG_MEM
    diag_mem_start();
#endif

    // Initialize the I2S audio output
    ESP_LOGI(TAG, "Initializing I2S audio output...");

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = I2S_DMA_DESC_NUM;
    chan_cfg.dma_frame_num = I2S_DMA_FRAME_NUM;
    chan_cfg.auto_clear = true;

    i2s_new_channel(&chan_cfg, &audio_handle, NULL);

    i2s_std_config_t std_cfg =
    {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg =
        {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCK_IO,
            .ws = I2S_WS_IO,
            .dout = I2S_DO_IO,
            .din = I2S_GPIO_UNUSED,
            .invert_flags =
            {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    i2s_channel_init_std_mode(audio_handle, &std_cfg);
    i2s_channel_enable(audio_handle);

    ESP_LOGI(TAG, "I2S initialized at %d Hz", SAMPLE_RATE);

    // Retrieve the default pthread configuration
    esp_pthread_cfg_t default_cfg = esp_pthread_get_default_config();
    esp_pthread_cfg_t thread_cfg;
    pthread_t tid;

    // Define the speech thread attributes
    //
    // TextToSpeechSpeak()+Sync() is CPU-intensive and does not yield to
    // the FreeRTOS scheduler while processing text.  If the speech task
    // runs on CPU 0 it starves the IDLE0 task, which is responsible
    // for resetting the Task Watchdog Timer, causing a WDT timeout.
    //
    // Pinning the speech task to CPU 1 keeps CPU 0 free so IDLE0
    // can always run and service the watchdog.  The IDLE1 watchdog
    // check is disabled in sdkconfig.defaults to accommodate the
    // long-running synthesis on CPU 1.
    thread_cfg = default_cfg;
    thread_cfg.pin_to_core = ESPRESS_SPEECH_TASK_CORE;
    thread_cfg.thread_name = "speech_thread";
    esp_pthread_set_cfg(&thread_cfg);

    // Create the speech thread
    pthread_create(&tid, NULL, speech_task, NULL);

    // Define the main thread attributes
    thread_cfg = default_cfg;
    thread_cfg.stack_size = ESPRESS_MAIN_STACK_SIZE;
    thread_cfg.thread_name = "main_thread";
    esp_pthread_set_cfg(&thread_cfg);

    // Create the main thread
    pthread_create(&tid, NULL, espress_task, NULL);

    // Restore default pthread config so any later threads are unaffected.
    esp_pthread_set_cfg(&default_cfg);

    // And allow the default task to cleanup and terminate
    return;
}
