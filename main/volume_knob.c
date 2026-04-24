// SPDX-License-Identifier: MIT
// Copyright (c) 2025 Leland Lucius
// ----------------------------------------------------------------
// Analog volume knob implementation.  See volume_knob.h for the
// high-level contract and design rationale.
//
// Algorithm summary (also documented inline):
//   1. esp_timer fires every CONFIG_DTESP_VOLUME_KNOB_POLL_MS.
//   2. Callback takes one ADC reading on the configured GPIO.
//   3. Reading is pushed through a fixed-size moving average.
//   4. Filtered value is mapped to a 0..TLV320_MAX_VOLUME candidate
//      step, with hysteresis so we do not flap between adjacent
//      steps when the pot sits near a boundary.
//   5. If the pot is "unlatched" (a FW volume change just
//      happened), the candidate is ignored until it comes within
//      ±CONFIG_DTESP_VOLUME_KNOB_LATCH_WINDOW steps of the current
//      actual volume — at which point the pot latches and is
//      allowed to drive the codec again.
//   6. When latched and the candidate differs from the current
//      actual volume, tlv320_set_volume() is called and the new
//      level is recorded as "actual".
// ----------------------------------------------------------------

#include "volume_knob.h"

#include "sdkconfig.h"

#if CONFIG_DTESP_VOLUME_KNOB_ENABLE && CONFIG_DTESP_DAC_TLV320

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_adc/adc_oneshot.h"

#include "tlv320.h"

static const char *TAG = "vol_knob";

// -------- compile-time configuration --------

#define KNOB_POLL_MS       CONFIG_DTESP_VOLUME_KNOB_POLL_MS
#define KNOB_AVG_SAMPLES   CONFIG_DTESP_VOLUME_KNOB_AVG_SAMPLES
#define KNOB_LATCH_WINDOW  CONFIG_DTESP_VOLUME_KNOB_LATCH_WINDOW
#define KNOB_HYST_COUNTS   CONFIG_DTESP_VOLUME_KNOB_HYSTERESIS
#define KNOB_ADC_MIN       CONFIG_DTESP_VOLUME_KNOB_ADC_MIN
#define KNOB_ADC_MAX       CONFIG_DTESP_VOLUME_KNOB_ADC_MAX
#define KNOB_GPIO          CONFIG_DTESP_VOLUME_KNOB_GPIO

// On the ESP32-C6, ADC1 channels 0..6 map 1:1 to GPIO0..GPIO6.
// Map the user-selected GPIO to its channel; reject anything else
// at init time rather than risk a silent mis-configuration.
#if KNOB_GPIO < 0 || KNOB_GPIO > 6
#error "CONFIG_DTESP_VOLUME_KNOB_GPIO must be a valid ADC1 GPIO (0..6) on ESP32-C6"
#endif
#define KNOB_ADC_CHANNEL ((adc_channel_t)(KNOB_GPIO))

#define VOL_MAX TLV320_MAX_VOLUME   // 9
#define VOL_STEPS (VOL_MAX + 1)     // 10 discrete levels 0..9

// -------- module state --------

// The sampler runs from a single esp_timer task, so plain statics
// are fine for everything the sampler touches.  The two fields
// that can be written from other contexts (external volume
// notifications) are guarded with atomics — we only need
// consistency, not ordering.
static adc_oneshot_unit_handle_t s_adc_handle;
static esp_timer_handle_t        s_timer;
static bool                      s_initialised;

// Moving-average ring buffer of raw ADC readings.
static int      s_samples[KNOB_AVG_SAMPLES];
static uint32_t s_sum;        // running sum of s_samples
static uint8_t  s_sample_idx;
static uint8_t  s_sample_count;  // saturates at KNOB_AVG_SAMPLES

// Last filtered ADC value that drove a step transition.  Used for
// hysteresis: we only cross a step boundary when the filtered
// reading has moved at least KNOB_HYST_COUNTS past the boundary.
static int     s_last_stable_adc;
static uint8_t s_last_pot_step;   // last quantised pot step we emitted

// Soft-takeover state.  s_actual_volume is the codec's current
// volume from our perspective.  s_pot_latched is cleared whenever
// an external source changes volume and re-set once the pot moves
// back into range.
static _Atomic uint8_t s_actual_volume;
static _Atomic bool    s_pot_latched;

