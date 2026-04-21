// SPDX-License-Identifier: MIT
// Copyright (c) 2025 Leland Lucius
// ----------------------------------------------------------------
// Firmware Settings — NVS-backed mirror of codec Kconfig values.
// See fw_settings.h for the high-level contract.
// ----------------------------------------------------------------

#include "fw_settings.h"

#include <string.h>

#include "sdkconfig.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#if CONFIG_DTESP_DAC_TLV320
#include "tlv320.h"
#include "tlv320_dsp.h"
#endif

static const char *TAG = "fw_settings";

#define FW_SETTINGS_NS          "dtesp_fw"
#define FW_SETTINGS_KEY_VOL     "vol"
#define FW_SETTINGS_KEY_PROFILE "profile"
#define FW_SETTINGS_KEY_AUTOSW  "autoswitch"
#define FW_SETTINGS_KEY_DSP     "eq_v1"

// Versioned binary blob layout for the DSP / audio-quality state.
// Kept separate from the u8 keys so it can grow without a schema
// migration: any unexpected size or magic causes a clean fallback
// to defaults.  Reserved bytes are zero-initialised so a future
// revision can reuse them without shifting existing fields.
#define FW_DSP_BLOB_MAGIC   0x44535001u   // 'D','S','P',0x01
#define FW_DSP_BLOB_VERSION 1

typedef struct __attribute__((packed))
{
    uint32_t magic;
    uint16_t version;
    uint16_t reserved_hdr;

    // Tone controls in tenths of a decibel so ±12 dB fits in a signed 16-bit.
    int16_t  bass_dtb;
    int16_t  treble_dtb;
    int16_t  eq_dtb[5];            // EQ band 1..5

    uint8_t  drc_enabled;
    uint8_t  drc_preset;           // 0=soft, 1=speech, 2=loud

    uint8_t  spk_gain_db;          // 6 / 12 / 18 / 24

    uint8_t  reserved_tail[7];     // pad to 32 bytes for alignment friendliness
} fw_dsp_blob_t;

_Static_assert(sizeof(fw_dsp_blob_t) == 32,
               "fw_dsp_blob_t must be exactly 32 bytes for forward compat");

// Compile-time factory defaults pulled from Kconfig.
#if CONFIG_DTESP_DAC_TLV320
#define FW_DEFAULT_VOLUME ((uint8_t)CONFIG_DTESP_TLV320_STARTUP_VOLUME)
#if CONFIG_DTESP_TLV320_DEFAULT_PROFILE_HEADPHONE
#define FW_DEFAULT_PROFILE ((uint8_t)1)
#else
#define FW_DEFAULT_PROFILE ((uint8_t)0)
#endif
#if CONFIG_DTESP_TLV320_HEADSET_AUTOSWITCH
#define FW_DEFAULT_AUTOSW ((uint8_t)1)
#else
#define FW_DEFAULT_AUTOSW ((uint8_t)0)
#endif
#else
// Non-TLV320 builds still compile the module, but the values have no
// effect on hardware.
#define FW_DEFAULT_VOLUME  ((uint8_t)5)
#define FW_DEFAULT_PROFILE ((uint8_t)0)
#define FW_DEFAULT_AUTOSW  ((uint8_t)1)
#endif

static uint8_t s_volume     = FW_DEFAULT_VOLUME;
static uint8_t s_profile    = FW_DEFAULT_PROFILE;
static uint8_t s_autoswitch = FW_DEFAULT_AUTOSW;
static bool    s_initialised = false;

#if CONFIG_DTESP_DAC_TLV320

#ifdef CONFIG_DTESP_TLV320_DEFAULT_SPK_GAIN_DB
#define FW_DEFAULT_SPK_GAIN ((uint8_t)CONFIG_DTESP_TLV320_DEFAULT_SPK_GAIN_DB)
#else
#define FW_DEFAULT_SPK_GAIN ((uint8_t)6)
#endif

#ifdef CONFIG_DTESP_TLV320_DRC_DEFAULT
#define FW_DEFAULT_DRC_ON 1
#else
#define FW_DEFAULT_DRC_ON 0
#endif

#if defined(CONFIG_DTESP_TLV320_DEFAULT_PRESET_SPEECH)
#define FW_DEFAULT_PRESET_NAME "speech"
#elif defined(CONFIG_DTESP_TLV320_DEFAULT_PRESET_CRISP)
#define FW_DEFAULT_PRESET_NAME "crisp"
#elif defined(CONFIG_DTESP_TLV320_DEFAULT_PRESET_WARM)
#define FW_DEFAULT_PRESET_NAME "warm"
#else
#define FW_DEFAULT_PRESET_NAME "flat"
#endif

static tlv320_dsp_state_t s_dsp;
static uint8_t            s_spk_gain_db = FW_DEFAULT_SPK_GAIN;

