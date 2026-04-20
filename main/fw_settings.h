// SPDX-License-Identifier: MIT
// Copyright (c) 2025 Leland Lucius
// ----------------------------------------------------------------
// Firmware Settings — NVS-backed mirror of codec-related Kconfig.
//
// The Kconfig values (DTESP_TLV320_STARTUP_VOLUME, default profile,
// headset autoswitch) act as factory defaults.  At runtime these
// three settings are loaded from NVS namespace "dtesp_fw" on boot
// (falling back to Kconfig when the keys are absent), can be mutated
// in-memory by the [:fw volume|profile|autoswitch ...] commands, and
// are persisted back to NVS by [:fw save].
//
// Keys (all u8):
//   vol        - 0..9
//   profile    - 0 = speaker, 1 = headphone
//   autoswitch - 0 = off, 1 = on
// ----------------------------------------------------------------

#ifndef FW_SETTINGS_H
#define FW_SETTINGS_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef CONFIG_DTESP_DAC_TLV320DAC3100
#include "tlv320_dsp.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialise the fw_settings module and load values from NVS.
 *
 * If a key is missing, the Kconfig factory default is used instead.
 * Call once from app_main() after nvs_flash_init() and before
 * applying settings to the codec.
 *
 * @return ESP_OK on success (including "NVS empty, defaults applied"),
 *         or a propagated NVS error code on hard failure.
 */
esp_err_t fw_settings_init(void);

/**
 * @brief Apply the currently loaded settings to the codec.
 *
 * Must be called after tlv320dac3100_init() so that the codec is
 * ready to accept runtime changes (profile switch, volume write,
 * autoswitch flag).
 */
void fw_settings_apply(void);

/**
 * @brief Persist the current in-memory settings to NVS.
 *
 * @return ESP_OK on success, or an NVS error code on failure.
 */
esp_err_t fw_settings_save(void);

// Getters return the current in-memory value.
uint8_t fw_settings_get_volume(void);
uint8_t fw_settings_get_profile(void);
uint8_t fw_settings_get_autoswitch(void);

// Setters update the in-memory value only.  Use fw_settings_save()
// to persist changes.
void fw_settings_set_volume(uint8_t level);
void fw_settings_set_profile(uint8_t profile);
void fw_settings_set_autoswitch(uint8_t enable);

#ifdef CONFIG_DTESP_DAC_TLV320DAC3100
/**
 * @brief Return a pointer to the in-memory DSP state (EQ + DRC).
 *
 * The returned pointer is valid until the next fw_settings_init().
 * Callers mutate the state in place and then either apply it to
 * the codec via tlv320dac3100_apply_dsp() and/or persist it with
 * fw_settings_save().
 */
tlv320_dsp_state_t *fw_settings_get_dsp(void);

/**
 * @brief Speaker driver analog gain in dB (6 / 12 / 18 / 24).
 */
uint8_t fw_settings_get_spk_gain_db(void);
void    fw_settings_set_spk_gain_db(uint8_t db);
#endif

#ifdef __cplusplus
}
#endif

#endif // FW_SETTINGS_H