// -------- helpers --------

// Map a filtered ADC reading in [KNOB_ADC_MIN, KNOB_ADC_MAX] to a
// discrete volume step 0..VOL_MAX.  Values outside the configured
// endpoints are clamped.  The mapping deliberately uses integer
// arithmetic with rounding so endpoint values produce a clean 0 or
// VOL_MAX.
static uint8_t adc_to_step(int adc)
{
    if (adc <= KNOB_ADC_MIN)
    {
        return 0;
    }
    if (adc >= KNOB_ADC_MAX)
    {
        return VOL_MAX;
    }
    const int span = KNOB_ADC_MAX - KNOB_ADC_MIN;
    // (adc - MIN) * VOL_STEPS / span, clamped to [0, VOL_MAX].
    int step = ((adc - KNOB_ADC_MIN) * VOL_STEPS) / span;
    if (step < 0)
    {
        step = 0;
    }
    if (step > VOL_MAX)
    {
        step = VOL_MAX;
    }
    return (uint8_t)step;
}

// Apply moving-average smoothing and return the filtered value.
// The window grows from 1 up to KNOB_AVG_SAMPLES on the first few
// calls so we do not need a separate warm-up path.
static int update_average(int raw)
{
    if (s_sample_count < KNOB_AVG_SAMPLES)
    {
        s_samples[s_sample_idx] = raw;
        s_sum += (uint32_t)raw;
        s_sample_count++;
    }
    else
    {
        s_sum -= (uint32_t)s_samples[s_sample_idx];
        s_samples[s_sample_idx] = raw;
        s_sum += (uint32_t)raw;
    }
    s_sample_idx = (uint8_t)((s_sample_idx + 1) % KNOB_AVG_SAMPLES);
    return (int)(s_sum / s_sample_count);
}

// Quantise with hysteresis.  Returns the pot-derived step the
// firmware should currently consider "active", given the previous
// active step and the configured hysteresis in ADC counts.
static uint8_t quantise_with_hysteresis(int filtered)
{
    uint8_t cand = adc_to_step(filtered);
    if (cand == s_last_pot_step)
    {
        s_last_stable_adc = filtered;
        return cand;
    }
    // The candidate is different from the last emitted step.  Only
    // accept the transition if the filtered reading has moved at
    // least KNOB_HYST_COUNTS away from the last stable reading.
    int delta = filtered - s_last_stable_adc;
    if (delta < 0)
    {
        delta = -delta;
    }
    if (delta < KNOB_HYST_COUNTS)
    {
        // Not enough movement yet — keep reporting the previous
        // step to suppress chatter at the boundary.
        return s_last_pot_step;
    }
    s_last_pot_step   = cand;
    s_last_stable_adc = filtered;
    return cand;
}

static void timer_cb(void *arg)
{
    (void)arg;

    int raw = 0;
    esp_err_t err = adc_oneshot_read(s_adc_handle, KNOB_ADC_CHANNEL, &raw);
    if (err != ESP_OK)
    {
        // Transient read failure — skip this tick and try again.
        ESP_LOGD(TAG, "adc_oneshot_read failed: %s", esp_err_to_name(err));
        return;
    }

    int      filtered = update_average(raw);
    uint8_t  cand     = quantise_with_hysteresis(filtered);
    uint8_t  actual   = atomic_load(&s_actual_volume);
    bool     latched  = atomic_load(&s_pot_latched);

    ESP_LOGV(TAG, "raw=%d filt=%d cand=%u actual=%u latched=%d",
             raw, filtered, (unsigned)cand, (unsigned)actual, (int)latched);

    if (!latched)
    {
        // Soft takeover: wait for the pot position to come within
        // ±KNOB_LATCH_WINDOW steps of the current actual volume
        // before we let it drive the codec.  This is what prevents
        // a "[:fw volume 2]" followed by a pot sitting at step 8
        // from immediately snapping back to 8.
        int diff = (int)cand - (int)actual;
        if (diff < 0)
        {
            diff = -diff;
        }
        if (diff <= KNOB_LATCH_WINDOW)
        {
            atomic_store(&s_pot_latched, true);
            ESP_LOGI(TAG, "pot latched at step %u (actual=%u)",
                     (unsigned)cand, (unsigned)actual);
            latched = true;
            // Fall through so a latch at a different-but-in-window
            // value still applies — this matches "whichever was
            // used last wins" once the window is reached.
        }
        else
        {
            return;
        }
    }

    if (cand != actual)
    {
        atomic_store(&s_actual_volume, cand);
        tlv320_set_volume(cand);
        ESP_LOGD(TAG, "pot -> volume %u", (unsigned)cand);
    }
}

