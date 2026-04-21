// SPDX-License-Identifier: MIT
// Copyright (c) 2025 Leland Lucius
// ----------------------------------------------------------------
// TLV320DAC3100 on-chip DSP — implementation
//
// See tlv320_dsp.h for the public contract.  This file is split
// into three clear sections:
//
//   §1  pure math: RBJ cookbook biquad coefficients + Q1.23 convert
//   §2  state helpers: defaults, setters, presets
//   §3  hardware I/O (ESP-IDF only): PRB / coefficient upload / DRC
//
// Sections §1 and §2 are compiled on the host for unit testing
// (the tests/ Makefile pulls this file in directly).  Section §3
// is guarded by ESP_PLATFORM.
// ----------------------------------------------------------------

#include "tlv320_dsp.h"

#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <string.h>
#include <stdio.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
static const char *TAG = "tlv320_dsp";
#endif

// ================================================================
// §1  Pure math: RBJ biquad coefficients + Q1.23 conversion
// ================================================================

// Q1.23 representable range: [-2^23, 2^23 - 1]  = [-8388608, +8388607]
#define TLV320_Q23_ONE        (1 << 23)
#define TLV320_Q23_MIN        (-(1 << 23))
#define TLV320_Q23_MAX        ((1 << 23) - 1)

// The TLV320DAC3100 (and the wider TI AIC31xx family) applies an
// implicit 2x multiplier on biquad coefficients before they are
// used in the difference equation.  See SLAA408 ("Biquad-Filter
// Coefficient Generation for TI Audio Codecs") §2 — "the five
// biquad coefficients are stored in Q1.23 format but interpreted
// as Q2.22 (×2)".  That effectively extends the representable
// coefficient range to [-2, +2), which is enough for shelving /
// peaking filters at ±12 dB with any reasonable Q.  Multiplying
// our floating-point coefficients by 0.5 before the Q1.23 encode
// reverses the hardware's 2x scaling and puts the numerical value
// in the right range.
#define TLV320_COEFF_HW_SCALE 0.5f

// Upper limit on the pre-scale float value.  Anything above this
// (after the hw 0.5 scale) overflows Q1.23 and will be clamped /
// logged.  Practical RBJ coefficients for the filter types and
// gains this module uses stay well under this threshold.
#define TLV320_PRESCALE_LIMIT 0.999f

int32_t tlv320_dsp_float_to_q23(float x)
{
    if (x >= 1.0f)
    {
        return TLV320_Q23_MAX;
    }
    if (x <= -1.0f)
    {
        return TLV320_Q23_MIN;
    }

    float scaled = x * (float)TLV320_Q23_ONE;
    // Round to nearest, ties away from zero.
    if (scaled >= 0.0f)
    {
        scaled += 0.5f;
    }
    else
    {
        scaled -= 0.5f;
    }
    int32_t q = (int32_t)scaled;
    if (q > TLV320_Q23_MAX)
    {
        q = TLV320_Q23_MAX;
    }
    if (q < TLV320_Q23_MIN)
    {
        q = TLV320_Q23_MIN;
    }
    return q;
}

// Encode five float coefficients (N0, N1, N2, D1, D2 — already in
// the DAC3100 "plus-sign" convention) into Q1.23, applying the
// hardware 2x scaling and clamping / logging when a value is still
// out of range afterward.
static void encode_biquad(float n0, float n1, float n2,
                          float d1, float d2,
                          int32_t coeffs[5],
                          bool *prescaled_out)
{
    // Apply the hardware's implicit 2x scale (divide by 2).  After
    // this, values are in [-1, +1) for any coefficient up to ±2.
    n0 *= TLV320_COEFF_HW_SCALE;
    n1 *= TLV320_COEFF_HW_SCALE;
    n2 *= TLV320_COEFF_HW_SCALE;
    d1 *= TLV320_COEFF_HW_SCALE;
    d2 *= TLV320_COEFF_HW_SCALE;

    // Detect rare extreme-boost cases that still overflow even with
    // the 2x headroom.  Halve all N coefficients for that biquad
    // (accepts a further -6 dB of insertion loss but keeps the
    // filter stable).  D coefficients cannot be halved without
    // altering pole placement, so we clamp and log.
    bool prescaled = false;
    float amax = fabsf(n0);
    if (fabsf(n1) > amax) amax = fabsf(n1);
    if (fabsf(n2) > amax) amax = fabsf(n2);
    if (amax > TLV320_PRESCALE_LIMIT)
    {
        n0 *= 0.5f;
        n1 *= 0.5f;
        n2 *= 0.5f;
        prescaled = true;
    }
    if (prescaled_out)
    {
        *prescaled_out = prescaled;
    }

    coeffs[0] = tlv320_dsp_float_to_q23(n0);
    coeffs[1] = tlv320_dsp_float_to_q23(n1);
    coeffs[2] = tlv320_dsp_float_to_q23(n2);
    coeffs[3] = tlv320_dsp_float_to_q23(d1);
    coeffs[4] = tlv320_dsp_float_to_q23(d2);
}

