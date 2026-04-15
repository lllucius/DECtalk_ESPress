// SPDX-License-Identifier: MIT
// Copyright (c) 2025 Leland Lucius
// ----------------------------------------------------------------
// TLV320DAC3100 I2C initialisation for the Adafruit breakout board
//
// Configures the TI TLV320DAC3100 stereo DAC as an I2S slave with
// CODEC_CLKIN = BCLK.  No PLL or MCLK is used.  The class-D
// speaker amplifier is enabled by default; headphones are disabled.
// Headset detection is enabled so that periodic polling can switch
// between speaker and headphone outputs automatically.
//
// Register addresses and bit layouts are taken from the
// TLV320DAC3100 datasheet (SLAS833) and cross-referenced against
// the Adafruit TLV320_I2S Arduino library.
// ----------------------------------------------------------------

#include "sdkconfig.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tlv320dac3100.h"

static const char *TAG = "TLV320DAC3100";

#define TLV320_I2C_ADDR    CONFIG_DECTALK_TLV320_I2C_ADDR
#define I2C_SDA_IO         CONFIG_DECTALK_I2C_SDA_GPIO
#define I2C_SCL_IO         CONFIG_DECTALK_I2C_SCL_GPIO

// ---- Page 0 registers -------------------------------------------
#define REG_PAGE_SELECT    0x00
#define REG_RESET          0x01
#define REG_CLOCK_MUX      0x04  // CODEC_CLKIN source
#define REG_NDAC           0x0B  // NDAC divider
#define REG_MDAC           0x0C  // MDAC divider
#define REG_DOSR_MSB       0x0D  // DOSR upper byte
#define REG_DOSR_LSB       0x0E  // DOSR lower byte
#define REG_CODEC_IF       0x1B  // Audio interface control
#define REG_DAC_PRB        0x3C  // DAC processing block
#define REG_DAC_DATAPATH   0x3F  // DAC data-path setup
#define REG_DAC_VOL_CTRL   0x40  // DAC volume / mute control
#define REG_DAC_LVOL       0x41  // Left DAC digital volume
#define REG_DAC_RVOL       0x42  // Right DAC digital volume
#define REG_HEADSET_DETECT 0x43  // Headset detection configuration

// ---- Page 1 registers -------------------------------------------
#define REG_HP_DRIVERS     0x1F  // Headphone driver control
#define REG_SPK_AMP        0x20  // Class-D speaker amplifier
#define REG_OUT_ROUTING    0x23  // DAC output mixer routing
#define REG_HPL_VOL        0x24  // Analog vol to HPL
#define REG_HPR_VOL        0x25  // Analog vol to HPR
#define REG_SPK_VOL        0x26  // Analog vol to class-D SPK
#define REG_HPL_DRIVER     0x28  // HPL driver gain / mute
#define REG_HPR_DRIVER     0x29  // HPR driver gain / mute
#define REG_SPK_DRIVER     0x2A  // SPK driver gain / mute

// ---- Headset detection status (bits 6:5 of REG_HEADSET_DETECT) ---
#define HEADSET_NONE        0x00  // No headset detected
#define HEADSET_WITHOUT_MIC 0x01  // Headphone (no microphone)
#define HEADSET_WITH_MIC    0x03  // Headset with microphone

// ---- Default volume level ----------------------------------------
#define DEFAULT_VOLUME      5
#define MAX_VOLUME          9

// Volume table: maps level 0–9 to DAC digital volume register values.
// The register uses two's complement in 0.5 dB steps:
//   0x00 = 0 dB, 0xFE = -1 dB, 0xC0 = -32 dB, 0x81 = -63.5 dB
static const uint8_t vol_table[MAX_VOLUME + 1] =
{
    0x81,  // 0: -63.5 dB (near mute)
    0xC0,  // 1: -32 dB
    0xC8,  // 2: -28 dB
    0xD0,  // 3: -24 dB
    0xD8,  // 4: -20 dB
    0xE0,  // 5: -16 dB
    0xE8,  // 6: -12 dB
    0xF0,  // 7: -8 dB
    0xF8,  // 8: -4 dB
    0x00,  // 9:  0 dB
};

// ---- Register write pair -----------------------------------------
typedef struct { uint8_t reg; uint8_t val; } reg_val_t;