// -------- public API --------

esp_err_t volume_knob_init(void)
{
    if (s_initialised)
    {
        return ESP_OK;
    }

    adc_oneshot_unit_init_cfg_t unit_cfg =
    {
        .unit_id  = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    esp_err_t err = adc_oneshot_new_unit(&unit_cfg, &s_adc_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "adc_oneshot_new_unit failed: %s", esp_err_to_name(err));
        return err;
    }

    adc_oneshot_chan_cfg_t ch_cfg =
    {
        // 12 dB / ~3.3 V full scale matches a pot wired across VCC
        // and GND; users with a divided rail can narrow the range
        // via ADC_MIN/ADC_MAX Kconfig calibration.
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    err = adc_oneshot_config_channel(s_adc_handle, KNOB_ADC_CHANNEL, &ch_cfg);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "adc_oneshot_config_channel failed: %s",
                 esp_err_to_name(err));
        adc_oneshot_del_unit(s_adc_handle);
        s_adc_handle = NULL;
        return err;
    }

    // Seed "actual" from whatever the codec currently reports so
    // the first sampler tick has a valid reference.  The pot
    // starts unlatched — movement will latch it once it reaches
    // the window.
    atomic_store(&s_actual_volume, tlv320_get_volume());
    atomic_store(&s_pot_latched, false);
    s_last_pot_step   = tlv320_get_volume();
    s_last_stable_adc = 0;
    s_sum             = 0;
    s_sample_idx      = 0;
    s_sample_count    = 0;
    memset(s_samples, 0, sizeof(s_samples));

    const esp_timer_create_args_t timer_args =
    {
        .callback        = &timer_cb,
        .arg             = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name            = "vol_knob",
        .skip_unhandled_events = true,
    };
    err = esp_timer_create(&timer_args, &s_timer);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_timer_create failed: %s", esp_err_to_name(err));
        adc_oneshot_del_unit(s_adc_handle);
        s_adc_handle = NULL;
        return err;
    }

    err = esp_timer_start_periodic(s_timer, (uint64_t)KNOB_POLL_MS * 1000ULL);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_timer_start_periodic failed: %s",
                 esp_err_to_name(err));
        esp_timer_delete(s_timer);
        s_timer = NULL;
        adc_oneshot_del_unit(s_adc_handle);
        s_adc_handle = NULL;
        return err;
    }

    s_initialised = true;
    ESP_LOGI(TAG,
             "analog volume knob on GPIO%d (ADC1_CH%d): poll=%dms avg=%d "
             "latch=±%d hyst=%d range=[%d..%d]",
             KNOB_GPIO, KNOB_GPIO, KNOB_POLL_MS, KNOB_AVG_SAMPLES,
             KNOB_LATCH_WINDOW, KNOB_HYST_COUNTS,
             KNOB_ADC_MIN, KNOB_ADC_MAX);
    return ESP_OK;
}

void volume_knob_notify_external_volume(uint8_t level)
{
    if (level > VOL_MAX)
    {
        level = VOL_MAX;
    }
    atomic_store(&s_actual_volume, level);
    // External source wins for now; unlatch so the pot must be
    // moved into the window again before it retakes control.
    atomic_store(&s_pot_latched, false);
}

#else  // !(CONFIG_DTESP_VOLUME_KNOB_ENABLE && CONFIG_DTESP_DAC_TLV320)

// Stubs so callers do not need to bracket every call with #ifdef.

esp_err_t volume_knob_init(void)
{
    return ESP_OK;
}

void volume_knob_notify_external_volume(uint8_t level)
{
    (void)level;
}

#endif
