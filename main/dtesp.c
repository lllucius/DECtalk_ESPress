// SPDX-License-Identifier: MIT
// Copyright (c) 2025 Leland Lucius
// ----------------------------------------------------------------
// DECtalk ESPress Serial Protocol Emulation - Implementation
//
// Emulates the DECtalk ESPress serial host <-> device protocol on ESP32.
// See dtesp.h for protocol documentation.
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
#include "esp_log.h"
#include "esp_pthread.h"
#if CONFIG_DTESP_DISABLE_RGB_LED
#include "driver/gpio.h"
#endif
#include "ttsapi.h"
#include "dtesp.h"
#include "diag_mem.h"
#include "dtesp_jobs.h"
#include "dtesp_job_pool.h"
#include "custom_commands.h"
#include "custom_actions.h"

// Select the serial transport layer based on the target chip.
// The vtable in dtesp_transport.h decouples this module from the
// physical link; each transport .c file exposes a single instance.
#include "dtesp_transport.h"
#include "dtesp_audio.h"

static const char *TAG = "DECtalk ESPress";

static esp_log_level_t dtesp_log_level(void)
{
#if CONFIG_DTESP_LOG_LEVEL_ERROR
    return ESP_LOG_ERROR;
#elif CONFIG_DTESP_LOG_LEVEL_WARN
    return ESP_LOG_WARN;
#elif CONFIG_DTESP_LOG_LEVEL_INFO
    return ESP_LOG_INFO;
#elif CONFIG_DTESP_LOG_LEVEL_DEBUG
    return ESP_LOG_DEBUG;
#else
    return ESP_LOG_VERBOSE;
#endif
}

static void configure_logging(void)
{
    esp_log_level_t log_level = dtesp_log_level();

    esp_log_level_set(TAG, log_level);
#if CONFIG_IDF_TARGET_ESP32C6
    esp_log_level_set("JTAG-Serial", log_level);
#else
    esp_log_level_set("USB-CDC", log_level);
#endif
    esp_log_level_set("DIAG", log_level);
}

#define DTESP_SPEECH_TASK_CORE CONFIG_DTESP_SPEECH_TASK_CORE
#define DTESP_MAIN_STACK_SIZE  CONFIG_DTESP_MAIN_TASK_STACK_SIZE


// -- Configuration ---------------------------------------------------

#define DTESP_TEXT_BUFSIZE  CONFIG_DTESP_TEXT_BUFFER_SIZE   // Text accumulation buffer size
#define DTESP_QUEUE_SIZE    CONFIG_DTESP_SPEECH_QUEUE_SIZE  // Speech queue depth
#define DTESP_RX_TIMEOUT_MS CONFIG_DTESP_RX_TIMEOUT_MS      // CDC read timeout in ms

// Flow control thresholds (fraction of text buffer size)
#define HIWATER_NUM 2   // Send XOFF at 2/3 full
#define HIWATER_DEN 3
#define LOWATER_NUM 1   // Send XON at 1/3 full
#define LOWATER_DEN 3

// Queue-level flow control thresholds (fraction of queue depth)
#define QUEUE_HIWATER_NUM   3   // Send XOFF at 3/4 full queue
#define QUEUE_HIWATER_DEN   4
#define QUEUE_LOWATER_NUM   1   // Send XON at 1/4 full queue
#define QUEUE_LOWATER_DEN   4

// Text flush timeout: if no new chars arrive for this many ms, flush
#define TEXT_IDLE_TIMEOUT_MS CONFIG_DTESP_TEXT_IDLE_TIMEOUT_MS

// Mask to extract the control sub-command class from cmd_sub,
// ignoring any data payload in the lower bits (e.g. volume level).
#define CTRL_CMD_MASK       0x0F00

// -- ESPress Protocol State ------------------------------------------

// Single receive-path state enum that replaces the former dle_state int
// and the pending_bracket_etx local variable.
typedef enum
{
    RX_STATE_NORMAL = 0,   // Default: processing text or control chars
    RX_STATE_DLE_1,        // Received DLE, awaiting byte 1
    RX_STATE_DLE_2,        // Awaiting byte 2 of DLE sequence
    RX_STATE_DLE_3,        // Awaiting byte 3 of DLE sequence
    RX_STATE_BRACKET_ETX,  // Received ']', watching for ETX
    RX_STATE_ETX_XON,      // Received ']' + ETX, watching for XON
} rx_state_t;

typedef struct
{
    // Protocol receive state (replaces former dle_state + pending_bracket_etx)
    rx_state_t rx_state;
    uint8_t dle_buf[4]; // DLE sequence accumulator

    // Device status
    uint16_t status;     // Current device status bits
    uint16_t last_index; // Last index marker value

    // Flow control
    int host_xoff; // Host sent XOFF: stop device TX
    int xoff_sent; // We sent XOFF to host

    // Text accumulation
    char text_buf[DTESP_TEXT_BUFSIZE];
    int text_pos;

    // Bracket-colon state: tracks whether the text buffer currently
    // contains an unclosed [: sequence.  While in_bracket_cmd is set,
    // the idle flush timer is suppressed so that a custom or native
    // DECtalk command is not split across separate queue entries.
    int in_bracket_cmd; // 1 if we have seen [: without a closing ]

    // Speech state
    volatile int paused;   // Speech output is paused (SO/SI)
    volatile int speaking; // Synthesis task is active
    volatile int flushing; // Flush in progress
} dtesp_state_t;

