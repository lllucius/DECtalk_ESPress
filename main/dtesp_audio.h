// SPDX-License-Identifier: MIT
// Copyright (c) 2025 Leland Lucius
// ----------------------------------------------------------------
// Audio Subsystem — Public API
//
// Initializes I2S output and (when selected) the TLV320DAC3100
// codec.  The returned channel handle is used by the TTS audio
// callback to write PCM samples.
// ----------------------------------------------------------------

#ifndef DTESP_AUDIO_H
#define DTESP_AUDIO_H

#include "esp_err.h"
#include "driver/i2s_std.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the I2S audio output (and codec if configured).
 *
 * Must be called before the speech task starts.
 * @return ESP_OK on success.
 */
esp_err_t dtesp_audio_init(void);

/**
 * @brief Get the I2S TX channel handle for writing audio samples.
 */
i2s_chan_handle_t dtesp_audio_get_handle(void);

#ifdef __cplusplus
}
#endif

#endif // DTESP_AUDIO_H
