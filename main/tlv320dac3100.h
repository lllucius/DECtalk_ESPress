// SPDX-License-Identifier: MIT
// Copyright (c) 2025 Leland Lucius

#ifndef TLV320DAC3100_H
#define TLV320DAC3100_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Initialize the TLV320DAC3100 DAC over I2C.
 *
 * Performs a software reset and configures the codec for I2S slave
 * operation with CODEC_CLKIN = BCLK (no PLL, no MCLK).  The speaker
 * amplifier is enabled and headphones are disabled by default.
 * Headset detection is enabled so that the polling function can
 * switch between speaker and headphone outputs.
 *
 * Must be called before I2S streaming begins so that the codec is
 * ready to accept audio data.
 *
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t tlv320dac3100_init(void);

/**
 * @brief Poll headset detection and switch audio outputs.
 *
 * Reads the TLV320DAC3100 headset detection status register.
 * When a headphone is detected the speaker is muted and headphone
 * drivers are enabled.  When removed, the headphones are disabled
 * and the speaker is re-enabled.
 *
 * Call this function periodically (e.g. every 500 ms) from a timer
 * or background task.
 */
void tlv320dac3100_poll_headset(void);

/**
 * @brief Set the DAC digital volume.
 *
 * @param level  Volume level 0–9 (0 = near-mute, 9 = 0 dB).
 */
void tlv320dac3100_set_volume(uint8_t level);

/**
 * @brief Get the current volume level.
 *
 * @return Current volume level 0–9.
 */
uint8_t tlv320dac3100_get_volume(void);

#endif // TLV320DAC3100_H
