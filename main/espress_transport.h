// SPDX-License-Identifier: MIT
// Copyright (c) 2025 Leland Lucius
// ----------------------------------------------------------------
// Transport Layer vtable
//
// Abstracts the serial transport so that the protocol handler is
// decoupled from the physical link (USB CDC-ACM, USB Serial/JTAG,
// or future BLE/Wi-Fi/UART transports).  Each implementation
// provides a static const instance of this struct.
// ----------------------------------------------------------------

#ifndef ESPRESS_TRANSPORT_H
#define ESPRESS_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct espress_transport
{
    /**
     * @brief One-time hardware / driver initialization.
     * @return ESP_OK on success.
     */
    esp_err_t (*init)(void);

    /**
     * @brief Read bytes from the transport receive buffer.
     *
     * Blocks for up to @p timeout ticks if no data is available.
     * @return Number of bytes actually read (0 on timeout).
     */
    int (*read)(uint8_t *buf, size_t len, TickType_t timeout);

    /**
     * @brief Write bytes to the transport transmit path.
     * Silently drops data when no host is connected.
     */
    void (*write)(const uint8_t *data, size_t len);

    /**
     * @brief Return true if a host is connected.
     */
    bool (*connected)(void);

    /**
     * @brief Check whether a host reconnection has occurred since last call.
     *
     * Returns true exactly once per disconnect→reconnect cycle.
     */
    bool (*check_reconnected)(void);
} espress_transport_t;

/**
 * @brief Get the transport instance for the current target.
 *
 * Returns a pointer to the static const transport vtable selected at
 * build time (USB CDC-ACM on ESP32-S3, USB Serial/JTAG on ESP32-C6).
 */
const espress_transport_t *espress_transport_get(void);

#ifdef __cplusplus
}
#endif

#endif // ESPRESS_TRANSPORT_H
