// SPDX-License-Identifier: MIT
// Copyright (c) 2025 Leland Lucius
// ----------------------------------------------------------------
// USB CDC-ACM Transport Layer - Implementation
//
// Uses the ESP-IDF esp_tinyusb component to present the ESP32-S3 as a
// USB CDC-ACM device.  Incoming data is buffered in a FreeRTOS stream
// buffer so the application can perform blocking reads with timeout,
// matching the former uart_read_bytes() behaviour.
//
// Host connection state is tracked via DTR so writes are silently
// dropped when the PC-side application has closed the COM port,
// preventing repeated flush-timeout warnings.
// ----------------------------------------------------------------

#include "usb_cdc_transport.h"

#include <string.h>
#include "sdkconfig.h"
#include "freertos/stream_buffer.h"
#include "esp_log.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_cdc_acm.h"

static const char *TAG = "USB-CDC";

// Stream buffer bridging the TinyUSB task and the application task
#define CDC_RX_STREAM_SIZE   CONFIG_DECTALK_CDC_RX_STREAM_SIZE
#define CDC_RX_TRIGGER_LEVEL 1

static StreamBufferHandle_t rx_stream;

// ----------------------------------------------------------------
// Host connection state.  Updated from the line-state callback when
// the host asserts or de-asserts DTR (Data Terminal Ready).  Most PC
// serial-port drivers assert DTR when the port is opened and
// de-assert it when the port is closed.
//
// Access is atomic on the ESP32 (single-word aligned bool), so no
// mutex is needed between the TinyUSB callback context and the
// application task.
// ----------------------------------------------------------------
static volatile bool cdc_connected = false;

// ----------------------------------------------------------------
// Reconnection detection.  The callback increments the counter each
// time a disconnect->connect transition occurs.  The counter starts
// at 1 so the very first call from the protocol task returns true
// once, causing the protocol layer to send the initial XON.  (The
// XON sent in app_main before USB is enumerated is silently dropped
// because cdc_connected is still false at that point.)
//
// The application task compares against its own last-seen value --
// a single uint32_t read is atomic on ESP32, so no lock is needed.
// ----------------------------------------------------------------
static volatile uint32_t cdc_reconnect_seq = 1;

// ------------------------------------------------------------------
// CDC-ACM callbacks (called from the TinyUSB task)
// ------------------------------------------------------------------

// Drain all available data from the CDC internal buffer into the
// application-side stream buffer.
static void cdc_rx_callback(int itf, cdcacm_event_t *event)
{
    uint8_t buf[64]; // USB full-speed bulk max packet size
    size_t rx_size = 0;

    while (tinyusb_cdcacm_read(itf, buf, sizeof(buf), &rx_size) == ESP_OK
           && rx_size > 0)
    {
        size_t sent = xStreamBufferSend(rx_stream, buf, rx_size, 0);
        if (sent < rx_size)
        {
            ESP_LOGW(TAG, "CDC RX: stream buffer full, dropped %u bytes",
                     (unsigned)(rx_size - sent));
        }
        rx_size = 0;
    }
}

// Accept line-coding changes from the host (baud rate, stop-bits, etc.)
// without reconfiguring any hardware -- native USB CDC ignores them.
static void cdc_line_coding_callback(int itf, cdcacm_event_t *event)
{
    const cdc_line_coding_t *lc =
        event->line_coding_changed_data.p_line_coding;
    ESP_LOGI(TAG, "Host set line coding: baud=%lu, data_bits=%u, "
             "parity=%u, stop_bits=%u",
             (unsigned long)lc->bit_rate,
             lc->data_bits,
             lc->parity,
             lc->stop_bits);
}

