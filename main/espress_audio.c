// SPDX-License-Identifier: MIT
// Copyright (c) 2025 Leland Lucius
// ----------------------------------------------------------------
// Audio Subsystem Initialization
//
// Configures the I2S output channel and, when selected, the
// TLV320DAC3100 codec over I2C.  Separated from dectalk_espress.c
// to keep audio hardware concerns in one place.
// ----------------------------------------------------------------

#include "espress_audio.h"

#include "sdkconfig.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_log.h"

#if CONFIG_DECTALK_DAC_TLV320DAC3100
#include "tlv320dac3100.h"
#include "fw_settings.h"
#include "nvs_flash.h"
#endif

static const char *TAG = "audio";

#define SAMPLE_RATE              11025
#define I2S_BCK_IO               CONFIG_DECTALK_I2S_BCK_GPIO
#define I2S_WS_IO                CONFIG_DECTALK_I2S_WS_GPIO
#define I2S_DO_IO                CONFIG_DECTALK_I2S_DO_GPIO
#define I2S_MCLK_IO              CONFIG_DECTALK_I2S_MCLK_GPIO
#define I2S_DMA_DESC_NUM         CONFIG_DECTALK_I2S_DMA_DESC_NUM
#define I2S_DMA_FRAME_NUM        CONFIG_DECTALK_I2S_DMA_FRAME_NUM

static i2s_chan_handle_t s_audio_handle;

i2s_chan_handle_t espress_audio_get_handle(void)
{
    return s_audio_handle;
}

esp_err_t espress_audio_init(void)
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

#if CONFIG_DECTALK_DAC_TLV320DAC3100
    // Initialise NVS so fw_settings can persist/restore codec state,
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
    ESP_ERROR_CHECK(tlv320dac3100_init());

    // Apply the NVS-backed settings now that the codec is ready.
    fw_settings_apply();
#endif

    return ESP_OK;
}
