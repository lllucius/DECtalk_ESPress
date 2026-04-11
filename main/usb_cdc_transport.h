// SPDX-License-Identifier: MIT
// Copyright (c) 2025 Leland Lucius
// ----------------------------------------------------------------
// USB CDC-ACM Transport Layer
//
// Provides a simple read/write interface over the ESP32-S3 native USB port
// using TinyUSB CDC-ACM.  Replaces the former UART-based host transport.
// ----------------------------------------------------------------

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

// ----------------------------------------------------------------
// Initialize the USB CDC-ACM transport.
// Installs the TinyUSB driver and sets up a CDC-ACM interface on the
// ESP32-S3 native USB port.  Must be called once before any read/write.
// ----------------------------------------------------------------
esp_err_t usb_cdc_transport_init(void);

// ----------------------------------------------------------------
// Read bytes from the USB CDC-ACM receive buffer.
//
// Blocks for up to @p timeout ticks if no data is available.
// Returns the number of bytes actually read (0 on timeout).
// ----------------------------------------------------------------
int usb_cdc_transport_read(uint8_t *buf, size_t len, TickType_t timeout);

// ----------------------------------------------------------------
// Write bytes to the USB CDC-ACM transmit path.
// Queues data and flushes so the host receives it promptly.
// Silently drops data when no host is connected.
// ----------------------------------------------------------------
void usb_cdc_transport_write(const uint8_t *data, size_t len);

// ----------------------------------------------------------------
// Return true if a USB host has the CDC port open (DTR asserted).
// ----------------------------------------------------------------
bool usb_cdc_transport_connected(void);

// ----------------------------------------------------------------
// Check whether a host reconnection has occurred since the last call.
//
// Returns true exactly once per disconnect->reconnect cycle.  The
// application should use this to reset protocol state (DLE state
// machine, flow control, text buffer, etc.) and re-send the initial
// XON so the new host knows the device is ready.
// ----------------------------------------------------------------
bool usb_cdc_transport_check_reconnected(void);