// Page 0: reset, clocking, audio interface, DAC data path.
//
// Clocking: CODEC_CLKIN = BCLK (no PLL, no MCLK).
//   BCLK = Fs × 32 (16-bit I2S standard, 2 slots)
//   NDAC = 1, MDAC = 1, DOSR = 32
//   → DAC_FS = BCLK / (NDAC × MDAC × DOSR) = Fs
static const reg_val_t page0_init[] =
{
    {REG_PAGE_SELECT,  0x00},  // Select Page 0

    // -- Clocking (BCLK, no PLL) ----------------------------------
    {REG_CLOCK_MUX,    0x01},  // CODEC_CLKIN = BCLK
    {REG_NDAC,         0x81},  // NDAC = 1, powered up
    {REG_MDAC,         0x81},  // MDAC = 1, powered up
    {REG_DOSR_MSB,     0x00},  // DOSR = 32 (0x0020)
    {REG_DOSR_LSB,     0x20},

    // -- Audio interface ------------------------------------------
    {REG_CODEC_IF,     0x00},  // I2S, 16-bit, BCLK+WCLK inputs
                               // (slave mode)

    // -- DAC data path --------------------------------------------
    {REG_DAC_PRB,      0x01},  // Processing block PRB_P1
    {REG_DAC_DATAPATH, 0xD8},  // L DAC on, R DAC on,
                               // L path = normal (left data),
                               // R path = swapped (left data)
                               //   → mono: both outputs play the
                               //     same audio from the L I2S slot;
                               // soft-step = 1 step/sample
    {REG_DAC_VOL_CTRL, 0x00},  // Both channels unmuted
    {REG_DAC_LVOL,     0xE0},  // Left digital vol  = -16 dB (level 5)
    {REG_DAC_RVOL,     0xE0},  // Right digital vol = -16 dB (level 5)

    // -- Headset detection ----------------------------------------
    // D7 = 1: enable, D4:D2 = 010: 64 ms debounce
    {REG_HEADSET_DETECT, 0x88},
};

// Page 1: headphone drivers, class-D speaker, output routing.
// Start with speaker ON and headphones OFF.  The headset detection
// polling will switch outputs when a headphone is inserted.
static const reg_val_t page1_init[] =
{
    {REG_PAGE_SELECT,  0x01},  // Select Page 1

    // -- Output drivers -------------------------------------------
    {REG_HP_DRIVERS,   0x04},  // HPL + HPR powered DOWN,
                               // common-mode = 1.35 V, de-pop on
    {REG_SPK_AMP,      0x86},  // Class-D speaker amp enabled

    // -- Mixer routing: DAC → mixer amp for both channels ---------
    {REG_OUT_ROUTING,  0x44},  // L DAC → mixer, R DAC → mixer

    // -- Analog volume to output drivers --------------------------
    {REG_HPL_VOL,      0x80},  // HPL routed, analog gain = 0 dB
    {REG_HPR_VOL,      0x80},  // HPR routed, analog gain = 0 dB
    {REG_SPK_VOL,      0x80},  // SPK routed, analog gain = 0 dB

    // -- Driver gain and unmute -----------------------------------
    {REG_HPL_DRIVER,   0x00},  // HPL: muted (headphones off)
    {REG_HPR_DRIVER,   0x00},  // HPR: muted (headphones off)
    {REG_SPK_DRIVER,   0x04},  // SPK: 6 dB class-D gain, unmuted
};

// ---- Module state ------------------------------------------------
static i2c_master_dev_handle_t s_dev;
static bool s_hp_active;    // true when headphone output is active
static uint8_t s_volume = DEFAULT_VOLUME;


static esp_err_t write_reg(i2c_master_dev_handle_t dev,
                           uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(dev, buf, sizeof(buf), -1);
}

static esp_err_t read_reg(i2c_master_dev_handle_t dev,
                          uint8_t reg, uint8_t *val)
{
    esp_err_t err = i2c_master_transmit(dev, &reg, 1, -1);
    if (err != ESP_OK)
        return err;
    return i2c_master_receive(dev, val, 1, -1);
}

static esp_err_t write_regs(i2c_master_dev_handle_t dev,
                            const reg_val_t *pairs, size_t count)
{
    for (size_t i = 0; i < count; i++)
    {
        esp_err_t err = write_reg(dev, pairs[i].reg, pairs[i].val);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "I2C write failed: reg 0x%02X val 0x%02X (%s)",
                     pairs[i].reg, pairs[i].val, esp_err_to_name(err));
            return err;
        }
    }
    return ESP_OK;
}

// Switch to speaker output (headphones off).
static void enable_speaker(void)
{
    // Page 1
    write_reg(s_dev, REG_PAGE_SELECT, 0x01);

    // Mute and power down headphones
    write_reg(s_dev, REG_HPL_DRIVER, 0x00);
    write_reg(s_dev, REG_HPR_DRIVER, 0x00);
    write_reg(s_dev, REG_HP_DRIVERS, 0x04);  // HPL+HPR off, de-pop on

    // Enable and unmute speaker
    write_reg(s_dev, REG_SPK_AMP, 0x86);
    write_reg(s_dev, REG_SPK_DRIVER, 0x04);  // 6 dB gain, unmuted

    // Return to page 0
    write_reg(s_dev, REG_PAGE_SELECT, 0x00);
}

