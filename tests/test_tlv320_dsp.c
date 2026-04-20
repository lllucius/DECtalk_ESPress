// SPDX-License-Identifier: MIT
// Copyright (c) 2025 Leland Lucius
// ----------------------------------------------------------------
// Host-native unit tests for the RBJ biquad / Q1.23 converter
// that lives in main/tlv320_dsp.c.  This binary deliberately
// stops at the pure-math / state helpers; it does NOT exercise
// the hardware-facing apply/upload path (which depends on
// ESP-IDF).
//
// Build:  make -C tests test
// ----------------------------------------------------------------

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../main/tlv320_dsp.h"

static int tests_run    = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) \
    static void test_##name(void); \
    static void run_test_##name(void) \
    { \
        tests_run++; \
        printf("TEST %-50s ", #name); \
        test_##name(); \
        tests_passed++; \
        printf("PASS\n"); \
    } \
    static void test_##name(void)

#define ASSERT_TRUE(x) \
    do { \
        if (!(x)) { \
            printf("FAIL\n  %s:%d: %s was false\n", __FILE__, __LINE__, #x); \
            tests_failed++; \
            return; \
        } \
    } while (0)

#define ASSERT_NEAR(a, b, tol) \
    do { \
        double _a = (double)(a); \
        double _b = (double)(b); \
        double _t = (double)(tol); \
        if (fabs(_a - _b) > _t) { \
            printf("FAIL\n  %s:%d: %s = %g, expected ~%g (tol %g)\n", \
                   __FILE__, __LINE__, #a, _a, _b, _t); \
            tests_failed++; \
            return; \
        } \
    } while (0)

// Q1.23 one-LSB in floating-point, used to size tolerances.
static const double Q23_LSB = 1.0 / (double)(1 << 23);

static double q23_to_float(int32_t q)
{
    return (double)q / (double)(1 << 23);
}

// ================================================================
// Q1.23 conversion
// ================================================================

TEST(q23_zero_one_neg_one)
{
    ASSERT_TRUE(tlv320_dsp_float_to_q23(0.0f) == 0);
    // 1.0 is clamped to the max representable positive value (Q1.23
    // can't exactly represent +1.0).
    ASSERT_TRUE(tlv320_dsp_float_to_q23(1.0f) == (1 << 23) - 1);
    ASSERT_TRUE(tlv320_dsp_float_to_q23(-1.0f) == -(1 << 23));
}

TEST(q23_clamp_overflow)
{
    // Values outside [-1, +1) are saturated, not wrapped.
    ASSERT_TRUE(tlv320_dsp_float_to_q23(5.0f) == (1 << 23) - 1);
    ASSERT_TRUE(tlv320_dsp_float_to_q23(-2.5f) == -(1 << 23));
}

TEST(q23_roundtrip_midrange)
{
    const float samples[] = { 0.25f, -0.5f, 0.75f, -0.125f };
    for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); i++)
    {
        int32_t q = tlv320_dsp_float_to_q23(samples[i]);
        double back = q23_to_float(q);
        ASSERT_NEAR(back, samples[i], Q23_LSB);
    }
}

// ================================================================
// Bypass / pass-through coefficients
// ================================================================

TEST(bypass_coefficients)
{
    // Chip applies an implicit 2x gain on stored coefficients; the
    // unity pass-through therefore stores N0 = 0.5 (= 0x400000)
    // with all other coefficients zero.  See encode_biquad() in
    // tlv320_dsp.c.
    int32_t c[5];
    tlv320_dsp_compute_bypass(c);
    ASSERT_TRUE(c[0] == (1 << 22));
    ASSERT_TRUE(c[1] == 0);
    ASSERT_TRUE(c[2] == 0);
    ASSERT_TRUE(c[3] == 0);
    ASSERT_TRUE(c[4] == 0);
}

// ================================================================
// Peaking / shelving RBJ coefficients at 0 dB
// ================================================================
//
// A 0 dB peaking or shelving filter has A = 1, so the numerator
// and denominator of the RBJ biquad are identical.  After the
// chip's internal 2x scaling the stored values are:
//   N0 = 0.5, N1 = -cos(w0), N2 = 0.5*(1 - alpha)/(1 + alpha)
//   D1 = cos(w0),  D2 = -0.5*(1 - alpha)/(1 + alpha)
// but what matters for an identity test is that the sum-of-
// impulse-response at steady state equals the input, i.e. the
// numerator and denominator coefficient *ratios* match.

TEST(peaking_zero_db_is_passthrough_shape)
{
    int32_t c[5];
    bool prescaled = false;
    tlv320_dsp_compute_peaking(11025.0f, 1000.0f, 1.0f, 0.0f,
                               c, &prescaled);
    ASSERT_TRUE(!prescaled);

    double N0 = q23_to_float(c[0]);
    double N1 = q23_to_float(c[1]);
    double N2 = q23_to_float(c[2]);
    double D1 = q23_to_float(c[3]);
    double D2 = q23_to_float(c[4]);

    // Stored N0 should be exactly 0.5 (pre-scaled unity).
    ASSERT_NEAR(N0, 0.5, 1e-4);

    // For a 0 dB peaking filter the stored N_k and D_k values
    // satisfy N1 == -D1 and N2 == -D2 (since numerator ==
    // denominator at A=1).  Exercise this symmetry.
    ASSERT_NEAR(N1, -D1, 1e-4);
    ASSERT_NEAR(N2, -D2, 1e-4);
}

TEST(low_shelf_cut_stays_in_range)
{
    int32_t c[5];
    bool prescaled = false;
    tlv320_dsp_compute_low_shelf(11025.0f, 200.0f, 0.707f, -3.0f,
                                 c, &prescaled);
    ASSERT_TRUE(!prescaled);
    // All stored coefficients must fit in Q1.23 exactly after
    // the hardware 2x pre-scale.
    for (int i = 0; i < 5; i++)
    {
        double v = q23_to_float(c[i]);
        ASSERT_TRUE(fabs(v) < 1.0);
    }
}

TEST(high_shelf_boost_stays_in_range)
{
    int32_t c[5];
    bool prescaled = false;
    tlv320_dsp_compute_high_shelf(11025.0f, 4500.0f, 0.707f, +4.0f,
                                  c, &prescaled);
    ASSERT_TRUE(!prescaled);
    for (int i = 0; i < 5; i++)
    {
        double v = q23_to_float(c[i]);
        ASSERT_TRUE(fabs(v) < 1.0);
    }
}

TEST(peaking_max_boost_accepts_prescale)
{
    // +12 dB at a low frequency can push the raw numerator slightly
    // beyond the ±2 range the hw 2x scaling gives us; the encoder
    // then halves N and reports prescaled=true.  Either way the
    // stored result must fit in Q1.23.
    int32_t c[5];
    bool prescaled = false;
    tlv320_dsp_compute_peaking(11025.0f, 160.0f, 1.0f, +12.0f,
                               c, &prescaled);
    for (int i = 0; i < 5; i++)
    {
        double v = q23_to_float(c[i]);
        ASSERT_TRUE(fabs(v) < 1.0);
    }
    (void)prescaled;
}

// ================================================================
// Coefficient-computation input clamping
// ================================================================

TEST(coefficients_clamp_gain_out_of_range)
{
    // Requesting +50 dB should be internally clamped to +12 dB;
    // the returned coefficients must still fit in Q1.23.
    int32_t c[5];
    bool prescaled = false;
    tlv320_dsp_compute_peaking(11025.0f, 1000.0f, 1.0f, +50.0f,
                               c, &prescaled);
    for (int i = 0; i < 5; i++)
    {
        double v = q23_to_float(c[i]);
        ASSERT_TRUE(fabs(v) < 1.0);
    }
}

TEST(coefficients_clamp_freq_out_of_range)
{
    // f0 well above Nyquist: should be clamped below Fs/2 so the
    // math stays well-defined (no NaN / inf).
    int32_t c[5];
    bool prescaled = false;
    tlv320_dsp_compute_peaking(11025.0f, 99000.0f, 1.0f, +3.0f,
                               c, &prescaled);
    for (int i = 0; i < 5; i++)
    {
        double v = q23_to_float(c[i]);
        ASSERT_TRUE(!isnan(v) && !isinf(v));
        ASSERT_TRUE(fabs(v) < 1.0);
    }
}

// ================================================================
// State helpers (bass / treble / EQ / presets)
// ================================================================

TEST(state_default_is_flat)
{
    tlv320_dsp_state_t s;
    tlv320_dsp_state_default(&s);
    ASSERT_TRUE(tlv320_dsp_state_get_bass(&s)   == 0.0f);
    ASSERT_TRUE(tlv320_dsp_state_get_treble(&s) == 0.0f);
    for (int i = 0; i < 5; i++)
    {
        ASSERT_TRUE(tlv320_dsp_state_get_eq_band(&s, i) == 0.0f);
    }
    ASSERT_TRUE(s.drc_enabled == false);
}

TEST(state_set_bass_treble_clamps)
{
    tlv320_dsp_state_t s;
    tlv320_dsp_state_default(&s);
    tlv320_dsp_state_set_bass(&s, -50.0f);
    tlv320_dsp_state_set_treble(&s, +80.0f);
    ASSERT_TRUE(tlv320_dsp_state_get_bass(&s)   == -12.0f);
    ASSERT_TRUE(tlv320_dsp_state_get_treble(&s) == +12.0f);
}

TEST(state_eq_band_index_bounds)
{
    tlv320_dsp_state_t s;
    tlv320_dsp_state_default(&s);
    tlv320_dsp_state_set_eq_band(&s, 2, +3.0f);
    // Out-of-range indices are ignored — nothing else changes.
    tlv320_dsp_state_set_eq_band(&s, -1, -5.0f);
    tlv320_dsp_state_set_eq_band(&s, 5, +5.0f);
    ASSERT_TRUE(tlv320_dsp_state_get_eq_band(&s, 2) == +3.0f);
    // Unaffected bands remain at 0 dB.
    ASSERT_TRUE(tlv320_dsp_state_get_eq_band(&s, 0) == 0.0f);
    ASSERT_TRUE(tlv320_dsp_state_get_eq_band(&s, 4) == 0.0f);
}

TEST(state_reset_eq_clears_bands_only)
{
    tlv320_dsp_state_t s;
    tlv320_dsp_state_default(&s);
    tlv320_dsp_state_set_bass(&s, -3.0f);
    tlv320_dsp_state_set_treble(&s, +2.0f);
    tlv320_dsp_state_set_eq_band(&s, 0, -5.0f);
    tlv320_dsp_state_set_eq_band(&s, 4, +4.0f);

    tlv320_dsp_state_reset_eq(&s);

    for (int i = 0; i < 5; i++)
    {
        ASSERT_TRUE(tlv320_dsp_state_get_eq_band(&s, i) == 0.0f);
    }
    // Bass / treble are left alone by reset_eq.
    ASSERT_TRUE(tlv320_dsp_state_get_bass(&s)   == -3.0f);
    ASSERT_TRUE(tlv320_dsp_state_get_treble(&s) == +2.0f);
}

TEST(state_preset_flat_matches_defaults)
{
    tlv320_dsp_state_t s;
    tlv320_dsp_state_default(&s);
    tlv320_dsp_state_set_bass(&s, -6.0f);
    tlv320_dsp_state_set_eq_band(&s, 1, -4.0f);
    ASSERT_TRUE(tlv320_dsp_state_apply_preset(&s, "flat"));
    ASSERT_TRUE(tlv320_dsp_state_get_bass(&s)   == 0.0f);
    for (int i = 0; i < 5; i++)
    {
        ASSERT_TRUE(tlv320_dsp_state_get_eq_band(&s, i) == 0.0f);
    }
}

TEST(state_preset_speech_sets_expected_bands)
{
    tlv320_dsp_state_t s;
    tlv320_dsp_state_default(&s);
    ASSERT_TRUE(tlv320_dsp_state_apply_preset(&s, "speech"));
    // Per the plan: bass -3, peak -2 @ 500, peak +3 @ 3k, treble +2.
    ASSERT_NEAR(tlv320_dsp_state_get_bass(&s),   -3.0, 0.01);
    ASSERT_NEAR(tlv320_dsp_state_get_treble(&s), +2.0, 0.01);
}

TEST(state_preset_unknown_rejected)
{
    tlv320_dsp_state_t s;
    tlv320_dsp_state_default(&s);
    ASSERT_TRUE(!tlv320_dsp_state_apply_preset(&s, "bogus"));
}

TEST(state_preset_case_insensitive)
{
    tlv320_dsp_state_t s;
    tlv320_dsp_state_default(&s);
    ASSERT_TRUE(tlv320_dsp_state_apply_preset(&s, "SPEECH"));
    ASSERT_NEAR(tlv320_dsp_state_get_bass(&s), -3.0, 0.01);
}

// ================================================================
// Main
// ================================================================

int main(void)
{
    run_test_q23_zero_one_neg_one();
    run_test_q23_clamp_overflow();
    run_test_q23_roundtrip_midrange();

    run_test_bypass_coefficients();

    run_test_peaking_zero_db_is_passthrough_shape();
    run_test_low_shelf_cut_stays_in_range();
    run_test_high_shelf_boost_stays_in_range();
    run_test_peaking_max_boost_accepts_prescale();

    run_test_coefficients_clamp_gain_out_of_range();
    run_test_coefficients_clamp_freq_out_of_range();

    run_test_state_default_is_flat();
    run_test_state_set_bass_treble_clamps();
    run_test_state_eq_band_index_bounds();
    run_test_state_reset_eq_clears_bands_only();
    run_test_state_preset_flat_matches_defaults();
    run_test_state_preset_speech_sets_expected_bands();
    run_test_state_preset_unknown_rejected();
    run_test_state_preset_case_insensitive();

    printf("\n=== Results: %d/%d passed, %d failed ===\n",
           tests_passed, tests_run, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
