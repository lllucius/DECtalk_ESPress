// SPDX-License-Identifier: MIT
// Copyright (c) 2025 Leland Lucius
// ----------------------------------------------------------------
// Analog volume knob (potentiometer) — optional input that
// cooperates with the existing firmware volume command path.
//
// Design goals:
//   * codec volume remains the single source of truth
//   * "whichever was used last wins"
//   * no abrupt jumps when the FW command is used while the pot is
//     sitting at a different position (soft takeover)
//   * no chatter between adjacent discrete volume steps (smoothing
//     + hysteresis)
//
// When CONFIG_DTESP_VOLUME_KNOB_ENABLE is not set, every function
// below compiles down to a no-op so the rest of the firmware can
// call them unconditionally.
// ----------------------------------------------------------------

#ifndef VOLUME_KNOB_H
#define VOLUME_KNOB_H

#include <stdint.h>

#include "esp_err.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the analog volume knob.
 *
 * Configures the ADC for the configured GPIO and starts a light
 * periodic sampler (via esp_timer) that applies smoothing,
 * hysteresis and soft-takeover before calling the codec volume
 * setter.  Safe to call when CONFIG_DTESP_VOLUME_KNOB_ENABLE is
 * off (returns ESP_OK and does nothing).  Safe to call more than
 * once (subsequent calls are no-ops).
 *
 * Must be called after the codec has been initialised and after
 * the restored volume has been applied, so the pot can use the
 * actual volume as its soft-takeover reference.
 *
 * @return ESP_OK on success, or an error code on ADC failure.
 */
esp_err_t volume_knob_init(void);

/**
 * @brief Notify the knob that an external (non-pot) source just
 *        changed the codec volume.
 *
 * Called from every code path that sets volume via the firmware
 * — [:fw volume ...], ctrl-code volume keys, and the settings
 * restore at startup.  Updates the knob's record of "current
 * actual volume" and unlatches the pot so it will not snap the
 * volume to its current physical position; the pot has to be
 * moved into a small window around the new volume before it
 * regains control.
 *
 * @param level  New volume level as passed to tlv320_set_volume.
 */
void volume_knob_notify_external_volume(uint8_t level);

#ifdef __cplusplus
}
#endif

#endif // VOLUME_KNOB_H