// ----------------------------------------------------------------
// Track host connection state via DTR.  When the PC application opens
// the COM port DTR is typically asserted (dtr=1); when it closes the
// port DTR is de-asserted (dtr=0).
//
// On disconnect we reset the RX stream buffer so stale partial data
// from a previous session does not confuse the protocol parser when
// the host reconnects.
// ----------------------------------------------------------------
static void cdc_line_state_callback(int itf, cdcacm_event_t *event)
{
    int dtr = event->line_state_changed_data.dtr;
    int rts = event->line_state_changed_data.rts;

    bool was_connected = cdc_connected;
    cdc_connected = (dtr != 0);

    if (cdc_connected && !was_connected)
    {
        ESP_LOGI(TAG, "Host connected (DTR=%d RTS=%d)", dtr, rts);
        // Every connect transition bumps the counter so the
        // application layer resets its protocol state on the next
        // poll of usb_cdc_transport_check_reconnected().
        cdc_reconnect_seq++;
    }
    else if (!cdc_connected && was_connected)
    {
        ESP_LOGI(TAG, "Host disconnected (DTR=%d RTS=%d)", dtr, rts);
        // Discard any stale RX data so the next session starts clean
        xStreamBufferReset(rx_stream);
    }
    else
    {
        ESP_LOGD(TAG, "Line state: DTR=%d RTS=%d", dtr, rts);
    }
}

// ------------------------------------------------------------------
// Public API
// ------------------------------------------------------------------

esp_err_t usb_cdc_transport_init(void)
{
    // Create the stream buffer used to bridge the USB and app tasks
    rx_stream = xStreamBufferCreate(CDC_RX_STREAM_SIZE,
                                    CDC_RX_TRIGGER_LEVEL);
    if (rx_stream == NULL)
    {
        ESP_LOGE(TAG, "Failed to create RX stream buffer");
        return ESP_ERR_NO_MEM;
    }

    // Install the TinyUSB device driver (default descriptors)
    const tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    esp_err_t err = tinyusb_driver_install(&tusb_cfg);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "tinyusb_driver_install failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    // Initialise CDC-ACM interface 0
    const tinyusb_config_cdcacm_t acm_cfg =
    {
        .cdc_port                     = TINYUSB_CDC_ACM_0,
        .callback_rx                  = cdc_rx_callback,
        .callback_line_coding_changed = cdc_line_coding_callback,
        .callback_line_state_changed  = cdc_line_state_callback,
    };
    err = tinyusb_cdcacm_init(&acm_cfg);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "tinyusb_cdcacm_init failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "USB CDC-ACM transport initialized");
    return ESP_OK;
}

int usb_cdc_transport_read(uint8_t *buf, size_t len, TickType_t timeout)
{
    return (int)xStreamBufferReceive(rx_stream, buf, len, timeout);
}

void usb_cdc_transport_write(const uint8_t *data, size_t len)
{
    if (!cdc_connected)
    {
        // Host is not listening -- silently drop to avoid flush timeouts
        return;
    }

    size_t queued = tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, data, len);
    if (queued < len)
    {
        ESP_LOGD(TAG, "CDC TX: only %u of %u bytes queued",
                 (unsigned)queued,
                 (unsigned)len);
    }

    // Use a short flush timeout.  If the host just disconnected between
    // the check above and this call, the flush will fail quickly rather
    // than blocking for a long time.  ESP_ERR_TIMEOUT during disconnect
    // is expected and logged at debug level only.
    esp_err_t err = tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0,
                                               pdMS_TO_TICKS(50));
    if (err != ESP_OK && err != ESP_ERR_NOT_FINISHED)
    {
        if (cdc_connected)
        {
            // Genuine failure while supposedly connected
            ESP_LOGW(TAG, "CDC TX flush: %s", esp_err_to_name(err));
        }
        else
        {
            // Host went away -- expected, do not spam warnings
            ESP_LOGD(TAG, "CDC TX flush after disconnect: %s",
                     esp_err_to_name(err));
        }
    }
}

bool usb_cdc_transport_connected(void)
{
    return cdc_connected;
}

bool usb_cdc_transport_check_reconnected(void)
{
    static uint32_t last_seen_seq = 0;
    uint32_t current = cdc_reconnect_seq;
    if (current != last_seen_seq)
    {
        last_seen_seq = current;
        return true;
    }

    return false;
}