void tlv320_dsp_compute_bypass(int32_t coeffs[5])
{
    // Pass-through: N0 = 1.0 (before hw scaling) → stored as 0.5
    // in Q1.23 = 0x400000.  With the chip's 2x multiplier the
    // effective biquad output is x[n] exactly.
    coeffs[0] = TLV320_Q23_ONE / 2;
    coeffs[1] = 0;
    coeffs[2] = 0;
    coeffs[3] = 0;
    coeffs[4] = 0;
}

// Clamp common parameters to sane ranges before generating
// coefficients.  Keeps the filter well-behaved regardless of what
// the host / command parser hands us.
static void clamp_params(float fs, float *f0, float *q, float *gain_db)
{
    const float f_min = 20.0f;
    const float f_max = (fs * 0.5f) - 100.0f;
    if (*f0 < f_min) *f0 = f_min;
    if (*f0 > f_max) *f0 = f_max;

    if (*q < 0.1f)   *q = 0.1f;
    if (*q > 10.0f)  *q = 10.0f;

    if (*gain_db > TLV320_DSP_MAX_GAIN_DB) *gain_db = TLV320_DSP_MAX_GAIN_DB;
    if (*gain_db < TLV320_DSP_MIN_GAIN_DB) *gain_db = TLV320_DSP_MIN_GAIN_DB;
}

void tlv320_dsp_compute_peaking(float fs,
                                float f0,
                                float q,
                                float gain_db,
                                int32_t coeffs[5],
                                bool *prescaled_out)
{
    clamp_params(fs, &f0, &q, &gain_db);

    const float A  = powf(10.0f, gain_db / 40.0f);
    const float w0 = 2.0f * (float)M_PI * (f0 / fs);
    const float cw = cosf(w0);
    const float sw = sinf(w0);
    const float al = sw / (2.0f * q);

    // RBJ peaking EQ (biquad / a0-normalised, minus-sign form)
    const float b0 = 1.0f + al * A;
    const float b1 = -2.0f * cw;
    const float b2 = 1.0f - al * A;
    const float a0 = 1.0f + al / A;
    const float a1 = -2.0f * cw;
    const float a2 = 1.0f - al / A;

    // Normalise and flip the sign of a1, a2 for the DAC3100 "plus"
    // difference equation.
    const float N0 =  b0 / a0;
    const float N1 =  b1 / a0;
    const float N2 =  b2 / a0;
    const float D1 = -a1 / a0;
    const float D2 = -a2 / a0;

    encode_biquad(N0, N1, N2, D1, D2, coeffs, prescaled_out);
}

void tlv320_dsp_compute_low_shelf(float fs,
                                  float f0,
                                  float q,
                                  float gain_db,
                                  int32_t coeffs[5],
                                  bool *prescaled_out)
{
    clamp_params(fs, &f0, &q, &gain_db);

    const float A  = powf(10.0f, gain_db / 40.0f);
    const float w0 = 2.0f * (float)M_PI * (f0 / fs);
    const float cw = cosf(w0);
    const float sw = sinf(w0);
    const float al = sw / (2.0f * q);
    const float sa = 2.0f * sqrtf(A) * al;

    const float b0 = A * ((A + 1.0f) - (A - 1.0f) * cw + sa);
    const float b1 = 2.0f * A * ((A - 1.0f) - (A + 1.0f) * cw);
    const float b2 = A * ((A + 1.0f) - (A - 1.0f) * cw - sa);
    const float a0 =      (A + 1.0f) + (A - 1.0f) * cw + sa;
    const float a1 = -2.0f *          ((A - 1.0f) + (A + 1.0f) * cw);
    const float a2 =      (A + 1.0f) + (A - 1.0f) * cw - sa;

    const float N0 =  b0 / a0;
    const float N1 =  b1 / a0;
    const float N2 =  b2 / a0;
    const float D1 = -a1 / a0;
    const float D2 = -a2 / a0;

    encode_biquad(N0, N1, N2, D1, D2, coeffs, prescaled_out);
}