static dtesp_state_t estate;
static QueueHandle_t speech_queue;

// TTS handle for the ttsapi interface
static LPTTS_HANDLE_T dtesp_tts_handle;

// In-memory buffer management
#define DTESP_TTS_NUM_BUFFERS 3
#define DTESP_TTS_BUFFER_SIZE 16384 // bytes (8192 16-bit samples)
#define DTESP_TTS_MAX_INDEXES 8     // max index marks per buffer

static TTS_BUFFER_T dtesp_tts_bufs[DTESP_TTS_NUM_BUFFERS] = {};
static char         dtesp_tts_audio[DTESP_TTS_NUM_BUFFERS][DTESP_TTS_BUFFER_SIZE];
static TTS_INDEX_T  dtesp_tts_indexes[DTESP_TTS_NUM_BUFFERS][DTESP_TTS_MAX_INDEXES];

// -- Protocol Decode Logging -----------------------------------------

// ----------------------------------------------------------------
// Append human-readable status flag names to a buffer.
// Returns the number of characters written (excluding NUL terminator).
// ----------------------------------------------------------------
static int status_bits_to_str(uint16_t status, char *buf, int bufsize)
{
    static const struct
    {
        uint16_t bit;
        const char *name;
    }
    flags[] =
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
        ESP_LOGD(TAG, "RX DLE cmd: NULL (post status) [0x%04X]", word);
        break;
    case CMD_control:
        switch (cmd_sub & CTRL_CMD_MASK)
        {
        case CTRL_vol_up:
            ESP_LOGD(TAG, "RX DLE cmd: CONTROL VOLUME_UP [0x%04X]", word);
            break;
        case CTRL_vol_down:
            ESP_LOGD(TAG, "RX DLE cmd: CONTROL VOLUME_DOWN [0x%04X]", word);
            break;
        case CTRL_vol_set:
            ESP_LOGD(TAG, "RX DLE cmd: CONTROL VOLUME_SET level=%u [0x%04X]",
                     cmd_sub & 0xFF,
                     word);
            break;
        case CTRL_pause:
            ESP_LOGD(TAG, "RX DLE cmd: CONTROL PAUSE [0x%04X]", word);
            break;
        case CTRL_resume:
            ESP_LOGD(TAG, "RX DLE cmd: CONTROL RESUME [0x%04X]", word);
            break;
        case CTRL_flush:
            ESP_LOGD(TAG, "RX DLE cmd: CONTROL FLUSH [0x%04X]", word);
            break;
        default:
            ESP_LOGD(TAG, "RX DLE cmd: CONTROL sub=0x%03X [0x%04X]",
                     cmd_sub,
                     word);
            break;
        }
        break;
    case CMD_test:
        ESP_LOGD(TAG, "RX DLE cmd: TEST [0x%04X]", word);
        break;
    case CMD_id:
        ESP_LOGD(TAG, "RX DLE cmd: ID (request identification) [0x%04X]",
                 word);
        break;
    default:
        ESP_LOGD(TAG, "RX DLE cmd: class=0x%X sub=0x%03X [0x%04X]",
                 cmd_class >> 12,
                 cmd_sub,
                 word);
        break;
    }
}


// -- USB CDC I/O Helpers ---------------------------------------------

// Transport vtable pointer, initialised in dtesp_task() before use.
static const dtesp_transport_t *s_transport;

// Send raw bytes to the host over the serial transport.
static void dtesp_send(const uint8_t *data, int len)
{
    s_transport->write(data, (size_t)len);
}

// Send a single byte to the host.
static void dtesp_send_byte(uint8_t c)
{
    dtesp_send(&c, 1);
}

// -- DLE Status / Index Transmission ---------------------------------

// ----------------------------------------------------------------
// Send a 4-byte DLE status sequence to the host.
// Called in response to ENQ or when status changes.
// ----------------------------------------------------------------
static void dtesp_send_status(void)
{
    uint8_t buf[4];
    char flags[128];

    dle_encode_word(DLE_PREFIX_STATUS, estate.status, buf);
    status_bits_to_str(estate.status, flags, sizeof(flags));

    ESP_LOGD(TAG, "TX DLE STATUS 0x%04X [%s]", estate.status, flags);
    dtesp_send(buf, 4);
}

// ----------------------------------------------------------------
// Send a DLE index marker sequence followed by a status update.
// Index: DLE 5X YY ZZ, then Status: DLE 4X YY ZZ
// ----------------------------------------------------------------
static void dtesp_send_index(uint16_t index_value)
{
    uint8_t buf[8];
    char flags[128];

    dle_encode_word(DLE_PREFIX_INDEX, index_value, buf);
    estate.status |= STAT_new_index | STAT_index_valid;

    dle_encode_word(DLE_PREFIX_STATUS, estate.status, buf + 4);
    status_bits_to_str(estate.status, flags, sizeof(flags));

    ESP_LOGD(TAG, "TX DLE INDEX %u + STATUS 0x%04X [%s]",
             index_value,
             estate.status,
             flags);

    dtesp_send(buf, 8);
    estate.last_index = index_value;
}

// -- Flow Control ----------------------------------------------------

