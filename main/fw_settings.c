// SPDX-License-Identifier: MIT
// Copyright (c) 2025 Leland Lucius
// ----------------------------------------------------------------
// Firmware Settings — NVS-backed mirror of codec Kconfig values.
// See fw_settings.h for the high-level contract.
// ----------------------------------------------------------------

#include "fw_settings.h"

#include "sdkconfig.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#if CONFIG_DECTALK_DAC_TLV320DAC3100
#include "tlv320dac3100.h"
#endif

static const char *TAG = "fw_settings";

#define FW_SETTINGS_NS          "dectalk_fw"
#define FW_SETTINGS_KEY_VOL     "vol"
#define FW_SETTINGS_KEY_PROFILE "profile"
#define FW_SETTINGS_KEY_AUTOSW  "autoswitch"

// Compile-time factory defaults pulled from Kconfig.
#if CONFIG_DECTALK_DAC_TLV320DAC3100
#define FW_DEFAULT_VOLUME ((uint8_t)CONFIG_DECTALK_TLV320_STARTUP_VOLUME)
#if CONFIG_DECTALK_TLV320_DEFAULT_PROFILE_HEADPHONE
#define FW_DEFAULT_PROFILE ((uint8_t)1)
#else
#define FW_DEFAULT_PROFILE ((uint8_t)0)
#endif
#if CONFIG_DECTALK_TLV320_HEADSET_AUTOSWITCH
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
#if CONFIG_DECTALK_DAC_TLV320DAC3100
    tlv320dac3100_set_autoswitch(s_autoswitch != 0);

    tlv320_profile_t tp = (s_profile == 1)
                              ? TLV320_PROFILE_HEADPHONE
                              : TLV320_PROFILE_SPEAKER;
    esp_err_t err = tlv320dac3100_set_profile(tp);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "set_profile on apply failed: %s",
                 esp_err_to_name(err));
    }

    tlv320dac3100_set_volume(s_volume);
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
