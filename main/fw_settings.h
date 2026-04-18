// SPDX-License-Identifier: MIT
// Copyright (c) 2025 Leland Lucius
// ----------------------------------------------------------------
// Firmware Settings — NVS-backed runtime overrides for Kconfig
//
// Provides getters and setters for the codec-related Kconfig options
// that may be adjusted at runtime through [:fw ...] commands and
// persisted to NVS so they survive power cycles.
//
// At startup fw_settings_init() loads any previously saved values
// from NVS; keys that are absent fall back to the compile-time
// Kconfig defaults.
// ----------------------------------------------------------------

#ifndef FW_SETTINGS_H
#define FW_SETTINGS_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Initialise the settings subsystem.
 *
 * Reads saved values from NVS.  Must be called after nvs_flash_init()
 * and before the codec is configured so that the loaded values can be
 * applied during initialisation.
 */
void fw_settings_init(void);

/**
 * @brief Persist the current runtime settings to NVS.
 */
void fw_settings_save(void);

/**
 * @brief Erase NVS settings and restore Kconfig defaults.
 */
void fw_settings_reset(void);

/** @brief Get the current startup volume level (0–9). */
uint8_t fw_settings_get_volume(void);
/** @brief Set the startup volume level (clamped to 0–9). */
void fw_settings_set_volume(uint8_t level);

/** @brief Get the default output profile (0 = speaker, 1 = headphone). */
int fw_settings_get_profile(void);
/** @brief Set the default output profile (0 = speaker, 1 = headphone). */
void fw_settings_set_profile(int profile);

/** @brief Get the headset auto-switch setting. */
bool fw_settings_get_autoswitch(void);
/** @brief Set the headset auto-switch setting. */
void fw_settings_set_autoswitch(bool enabled);

#endif // FW_SETTINGS_H
