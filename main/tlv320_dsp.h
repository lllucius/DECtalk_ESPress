// SPDX-License-Identifier: MIT
// Copyright (c) 2025 Leland Lucius
// ----------------------------------------------------------------
// TLV320DAC3100 on-chip DSP — Public API
//
// Owns the codec's DAC processing block (REG_DAC_PRB) and playback
// coefficient RAM so that tone controls (bass / treble), a small
// multi-band peaking EQ, and DRC can be driven from the [:fw …]
// custom commands.
//
// The pure-math helpers below keep the existing internal Q1.23-ish
// representation and the codec-family "plus" sign convention:
//
//     y[n] = N0·x[n] + N1·x[n-1] + N2·x[n-2]
//                   + D1·y[n-1] + D2·y[n-2]
//
// so D1,D2 are stored as the negated normalised a1,a2 from the
// standard RBJ cookbook.  The ESP-platform backend converts that
// internal form into the codec's 16-bit PRB_P2 playback-biquad
// storage format before touching hardware.  The sign convention is
// documented across the TI AIC31xx family; the exact 16-bit playback
// packing used here is family-derived rather than spelled out in the
// DAC3100 datasheet itself.
// ----------------------------------------------------------------

#ifndef TLV320_DSP_H
#define TLV320_DSP_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef ESP_PLATFORM
#include "esp_err.h"
#else
// Host test builds compile the pure-math portion of this module
// without ESP-IDF.  esp_err_t is only returned by the hardware
// functions, which are ESP_PLATFORM-only.
typedef int esp_err_t;
#endif