void tlv320_dsp_compute_high_shelf(float fs,
                                   float f0,
                                   float q,
                                   float gain_db,
                                   int32_t coeffs[5],
                                   bool *prescaled_out)
{
    clamp_params(fs, &f0, &q, &gain_db);

    const float A  = powf(10.0f, gain_db / 40.0f);
    const float w0 = 2.0f * (float)M_PI * (f0 / fs);
    const float cw = cosf(w0);
    const float sw = sinf(w0);
    const float al = sw / (2.0f * q);
    const float sa = 2.0f * sqrtf(A) * al;

    const float b0 =  A * ((A + 1.0f) + (A - 1.0f) * cw + sa);
    const float b1 = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cw);
    const float b2 =  A * ((A + 1.0f) + (A - 1.0f) * cw - sa);
    const float a0 =       (A + 1.0f) - (A - 1.0f) * cw + sa;
    const float a1 =  2.0f *          ((A - 1.0f) - (A + 1.0f) * cw);
    const float a2 =       (A + 1.0f) - (A - 1.0f) * cw - sa;

    const float N0 =  b0 / a0;
    const float N1 =  b1 / a0;
    const float N2 =  b2 / a0;
    const float D1 = -a1 / a0;
    const float D2 = -a2 / a0;

    encode_biquad(N0, N1, N2, D1, D2, coeffs, prescaled_out);
}

// ================================================================
// §2  State helpers (host-testable)
// ================================================================

// Default band centres and Q values for the speech-tuned EQ.
static const float eq_band_freqs[TLV320_DSP_NUM_EQ_BANDS] =
    { 160.0f, 500.0f, 1500.0f, 3000.0f, 5000.0f };
static const float eq_band_q = 1.0f;

static const float bass_freq    = 200.0f;
static const float bass_q       = 0.707f;
static const float treble_freq  = 4500.0f;
static const float treble_q     = 0.707f;

static float clamp_gain(float g)
{
    if (g > TLV320_DSP_MAX_GAIN_DB) g = TLV320_DSP_MAX_GAIN_DB;
    if (g < TLV320_DSP_MIN_GAIN_DB) g = TLV320_DSP_MIN_GAIN_DB;
    return g;
}

void tlv320_dsp_state_default(tlv320_dsp_state_t *state)
{
    if (!state) return;
    memset(state, 0, sizeof(*state));
    for (int i = 0; i < TLV320_DSP_NUM_SLOTS; i++)
    {
        state->bands[i].enabled = false;
        state->bands[i].type    = TLV320_DSP_FILTER_BYPASS;
        state->bands[i].gain_db = 0.0f;
    }

    state->bands[TLV320_DSP_SLOT_BASS].type    = TLV320_DSP_FILTER_LOW_SHELF;
    state->bands[TLV320_DSP_SLOT_BASS].freq_hz = bass_freq;
    state->bands[TLV320_DSP_SLOT_BASS].q       = bass_q;

    state->bands[TLV320_DSP_SLOT_TREBLE].type    = TLV320_DSP_FILTER_HIGH_SHELF;
    state->bands[TLV320_DSP_SLOT_TREBLE].freq_hz = treble_freq;
    state->bands[TLV320_DSP_SLOT_TREBLE].q       = treble_q;

    for (int i = 0; i < TLV320_DSP_NUM_EQ_BANDS; i++)
    {
        int slot = TLV320_DSP_SLOT_EQ_BASE + i;
        state->bands[slot].type    = TLV320_DSP_FILTER_PEAKING;
        state->bands[slot].freq_hz = eq_band_freqs[i];
        state->bands[slot].q       = eq_band_q;
    }

    state->drc_enabled = false;
    state->drc_preset  = TLV320_DSP_DRC_PRESET_SPEECH;
}