// ----------------------------------------------------------------
// Check if we need to send XOFF/XON based on text buffer and queue usage.
// Considers both the text accumulation buffer level and the speech queue
// depth so the host is throttled before the queue overflows.
// ----------------------------------------------------------------
static void dtesp_check_flow_control(void)
{
    int hiwater = (DTESP_TEXT_BUFSIZE * HIWATER_NUM) / HIWATER_DEN;
    int lowater = (DTESP_TEXT_BUFSIZE * LOWATER_NUM) / LOWATER_DEN;

    int q_used = (int)uxQueueMessagesWaiting(speech_queue);
    int q_hi   = (DTESP_QUEUE_SIZE * QUEUE_HIWATER_NUM) / QUEUE_HIWATER_DEN;
    int q_lo   = (DTESP_QUEUE_SIZE * QUEUE_LOWATER_NUM) / QUEUE_LOWATER_DEN;

    // XOFF triggers when *either* threshold is exceeded (aggressive),
    // XON requires *both* to be below limits (conservative) to avoid
    // rapid XON/XOFF oscillation.
    int need_xoff = (estate.text_pos >= hiwater) || (q_used >= q_hi);
    int need_xon  = (estate.text_pos <= lowater) && (q_used <= q_lo);

    if (!estate.xoff_sent && need_xoff)
    {
        ESP_LOGD(TAG, "TX XOFF (pause host, buffer %d/%d, queue %d/%d)",
                 estate.text_pos,
                 DTESP_TEXT_BUFSIZE,
                 q_used,
                 DTESP_QUEUE_SIZE);
        dtesp_send_byte(XOFF);
        estate.xoff_sent = 1;
    }
    else if (estate.xoff_sent && need_xon)
    {
        ESP_LOGD(TAG, "TX XON (resume host, buffer %d/%d, queue %d/%d)",
                 estate.text_pos,
                 DTESP_TEXT_BUFSIZE,
                 q_used,
                 DTESP_QUEUE_SIZE);
        dtesp_send_byte(XON);
        estate.xoff_sent = 0;
    }
}

// -- Text Buffer Management ------------------------------------------

// Send a flush signal to the speech task.
static void dtesp_send_flush(void)
{
    dtesp_job_t *job = dtesp_job_pool_alloc_flush();
    if (!job)
    {
        ESP_LOGE(TAG, "Failed to allocate FLUSH job");
        return;
    }
    if (xQueueSend(speech_queue, &job, pdMS_TO_TICKS(50)) != pdTRUE)
    {
        ESP_LOGW(TAG, "Speech queue full; dropping FLUSH job");
        dtesp_job_pool_free(job);
    }
}

// ----------------------------------------------------------------
// Flush the text buffer to the speech queue.
//
// Scans the buffered text for [:fw ...] custom command tokens and
// splits it into an ordered sequence of SPEAK_TEXT and ACTION jobs.
// Native DECtalk inline commands ([:nb], [:ra 200], etc.) are left
// in the text and reach TextToSpeechSpeak() unchanged.
//
// If custom commands are disabled at build time, the entire buffer
// is queued as a single SPEAK_TEXT job (no scanning overhead).
// ----------------------------------------------------------------
static void dtesp_flush_text_to_queue(void)
{
    if (estate.text_pos == 0)
    {
        return;
    }

    estate.text_buf[estate.text_pos] = '\0';
    ESP_LOGD(TAG, "RX text queued (%d bytes): %s",
             estate.text_pos,
             estate.text_buf);
    ESP_LOG_BUFFER_HEXDUMP(TAG, estate.text_buf, estate.text_pos, ESP_LOG_DEBUG);

    // Tokenise the text into jobs.  Voice/rate prefix application is
    // deferred to the speech task so that actions and text are applied
    // in strict submission order; the tokenizer simply produces jobs.
    dtesp_job_list_t jobs;
    custom_commands_tokenize(estate.text_buf, &jobs);

    for (int i = 0; i < jobs.count; i++)
    {
        dtesp_job_t *job = jobs.jobs[i];

        if (xQueueSend(speech_queue, &job, pdMS_TO_TICKS(500)) != pdTRUE)
        {
            if (job->type == DTESP_JOB_SPEAK_TEXT)
            {
                ESP_LOGW(TAG, "Speech queue full, dropped: %.30s%s",
                         job->text,
                         strlen(job->text) > 30 ? "..." : "");
            }
            else
            {
                ESP_LOGW(TAG, "Speech queue full, dropped job type %d",
                         job->type);
            }
            dtesp_job_pool_free(job);
        }
    }

    // Free the list container (jobs were moved to the queue)
    free(jobs.jobs);
    jobs.jobs = NULL;
    jobs.count = 0;

    estate.text_pos = 0;
    estate.in_bracket_cmd = 0;
    dtesp_check_flow_control();
}

// ----------------------------------------------------------------
// Add a text character to the accumulation buffer.
// Triggers speech on clause boundaries (CR, LF) or when buffer is nearly full.
// Tracks [: ... ] sequences so the idle flush timer does not split
// a custom or native DECtalk command across separate queue entries.
// ----------------------------------------------------------------
static void dtesp_add_char(uint8_t c)
{
    if (estate.text_pos >= DTESP_TEXT_BUFSIZE - 1)
    {
        dtesp_flush_text_to_queue();
    }

    estate.text_buf[estate.text_pos++] = (char)c;

    // Track bracket-colon state.  When we see '[' followed by ':'
    // we set in_bracket_cmd so the idle flush is suppressed.
    // A closing ']' clears it.
    if (c == ']')
    {
        estate.in_bracket_cmd = 0;
    }
    else if (c == ':' && estate.text_pos >= 2 &&
             estate.text_buf[estate.text_pos - 2] == '[')
    {
        estate.in_bracket_cmd = 1;
    }

    dtesp_check_flow_control();

    // Flush on clause boundaries, but not while inside a bracket
    // command — that would split the command across queue entries
    // and break parsing.
    if ((c == '\r' || c == '\n') && !estate.in_bracket_cmd)
    {
        dtesp_flush_text_to_queue();
    }
}