// Switch to headphone output (speaker off).
static void enable_headphone(void)
{
    // Page 1
    write_reg(s_dev, REG_PAGE_SELECT, 0x01);

    // Mute and power down speaker
    write_reg(s_dev, REG_SPK_DRIVER, 0x00);
    write_reg(s_dev, REG_SPK_AMP, 0x06);     // Speaker amp off

    // Power up and unmute headphones
    write_reg(s_dev, REG_HP_DRIVERS, 0xC4);  // HPL+HPR on, de-pop on
    write_reg(s_dev, REG_HPL_DRIVER, 0x04);  // 0 dB gain, unmuted
    write_reg(s_dev, REG_HPR_DRIVER, 0x04);  // 0 dB gain, unmuted

    // Return to page 0
    write_reg(s_dev, REG_PAGE_SELECT, 0x00);
}


esp_err_t tlv320dac3100_init(void)
{
    esp_err_t err;

    ESP_LOGI(TAG, "Initializing TLV320DAC3100 (I2C addr 0x%02X)...",
             TLV320_I2C_ADDR);

    // ---- Create I2C master bus ----------------------------------
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA_IO,
        .scl_io_num = I2C_SCL_IO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    i2c_master_bus_handle_t bus;
    err = i2c_new_master_bus(&bus_cfg, &bus);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to create I2C bus: %s", esp_err_to_name(err));
        return err;
    }

    // ---- Attach TLV320DAC3100 device ----------------------------
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TLV320_I2C_ADDR,
        .scl_speed_hz = 100000,
    };

    err = i2c_master_bus_add_device(bus, &dev_cfg, &s_dev);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to add I2C device: %s", esp_err_to_name(err));
        return err;
    }

    // ---- Software reset -----------------------------------------
    err = write_reg(s_dev, REG_PAGE_SELECT, 0x00);
    if (err == ESP_OK)
        err = write_reg(s_dev, REG_RESET, 0x01);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Software reset failed: %s", esp_err_to_name(err));
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(10));

    // ---- Page 0 registers ---------------------------------------
    err = write_regs(s_dev, page0_init,
                     sizeof(page0_init) / sizeof(page0_init[0]));
    if (err != ESP_OK)
        return err;

    // ---- Page 1 registers ---------------------------------------
    err = write_regs(s_dev, page1_init,
                     sizeof(page1_init) / sizeof(page1_init[0]));
    if (err != ESP_OK)
        return err;

    // Return to Page 0 for normal operation
    err = write_reg(s_dev, REG_PAGE_SELECT, 0x00);
    if (err != ESP_OK)
        return err;

    s_hp_active = false;
    s_volume = DEFAULT_VOLUME;

    // Perform an initial headset check so we start in the correct mode
    tlv320dac3100_poll_headset();

    ESP_LOGI(TAG, "TLV320DAC3100 initialized successfully");
    return ESP_OK;
}


void tlv320dac3100_poll_headset(void)
{
    // Read headset detection status from bits 6:5 of REG_HEADSET_DETECT
    // (Page 0).
    uint8_t reg_val = 0;
    esp_err_t err = read_reg(s_dev, REG_HEADSET_DETECT, &reg_val);
    if (err != ESP_OK)
        return;

    uint8_t status = (reg_val >> 5) & 0x03;
    bool hp_detected = (status == HEADSET_WITHOUT_MIC ||
                        status == HEADSET_WITH_MIC);

    if (hp_detected && !s_hp_active)
    {
        ESP_LOGI(TAG, "Headphone inserted – switching to headphone output");
        enable_headphone();
        s_hp_active = true;
    }
    else if (!hp_detected && s_hp_active)
    {
        ESP_LOGI(TAG, "Headphone removed – switching to speaker output");
        enable_speaker();
        s_hp_active = false;
    }
}


void tlv320dac3100_set_volume(uint8_t level)
{
    if (level > MAX_VOLUME)
        level = MAX_VOLUME;

    s_volume = level;
    uint8_t reg_val = vol_table[level];

    // Both channels get the same digital volume (page 0)
    write_reg(s_dev, REG_DAC_LVOL, reg_val);
    write_reg(s_dev, REG_DAC_RVOL, reg_val);

    ESP_LOGI(TAG, "Volume set to %u (reg 0x%02X)", level, reg_val);
}


uint8_t tlv320dac3100_get_volume(void)
{
    return s_volume;
}