void tlv320_dsp_state_set_bass(tlv320_dsp_state_t *state, float gain_db)
{
    if (!state) return;
    gain_db = clamp_gain(gain_db);
    tlv320_dsp_band_t *b = &state->bands[TLV320_DSP_SLOT_BASS];
    b->gain_db = gain_db;
    b->enabled = (gain_db != 0.0f);
    b->type    = TLV320_DSP_FILTER_LOW_SHELF;
    b->freq_hz = bass_freq;
    b->q       = bass_q;
}

void tlv320_dsp_state_set_treble(tlv320_dsp_state_t *state, float gain_db)
{
    if (!state) return;
    gain_db = clamp_gain(gain_db);
    tlv320_dsp_band_t *b = &state->bands[TLV320_DSP_SLOT_TREBLE];
    b->gain_db = gain_db;
    b->enabled = (gain_db != 0.0f);
    b->type    = TLV320_DSP_FILTER_HIGH_SHELF;
    b->freq_hz = treble_freq;
    b->q       = treble_q;
}

void tlv320_dsp_state_set_eq_band(tlv320_dsp_state_t *state,
                                  int band_index,
                                  float gain_db)
{
    if (!state) return;
    if (band_index < 0 || band_index >= TLV320_DSP_NUM_EQ_BANDS) return;
    gain_db = clamp_gain(gain_db);
    int slot = TLV320_DSP_SLOT_EQ_BASE + band_index;
    tlv320_dsp_band_t *b = &state->bands[slot];
    b->gain_db = gain_db;
    b->enabled = (gain_db != 0.0f);
    b->type    = TLV320_DSP_FILTER_PEAKING;
    b->freq_hz = eq_band_freqs[band_index];
    b->q       = eq_band_q;
}

float tlv320_dsp_state_get_bass(const tlv320_dsp_state_t *state)
{
    if (!state) return 0.0f;
    return state->bands[TLV320_DSP_SLOT_BASS].enabled
        ? state->bands[TLV320_DSP_SLOT_BASS].gain_db : 0.0f;
}

float tlv320_dsp_state_get_treble(const tlv320_dsp_state_t *state)
{
    if (!state) return 0.0f;
    return state->bands[TLV320_DSP_SLOT_TREBLE].enabled
        ? state->bands[TLV320_DSP_SLOT_TREBLE].gain_db : 0.0f;
}

float tlv320_dsp_state_get_eq_band(const tlv320_dsp_state_t *state,
                                   int band_index)
{
    if (!state) return 0.0f;
    if (band_index < 0 || band_index >= TLV320_DSP_NUM_EQ_BANDS) return 0.0f;
    int slot = TLV320_DSP_SLOT_EQ_BASE + band_index;
    return state->bands[slot].enabled ? state->bands[slot].gain_db : 0.0f;
}

void tlv320_dsp_state_reset_eq(tlv320_dsp_state_t *state)
{
    if (!state) return;
    // "eq reset" flattens only the 5 peaking EQ bands; bass and
    // treble are owned by their own sub-commands.
    for (int i = 0; i < TLV320_DSP_NUM_EQ_BANDS; i++)
    {
        int slot = TLV320_DSP_SLOT_EQ_BASE + i;
        state->bands[slot].enabled = false;
        state->bands[slot].gain_db = 0.0f;
    }
}

// ---- Named presets ---------------------------------------------
//
// Each preset sets the seven slots (bass, EQ1..5, treble) from a
// compact table.  A gain of 0 dB disables that slot.
typedef struct
{
    const char *name;
    float       bass_db;
    float       eq_db[TLV320_DSP_NUM_EQ_BANDS];
    float       treble_db;
    bool        drc_on;
    tlv320_dsp_drc_preset_t drc_preset;
} dsp_preset_t;

static const dsp_preset_t presets[] =
{
    // flat: pass-through baseline.
    { "flat",
      0.0f,
      { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f },
      0.0f,
      false, TLV320_DSP_DRC_PRESET_SPEECH },

    // speech: gentle cut in the boxy 500 Hz zone, small presence
    // boost at 3 kHz.  Mild DRC tightens intelligibility.
    { "speech",
      -3.0f,
      { 0.0f, -2.0f, 0.0f, +3.0f, 0.0f },
      +2.0f,
      true,  TLV320_DSP_DRC_PRESET_SPEECH },

    // crisp: speech tuning + more presence and high-end air.  Use
    // for small speakers that sound dull.
    { "crisp",
      -3.0f,
      { 0.0f, -2.0f, 0.0f, +4.0f, +3.0f },
      +2.0f,
      true,  TLV320_DSP_DRC_PRESET_LOUD },

    // warm: opposite of crisp — bass lift, no top-end boost, for
    // users who find the default too bright.
    { "warm",
      +2.0f,
      { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f },
      0.0f,
      false, TLV320_DSP_DRC_PRESET_SOFT },
};

