// SPDX-License-Identifier: MIT
// Copyright (c) 2025 Leland Lucius

#ifndef TLV320_H
#define TLV320_H

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
 * operation.  When MCLK is wired the codec uses CODEC_CLKIN = MCLK
 * directly; otherwise the internal PLL locks to BCLK and feeds
 * CODEC_CLKIN.  Startup profile, startup volume, optional hardware
 * reset, and optional deferred codec event handling are taken from
 * Kconfig.
 *
 * Must be called before I2S streaming begins so that the codec is
 * ready to accept audio data.
 *
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t tlv320_init(void);

/**
 * @brief Switch between the speaker and headphone output profiles.
 *
 * Reconfigures routing and reapplies conservative analog/output-path
 * defaults for the selected output path while preserving the current
 * digital volume after initialization.
 *
 * @param profile  Output profile to apply.
 *
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t tlv320_set_profile(tlv320_profile_t profile);

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
esp_err_t tlv320_mute(bool enable);

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
esp_err_t tlv320_check_headset(void);

/**
 * @brief Timer callback for polling headset detection.
 */
void tlv320_poll_headset(void);


/**
 * @brief Maximum volume level.
 */
#define TLV320_MAX_VOLUME 9

/**
 * @brief Set the DAC digital volume.
 *
 * @param level  Volume level 0–TLV320_MAX_VOLUME
 *               (0 = near-mute, TLV320_MAX_VOLUME = 0 dB).
 *               Thin wrapper that maps to the dB-based setter.
 */
void tlv320_set_volume(uint8_t level);

/**
 * @brief Get the current volume level.
 *
 * @return Current volume level 0–9.
 */
uint8_t tlv320_get_volume(void);

/**
 * @brief Set the DAC digital volume in decibels.
 *
 * Accepts values from -63.5 dB to 0.0 dB.  Out-of-range values are
 * clamped.  The nearest discrete level (0–9) is stored for
 * `tlv320_get_volume()`.
 *
 * @param db  Target volume in dB (0 dB = full scale).
 */
void tlv320_set_volume_db(float db);

/**
 * @brief Get the current volume in decibels.
 *
 * @return Current volume setting in dB.
 */
float tlv320_get_volume_db(void);

/**
 * @brief Enable or disable headset auto-switching at runtime.
 *
 * When enabled, a detected headset insertion switches to the
 * headphone profile and removal switches back to the speaker
 * profile.  When disabled, profile changes must be performed
 * explicitly.  Initial value is taken from Kconfig.
 */
void tlv320_set_autoswitch(bool enable);

/**
 * @brief Query the current headset auto-switching flag.
 */
bool tlv320_get_autoswitch(void);

/**
 * @brief Query the currently-active output profile.
 */
tlv320_profile_t tlv320_get_profile(void);

// ----------------------------------------------------------------
// DSP / EQ / analog-gain extensions
// ----------------------------------------------------------------

#include "tlv320_dsp.h"

/**
 * @brief Apply a DSP state (EQ bands + DRC) to the codec.
 *
 * Briefly mutes while rewriting biquad coefficients so the
 * transition is silent.  A "flat" state leaves the codec in
 * PRB_P1 (cheap processing block) with DRC disabled — i.e. the
 * pre-DSP-feature behaviour.
 *
 * @return ESP_OK on success, an ESP-IDF error on hardware fault.
 */
esp_err_t tlv320_apply_dsp(const tlv320_dsp_state_t *state);

/**
 * @brief Allowed class-D speaker-driver analog gain stages (dB).
 *
 * The class-D amplifier's gain is coarsely quantised by the codec.
 * Per TLV320DAC3100 datasheet (SLAS833) REG_SPK_DRIVER bits 4:3:
 * 00=6 dB, 01=12 dB, 10=18 dB, 11=24 dB.  6 dB matches the pre-
 * feature firmware behaviour.
 */
#define TLV320_SPK_GAIN_VALID(db) \
    ((db) == 6 || (db) == 12 || (db) == 18 || (db) == 24)

/**
 * @brief Set the class-D speaker driver analog gain.
 *
 * Only affects the speaker output path.  No-op when the codec is
 * currently routed to headphones.  Valid values: 6, 12, 18, 24.
 * Other values are rejected with ESP_ERR_INVALID_ARG.
 */
esp_err_t tlv320_set_speaker_gain_db(uint8_t db);

/**
 * @brief Return the current class-D speaker driver analog gain in dB.
 */
uint8_t tlv320_get_speaker_gain_db(void);

#endif // TLV320_H