static void fw_dsp_reset_defaults(void)
{
    tlv320_dsp_state_default(&s_dsp);
    // Apply the factory-default preset.  "flat" leaves every slot
    // disabled and DRC off, matching the pre-DSP-feature behaviour.
    if (!tlv320_dsp_state_apply_preset(&s_dsp, FW_DEFAULT_PRESET_NAME))
    {
        // Defensive fallback; preset table is compile-time constant
        // so this should never fire.
        (void)tlv320_dsp_state_apply_preset(&s_dsp, "flat");
    }
    s_dsp.drc_enabled = (FW_DEFAULT_DRC_ON != 0);
    s_spk_gain_db     = FW_DEFAULT_SPK_GAIN;
}

static void fw_dsp_to_blob(fw_dsp_blob_t *blob)
{
    memset(blob, 0, sizeof(*blob));
    blob->magic   = FW_DSP_BLOB_MAGIC;
    blob->version = FW_DSP_BLOB_VERSION;
    blob->bass_dtb   = (int16_t)(tlv320_dsp_state_get_bass(&s_dsp)   * 10.0f);
    blob->treble_dtb = (int16_t)(tlv320_dsp_state_get_treble(&s_dsp) * 10.0f);
    for (int i = 0; i < 5; i++)
    {
        blob->eq_dtb[i] = (int16_t)(tlv320_dsp_state_get_eq_band(&s_dsp, i) * 10.0f);
    }
    blob->drc_enabled = s_dsp.drc_enabled ? 1 : 0;
    blob->drc_preset  = (uint8_t)s_dsp.drc_preset;
    blob->spk_gain_db = s_spk_gain_db;
}

static bool fw_dsp_from_blob(const fw_dsp_blob_t *blob)
{
    if (blob->magic != FW_DSP_BLOB_MAGIC || blob->version != FW_DSP_BLOB_VERSION)
    {
        return false;
    }
    tlv320_dsp_state_default(&s_dsp);
    tlv320_dsp_state_set_bass(&s_dsp,   blob->bass_dtb   / 10.0f);
    tlv320_dsp_state_set_treble(&s_dsp, blob->treble_dtb / 10.0f);
    for (int i = 0; i < 5; i++)
    {
        tlv320_dsp_state_set_eq_band(&s_dsp, i, blob->eq_dtb[i] / 10.0f);
    }
    s_dsp.drc_enabled = (blob->drc_enabled != 0);
    if (blob->drc_preset <= (uint8_t)TLV320_DSP_DRC_PRESET_LOUD)
    {
        s_dsp.drc_preset = (tlv320_dsp_drc_preset_t)blob->drc_preset;
    }
    uint8_t g = blob->spk_gain_db;
    if (g != 6 && g != 12 && g != 18 && g != 24)
    {
        g = FW_DEFAULT_SPK_GAIN;
    }
    s_spk_gain_db = g;
    return true;
}

#endif // CONFIG_DTESP_DAC_TLV320

static esp_err_t load_u8(nvs_handle_t h, const char *key, uint8_t *out,
                         uint8_t fallback)
{
    uint8_t v = 0;
    esp_err_t err = nvs_get_u8(h, key, &v);
    if (err == ESP_OK)
    {
        *out = v;
        return ESP_OK;
    }
    if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        *out = fallback;
        return ESP_OK;
    }
    ESP_LOGW(TAG, "nvs_get_u8(%s) failed: %s (using default %u)",
             key, esp_err_to_name(err), (unsigned)fallback);
    *out = fallback;
    return err;
}

esp_err_t fw_settings_init(void)
{
#if CONFIG_DTESP_DAC_TLV320
    // Always start from factory DSP defaults; the NVS blob (if
    // present) then overwrites any fields it covers.
    fw_dsp_reset_defaults();
#endif

    nvs_handle_t h;
    esp_err_t err = nvs_open(FW_SETTINGS_NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        // Namespace has never been written — stick with defaults.
        s_volume     = FW_DEFAULT_VOLUME;
        s_profile    = FW_DEFAULT_PROFILE;
        s_autoswitch = FW_DEFAULT_AUTOSW;
        s_initialised = true;
        ESP_LOGI(TAG,
                 "no NVS namespace yet; using defaults vol=%u profile=%u autoswitch=%u",
                 (unsigned)s_volume, (unsigned)s_profile, (unsigned)s_autoswitch);
        return ESP_OK;
    }
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "nvs_open(%s) failed: %s", FW_SETTINGS_NS,
                 esp_err_to_name(err));
        s_initialised = true;
        return err;
    }

    (void)load_u8(h, FW_SETTINGS_KEY_VOL,     &s_volume,     FW_DEFAULT_VOLUME);
    (void)load_u8(h, FW_SETTINGS_KEY_PROFILE, &s_profile,    FW_DEFAULT_PROFILE);
    (void)load_u8(h, FW_SETTINGS_KEY_AUTOSW,  &s_autoswitch, FW_DEFAULT_AUTOSW);

