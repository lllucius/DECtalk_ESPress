// SPDX-License-Identifier: MIT
// Copyright (c) 2025 Leland Lucius

#ifndef TLV320DAC3100_H
#define TLV320DAC3100_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum
{
    TLV320_PROFILE_SPEAKER = 0,
    TLV320_PROFILE_HEADPHONE
} tlv320_profile_t;

/**
 * @brief Initialize the TLV320DAC3100 DAC over I2C.
 *
 * Performs a software reset and configures the codec for I2S slave
 * operation with CODEC_CLKIN = BCLK (no PLL, no MCLK).  Startup
 * profile, startup volume, optional hardware reset, and optional
 * deferred codec event handling are taken from Kconfig.
 *
 * Must be called before I2S streaming begins so that the codec is
 * ready to accept audio data.
 *
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t tlv320dac3100_init(void);

/**
 * @brief Switch between the speaker and headphone output profiles.
 *
 * Reconfigures routing, reapplies conservative analog/output-path defaults,
 * and updates the speech EQ scaffold for the selected output path while
 * preserving the current digital volume after initialization.
 *
 * @param profile  Output profile to apply.
 *
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t tlv320dac3100_set_profile(tlv320_profile_t profile);

/**
 * @brief Set the DAC digital volume in dB.
 *
 * @param db  Desired digital gain in dB, clamped to a safe range.
 *
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t tlv320dac3100_set_volume_db(float db);

/**
 * @brief Mute or unmute the codec output.
 *
 * Applies a lightweight soft mute by forcing the DAC digital volume to a
 * near-minimum setting instead of toggling a dedicated hardware mute bit.
 *
 * @param enable  True to mute, false to restore the current target volume.
 *
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t tlv320dac3100_mute(bool enable);

/**
 * @brief Check headset detection and switch audio outputs.
 *
 * Reads the TLV320DAC3100 headset detection status register.
 * When a headphone is detected the speaker is muted and headphone
 * drivers are enabled.  When removed, the headphones are disabled
 * and the speaker is re-enabled. This may attempt profile switching
 * internally.
 *
 * The driver also uses this internally for its deferred event path
 * when headset auto-switching or codec IRQ handling is enabled.
 *
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t tlv320dac3100_check_headset(void);

/**
 * @brief Timer callback for polling headset detection.
 */
void tlv320dac3100_poll_headset(void);


/**
 * @brief Enable or disable headset auto-switching at runtime.
 *
 * When enabled, the driver automatically switches between the speaker
 * and headphone profiles in response to headset insertion/removal.
 * When disabled, the active profile stays as-is until changed through
 * an explicit tlv320dac3100_set_profile() call.
 *
 * If event handling infrastructure was not started during init (e.g.
 * because autoswitch was off at compile time and no IRQ GPIO was
 * configured), calling this with @c true will start it.
 *
 * @param enabled  true to enable auto-switching, false to disable.
 *
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t tlv320dac3100_set_autoswitch(bool enabled);

/**
 * @brief Query the current headset auto-switch setting.
 *
 * @return true if headset auto-switching is active.
 */
bool tlv320dac3100_get_autoswitch(void);

/**
 * @brief Get the currently active output profile.
 *
 * @return The active profile (TLV320_PROFILE_SPEAKER or
 *         TLV320_PROFILE_HEADPHONE).
 */
tlv320_profile_t tlv320dac3100_get_profile(void);

/**
 * @brief Maximum volume level.
 */
#define TLV320DAC3100_MAX_VOLUME 9

/**
 * @brief Set the DAC digital volume.
 *
 * @param level  Volume level 0–TLV320DAC3100_MAX_VOLUME
 *               (0 = near-mute, TLV320DAC3100_MAX_VOLUME = 0 dB).
 */
void tlv320dac3100_set_volume(uint8_t level);

/**
 * @brief Get the current volume level.
 *
 * @return Current volume level 0–9.
 */
uint8_t tlv320dac3100_get_volume(void);

#endif // TLV320DAC3100_H
