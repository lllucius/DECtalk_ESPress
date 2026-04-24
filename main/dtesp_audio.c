// SPDX-License-Identifier: MIT
// Copyright (c) 2025 Leland Lucius
// ----------------------------------------------------------------
// Audio Subsystem Initialization
//
// Configures the I2S output channel and, when selected, the
// TLV320DAC3100 codec over I2C.  Separated from dtesp.c
// to keep audio hardware concerns in one place.
// ----------------------------------------------------------------

#include "dtesp_audio.h"

#include "sdkconfig.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_log.h"

#if CONFIG_DTESP_DAC_TLV320
#include "tlv320.h"
#include "fw_settings.h"
#include "nvs_flash.h"
#include "volume_knob.h"
#endif

static const char *TAG = "audio";

#define SAMPLE_RATE              11025
#define I2S_BCK_IO               CONFIG_DTESP_I2S_BCK_GPIO
#define I2S_WS_IO                CONFIG_DTESP_I2S_WS_GPIO
#define I2S_DO_IO                CONFIG_DTESP_I2S_DO_GPIO
#define I2S_MCLK_IO              CONFIG_DTESP_I2S_MCLK_GPIO
#define I2S_DMA_DESC_NUM         CONFIG_DTESP_I2S_DMA_DESC_NUM
#define I2S_DMA_FRAME_NUM        CONFIG_DTESP_I2S_DMA_FRAME_NUM

static i2s_chan_handle_t s_audio_handle;

i2s_chan_handle_t dtesp_audio_get_handle(void)
{
    return s_audio_handle;
}

esp_err_t dtesp_audio_init(void)
{
    ESP_LOGI(TAG, "Initializing I2S audio output...");

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = I2S_DMA_DESC_NUM;
    chan_cfg.dma_frame_num = I2S_DMA_FRAME_NUM;
    chan_cfg.auto_clear = true;

    esp_err_t err = i2s_new_channel(&chan_cfg, &s_audio_handle, NULL);
    if (err != ESP_OK)
    {
        return err;
    }

    i2s_std_config_t std_cfg =
    {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg =
        {
            .mclk = (I2S_MCLK_IO >= 0) ? (gpio_num_t)I2S_MCLK_IO : I2S_GPIO_UNUSED,
            .bclk = I2S_BCK_IO,
            .ws = I2S_WS_IO,
            .dout = I2S_DO_IO,
            .din = I2S_GPIO_UNUSED,
            .invert_flags =
            {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    if (I2S_MCLK_IO >= 0)
    {
        std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    }

    err = i2s_channel_init_std_mode(s_audio_handle, &std_cfg);
    if (err != ESP_OK)
    {
        return err;
    }

    err = i2s_channel_enable(s_audio_handle);
    if (err != ESP_OK)
    {
        return err;
    }

    if (I2S_MCLK_IO >= 0)
    {
        ESP_LOGI(TAG, "I2S initialized at %d Hz (MCLK on GPIO %d, 256xFs)",
                 SAMPLE_RATE, I2S_MCLK_IO);
    }
    else
    {
        ESP_LOGI(TAG, "I2S initialized at %d Hz (BCLK-only, codec PLL)",
                 SAMPLE_RATE);
    }

#if CONFIG_DTESP_DAC_TLV320
    // Initialize NVS so fw_settings can persist/restore codec state,
    // then load the stored firmware settings (with Kconfig fallbacks).
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_LOGW(TAG, "NVS partition needs erase (%s); reformatting",
                 esp_err_to_name(nvs_err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);
    ESP_ERROR_CHECK(fw_settings_init());

    // The TLV320DAC3100 is configured over I2C after BCLK/MCLK are running
    // so that its internal PLL (when used in BCLK-only mode) has a stable
    // reference clock to lock onto.
    ESP_ERROR_CHECK(tlv320_init());

    // Apply the NVS-backed settings now that the codec is ready.
    fw_settings_apply();

    // Start the optional analog volume knob after the codec has
    // its restored volume, so the pot uses that as its initial
    // soft-takeover reference.  No-op when the feature is disabled.
    esp_err_t knob_err = volume_knob_init();
    if (knob_err != ESP_OK)
    {
        ESP_LOGW(TAG, "volume_knob_init failed: %s",
                 esp_err_to_name(knob_err));
    }
#endif

    return ESP_OK;
}