#if CONFIG_DTESP_DAC_TLV320
    // Load the DSP blob when present.  Any schema mismatch silently
    // falls back to the factory defaults computed above.
    {
        fw_dsp_blob_t blob;
        size_t blob_len = sizeof(blob);
        esp_err_t berr = nvs_get_blob(h, FW_SETTINGS_KEY_DSP, &blob, &blob_len);
        if (berr == ESP_OK && blob_len == sizeof(blob))
        {
            if (!fw_dsp_from_blob(&blob))
            {
                ESP_LOGW(TAG, "DSP blob magic/version mismatch; using defaults");
                fw_dsp_reset_defaults();
            }
            else
            {
                ESP_LOGI(TAG, "loaded DSP state from NVS");
            }
        }
        else if (berr != ESP_ERR_NVS_NOT_FOUND)
        {
            ESP_LOGW(TAG, "nvs_get_blob(%s) failed (%s); using defaults",
                     FW_SETTINGS_KEY_DSP, esp_err_to_name(berr));
        }
    }
#endif

    nvs_close(h);

    // Clamp any out-of-range values that might have been written by
    // earlier firmware revisions.
    if (s_volume > 9)
    {
        s_volume = FW_DEFAULT_VOLUME;
    }
    if (s_profile > 1)
    {
        s_profile = FW_DEFAULT_PROFILE;
    }
    if (s_autoswitch > 1)
    {
        s_autoswitch = FW_DEFAULT_AUTOSW;
    }

    s_initialised = true;
    ESP_LOGI(TAG, "loaded vol=%u profile=%u autoswitch=%u",
             (unsigned)s_volume, (unsigned)s_profile, (unsigned)s_autoswitch);
    return ESP_OK;
}

void fw_settings_apply(void)
{
#if CONFIG_DTESP_DAC_TLV320
    tlv320_set_autoswitch(s_autoswitch != 0);

    tlv320_profile_t tp = (s_profile == 1)
                              ? TLV320_PROFILE_HEADPHONE
                              : TLV320_PROFILE_SPEAKER;

    // Push the desired speaker analog gain before the profile
    // switch so the first set_profile() picks it up.
    (void)tlv320_set_speaker_gain_db(s_spk_gain_db);

    esp_err_t err = tlv320_set_profile(tp);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "set_profile on apply failed: %s",
                 esp_err_to_name(err));
    }

    tlv320_set_volume(s_volume);

    // Apply DSP state last so EQ / DRC sit on top of the profile's
    // cleanly programmed base routing.
    esp_err_t derr = tlv320_apply_dsp(&s_dsp);
    if (derr != ESP_OK)
    {
        ESP_LOGW(TAG, "apply_dsp failed: %s", esp_err_to_name(derr));
    }
#endif
}

esp_err_t fw_settings_save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(FW_SETTINGS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "nvs_open(rw) failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_u8(h, FW_SETTINGS_KEY_VOL, s_volume);
    if (err == ESP_OK)
    {
        err = nvs_set_u8(h, FW_SETTINGS_KEY_PROFILE, s_profile);
    }
    if (err == ESP_OK)
    {
        err = nvs_set_u8(h, FW_SETTINGS_KEY_AUTOSW, s_autoswitch);
    }

#if CONFIG_DTESP_DAC_TLV320
    if (err == ESP_OK)
    {
        fw_dsp_blob_t blob;
        fw_dsp_to_blob(&blob);
        err = nvs_set_blob(h, FW_SETTINGS_KEY_DSP, &blob, sizeof(blob));
    }
#endif

    if (err == ESP_OK)
    {
        err = nvs_commit(h);
    }

    nvs_close(h);

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "saved vol=%u profile=%u autoswitch=%u",
                 (unsigned)s_volume, (unsigned)s_profile,
                 (unsigned)s_autoswitch);
    }
    else
    {
        ESP_LOGE(TAG, "save failed: %s", esp_err_to_name(err));
    }
    return err;
}

uint8_t fw_settings_get_volume(void)     { return s_volume; }
uint8_t fw_settings_get_profile(void)    { return s_profile; }
uint8_t fw_settings_get_autoswitch(void) { return s_autoswitch; }

void fw_settings_set_volume(uint8_t level)
{
    if (level > 9)
    {
        level = 9;
    }
    s_volume = level;
}

void fw_settings_set_profile(uint8_t profile)
{
    s_profile = (profile == 1) ? 1 : 0;
}

void fw_settings_set_autoswitch(uint8_t enable)
{
    s_autoswitch = enable ? 1 : 0;
}

#if CONFIG_DTESP_DAC_TLV320
tlv320_dsp_state_t *fw_settings_get_dsp(void)
{
    return &s_dsp;
}

uint8_t fw_settings_get_spk_gain_db(void)
{
    return s_spk_gain_db;
}

void fw_settings_set_spk_gain_db(uint8_t db)
{
    if (db != 6 && db != 12 && db != 18 && db != 24)
    {
        return;
    }
    s_spk_gain_db = db;
}
#endif