// ----------------------------------------------------------------
// Handle an internal DECtalk synchronisation marker.
// The original ESPress parser treats raw 0xFF as CMD_sync_char, which is not
// spoken text.  Use it to advance buffered text through the pipeline.
// ----------------------------------------------------------------
static void dtesp_handle_sync_char(const char *source)
{
    ESP_LOGD(TAG, "RX sync marker from %s", source);
    if (estate.text_pos > 0)
    {
        dtesp_flush_text_to_queue();
        dtesp_send_byte(XON);
    }
}

// -- DLE Command Processing ------------------------------------------

// Process a completed 4-byte DLE sequence received from the host.
static void dtesp_process_dle(void)
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
        ESP_LOGD(TAG, "RX DLE FLUSHCH char=0x%02X '%c'",
                 ch,
                 (ch >= 0x20 && ch < 0x7F) ? (char)ch : '.');
        // Flush current speech
        dtesp_send_flush();
        estate.text_pos = 0;
        // Queue the single character for speech
        char single[2] = {(char)ch, '\0'};
        dtesp_job_t *job = dtesp_job_pool_alloc_text(single, 1);
        if (!job)
        {
            ESP_LOGE(TAG, "Failed to allocate text job for FLUSHCH");
        }
        else if (xQueueSend(speech_queue, &job, pdMS_TO_TICKS(50)) != pdTRUE)
        {
            ESP_LOGW(TAG, "Speech queue full; dropping FLUSHCH char 0x%02X", ch);
            dtesp_job_pool_free(job);
        }
    }
    else if (type_byte == DLE_PREFIX_SYNC)
    {
        // 0x70 'p': DMA sync - equivalent to internal CMD_sync_char (0xFF)
        dtesp_handle_sync_char("DLE SYNC");
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
            switch (cmd_sub & CTRL_CMD_MASK)
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
                TextToSpeechReset(dtesp_tts_handle, FALSE);
                dtesp_send_flush();
                estate.status |= STAT_flushing;
                // Match real DECtalk: XON + SOH, no DLE STATUS
                dtesp_send_byte(XON);
                estate.xoff_sent = 0;
                dtesp_send_byte(SOH);
                break;

#if CONFIG_DTESP_DAC_TLV320
            case CTRL_vol_up:
            {
                uint8_t vol = tlv320_get_volume();
                if (vol < TLV320_MAX_VOLUME)
                {
                    tlv320_set_volume(vol + 1);
                }
                break;
            }
            case CTRL_vol_down:
            {
                uint8_t vol = tlv320_get_volume();
                if (vol > 0)
                {
                    tlv320_set_volume(vol - 1);
                }
                break;
            }
            case CTRL_vol_set:
                tlv320_set_volume(cmd_sub & 0xFF);
                break;
#else
            case CTRL_vol_up:
            case CTRL_vol_down:
            case CTRL_vol_set:
                // Volume control not available for generic DACs.
                break;
#endif

            default:
                break;
            }
        }
        else if (cmd_class == CMD_null)
        {
            // CMD_null: post current status
            dtesp_send_status();
        }
    }
    else if (type_byte <= DLE_PREFIX_DATA_HI)
    {
        // 0x30-0x3F: Data sequence from host (e.g., volume level)
        word = dle_decode_word(estate.dle_buf);
        ESP_LOGD(TAG, "RX DLE DATA prefix=0x%02X value=0x%04X",
                 type_byte,
                 word);
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
// The rx_state enum tracks which byte position we expect next.
// ----------------------------------------------------------------
static void dtesp_dle_byte(uint8_t c)
{
    switch (estate.rx_state)
    {
    case RX_STATE_DLE_1:
        estate.dle_buf[1] = c;
        estate.rx_state = RX_STATE_DLE_2;
        break;
    case RX_STATE_DLE_2:
        estate.dle_buf[2] = c;
        estate.rx_state = RX_STATE_DLE_3;
        break;
    case RX_STATE_DLE_3:
        estate.dle_buf[3] = c;
        // Complete 4-byte sequence received
        dtesp_process_dle();
        estate.rx_state = RX_STATE_NORMAL;
        break;
    default:
        // Should never happen; reset
        estate.rx_state = RX_STATE_NORMAL;
        break;
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
static void dtesp_handle_etx(void)
{
    // Discard any buffered text and reset bracket tracking
    estate.text_pos = 0;
    estate.in_bracket_cmd = 0;

    // Immediately interrupt any in-progress synthesis.
    // TextToSpeechReset() halts the speech pipeline, which is safe
    // to call from this task while TextToSpeechSpeak()/Sync() runs
    // on the speech task.  The original DECtalk ESPress hardware
    // uses the same pattern (ISR sets halting while the ISA task is
    // running).
    TextToSpeechReset(dtesp_tts_handle, FALSE);

    // Signal flush to speech task so it drains stale queue entries
    dtesp_send_flush();

    estate.status |= STAT_flushing;

    // Match real DECtalk ESPress: send XON then SOH.
    // The real hardware sends XON (flow-control resume because the
    // input ring was emptied by start_flush) followed by SOH (flush
    // acknowledge from the ISA task via p_putc('\001')).  No DLE
    // STATUS is sent -- only ENQ triggers status output.
    dtesp_send_byte(XON);
    estate.xoff_sent = 0;
    dtesp_send_byte(SOH);
}

// ----------------------------------------------------------------
// Handle the ']' + ETX + XON flush sequence.
// This is the TSR FLUSH_TEXT sequence.
// dtesp_handle_etx() already sends XON + SOH, matching the real
// DECtalk ESPress behavior.
// ----------------------------------------------------------------
static void dtesp_handle_flush_sequence(void)
{
    ESP_LOGD(TAG, "RX ] + ETX + XON flush sequence (TSR FLUSH_TEXT)");
    dtesp_handle_etx();
}

// -- Audio Callback for ESPress Mode ---------------------------------

// audio_cb_call_count / audio_cb_total_samples are static locals inside
// dtesp_tts_callback() and reset by the speech task before each chunk.
// See comment below.

// ----------------------------------------------------------------
// DtCallbackRoutine for the ttsapi interface.
// Handles:
//   TTS_MSG_BUFFER     - synthesized audio data -> I2S output
//   TTS_MSG_INDEX_MARK - index markers -> DLE INDEX sequence to host
// Respects the pause state by silencing audio output while paused.
// ----------------------------------------------------------------
static void dtesp_tts_callback(LONG lParam1, LONG lParam2,
                                  DWORD dwInstanceParam, UINT uiMsg)
{
    // Throttle audio callback logging so it doesn't flood the console.
    // These are updated from whichever task drives the TTS callback and
    // read from the speech task for chunk-summary logs.  Full atomicity
    // is not required because they are logging-only diagnostics.
    static int audio_cb_call_count = 0;
    static int audio_cb_total_samples = 0;
    if (uiMsg == TTS_MSG_INDEX_MARK)
    {
        dtesp_send_index((uint16_t)lParam2);
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
                    dtesp_send_index(
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
                    ESP_LOGD(TAG, "audio_cb #%d: %ld samples, paused=%d, "
                             "first3=[%d,%d,%d], total_samples=%d",
                             audio_cb_call_count,
                             num_samples,
                             estate.paused,
                             s0,
                             s1,
                             s2,
                             audio_cb_total_samples);
                }

                if (estate.paused)
                {
                    memset(samples, 0, pBuf->dwBufferLength);
                }

                // Write audio to I2S
                i2s_chan_handle_t tx = dtesp_audio_get_handle();
                if (tx)
                {
                    size_t bytes_written;
                    i2s_channel_write(tx,
                                      samples,
                                      pBuf->dwBufferLength,
                                      &bytes_written,
                                      portMAX_DELAY);
                }
            }

            // Reset and re-queue the buffer for reuse
            pBuf->dwBufferLength = 0;
            pBuf->dwNumberOfPhonemeChanges = 0;
            pBuf->dwNumberOfIndexMarks = 0;
            TextToSpeechAddBuffer(dtesp_tts_handle, pBuf);
        }
    }
}

// -- Speech Synthesis Task -------------------------------------------

// ----------------------------------------------------------------
// Pthread that dequeues jobs and performs speech synthesis or
// firmware actions.  Runs on a separate thread to keep the
// protocol handler responsive.
//
// Job types:
//   DTESP_JOB_SPEAK_TEXT – pass text to TextToSpeechSpeak()
//   DTESP_JOB_ACTION     – execute firmware action callback
//   DTESP_JOB_FLUSH      – cancel speech, drain queue
// ----------------------------------------------------------------
static void *speech_task(void *arg)
{
    ESP_LOGI(TAG, "Speech task started");

    while (1)
    {
        dtesp_job_t *job;

        if (xQueueReceive(speech_queue, &job, portMAX_DELAY))
        {
            if (job->type == DTESP_JOB_FLUSH)
            {
                // Flush: cancel any ongoing synthesis
                ESP_LOGI(TAG, "Speech task: FLUSH job received, "
                         "resetting TTS and clearing I2S DMA");
                dtesp_job_pool_free(job);
                TextToSpeechReset(dtesp_tts_handle, FALSE);

                // Clear the I2S DMA buffer so that any audio already
                // queued for playback is silenced immediately.  This
                // matches the real DECtalk ESPress behaviour where ETX
                // stops speech output at once.
                i2s_channel_disable(dtesp_audio_get_handle());
                i2s_channel_enable(dtesp_audio_get_handle());

                // Drain any remaining jobs that were queued before the
                // flush.  Without this, stale text or actions would
                // execute after the flush completes.
                {
                    dtesp_job_t *stale;
                    int drained = 0;
                    while (xQueueReceive(speech_queue, &stale, 0) == pdTRUE)
                    {
                        if (stale->type == DTESP_JOB_SPEAK_TEXT)
                        {
                            ESP_LOGD(TAG, "Speech task: drained stale text: %.30s%s",
                                     stale->text,
                                     strlen(stale->text) > 30 ? "..." : "");
                        }
                        else if (stale->type == DTESP_JOB_ACTION)
                        {
                            ESP_LOGD(TAG, "Speech task: drained stale action");
                        }
                        dtesp_job_pool_free(stale);
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
                dtesp_check_flow_control();
                ESP_LOGI(TAG, "Speech task: flush complete, ready for new jobs");
                continue;
            }

            if (job->type == DTESP_JOB_ACTION)
            {
                // Execute firmware action in order
                ESP_LOGD(TAG, "Speech task: executing ACTION job");
                if (job->action.execute)
                {
                    job->action.execute(job->action.ctx);
                }
                dtesp_job_pool_free(job);
                continue;
            }

            // DTESP_JOB_SPEAK_TEXT: synthesise text
            estate.speaking = 1;
            estate.status |= STAT_tr_char;

            char *text = job->text;

            ESP_LOGD(TAG, "Speech task: text (%d bytes): \"%.60s%s\"",
                     (int)strlen(text),
                     text,
                     strlen(text) > 60 ? "..." : "");
            ESP_LOGD(TAG, "Speech task: queue depth before speak: %d/%d",
                     (int)uxQueueMessagesWaiting(speech_queue),
                     DTESP_QUEUE_SIZE);

            if (text[0] == '\0')
            {
                ESP_LOGD(TAG, "Speech task: text empty, skipping synthesis");
                dtesp_job_pool_free(job);
                estate.speaking = 0;
                estate.status &= ~STAT_tr_char;
                estate.status |= STAT_rr_char | STAT_cmd_ready;
                dtesp_check_flow_control();
                continue;
            }

            // Prepend current session voice/rate prefixes in-order so
            // DECtalk sees the selected voice/rate for this chunk.
            // This runs on the speech task (after any preceding ACTION
            // jobs have executed), ensuring the prefix reflects the
            // true session state at the point this text is spoken.
            const char *vp = custom_action_get_voice_prefix();
            const char *rp = custom_action_get_rate_prefix();
            char *composed = NULL;
            if (vp || rp)
            {
                int vp_len = vp ? (int)strlen(vp) : 0;
                int rp_len = rp ? (int)strlen(rp) : 0;
                int txt_len = (int)strlen(text);
                composed = malloc((size_t)(vp_len + rp_len + txt_len) + 1);
                if (composed)
                {
                    int pos = 0;
                    if (vp)
                    {
                        memcpy(composed + pos, vp, (size_t)vp_len);
                        pos += vp_len;
                    }
                    if (rp)
                    {
                        memcpy(composed + pos, rp, (size_t)rp_len);
                        pos += rp_len;
                    }
                    memcpy(composed + pos, text, (size_t)txt_len + 1);
                    text = composed;
                }
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
            ESP_LOGD(TAG, "Speech task: calling TextToSpeechSpeak()...");
            TextToSpeechSpeak(dtesp_tts_handle, text, TTS_FORCE);
            TextToSpeechSync(dtesp_tts_handle);
            ESP_LOGD(TAG, "Speech task: TextToSpeechSpeak()+Sync() returned");
            free(composed);
            dtesp_job_pool_free(job);

            estate.speaking = 0;
            estate.status &= ~STAT_tr_char;
            estate.status |= STAT_rr_char | STAT_cmd_ready;
            dtesp_check_flow_control();
        }
    }

    return NULL; // unreachable - task loops forever
}

// -- Main Protocol Loop ----------------------------------------------

// Process a single byte received from the host.
static void dtesp_process_byte(uint8_t c)
{
    // If collecting a DLE sequence, feed bytes to state machine
    if (estate.rx_state >= RX_STATE_DLE_1 && estate.rx_state <= RX_STATE_DLE_3)
    {
        dtesp_dle_byte(c);
        return;
    }

    // Control character handling
    switch (c)
    {
    case XON:
        ESP_LOGD(TAG, "RX XON (host resumes device TX)");
        estate.host_xoff = 0;
        return;

    case XOFF:
        ESP_LOGD(TAG, "RX XOFF (host pauses device TX)");
        estate.host_xoff = 1;
        return;

    case DLE:
        estate.rx_state = RX_STATE_DLE_1;
        estate.dle_buf[0] = DLE;
        return;

    case ETX:
        ESP_LOGD(TAG, "RX ETX (flush/cancel all speech)");
        dtesp_handle_etx();
        return;

    case ENQ:
        ESP_LOGD(TAG, "RX ENQ (host requests status)");
        dtesp_send_status();
        return;

    case SO:
        ESP_LOGD(TAG, "RX SO (pause speech output)");
        estate.paused = 1;
        return;

    case SI:
        ESP_LOGD(TAG, "RX SI (resume speech output)");
        estate.paused = 0;
        return;

    case VT:
        ESP_LOGD(TAG, "RX VT (sync marker)");
        // Sync marker: pass through as regular character
        break;

    case SOH:
        // SOH from host: ignore (this is a device->host byte)
        ESP_LOGD(TAG, "RX SOH (unexpected from host, ignored)");
        return;

    default:
        if (dtesp_is_sync_char(c))
        {
            dtesp_handle_sync_char("raw 0xFF");
            return;
        }

        // Accept 7-bit ASCII printable and VT/CR/LF/TAB only.  The
        // DECtalk TTS engine handles only 7-bit input and can hang on
        // high-bit bytes (0x80-0xFF), so they are dropped at the
        // protocol boundary.  Control characters below 0x20 that we
        // do not explicitly handle are also dropped.
        if (c > 0x7F)
        {
            return;
        }
        if (c < 0x20 && c != '\r' && c != '\n' && c != '\t')
        {
            return;
        }
        break;
    }

    // Regular text character (or VT/CR/LF/TAB): add to buffer
    dtesp_add_char(c);
}

// ----------------------------------------------------------------
// DECtalk ESPress protocol emulation pthread entry point.
// Initialises the TTS engine and USB CDC-ACM transport, then enters
// the main protocol loop that reads host bytes, decodes DLE commands,
// and dispatches text to the speech queue.  Does not return.
// ----------------------------------------------------------------
static void *dtesp_task(void *arg)
{
    (void)arg;

    // Initialize state
    estate.status = STAT_rr_char | STAT_cmd_ready;

    // Initialize DECtalk engine with the ttsapi interface
    ESP_LOGI(TAG, "Initializing DECtalk TTS engine (ttsapi)...");
    MMRESULT tts_status = TextToSpeechStartup(&dtesp_tts_handle,
                                              WAVE_MAPPER,
                                              DO_NOT_USE_AUDIO_DEVICE,
                                              dtesp_tts_callback,
                                              0);
    if (tts_status != MMSYSERR_NOERROR)
    {
        ESP_LOGE(TAG, "TextToSpeechStartup failed with code %d", (int)tts_status);
        ESP_LOGE(TAG, "Cannot continue without TTS engine, restarting...");
        esp_restart();
    }

    // Switch TTS to in-memory mode: synthesised PCM is delivered via
    // the audio callback rather than played through a system device.
    TextToSpeechOpenInMemory(dtesp_tts_handle, WAVE_FORMAT_1M16);

    // Initialise the pre-allocated buffer pool and hand each buffer
    // to the TTS engine so it can begin filling them with audio data.
    for (int i = 0; i < DTESP_TTS_NUM_BUFFERS; i++)
    {
        memset(&dtesp_tts_bufs[i], 0, sizeof(TTS_BUFFER_T));
        dtesp_tts_bufs[i].lpData = dtesp_tts_audio[i];
        dtesp_tts_bufs[i].dwMaximumBufferLength = DTESP_TTS_BUFFER_SIZE;
        dtesp_tts_bufs[i].lpIndexArray = dtesp_tts_indexes[i];
        dtesp_tts_bufs[i].dwMaximumNumberOfIndexMarks = DTESP_TTS_MAX_INDEXES;
        TextToSpeechAddBuffer(dtesp_tts_handle, &dtesp_tts_bufs[i]);
    }
    ESP_LOGI(TAG, "DECtalk TTS engine initialized (handle=%p)", (void *)dtesp_tts_handle);

    // Install the serial transport for ESPress protocol communication.
    // The host sees the device as a USB serial port.
    // Console / ESP_LOG output remains on UART0.
    ESP_LOGI(TAG, "Initializing serial transport...");
    s_transport = dtesp_transport_get();
    ESP_ERROR_CHECK(s_transport->init());

    ESP_LOGI(TAG, "ESPress protocol ready. Waiting for host communication "
             "on USB serial.");

    // Send initial XON to indicate device is ready.
    // The real DECtalk ESPress hardware (serial_task in serial.c) sends
    // only XON at startup -- no unsolicited DLE STATUS.  Match that
    // behaviour so the host driver (e.g. JAWS) does not interpret the
    // extra 4-byte DLE sequence as unexpected/bogus data.
    ESP_LOGI(TAG, "TX XON (device ready)");
    dtesp_send_byte(XON);

    // Track time since last character for idle flush
    TickType_t last_char_time = xTaskGetTickCount();
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
        if (s_transport->check_reconnected())
        {
            ESP_LOGI(TAG, "Host reconnected -- resetting protocol state");

            // Cancel any in-progress synthesis first
            TextToSpeechReset(dtesp_tts_handle, FALSE);
            dtesp_send_flush();

            // Reset receive state machine (DLE + bracket flush tracker)
            estate.rx_state = RX_STATE_NORMAL;

            // Reset text buffer and bracket state
            estate.text_pos = 0;
            estate.in_bracket_cmd = 0;

            // Reset flow control
            estate.host_xoff = 0;
            estate.xoff_sent = 0;

            // Reset speech/flush flags
            estate.paused = 0;

            // Restore clean status
            estate.status = STAT_rr_char | STAT_cmd_ready;

            // Reset custom command session state if configured
#if defined(CONFIG_DTESP_FW_CMD_ENABLE) && defined(CONFIG_DTESP_FW_CMD_RESET_ON_RECONNECT)
            custom_commands_reset_session();
#endif

            // Signal readiness to the new host
            ESP_LOGI(TAG, "TX XON (device ready after reconnect)");
            dtesp_send_byte(XON);
        }

        // Read one byte with timeout -- yields to scheduler, preventing WDT
        cnt = s_transport->read(&rx_byte, 1, pdMS_TO_TICKS(DTESP_RX_TIMEOUT_MS));
        if (cnt > 0)
        {
            c = (int)rx_byte;
            rx_byte_count++;
        }
        else
        {
            // No data available - check for idle text flush.
            // Suppress idle flush while inside a [: ... ] sequence so
            // that custom or native DECtalk commands are not split
            // across separate queue entries.
            bool bracket_pending =
                (estate.rx_state == RX_STATE_BRACKET_ETX ||
                 estate.rx_state == RX_STATE_ETX_XON);

            if (estate.text_pos > 0 || bracket_pending)
            {
                TickType_t elapsed = xTaskGetTickCount() - last_char_time;
                if (elapsed >= pdMS_TO_TICKS(TEXT_IDLE_TIMEOUT_MS) &&
                    !estate.in_bracket_cmd)
                {
                    // Release any held ']' from the flush-sequence
                    // detector before flushing.  Without this, a DECtalk
                    // command like [:version speak] would be split into
                    // "[:version speak" (without the closing bracket),
                    // and the TTS_FORCE character appended by
                    // TextToSpeechSpeak() would corrupt the string
                    // parameter, causing "command error in string value".
                    if (bracket_pending)
                    {
                        dtesp_add_char(']');
                        if (estate.rx_state == RX_STATE_ETX_XON)
                        {
                            dtesp_process_byte(ETX);
                        }
                        estate.rx_state = RX_STATE_NORMAL;
                    }

                    ESP_LOGD(TAG, "Idle timeout: flushing %d buffered text bytes to queue",
                             estate.text_pos);
                    dtesp_flush_text_to_queue();
                }
            }
            continue;
        }

        last_char_time = xTaskGetTickCount();

        // Detect the ']' + ETX + XON flush sequence from TSR FLUSH_TEXT.
        // State machine via rx_state enum.
        if (estate.rx_state == RX_STATE_BRACKET_ETX && c == ETX)
        {
            estate.rx_state = RX_STATE_ETX_XON;
            continue;
        }
        else if (estate.rx_state == RX_STATE_ETX_XON && c == XON)
        {
            estate.rx_state = RX_STATE_NORMAL;
            dtesp_handle_flush_sequence();
            continue;
        }
        else if (estate.rx_state == RX_STATE_BRACKET_ETX ||
                 estate.rx_state == RX_STATE_ETX_XON)
        {
            // Sequence broken: process the ']' as text, then current byte
            dtesp_add_char(']');
            if (estate.rx_state == RX_STATE_ETX_XON)
            {
                dtesp_process_byte(ETX);
            }
            estate.rx_state = RX_STATE_NORMAL;
            // Fall through to process current byte normally
        }

        if (c == ']')
        {
            estate.rx_state = RX_STATE_BRACKET_ETX;
            continue;
        }

        dtesp_process_byte((uint8_t)c);
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

#if CONFIG_DTESP_ENABLE_DIAG_MEM
    diag_mem_start();
#endif

#if CONFIG_DTESP_DISABLE_RGB_LED
    // Drive the onboard RGB LED data line low to keep the LED dark.
    gpio_config_t led_cfg = {
        .pin_bit_mask = 1ULL << CONFIG_DTESP_RGB_LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&led_cfg));
    ESP_ERROR_CHECK(gpio_set_level(CONFIG_DTESP_RGB_LED_GPIO, 0));
#endif

    // Initialize audio subsystem (I2S + codec if configured)
    ESP_ERROR_CHECK(dtesp_audio_init());

    // Create the speech queue and initialise the custom-command
    // subsystem BEFORE spawning the worker threads so both threads see
    // a fully-initialised queue from their first iteration.
    speech_queue = xQueueCreate(DTESP_QUEUE_SIZE, sizeof(dtesp_job_t *));
    ESP_ERROR_CHECK(speech_queue != NULL ? ESP_OK : ESP_ERR_NO_MEM);
    dtesp_job_pool_init();
    custom_commands_init();

    // Retrieve the default pthread configuration
    esp_pthread_cfg_t default_cfg = esp_pthread_get_default_config();
    esp_pthread_cfg_t thread_cfg;
    pthread_t tid;

    // Define the speech thread attributes
    //
    // TextToSpeechSpeak()+Sync() is CPU-intensive and does not yield to
    // the FreeRTOS scheduler while processing text.  On dual-core chips
    // (e.g. ESP32-S3) the speech task is pinned to CPU 1 so IDLE0 can
    // always service the Task Watchdog Timer.  On single-core chips
    // (e.g. ESP32-C6) the watchdog is disabled instead (see
    // sdkconfig.defaults).
    thread_cfg = default_cfg;
    thread_cfg.pin_to_core = DTESP_SPEECH_TASK_CORE;
    thread_cfg.thread_name = "speech_thread";
    esp_pthread_set_cfg(&thread_cfg);

    // Create the speech thread
    int prc = pthread_create(&tid, NULL, speech_task, NULL);
    if (prc != 0)
    {
        ESP_LOGE(TAG, "pthread_create(speech_task) failed: %d", prc);
        esp_restart();
    }

    // Define the main thread attributes
    thread_cfg = default_cfg;
    thread_cfg.stack_size = DTESP_MAIN_STACK_SIZE;
    thread_cfg.thread_name = "main_thread";
    esp_pthread_set_cfg(&thread_cfg);

    // Create the main thread
    prc = pthread_create(&tid, NULL, dtesp_task, NULL);
    if (prc != 0)
    {
        ESP_LOGE(TAG, "pthread_create(dtesp_task) failed: %d", prc);
        esp_restart();
    }

    // Restore default pthread config so any later threads are unaffected.
    esp_pthread_set_cfg(&default_cfg);

    // And allow the default task to cleanup and terminate
    return;
}
