// SPDX-License-Identifier: MIT
// Copyright (c) 2025 Leland Lucius
// ----------------------------------------------------------------
// JTAG Serial Transport Layer
//
// Provides a simple read/write interface over the ESP32-C6 built-in
// USB Serial/JTAG controller.  Replaces the TinyUSB CDC-ACM transport
// used on chips with native USB-OTG (e.g. ESP32-S3).
//
// The RTS-triggered chip reset is disabled during initialisation so
// that opening a host serial terminal does not reboot the device.
// ----------------------------------------------------------------

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

// ----------------------------------------------------------------
// Initialize the USB Serial/JTAG transport.
// Installs the driver and disables RTS-triggered chip resets.
// Must be called once before any read/write.
// ----------------------------------------------------------------
esp_err_t jtag_serial_transport_init(void);

// ----------------------------------------------------------------
// Read bytes from the USB Serial/JTAG receive buffer.
//
// Blocks for up to @p timeout ticks if no data is available.
// Returns the number of bytes actually read (0 on timeout).
// ----------------------------------------------------------------
int jtag_serial_transport_read(uint8_t *buf, size_t len, TickType_t timeout);

// ----------------------------------------------------------------
// Write bytes to the USB Serial/JTAG transmit path.
// Silently drops data when no host is connected.
// ----------------------------------------------------------------
void jtag_serial_transport_write(const uint8_t *data, size_t len);

// ----------------------------------------------------------------
// Return true if the USB Serial/JTAG port is connected to a host.
// Uses SOF packet detection -- true whenever a USB host is present,
// even if no serial terminal has the port open.
// ----------------------------------------------------------------
bool jtag_serial_transport_connected(void);

// ----------------------------------------------------------------
// Check whether a host reconnection has occurred since the last call.
//
// Returns true exactly once per disconnect->reconnect cycle.  The
// application should use this to reset protocol state (DLE state
// machine, flow control, text buffer, etc.) and re-send the initial
// XON so the new host knows the device is ready.
// ----------------------------------------------------------------
bool jtag_serial_transport_check_reconnected(void);