static int strcasecmp_ascii(const char *a, const char *b)
{
    // Tiny ASCII case-insensitive compare so that this module has
    // no host-vs-ESP-IDF strcasecmp portability dance.
    while (*a && *b)
    {
        char ca = *a;
        char cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + ('a' - 'A'));
        if (ca != cb) return (int)(unsigned char)ca - (int)(unsigned char)cb;
        a++; b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

bool tlv320_dsp_state_apply_preset(tlv320_dsp_state_t *state,
                                   const char *preset_name)
{
    if (!state || !preset_name) return false;
    for (size_t i = 0; i < sizeof(presets) / sizeof(presets[0]); i++)
    {
        if (strcasecmp_ascii(presets[i].name, preset_name) == 0)
        {
            tlv320_dsp_state_default(state);
            tlv320_dsp_state_set_bass(state,   presets[i].bass_db);
            tlv320_dsp_state_set_treble(state, presets[i].treble_db);
            for (int b = 0; b < TLV320_DSP_NUM_EQ_BANDS; b++)
            {
                tlv320_dsp_state_set_eq_band(state, b, presets[i].eq_db[b]);
            }
            state->drc_enabled = presets[i].drc_on;
            state->drc_preset  = presets[i].drc_preset;
            return true;
        }
    }
    return false;
}

bool tlv320_dsp_drc_preset_from_name(const char *name,
                                     tlv320_dsp_drc_preset_t *out)
{
    if (!name || !out) return false;
    if (strcasecmp_ascii(name, "soft")   == 0) { *out = TLV320_DSP_DRC_PRESET_SOFT;   return true; }
    if (strcasecmp_ascii(name, "speech") == 0) { *out = TLV320_DSP_DRC_PRESET_SPEECH; return true; }
    if (strcasecmp_ascii(name, "loud")   == 0) { *out = TLV320_DSP_DRC_PRESET_LOUD;   return true; }
    return false;
}

const char *tlv320_dsp_drc_preset_name(tlv320_dsp_drc_preset_t preset)
{
    switch (preset)
    {
    case TLV320_DSP_DRC_PRESET_SOFT:   return "soft";
    case TLV320_DSP_DRC_PRESET_SPEECH: return "speech";
    case TLV320_DSP_DRC_PRESET_LOUD:   return "loud";
    default:                           return "?";
    }
}

// ================================================================
// §3  Hardware I/O (ESP-IDF only)
// ================================================================
#ifdef ESP_PLATFORM

// ---- Register map (SLAS833 + AIC31xx family) --------------------
//
// PRB_P1 is the cheap default used at startup and whenever the DSP
// state is flat.  PRB_P2 is the "EQ active" block: per SLAS833
// Table 5-3 it provides a first-order IIR plus six biquads per DAC
// channel, which is exactly enough for slot 0 (bass shelf) + slots
// 1..4 (four peaking bands) + slot 6 (treble shelf).  EQ band 5
// (slot 5) has no biquad slot available in PRB_P2 and is silently
// dropped per design.
//
// The biquad-coefficient base register on page 8 for PRB_P2 is
// 0x18 on the AIC31xx family; each biquad occupies five 24-bit
// coefficients = 15 bytes, stored big-endian.  Page 9 holds the
// mirror image for the right channel.  This is consistent with
// TI's SLAA408 application report and with the Adafruit_TLV320_I2S
// reference.  If a future revision of the datasheet places the
// coefficient RAM at a different offset, change only these two
// constants.
// ----------------------------------------------------------------
#define DSP_PRB_P1                  0x01
#define DSP_PRB_P2                  0x02

#define DSP_PAGE_LEFT_COEFFS        8
#define DSP_PAGE_RIGHT_COEFFS       9
#define DSP_COEFF_BIQUAD_BASE_REG   0x18
#define DSP_COEFF_BYTES_PER_BIQUAD  15   // 5 coeffs × 3 bytes

// PRB_P2 provides six biquad slots per channel (per SLAS833
// Table 5-3).  Slots beyond this limit are skipped.
#define DSP_PRB_P2_BIQUAD_COUNT     6

#define REG_DAC_PRB                 0x3C

// ---- DRC registers (page 0, SLAS833) ----------------------------
#define REG_DRC_CTRL1               0x44   // DRC enable + threshold hi-nibble
#define REG_DRC_CTRL2               0x45   // DRC hysteresis + threshold lo
#define REG_DRC_CTRL3               0x46   // DRC hold / attack / decay

// DRC preset values.  These bit patterns correspond to the
// "threshold / attack / decay / ratio" combinations documented in
// SLAS833 §5.1.6.  Ratios are hard-coded to 2:1 — the DAC3100 DRC
// is fixed ratio and only the thresholds/time constants are
// programmable.
typedef struct
{
    uint8_t ctrl1;
    uint8_t ctrl2;
    uint8_t ctrl3;
} drc_preset_regs_t;

static const drc_preset_regs_t drc_presets_regs[] =
{
    // soft:   threshold -24 dB, slow attack/decay
    { 0x60, 0x14, 0x44 },
    // speech: threshold -18 dB, fast attack, medium decay
    { 0x60, 0x0C, 0x22 },
    // loud:   threshold -12 dB, fast attack, slow decay
    { 0x60, 0x04, 0x24 },
};

static const drc_preset_regs_t drc_bypass_regs =
{
    0x00, 0x00, 0x00,
};

// ---- Module state -----------------------------------------------
static tlv320_dsp_hw_ops_t s_ops;
static bool                s_initialised = false;
static tlv320_dsp_state_t  s_last_applied;
static bool                s_has_last_applied = false;

// Return the biquad slot's per-slot filter computation function
// result, or bypass coefficients if the slot is disabled.
static void compute_slot_coeffs(const tlv320_dsp_band_t *b,
                                int32_t coeffs[5],
                                bool *prescaled_out)
{
    if (!b || !b->enabled)
    {
        tlv320_dsp_compute_bypass(coeffs);
        if (prescaled_out) *prescaled_out = false;
        return;
    }
    switch (b->type)
    {
    case TLV320_DSP_FILTER_PEAKING:
        tlv320_dsp_compute_peaking(TLV320_DSP_SAMPLE_RATE,
                                   b->freq_hz, b->q, b->gain_db,
                                   coeffs, prescaled_out);
        return;
    case TLV320_DSP_FILTER_LOW_SHELF:
        tlv320_dsp_compute_low_shelf(TLV320_DSP_SAMPLE_RATE,
                                     b->freq_hz, b->q, b->gain_db,
                                     coeffs, prescaled_out);
        return;
    case TLV320_DSP_FILTER_HIGH_SHELF:
        tlv320_dsp_compute_high_shelf(TLV320_DSP_SAMPLE_RATE,
                                      b->freq_hz, b->q, b->gain_db,
                                      coeffs, prescaled_out);
        return;
    case TLV320_DSP_FILTER_BYPASS:
    default:
        tlv320_dsp_compute_bypass(coeffs);
        if (prescaled_out) *prescaled_out = false;
        return;
    }
}

static esp_err_t write_coeff_bytes(uint8_t page,
                                   uint8_t reg,
                                   const int32_t coeffs[5])
{
    uint8_t bytes[DSP_COEFF_BYTES_PER_BIQUAD];
    for (int i = 0; i < 5; i++)
    {
        int32_t c = coeffs[i];
        // Encode as 24-bit two's-complement, big-endian.
        bytes[i * 3 + 0] = (uint8_t)((c >> 16) & 0xFF);
        bytes[i * 3 + 1] = (uint8_t)((c >>  8) & 0xFF);
        bytes[i * 3 + 2] = (uint8_t)((c >>  0) & 0xFF);
    }

    for (int i = 0; i < DSP_COEFF_BYTES_PER_BIQUAD; i++)
    {
        esp_err_t err = s_ops.write_reg(page, (uint8_t)(reg + i), bytes[i]);
        if (err != ESP_OK)
        {
            return err;
        }
    }
    return ESP_OK;
}

static esp_err_t upload_biquads(const tlv320_dsp_state_t *state)
{
    // Write as many slots as the PRB offers biquads for.  Order on
    // the wire is slot 0 -> slot N-1 mapped to biquad A, B, C, ….
    int biquad_count = DSP_PRB_P2_BIQUAD_COUNT;
    if (biquad_count > TLV320_DSP_NUM_SLOTS) biquad_count = TLV320_DSP_NUM_SLOTS;

    int prescaled_count = 0;
    bool dropped_logged = false;

    for (int slot = 0; slot < TLV320_DSP_NUM_SLOTS; slot++)
    {
        if (slot >= biquad_count)
        {
            // Slot doesn't fit in the active PRB.  Log once per apply
            // so the user knows why band 5 seems ineffective.
            if (!dropped_logged && state->bands[slot].enabled)
            {
                ESP_LOGW(TAG,
                         "band slot %d has no biquad in active PRB; skipped",
                         slot);
                dropped_logged = true;
            }
            continue;
        }

        int32_t coeffs[5];
        bool prescaled = false;
        compute_slot_coeffs(&state->bands[slot], coeffs, &prescaled);
        if (prescaled) prescaled_count++;

        uint8_t reg = (uint8_t)(DSP_COEFF_BIQUAD_BASE_REG
                                + slot * DSP_COEFF_BYTES_PER_BIQUAD);

        esp_err_t err = write_coeff_bytes(DSP_PAGE_LEFT_COEFFS, reg, coeffs);
        if (err != ESP_OK) return err;
        err = write_coeff_bytes(DSP_PAGE_RIGHT_COEFFS, reg, coeffs);
        if (err != ESP_OK) return err;
    }

    if (prescaled_count > 0)
    {
        ESP_LOGI(TAG,
                 "%d biquad(s) prescaled to fit Q1.23 (expect ~-6 dB each)",
                 prescaled_count);
    }

    return ESP_OK;
}

static esp_err_t program_drc(const tlv320_dsp_state_t *state)
{
    const drc_preset_regs_t *regs = &drc_bypass_regs;
    if (state->drc_enabled)
    {
        int idx = (int)state->drc_preset;
        if (idx < 0 || idx >= (int)(sizeof(drc_presets_regs)
                                     / sizeof(drc_presets_regs[0])))
        {
            idx = (int)TLV320_DSP_DRC_PRESET_SPEECH;
        }
        regs = &drc_presets_regs[idx];
    }

    esp_err_t err;
    err = s_ops.write_reg(0x00, REG_DRC_CTRL1, regs->ctrl1);
    if (err != ESP_OK) return err;
    err = s_ops.write_reg(0x00, REG_DRC_CTRL2, regs->ctrl2);
    if (err != ESP_OK) return err;
    err = s_ops.write_reg(0x00, REG_DRC_CTRL3, regs->ctrl3);
    return err;
}

bool tlv320_dsp_state_is_flat(const tlv320_dsp_state_t *state)
{
    if (!state) return true;
    if (state->drc_enabled) return false;
    for (int i = 0; i < TLV320_DSP_NUM_SLOTS; i++)
    {
        if (state->bands[i].enabled && state->bands[i].gain_db != 0.0f)
        {
            return false;
        }
    }
    return true;
}

esp_err_t tlv320_dsp_init(const tlv320_dsp_hw_ops_t *ops)
{
    if (!ops || !ops->write_reg || !ops->mute || !ops->dac_power)
    {
        return ESP_ERR_INVALID_ARG;
    }
    s_ops = *ops;
    s_initialised = true;
    s_has_last_applied = false;

    // Leave the codec in its current PRB (PRB_P1 from the init
    // table) and explicitly disable DRC.  This is a no-op on the
    // audio path compared to the pre-feature firmware.
    tlv320_dsp_state_t flat;
    tlv320_dsp_state_default(&flat);
    return tlv320_dsp_apply(&flat);
}

esp_err_t tlv320_dsp_apply(const tlv320_dsp_state_t *state)
{
    if (!s_initialised) return ESP_ERR_INVALID_STATE;
    if (!state)         return ESP_ERR_INVALID_ARG;

    bool flat = tlv320_dsp_state_is_flat(state);

    // Soft-mute briefly so coefficient writes can't be heard as
    // clicks.  On failure we leave the codec muted and let the
    // caller decide how to recover (usually by re-running apply
    // with the last known-good state).
    esp_err_t err = s_ops.mute(true);
    if (err != ESP_OK) return err;

    // Give the digital-volume soft-step ramp time to reach the
    // muted level before we power the DAC down.  At Fs = 11.025 kHz
    // the 126-step ramp takes ~11.4 ms; 15 ms is a safe margin.
    vTaskDelay(pdMS_TO_TICKS(15));

    // Per SLAS833 §6.5 the DAC Processing Block selection register
    // (0x3C) and the biquad coefficient RAM (pages 8/9) must be
    // written while the DAC is powered DOWN -- otherwise the new
    // PRB and/or coefficients are silently ignored and the audio
    // path keeps running the previous settings.  `dac_power(false)`
    // performs the transition instantaneously by bypassing the
    // soft-step ramp (see tlv320.c), so the coefficient writes that
    // follow really do happen while the DAC is off.
    esp_err_t dac_err = s_ops.dac_power(false);
    if (dac_err != ESP_OK)
    {
        // Can't safely touch the coefficient RAM if we couldn't
        // confirm the DAC is down; bail and unmute.
        err = dac_err;
        goto out;
    }

    // Drop to PRB_P1 before rewriting coefficients so the DSP is in
    // a known quiescent state when it powers back up.
    err = s_ops.write_reg(0x00, REG_DAC_PRB, DSP_PRB_P1);
    if (err != ESP_OK) goto out_power;

    if (!flat)
    {
        err = upload_biquads(state);
        if (err != ESP_OK) goto out_power;

        // Switch to PRB_P2 so the freshly-written biquads are active.
        err = s_ops.write_reg(0x00, REG_DAC_PRB, DSP_PRB_P2);
        if (err != ESP_OK) goto out_power;
    }

    err = program_drc(state);
    if (err != ESP_OK) goto out_power;

    s_last_applied     = *state;
    s_has_last_applied = true;

out_power:
    {
        esp_err_t pwr_err = s_ops.dac_power(true);
        if (err == ESP_OK) err = pwr_err;
    }

out:
    {
        esp_err_t mute_err = s_ops.mute(false);
        if (err == ESP_OK) err = mute_err;
    }

    if (err != ESP_OK && s_has_last_applied)
    {
        // Best-effort revert to the last known-good configuration.
        // Log and continue; don't recurse if the revert also fails.
        ESP_LOGE(TAG, "DSP apply failed (%d); reverting to last good", err);
        (void)s_ops.mute(true);
        vTaskDelay(pdMS_TO_TICKS(15));
        (void)s_ops.dac_power(false);
        (void)s_ops.write_reg(0x00, REG_DAC_PRB, DSP_PRB_P1);
        (void)upload_biquads(&s_last_applied);
        if (!tlv320_dsp_state_is_flat(&s_last_applied))
        {
            (void)s_ops.write_reg(0x00, REG_DAC_PRB, DSP_PRB_P2);
        }
        (void)program_drc(&s_last_applied);
        (void)s_ops.dac_power(true);
        (void)s_ops.mute(false);
    }

    return err;
}

void tlv320_dsp_state_format(const tlv320_dsp_state_t *state,
                             char *buf,
                             size_t buf_len)
{
    if (!buf || buf_len == 0) return;
    if (!state)
    {
        buf[0] = '\0';
        return;
    }

    int n = snprintf(buf, buf_len,
                     "bass=%.1fdB",
                     (double)tlv320_dsp_state_get_bass(state));
    for (int i = 0; i < TLV320_DSP_NUM_EQ_BANDS; i++)
    {
        if (n < 0 || (size_t)n >= buf_len) return;
        int n2 = snprintf(buf + n, buf_len - (size_t)n,
                          " eq%d=%.1fdB",
                          i + 1,
                          (double)tlv320_dsp_state_get_eq_band(state, i));
        if (n2 < 0) return;
        n += n2;
    }
    if (n >= 0 && (size_t)n < buf_len)
    {
        int n2 = snprintf(buf + n, buf_len - (size_t)n,
                          " treble=%.1fdB drc=%s(%s)",
                          (double)tlv320_dsp_state_get_treble(state),
                          state->drc_enabled ? "on" : "off",
                          tlv320_dsp_drc_preset_name(state->drc_preset));
        (void)n2;
    }
}

#endif // ESP_PLATFORM