#ifdef __cplusplus
extern "C" {
#endif

// ------------------------------------------------------------
// Biquad slot layout.
//
//   slot 0   : bass          (low-shelf, ≈200 Hz,   Q≈0.707)
//   slots 1-5: EQ bands 1..5 (peaking, 160/500/1500/3000/5000 Hz, Q=1.0)
//   slot 6   : treble        (high-shelf, ≈4500 Hz, Q≈0.707)
//
// PRB_P2 exposes six playback biquads per channel.  This backend maps
// them as:
//   A=bass, B=EQ1, C=EQ2, D=EQ3, E=EQ4, F=treble
// User-visible EQ5 therefore has no hardware slot in this backend and
// is skipped with a warning when enabled.
// ------------------------------------------------------------
#define TLV320_DSP_NUM_EQ_BANDS     5
#define TLV320_DSP_SLOT_BASS        0
#define TLV320_DSP_SLOT_EQ_BASE     1
#define TLV320_DSP_SLOT_TREBLE      (TLV320_DSP_SLOT_EQ_BASE + TLV320_DSP_NUM_EQ_BANDS)
#define TLV320_DSP_NUM_SLOTS        (TLV320_DSP_SLOT_TREBLE + 1)

// Sample rate used by the I2S/DSP path.  Must match dtesp_audio.c.
#define TLV320_DSP_SAMPLE_RATE      11025.0f

// Per-band gain is clamped to this range.  ±12 dB roughly covers
// tone-shaping needs without running the Q1.23 coefficient format
// too deep into the safety-prescale region.
#define TLV320_DSP_MAX_GAIN_DB      12.0f
#define TLV320_DSP_MIN_GAIN_DB      (-12.0f)

typedef enum
{
    TLV320_DSP_FILTER_BYPASS = 0,   // Pass-through (N0=1, others 0)
    TLV320_DSP_FILTER_PEAKING,
    TLV320_DSP_FILTER_LOW_SHELF,
    TLV320_DSP_FILTER_HIGH_SHELF,
} tlv320_dsp_filter_type_t;

typedef struct
{
    bool                       enabled;
    tlv320_dsp_filter_type_t   type;
    float                      freq_hz;
    float                      gain_db;
    float                      q;
} tlv320_dsp_band_t;

typedef enum
{
    TLV320_DSP_DRC_PRESET_SOFT   = 0,
    TLV320_DSP_DRC_PRESET_SPEECH = 1,
    TLV320_DSP_DRC_PRESET_LOUD   = 2,
} tlv320_dsp_drc_preset_t;

typedef struct
{
    // Per-slot band configuration.  See slot layout above.
    tlv320_dsp_band_t           bands[TLV320_DSP_NUM_SLOTS];

    // Dynamic Range Compression state.
    bool                        drc_enabled;
    tlv320_dsp_drc_preset_t     drc_preset;
} tlv320_dsp_state_t;

// ------------------------------------------------------------
// State builders (pure functions, host-testable).
// ------------------------------------------------------------

// Initialise to "flat": every slot bypassed, DRC off.  This
// reproduces the pre-feature behaviour exactly.
void tlv320_dsp_state_default(tlv320_dsp_state_t *state);

// Tone-control helpers.  0 dB disables the slot.
void tlv320_dsp_state_set_bass(tlv320_dsp_state_t *state, float gain_db);
void tlv320_dsp_state_set_treble(tlv320_dsp_state_t *state, float gain_db);

// Peaking EQ band setters.  `band_index` is 0-based and must be
// < TLV320_DSP_NUM_EQ_BANDS.  0 dB disables the slot.
void tlv320_dsp_state_set_eq_band(tlv320_dsp_state_t *state,
                                  int band_index,
                                  float gain_db);

// Accessors (0 dB for disabled slots).
float tlv320_dsp_state_get_bass(const tlv320_dsp_state_t *state);
float tlv320_dsp_state_get_treble(const tlv320_dsp_state_t *state);
float tlv320_dsp_state_get_eq_band(const tlv320_dsp_state_t *state,
                                   int band_index);

// Flatten every EQ/shelf slot back to pass-through.
void tlv320_dsp_state_reset_eq(tlv320_dsp_state_t *state);

// Apply a named preset ("flat", "speech", "crisp", "warm").
// Returns true on success, false if the name is unknown (state
// is left unchanged).
bool tlv320_dsp_state_apply_preset(tlv320_dsp_state_t *state,
                                   const char *preset_name);

// Look up the DRC preset enum by name ("soft" | "speech" | "loud").
// Returns true on success, false if unknown.
bool tlv320_dsp_drc_preset_from_name(const char *name,
                                     tlv320_dsp_drc_preset_t *out);
const char *tlv320_dsp_drc_preset_name(tlv320_dsp_drc_preset_t preset);

// ------------------------------------------------------------
// Pure math helpers (host-testable; no hardware I/O).
//
// Each "compute_*" returns five internal signed coefficients
// (N0, N1, N2, D1, D2).  A trailing boolean `prescaled_out`
// (optional, may be NULL) is set to true when the function had to
// halve N0/N1/N2 to keep them inside the internal fixed-point range.
// ------------------------------------------------------------
void tlv320_dsp_compute_bypass(int32_t coeffs[5]);

void tlv320_dsp_compute_peaking(float fs,
                                float f0,
                                float q,
                                float gain_db,
                                int32_t coeffs[5],
                                bool *prescaled_out);

void tlv320_dsp_compute_low_shelf(float fs,
                                  float f0,
                                  float q,
                                  float gain_db,
                                  int32_t coeffs[5],
                                  bool *prescaled_out);

void tlv320_dsp_compute_high_shelf(float fs,
                                   float f0,
                                   float q,
                                   float gain_db,
                                   int32_t coeffs[5],
                                   bool *prescaled_out);

// Convert a floating-point value in [-1, +1) to Q1.23 two's-
// complement (fits in a signed 24-bit integer, returned sign-
// extended).  Values outside the representable range are clamped.
int32_t tlv320_dsp_float_to_q23(float x);

// ------------------------------------------------------------
// Hardware integration (ESP-IDF only).
// ------------------------------------------------------------
#ifdef ESP_PLATFORM

// Low-level hooks provided by tlv320.c so the DSP module
// can talk to the codec without duplicating I2C plumbing.
//
// NOTE: a previous revision of this module also exposed a
// `dac_power(bool)` hook that bracketed coefficient / PRB writes
// with a DAC power-down / power-up on the theory that the
// TLV320DAC3100 ignores such writes while the DAC is running.
// In practice that sequence left the codec silent after any
// non-flat apply, so the bracket has been removed.  Coefficient /
// PRB writes are now issued while the DAC remains running (soft
// muted via the digital volume register) — the codec's coefficient
// RAM is double-buffered, so the writes are safe; making them
// actually take audible effect is a separate problem that should
// be solved without power-cycling the DAC.
typedef struct
{
    esp_err_t (*write_reg)(uint8_t page, uint8_t reg, uint8_t val);
    esp_err_t (*mute)(bool enable);
} tlv320_dsp_hw_ops_t;

// Install the hw ops and initialise the DSP to a flat / bypassed
// state.  Call once from tlv320_init() *after* the codec is
// out of reset.  Safe to call repeatedly.
esp_err_t tlv320_dsp_init(const tlv320_dsp_hw_ops_t *ops);

// Apply a DSP state (EQ + DRC) to the codec.  Briefly mutes while
// writing coefficients so the transition is silent.  On I2C error
// the codec is left muted and the caller should retry or reset.
esp_err_t tlv320_dsp_apply(const tlv320_dsp_state_t *state);

// Return true if every EQ slot is disabled and DRC is off (so the
// driver can keep using PRB_P1, the cheapest processing block).
bool tlv320_dsp_state_is_flat(const tlv320_dsp_state_t *state);

// Format a human-readable summary of the current state into `buf`
// (at most `buf_len` bytes).  Used by [:fw eq show] logging.
void tlv320_dsp_state_format(const tlv320_dsp_state_t *state,
                             char *buf,
                             size_t buf_len);

#endif // ESP_PLATFORM

#ifdef __cplusplus
}
#endif

#endif // TLV320_DSP_H
