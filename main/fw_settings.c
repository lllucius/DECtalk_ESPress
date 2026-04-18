// SPDX-License-Identifier: MIT
// Copyright (c) 2025 Leland Lucius
// ----------------------------------------------------------------
// Firmware Settings — NVS-backed runtime overrides for Kconfig
// ----------------------------------------------------------------

#include "sdkconfig.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "fw_settings.h"

static const char *TAG = "FW Settings";

#define NVS_NAMESPACE   "dectalk_fw"
#define NVS_KEY_VOLUME      "volume"
#define NVS_KEY_PROFILE     "profile"
#define NVS_KEY_AUTOSWITCH  "autoswitch"

// ---- Kconfig compile-time defaults --------------------------------

static uint8_t kconfig_default_volume(void)
{
#if CONFIG_DECTALK_DAC_TLV320DAC3100
    return (uint8_t)CONFIG_DECTALK_TLV320_STARTUP_VOLUME;
#else
    return 5;
#endif
}

static int kconfig_default_profile(void)
{
#if CONFIG_DECTALK_DAC_TLV320DAC3100
#if CONFIG_DECTALK_TLV320_DEFAULT_PROFILE_HEADPHONE
    return 1;
#else
    return 0;
#endif
#else
    return 0;
#endif
}

static bool kconfig_default_autoswitch(void)
{
#if CONFIG_DECTALK_DAC_TLV320DAC3100
#if CONFIG_DECTALK_TLV320_HEADSET_AUTOSWITCH
    return true;
#else
    return false;
#endif
#else
    return false;
#endif
}

// ---- Runtime state ------------------------------------------------

static uint8_t s_volume;
static int     s_profile;
static bool    s_autoswitch;

// ---- NVS helpers --------------------------------------------------

static esp_err_t nvs_open_rw(nvs_handle_t *handle)
{
    return nvs_open(NVS_NAMESPACE, NVS_READWRITE, handle);
}

// ---- Public API ---------------------------------------------------

void fw_settings_init(void)
{
    // Start from Kconfig defaults.
    s_volume     = kconfig_default_volume();
    s_profile    = kconfig_default_profile();
    s_autoswitch = kconfig_default_autoswitch();

    // Attempt to override from NVS.
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK)
    {
        uint8_t u8;

        if (nvs_get_u8(nvs, NVS_KEY_VOLUME, &u8) == ESP_OK)
        {
            s_volume = (u8 <= 9) ? u8 : 9;
            ESP_LOGI(TAG, "NVS: volume = %u", s_volume);
        }

        if (nvs_get_u8(nvs, NVS_KEY_PROFILE, &u8) == ESP_OK)
        {
            s_profile = (u8 <= 1) ? (int)u8 : 0;
            ESP_LOGI(TAG, "NVS: profile = %s",
                     s_profile ? "headphone" : "speaker");
        }

        if (nvs_get_u8(nvs, NVS_KEY_AUTOSWITCH, &u8) == ESP_OK)
        {
            s_autoswitch = (u8 != 0);
            ESP_LOGI(TAG, "NVS: autoswitch = %s",
                     s_autoswitch ? "on" : "off");
        }

        nvs_close(nvs);
    }
    else
    {
        ESP_LOGI(TAG, "No saved settings — using Kconfig defaults");
    }
}

void fw_settings_save(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open_rw(&nvs);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "NVS open for write failed: %s", esp_err_to_name(err));
        return;
    }

    nvs_set_u8(nvs, NVS_KEY_VOLUME,     s_volume);
    nvs_set_u8(nvs, NVS_KEY_PROFILE,    (uint8_t)s_profile);
    nvs_set_u8(nvs, NVS_KEY_AUTOSWITCH, s_autoswitch ? 1 : 0);

    err = nvs_commit(nvs);
    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "Settings saved to NVS");
    }
    else
    {
        ESP_LOGE(TAG, "NVS commit failed: %s", esp_err_to_name(err));
    }

    nvs_close(nvs);
}

void fw_settings_reset(void)
{
    // Erase the namespace.
    nvs_handle_t nvs;
    esp_err_t err = nvs_open_rw(&nvs);
    if (err == ESP_OK)
    {
        nvs_erase_all(nvs);
        nvs_commit(nvs);
        nvs_close(nvs);
    }

    // Restore Kconfig defaults.
    s_volume     = kconfig_default_volume();
    s_profile    = kconfig_default_profile();
    s_autoswitch = kconfig_default_autoswitch();

    ESP_LOGI(TAG, "Settings reset to Kconfig defaults");
}

// ---- Individual getters / setters ---------------------------------

uint8_t fw_settings_get_volume(void)
{
    return s_volume;
}

void fw_settings_set_volume(uint8_t level)
{
    if (level > 9)
    {
        level = 9;
    }

    s_volume = level;
}

int fw_settings_get_profile(void)
{
    return s_profile;
}

void fw_settings_set_profile(int profile)
{
    s_profile = (profile == 1) ? 1 : 0;
}

bool fw_settings_get_autoswitch(void)
{
    return s_autoswitch;
}

void fw_settings_set_autoswitch(bool enabled)
{
    s_autoswitch = enabled;
}
