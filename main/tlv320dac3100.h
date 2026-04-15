// SPDX-License-Identifier: MIT
// Copyright (c) 2025 Leland Lucius

#ifndef TLV320DAC3100_H
#define TLV320DAC3100_H

#include "esp_err.h"

/**
 * @brief Initialize the TLV320DAC3100 DAC over I2C.
 *
 * Performs a software reset and configures the codec for I2S slave
 * operation with CODEC_CLKIN = MCLK (256 × Fs, no PLL).  Both
 * headphone outputs and the class-D speaker amplifier are enabled
 * at 0 dB gain.
 *
 * Must be called before I2S streaming begins so that the codec is
 * ready to accept audio data.
 *
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t tlv320dac3100_init(void);

#endif // TLV320DAC3100_H
