// SPDX-License-Identifier: MIT
// Copyright (c) 2025 Leland Lucius
// ----------------------------------------------------------------
// JTAG Serial Transport Layer - Implementation
//
// Uses the ESP-IDF usb_serial_jtag driver to communicate over the
// built-in USB Serial/JTAG controller found on chips such as the
// ESP32-C6.  The driver provides interrupt-driven buffered I/O with
// blocking read/write that matches the behaviour of the TinyUSB
// CDC-ACM transport used on ESP32-S3.
//
// At initialisation the hardware register that allows the host RTS
// signal to trigger a chip reset is disabled, preventing accidental
// reboots when a serial terminal opens the port.
//
// Host connection state is tracked via SOF (Start-of-Frame) packet
// detection so writes can be silently dropped when no host is
// present.
// ----------------------------------------------------------------

#include "jtag_serial_transport.h"

#include <string.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "driver/usb_serial_jtag.h"
#include "soc/usb_serial_jtag_struct.h"

static const char *TAG = "JTAG-Serial";

// ----------------------------------------------------------------
// Reconnection detection.  Tracked by polling
// usb_serial_jtag_is_connected() and watching for disconnect ->
// connect transitions.  Initialized to true so that the very first
// host connection after boot is treated as a reconnection event,
// causing the protocol layer to send the initial XON.
//
// All state here is accessed only from the single ESPress protocol
// task (via jtag_serial_transport_check_reconnected()), so no
// locking is needed.  The volatile qualifier prevents the compiler
// from caching the value across calls.
// ----------------------------------------------------------------
static volatile uint32_t jtag_reconnect_seq = 0;
static bool jtag_was_connected = false;
static bool jtag_had_disconnect = true;

// ------------------------------------------------------------------
// Public API
// ------------------------------------------------------------------

esp_err_t jtag_serial_transport_init(void)
{
    // Disable the RTS-triggered chip reset.  The USB Serial/JTAG
    // controller can interpret host RTS toggling as a reset request.
    // Setting usb_uart_chip_rst_dis prevents this so that opening a
    // serial terminal does not reboot the device.
    USB_SERIAL_JTAG.chip_rst.usb_uart_chip_rst_dis = 1;

    // Install the USB Serial/JTAG driver with application-level
    // RX and TX ring buffers.
    usb_serial_jtag_driver_config_t cfg = {
        .rx_buffer_size = CONFIG_DECTALK_JTAG_RX_BUF_SIZE,
        .tx_buffer_size = CONFIG_DECTALK_JTAG_TX_BUF_SIZE,
    };

    esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "usb_serial_jtag_driver_install failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "USB Serial/JTAG transport initialized "
             "(RTS reset disabled)");
    return ESP_OK;
}

int jtag_serial_transport_read(uint8_t *buf, size_t len, TickType_t timeout)
{
    return usb_serial_jtag_read_bytes(buf, len, timeout);
}

void jtag_serial_transport_write(const uint8_t *data, size_t len)
{
    if (!usb_serial_jtag_is_connected())
    {
        // No host -- silently drop to avoid blocking
        return;
    }

    int written = usb_serial_jtag_write_bytes(data, len, pdMS_TO_TICKS(50));
    if (written < 0)
    {
        ESP_LOGW(TAG, "JTAG Serial TX: write error");
    }
    else if ((size_t)written < len)
    {
        ESP_LOGD(TAG, "JTAG Serial TX: only %d of %u bytes written",
                 written, (unsigned)len);
    }
}

bool jtag_serial_transport_connected(void)
{
    return usb_serial_jtag_is_connected();
}

bool jtag_serial_transport_check_reconnected(void)
{
    bool is_connected = usb_serial_jtag_is_connected();

    if (is_connected && !jtag_was_connected)
    {
        // Transition from disconnected to connected
        if (jtag_had_disconnect)
        {
            jtag_reconnect_seq++;
            ESP_LOGI(TAG, "Host connected (reconnect seq=%lu)",
                     (unsigned long)jtag_reconnect_seq);
        }
        jtag_was_connected = true;
    }
    else if (!is_connected && jtag_was_connected)
    {
        ESP_LOGI(TAG, "Host disconnected");
        jtag_was_connected = false;
        jtag_had_disconnect = true;
    }

    // Return true exactly once per reconnection
    static uint32_t last_seen_seq = 0;
    uint32_t current = jtag_reconnect_seq;
    if (current != last_seen_seq)
    {
        last_seen_seq = current;
        return true;
    }

    return false;
}
