// SPDX-License-Identifier: MIT
// Copyright (c) 2025 Leland Lucius
// ----------------------------------------------------------------
// TLV320DAC3100 I2C initialisation for the Adafruit breakout board
//
// Configures the TI TLV320DAC3100 stereo DAC as an I2S slave.  When a
// master clock GPIO is configured (ESPRESS_I2S_MCLK_GPIO >= 0) the
// codec uses CODEC_CLKIN = MCLK directly; otherwise the codec's internal
// PLL is locked onto BCLK.  Startup profile, startup volume, optional
// reset GPIO, and optional codec-event handling are controlled by
// Kconfig.
//
// Register addresses and bit layouts are taken from the TLV320DAC3100
// datasheet (SLAS833) and cross-referenced against the Adafruit
// TLV320_I2S Arduino library.
// ----------------------------------------------------------------

#include "sdkconfig.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <stdint.h>
#include "tlv320dac3100.h"

static const char *TAG = "TLV320DAC3100";

#define TLV320_I2C_ADDR     CONFIG_ESPRESS_TLV320_I2C_ADDR
#define I2C_SDA_IO          CONFIG_ESPRESS_I2C_SDA_GPIO
#define I2C_SCL_IO          CONFIG_ESPRESS_I2C_SCL_GPIO
#define CODEC_INT_GPIO      CONFIG_ESPRESS_CODEC_INT_GPIO
#define CODEC_RESET_GPIO    CONFIG_ESPRESS_CODEC_RESET_GPIO
#define CODEC_MCLK_GPIO     CONFIG_ESPRESS_I2S_MCLK_GPIO
#define TLV320_I2C_TIMEOUT_MS 50

// ---- Page 0 registers -------------------------------------------
#define REG_PAGE_SELECT     0x00
#define REG_RESET           0x01
#define REG_CLOCK_MUX       0x04    // CODEC_CLKIN / PLL_CLKIN source
#define REG_PLL_P_R         0x05    // PLL power / P / R dividers
#define REG_PLL_J           0x06    // PLL J multiplier
#define REG_PLL_D_MSB       0x07    // PLL D fractional (MSB)
#define REG_PLL_D_LSB       0x08    // PLL D fractional (LSB)
#define REG_NDAC            0x0B    // NDAC divider
#define REG_MDAC            0x0C    // MDAC divider
#define REG_DOSR_MSB        0x0D    // DOSR upper byte
#define REG_DOSR_LSB        0x0E    // DOSR lower byte
#define REG_CODEC_IF        0x1B    // Audio interface control
#define REG_OVERFLOW_FLAGS  0x27    // DAC overflow flags
#define REG_DAC_INT_FLAGS   0x2C    // Sticky interrupt flags
#define REG_DAC_INT_STATUS  0x2E    // Current interrupt status
#define REG_INT1_CTRL       0x30    // INT1 routing / pulse mode
#define REG_INT2_CTRL       0x31    // INT2 routing / pulse mode
#define REG_GPIO1_CTRL      0x33    // GPIO1 control / INT1 output mux
#define REG_DAC_PRB         0x3C    // DAC processing block
#define REG_DAC_DATAPATH    0x3F    // DAC data-path setup
#define REG_DAC_VOL_CTRL    0x40    // DAC volume / mute control
#define REG_DAC_LVOL        0x41    // Left DAC digital volume
#define REG_DAC_RVOL        0x42    // Right DAC digital volume
#define REG_HEADSET_DETECT  0x43    // Headset detection configuration

// ---- Page 1 registers -------------------------------------------
#define REG_HP_DRIVERS      0x1F    // Headphone driver control
#define REG_SPK_AMP         0x20    // Class-D speaker amplifier
#define REG_OUT_ROUTING     0x23    // DAC output mixer routing
#define REG_HPL_VOL         0x24    // Analog vol to HPL
#define REG_HPR_VOL         0x25    // Analog vol to HPR
#define REG_SPK_VOL         0x26    // Analog vol to class-D SPK
#define REG_HPL_DRIVER      0x28    // HPL driver gain / mute
#define REG_HPR_DRIVER      0x29    // HPR driver gain / mute
#define REG_SPK_DRIVER      0x2A    // SPK driver gain / mute

// ---- Headset detection status (bits 6:5 of REG_HEADSET_DETECT) ---
#define HEADSET_NONE        0x00    // No headset detected
#define HEADSET_WITHOUT_MIC 0x01    // Headphone (no microphone)
#define HEADSET_WITH_MIC    0x03    // Headset with microphone

#define TLV320_BIT(n)               ((uint8_t)(1U << (n)))
#define TLV320_INT_HEADSET          TLV320_BIT(7)
#define TLV320_INT_BUTTON           TLV320_BIT(6)
#define TLV320_INT_DRC              TLV320_BIT(5)
#define TLV320_INT_SHORT_CIRCUIT    TLV320_BIT(3)
#define TLV320_INT_DAC_OVERFLOW     TLV320_BIT(2)
#define TLV320_INT_REPEAT           TLV320_BIT(0)

#define TLV320_IRQ_SHORT_HPL        TLV320_BIT(7)
#define TLV320_IRQ_SHORT_HPR        TLV320_BIT(6)
#define TLV320_IRQ_BUTTON           TLV320_BIT(5)
#define TLV320_IRQ_HEADSET          TLV320_BIT(4)
#define TLV320_IRQ_LEFT_DRC         TLV320_BIT(3)
#define TLV320_IRQ_RIGHT_DRC        TLV320_BIT(2)

#define TLV320_OVERFLOW_LEFT_DAC    TLV320_BIT(1)
#define TLV320_OVERFLOW_RIGHT_DAC   TLV320_BIT(0)

#define TLV320_HEADSET_ENABLE        TLV320_BIT(7)
#define TLV320_HEADSET_DEBOUNCE_64MS (2U << 2)
#define TLV320_HEADSET_STATUS_SHIFT  5
#define TLV320_HEADSET_STATUS_MASK   0x03

#define TLV320_GPIO1_MODE_SHIFT     2
#define TLV320_GPIO1_MODE_DISABLED  (0x0U << TLV320_GPIO1_MODE_SHIFT)
#define TLV320_GPIO1_MODE_INT1      (0x5U << TLV320_GPIO1_MODE_SHIFT)

// ---- Default runtime settings ------------------------------------
#define MAX_VOLUME                  TLV320DAC3100_MAX_VOLUME
#define TLV320_MUTED_REG_VALUE      0x81
#define TLV320_INVALID_PAGE         0xFF
#define TLV320_HP_DRIVERS_DEFAULT_MODE  0xC4  // HPL/HPR powered, 1.35 V CM
#define TLV320_HP_DRIVER_GAIN_0DB_UNMUTED 0x04 // 0 dB gain + unmute (safe default)
#define TLV320_EVENT_QUEUE_LEN      8
#define TLV320_EVENT_TASK_STACK     3072
#define TLV320_EVENT_IRQ            TLV320_BIT(0)
#define TLV320_EVENT_POLL           TLV320_BIT(1)
#define TLV320_HEADSET_POLL_US      500000ULL
#define TLV320_RESET_ASSERT_MS      1
#define TLV320_RESET_SETTLE_MS      10
#define TLV320_STARTUP_VOLUME_LEVEL CONFIG_ESPRESS_TLV320_STARTUP_VOLUME
// Use conservative CM=1.35V and 0 dB HP driver gain by default; the previous
// 1.50V + 6 dB diagnostic combination risked clipping the headphone output
// with sensitive / low-impedance phones and contributed to audible distortion.
#define TLV320_HP_DRIVERS_HEADPHONE_VALUE TLV320_HP_DRIVERS_DEFAULT_MODE
#define TLV320_HP_DRIVER_GAIN_VALUE       TLV320_HP_DRIVER_GAIN_0DB_UNMUTED
// Mono TTS is duplicated on both left and right DACs so that both earcups /
// speakers reproduce the same signal.  0xD0 would power both DACs but leave
// the right DAC's data path silenced.
#define TLV320_DAC_DATAPATH_VALUE         0xD8  // LDAC<-L, RDAC<-L, soft-step on
#define TLV320_DAC_DATAPATH_MODE_LOG      "mono-left duplicated to L+R"

// Volume table: maps level 0–9 to DAC digital volume register values.
// The register uses two's complement in 0.5 dB steps:
//   0x00 = 0 dB, 0xFE = -1 dB, 0xC0 = -32 dB, 0x81 = -63.5 dB
static const uint8_t vol_table[MAX_VOLUME + 1] =
{
    0x81,   // 0: -63.5 dB (near mute)
    0xC0,   // 1: -32 dB
    0xC8,   // 2: -28 dB
    0xD0,   // 3: -24 dB
    0xD8,   // 4: -20 dB
    0xE0,   // 5: -16 dB
    0xE8,   // 6: -12 dB
    0xF0,   // 7: -8 dB
    0xF8,   // 8: -4 dB
    0x00,   // 9:  0 dB
};

static const float vol_db_table[MAX_VOLUME + 1] =
{
    -60.0f,
    -32.0f,
    -28.0f,
    -24.0f,
    -20.0f,
    -16.0f,
    -12.0f,
    -8.0f,
    -4.0f,
    0.0f,
};

// ---- Register write pair -----------------------------------------
typedef struct
{
    uint8_t reg;
    uint8_t val;
} reg_val_t;

// Page 0 phase: configure clocking.
//
// Two modes are supported, selected via Kconfig (ESPRESS_I2S_MCLK_GPIO):
//
//   * MCLK mode (CODEC_MCLK_GPIO >= 0): the ESP32 drives MCLK at 256xFs.
//     CODEC_CLKIN = MCLK, NDAC=1, MDAC=1, DOSR=256 gives DAC_MOD_CLK =
//     MCLK = 2.8224 MHz and DAC_FS = Fs = 11025 Hz.  This keeps
//     DAC_MOD_CLK inside the codec's ~2.8-6.758 MHz valid range.
//
//   * BCLK-only mode (CODEC_MCLK_GPIO < 0): CODEC_CLKIN is driven from the
//     codec's internal PLL, which is clocked from BCLK.  At Fs=11025 and
//     16-bit stereo I2S, BCLK = 2*16*Fs = 352.8 kHz, which is far below the
//     codec's ~2.8 MHz minimum DAC_MOD_CLK, so using BCLK directly leaves
//     the DAC modulator out of spec.  The PLL multiplies BCLK by
//     R * (J + D/10000) / P = 4 * 62.5 / 1 = 250, giving PLL_CLK = 88.2 MHz.
//     With NDAC=2, MDAC=8, DOSR=500, DAC_MOD_CLK = 88.2 MHz / 16 = 5.5125
//     MHz (in spec) and DAC_FS = 88.2 MHz / (2*8*500) = 11025 Hz.
//
// REG_CLOCK_MUX (0x04) bit layout (per TLV320DAC3100 datasheet and the
// Adafruit_TLV320_I2S reference library):
//   bits 3:2 = PLL_CLKIN source (00=MCLK, 01=BCLK, 10=GPIO1, 11=DIN)
//   bits 1:0 = CODEC_CLKIN source (00=MCLK, 01=BCLK, 10=GPIO1, 11=PLL_CLK)
#if CODEC_MCLK_GPIO >= 0
static const reg_val_t clocking_init[] =
{
    {REG_CLOCK_MUX,    0x00},   // CODEC_CLKIN = MCLK, PLL off
    {REG_NDAC,         0x81},   // NDAC = 1, powered up
    {REG_MDAC,         0x81},   // MDAC = 1, powered up
    {REG_DOSR_MSB,     0x01},   // DOSR = 256 (0x0100)
    {REG_DOSR_LSB,     0x00},
};
#define TLV320_CLOCK_MODE_LOG "MCLK (256xFs, no PLL)"
#else
static const reg_val_t clocking_init[] =
{
    // PLL_CLKIN = BCLK (01 at bits 3:2), CODEC_CLKIN = PLL_CLK (11 at bits
    // 1:0).  Combined value: (01 << 2) | (11 << 0) = 0b0000_0111 = 0x07.
    {REG_CLOCK_MUX,    0x07},
    // PLL: power on, P = 1, R = 4.
    //   Bit 7    = 1  (PLL enable)
    //   Bits 6:4 = P, literal 1..7 with 000 meaning 8; here 001 = 1
    //   Bits 3:0 = R, literal 1..15 with 0000 meaning 16; here 0100 = 4
    {REG_PLL_P_R,      0x94},   // 0b1_001_0100
    {REG_PLL_J,        62},     // J = 62 (range 1..63)
    // D is a 14-bit fractional (0..9999) split across two registers.
    // For J.D = 62.5000, D = 5000 = 0x1388.
    //   REG_PLL_D_MSB holds D[13:8] in bits 5:0  -> 0x13
    //   REG_PLL_D_LSB holds D[7:0]               -> 0x88
    {REG_PLL_D_MSB,    0x13},
    {REG_PLL_D_LSB,    0x88},
    {REG_NDAC,         0x82},   // NDAC = 2, powered up
    {REG_MDAC,         0x88},   // MDAC = 8, powered up
    {REG_DOSR_MSB,     0x01},   // DOSR = 500 (0x01F4)
    {REG_DOSR_LSB,     0xF4},
};
#define TLV320_CLOCK_MODE_LOG "PLL from BCLK (~88.2 MHz CODEC_CLKIN)"
#endif

// Page 0 phase: configure the I2S data interface.
static const reg_val_t audio_interface_init[] =
{
    {REG_CODEC_IF,     0x00},   // I2S, 16-bit, BCLK+WCLK inputs
                                // (slave mode)
};

// Page 0 phase: select a simple DAC processing block and DAC data path.
static const reg_val_t dac_processing_init[] =
{
    {REG_DAC_PRB,      0x01},   // Processing block PRB_P1
    {REG_DAC_DATAPATH, TLV320_DAC_DATAPATH_VALUE},
                                // 0xD8: LDAC powered + fed from left I2S
                                // slot, RDAC powered + also fed from left
                                // slot, so mono TTS audio appears on both
                                // analog outputs (left earcup and right
                                // earcup / both speakers).
    {REG_DAC_VOL_CTRL, 0x00},   // Leave volume control in its normal mode
};

// Page 0 phase: enable headset detection for profile auto-switching.
static const reg_val_t headset_detect_init[] =
{
    {REG_HEADSET_DETECT, TLV320_HEADSET_ENABLE | TLV320_HEADSET_DEBOUNCE_64MS},
};

static const reg_val_t interrupt_routing_init[] =
{
    {REG_INT1_CTRL, TLV320_INT_SHORT_CIRCUIT | TLV320_INT_DAC_OVERFLOW | TLV320_INT_REPEAT},
    {REG_INT2_CTRL, TLV320_INT_HEADSET | TLV320_INT_BUTTON},
};

// Page 1 phase: route DAC outputs to the analog mixer paths.
static const reg_val_t output_routing_init[] =
{
    {REG_OUT_ROUTING,  0x44},   // L DAC → mixer, R DAC → mixer
};

// Page 1 phase: power up analog drivers in a safe muted baseline state.
static const reg_val_t analog_driver_init[] =
{
    {REG_HP_DRIVERS,   0x04},   // HPL + HPR powered DOWN,
                                // common-mode = 1.35 V, de-pop on
    {REG_SPK_AMP,      0x86},   // Class-D speaker amp enabled
    {REG_HPL_VOL,      0x80},   // HPL routed, analog gain = 0 dB
    {REG_HPR_VOL,      0x80},   // HPR routed, analog gain = 0 dB
    {REG_SPK_VOL,      0x80},   // SPK routed, analog gain = 0 dB
    {REG_HPL_DRIVER,   0x00},   // HPL: muted (headphones off)
    {REG_HPR_DRIVER,   0x00},   // HPR: muted (headphones off)
    {REG_SPK_DRIVER,   0x04},   // SPK: 6 dB class-D gain, unmuted
};

// ---- Module state ------------------------------------------------
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static uint8_t s_current_page = TLV320_INVALID_PAGE;
static bool s_hp_active; // true when headphone output is active
static bool s_headset_present;
static uint8_t s_volume = TLV320_STARTUP_VOLUME_LEVEL;
static float s_volume_db = 0.0f; // initialised from vol_db_table in tlv320_reset_state
static uint8_t s_digital_volume_reg = 0; // initialised from vol_table in tlv320_reset_state
static bool s_muted = true;
static tlv320_profile_t s_profile = TLV320_PROFILE_SPEAKER;
static bool s_apply_profile_volume_default;
static QueueHandle_t s_event_queue = NULL;
static TaskHandle_t s_event_task = NULL;
static esp_timer_handle_t s_poll_timer = NULL;
static bool s_irq_handler_registered = false;
static int s_irq_gpio = -1;
#if CONFIG_ESPRESS_TLV320_HEADSET_AUTOSWITCH
static bool s_autoswitch = true;
#else
static bool s_autoswitch = false;
#endif


static tlv320_profile_t tlv320_get_default_profile(void)
{
#if CONFIG_ESPRESS_TLV320_DEFAULT_PROFILE_HEADPHONE
    return TLV320_PROFILE_HEADPHONE;
#else
    return TLV320_PROFILE_SPEAKER;
#endif
}

static bool tlv320_headset_events_enabled(void)
{
#if CONFIG_ESPRESS_TLV320_HEADSET_AUTOSWITCH
    return true;
#else
    return CODEC_INT_GPIO >= 0;
#endif
}

static uint8_t tlv320_get_headset_status(uint8_t reg_val)
{
    return (uint8_t)((reg_val >> TLV320_HEADSET_STATUS_SHIFT) & TLV320_HEADSET_STATUS_MASK);
}

static bool tlv320_headset_present_from_reg(uint8_t reg_val)
{
    uint8_t status = tlv320_get_headset_status(reg_val);

    return status == HEADSET_WITHOUT_MIC || status == HEADSET_WITH_MIC;
}

static void IRAM_ATTR tlv320_codec_gpio_isr(void *arg)
{
    if (s_event_queue == NULL)
    {
        return;
    }

    uint32_t event = TLV320_EVENT_IRQ;
    BaseType_t higher_priority_task_woken = pdFALSE;
    xQueueSendFromISR(s_event_queue, &event, &higher_priority_task_woken);
    if (higher_priority_task_woken == pdTRUE)
    {
        portYIELD_FROM_ISR();
    }
}

static void tlv320_codec_poll_timer_cb(void *arg)
{
    if (s_event_queue == NULL)
    {
        return;
    }

    uint32_t event = TLV320_EVENT_POLL;
    (void)xQueueSend(s_event_queue, &event, 0);
}


static esp_err_t write_reg_raw(i2c_master_dev_handle_t dev,
                               uint8_t reg,
                                uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(dev, buf, sizeof(buf), TLV320_I2C_TIMEOUT_MS);
}

static esp_err_t read_reg_raw(i2c_master_dev_handle_t dev,
                              uint8_t reg,
                              uint8_t *val)
{
    esp_err_t err = i2c_master_transmit(dev, &reg, 1, TLV320_I2C_TIMEOUT_MS);
    if (err != ESP_OK)
    {
        return err;
    }

    return i2c_master_receive(dev, val, 1, TLV320_I2C_TIMEOUT_MS);
}

static esp_err_t select_page(uint8_t page)
{
    if (s_current_page == page)
    {
        return ESP_OK;
    }

    esp_err_t err = write_reg_raw(s_dev, REG_PAGE_SELECT, page);
    if (err == ESP_OK)
    {
        s_current_page = page;
    }

    return err;
}

// Invalidate the page-select cache.  Call this after any operation that
// resets the codec's internal page state (software reset register write,
// hardware reset pulse) so the next select_page() always issues an I2C
// write regardless of the cached value.
static inline void invalidate_page_cache(void)
{
    s_current_page = TLV320_INVALID_PAGE;
}

static esp_err_t write_reg(uint8_t page, uint8_t reg, uint8_t val)
{
    esp_err_t err = select_page(page);
    if (err != ESP_OK)
    {
        return err;
    }

    return write_reg_raw(s_dev, reg, val);
}

static esp_err_t read_reg(uint8_t page, uint8_t reg, uint8_t *val)
{
    esp_err_t err = select_page(page);
    if (err != ESP_OK)
    {
        return err;
    }

    return read_reg_raw(s_dev, reg, val);
}

static esp_err_t write_regs(uint8_t page, const reg_val_t *pairs, size_t count)
{
    esp_err_t err = select_page(page);
    if (err != ESP_OK)
    {
        return err;
    }

    for (size_t i = 0; i < count; i++)
    {
        err = write_reg_raw(s_dev, pairs[i].reg, pairs[i].val);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "I2C write failed: page 0x%02X reg 0x%02X val 0x%02X (%s)",
                     page,
                     pairs[i].reg,
                     pairs[i].val,
                     esp_err_to_name(err));
            return err;
        }
    }

    return ESP_OK;
}

static esp_err_t tlv320_gpio1_set_mode(uint8_t mode)
{
    return write_reg(0x00, REG_GPIO1_CTRL, mode);
}

static esp_err_t tlv320_codec_hardware_reset(void)
{
    if (CODEC_RESET_GPIO < 0)
    {
        return ESP_OK;
    }

    gpio_config_t reset_cfg = {
        .pin_bit_mask = 1ULL << CODEC_RESET_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&reset_cfg);
    if (err != ESP_OK)
    {
        return err;
    }

    // /RESET is active low: hold the line inactive-high, pulse it low,
    // then return it high before I2C traffic begins.
    err = gpio_set_level(CODEC_RESET_GPIO, 1);
    if (err != ESP_OK)
    {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(TLV320_RESET_ASSERT_MS));
    err = gpio_set_level(CODEC_RESET_GPIO, 0);
    if (err != ESP_OK)
    {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(TLV320_RESET_ASSERT_MS));
    err = gpio_set_level(CODEC_RESET_GPIO, 1);
    if (err != ESP_OK)
    {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(TLV320_RESET_SETTLE_MS));

    return ESP_OK;
}

static void tlv320_reset_state(void)
{
    tlv320_profile_t default_profile = tlv320_get_default_profile();

    invalidate_page_cache();
    s_hp_active = (default_profile == TLV320_PROFILE_HEADPHONE);
    s_headset_present = false;
    s_volume = CONFIG_ESPRESS_TLV320_STARTUP_VOLUME;
    s_volume_db = vol_db_table[CONFIG_ESPRESS_TLV320_STARTUP_VOLUME];
    s_digital_volume_reg = vol_table[CONFIG_ESPRESS_TLV320_STARTUP_VOLUME];
    s_muted = true;
    s_profile = default_profile;
    s_apply_profile_volume_default = false;
}

static void tlv320_release_i2c_resources(void)
{
    if (s_dev != NULL)
    {
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
    }

    if (s_bus != NULL)
    {
        i2c_del_master_bus(s_bus);
        s_bus = NULL;
    }

    invalidate_page_cache();
    s_headset_present = false;
}

// Combined "release I2C handles + reset all codec driver state + invalidate
// the page-select cache".  Used on both hardware reset and cleanup paths.
static void tlv320_full_reset(void)
{
    tlv320_release_i2c_resources();
    tlv320_reset_state();
}

static esp_err_t tlv320_handle_headset_status(uint8_t headset_reg, bool headset_irq_seen)
{
    bool headset_present = tlv320_headset_present_from_reg(headset_reg);
    esp_err_t err = ESP_OK;

    if (headset_present != s_headset_present)
    {
        ESP_LOGI(TAG, "%s",
                 headset_present
                    ? "Headset inserted"
                    : "Headset removed");
        s_headset_present = headset_present;
    }
    else if (!headset_irq_seen)
    {
        return ESP_OK;
    }

    // Honor the runtime s_autoswitch flag (initialised from Kconfig,
    // but switchable via [:fw autoswitch ...]).  When disabled,
    // headset insertion/removal only updates the presence flag.
    if (s_autoswitch && headset_present && !s_hp_active)
    {
        err = tlv320dac3100_set_profile(TLV320_PROFILE_HEADPHONE);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to switch to headphone profile: %s",
                     esp_err_to_name(err));
        }
    }
    else if (s_autoswitch && !headset_present && s_hp_active)
    {
        err = tlv320dac3100_set_profile(TLV320_PROFILE_SPEAKER);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to switch to speaker profile: %s",
                     esp_err_to_name(err));
        }
    }

    return err;
}

static esp_err_t tlv320_service_interrupts(void)
{
    uint8_t interrupt_flags = 0;
    uint8_t interrupt_status = 0;
    uint8_t overflow_flags = 0;
    uint8_t headset_reg = 0;
    esp_err_t err = read_reg(0x00, REG_DAC_INT_FLAGS, &interrupt_flags);
    if (err != ESP_OK)
    {
        return err;
    }

    err = read_reg(0x00, REG_DAC_INT_STATUS, &interrupt_status);
    if (err != ESP_OK)
    {
        return err;
    }

    err = read_reg(0x00, REG_OVERFLOW_FLAGS, &overflow_flags);
    if (err != ESP_OK)
    {
        return err;
    }

    err = read_reg(0x00, REG_HEADSET_DETECT, &headset_reg);
    if (err != ESP_OK)
    {
        return err;
    }

    // Read the current interrupt-status mirror after the sticky flags so the
    // deferred handler follows the codec's read-to-observe register flow even
    // when only the sticky flags drive the software decisions today.
    (void)interrupt_status;

    if ((interrupt_flags & (TLV320_IRQ_SHORT_HPL | TLV320_IRQ_SHORT_HPR)) != 0)
    {
        ESP_LOGE(TAG, "Output short circuit detected (flags=0x%02X)", interrupt_flags);
    }

    if ((overflow_flags & (TLV320_OVERFLOW_LEFT_DAC | TLV320_OVERFLOW_RIGHT_DAC)) != 0)
    {
        ESP_LOGE(TAG, "DAC overflow detected (flags=0x%02X)", overflow_flags);
    }

    bool headset_status_changed =
        tlv320_headset_present_from_reg(headset_reg) != s_headset_present;

    if ((interrupt_flags & TLV320_IRQ_HEADSET) != 0 || headset_status_changed)
    {
        err = tlv320_handle_headset_status(headset_reg,
                                           (interrupt_flags & TLV320_IRQ_HEADSET) != 0);
        if (err != ESP_OK)
        {
            return err;
        }
    }

    if ((interrupt_flags & TLV320_IRQ_BUTTON) != 0)
    {
        ESP_LOGI(TAG, "Headset button press detected");
    }

    return ESP_OK;
}

static void tlv320_codec_event_task(void *arg)
{
    uint32_t event = 0;

    while (xQueueReceive(s_event_queue, &event, portMAX_DELAY) == pdTRUE)
    {
        if ((event & (TLV320_EVENT_IRQ | TLV320_EVENT_POLL)) == 0)
        {
            continue;
        }

        if (s_dev == NULL)
        {
            continue;
        }

        esp_err_t err = tlv320_service_interrupts();
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "Codec event handling failed: %s", esp_err_to_name(err));
        }
    }
}

static esp_err_t tlv320_start_event_handling(void)
{
    if (s_event_queue == NULL)
    {
        s_event_queue = xQueueCreate(TLV320_EVENT_QUEUE_LEN, sizeof(uint32_t));
        if (s_event_queue == NULL)
        {
            return ESP_ERR_NO_MEM;
        }
    }

    if (s_event_task == NULL)
    {
        BaseType_t task_ok = xTaskCreate(tlv320_codec_event_task,
                                         "tlv320_evt",
                                         TLV320_EVENT_TASK_STACK,
                                         NULL,
                                         tskIDLE_PRIORITY + 1,
                                         &s_event_task);
        if (task_ok != pdPASS)
        {
            return ESP_ERR_NO_MEM;
        }
    }

    if (s_irq_handler_registered && CODEC_INT_GPIO < 0 && s_irq_gpio >= 0)
    {
        esp_err_t err = gpio_isr_handler_remove(s_irq_gpio);
        if (err != ESP_OK)
        {
            return err;
        }
        s_irq_handler_registered = false;
        s_irq_gpio = -1;
    }

    if (CODEC_INT_GPIO >= 0 && !s_irq_handler_registered)
    {
        int irq_gpio = CODEC_INT_GPIO;
        gpio_config_t irq_cfg = {
            .pin_bit_mask = 1ULL << irq_gpio,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_NEGEDGE,
        };

        esp_err_t err = gpio_config(&irq_cfg);
        if (err != ESP_OK)
        {
            return err;
        }

        err = gpio_install_isr_service(0);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
        {
            return err;
        }

        err = gpio_isr_handler_add(irq_gpio, tlv320_codec_gpio_isr, NULL);
        if (err != ESP_OK)
        {
            return err;
        }
        s_irq_handler_registered = true;
        s_irq_gpio = irq_gpio;
    }

    if (tlv320_headset_events_enabled() && s_poll_timer == NULL)
    {
        const esp_timer_create_args_t poll_timer_args = {
            .callback = tlv320_codec_poll_timer_cb,
            .name = "tlv320_poll",
        };
        esp_err_t err = esp_timer_create(&poll_timer_args, &s_poll_timer);
        if (err != ESP_OK)
        {
            return err;
        }
    }

    if (s_poll_timer != NULL && !esp_timer_is_active(s_poll_timer))
    {
        esp_err_t err = esp_timer_start_periodic(s_poll_timer, TLV320_HEADSET_POLL_US);
        if (err != ESP_OK)
        {
            return err;
        }
    }

    return ESP_OK;
}

static esp_err_t write_digital_volume(uint8_t reg_val)
{
    esp_err_t err = write_reg(0x00, REG_DAC_LVOL, reg_val);
    if (err != ESP_OK)
    {
        return err;
    }

    return write_reg(0x00, REG_DAC_RVOL, reg_val);
}

static uint8_t get_effective_volume_reg(void)
{
    return s_muted
        ? TLV320_MUTED_REG_VALUE
        : s_digital_volume_reg;
}

static esp_err_t configure_profile_outputs(tlv320_profile_t profile)
{
    static const reg_val_t speaker_output_cfg[] =
    {
        {REG_HP_DRIVERS, 0x04},
        {REG_HPL_DRIVER, 0x00},
        {REG_HPR_DRIVER, 0x00},
        {REG_SPK_AMP,    0x86},
        {REG_SPK_DRIVER, 0x04},
    };

    // Conservative headphone bring-up: HP drivers powered with 1.35V CM and
    // analog gain left at 0 dB.  Digital volume controls loudness; avoiding
    // the earlier +6 dB analog boost prevents audible clipping / distortion
    // with sensitive or low-impedance headphones.
    static const reg_val_t headphone_output_cfg[] =
    {
        {REG_SPK_DRIVER, 0x00},
        {REG_SPK_AMP,    0x06},
        {REG_HP_DRIVERS, TLV320_HP_DRIVERS_HEADPHONE_VALUE},
        {REG_HPL_DRIVER, TLV320_HP_DRIVER_GAIN_VALUE},
        {REG_HPR_DRIVER, TLV320_HP_DRIVER_GAIN_VALUE},
    };

    const reg_val_t *cfg = (profile == TLV320_PROFILE_HEADPHONE)
        ? headphone_output_cfg
        : speaker_output_cfg;
    size_t count = (profile == TLV320_PROFILE_HEADPHONE)
        ? sizeof(headphone_output_cfg) / sizeof(headphone_output_cfg[0])
        : sizeof(speaker_output_cfg) / sizeof(speaker_output_cfg[0]);

    return write_regs(0x01, cfg, count);
}

static esp_err_t tlv320_apply_gain_defaults(tlv320_profile_t profile,
                                            bool apply_startup_volume)
{
    // Both profiles share the same conservative 0 dB analog gain defaults;
    // loudness is controlled by digital volume.
    static const reg_val_t analog_gain_cfg[] =
    {
        {REG_HPL_VOL, 0x80},   // HPL routed, analog gain = 0 dB
        {REG_HPR_VOL, 0x80},   // HPR routed, analog gain = 0 dB
        {REG_SPK_VOL, 0x80},   // SPK routed, analog gain = 0 dB
    };

    (void)profile;

    esp_err_t err = write_regs(0x01,
                               analog_gain_cfg,
                               sizeof(analog_gain_cfg) / sizeof(analog_gain_cfg[0]));
    if (err != ESP_OK)
    {
        return err;
    }

    if (!apply_startup_volume)
    {
        return ESP_OK;
    }

    return write_digital_volume(s_digital_volume_reg);
}


esp_err_t tlv320dac3100_init(void)
{
    esp_err_t err;
    tlv320_profile_t default_profile = tlv320_get_default_profile();

    ESP_LOGI(TAG, "Initializing TLV320DAC3100 (I2C addr 0x%02X)...",
             TLV320_I2C_ADDR);
    ESP_LOGI(TAG, "Clocking: %s", TLV320_CLOCK_MODE_LOG);
    ESP_LOGI(TAG, "DAC path: %s", TLV320_DAC_DATAPATH_MODE_LOG);

    // ---- Create I2C master bus ----------------------------------
    i2c_master_bus_config_t bus_cfg =
    {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA_IO,
        .scl_io_num = I2C_SCL_IO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    tlv320_full_reset();

    err = tlv320_codec_hardware_reset();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Hardware reset failed: %s", esp_err_to_name(err));
        return err;
    }

    err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to create I2C bus: %s", esp_err_to_name(err));
        return err;
    }

    // ---- Attach TLV320DAC3100 device ----------------------------
    i2c_device_config_t dev_cfg =
    {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TLV320_I2C_ADDR,
        .scl_speed_hz = 100000,
    };

    err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to add I2C device: %s", esp_err_to_name(err));
        tlv320_release_i2c_resources();
        return err;
    }

    // ---- Phase 1: reset device ----------------------------------
    err = select_page(0x00);
    if (err == ESP_OK)
    {
        err = write_reg(0x00, REG_RESET, 0x01);
    }
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Software reset failed: %s", esp_err_to_name(err));
        goto init_fail;
    }
    vTaskDelay(pdMS_TO_TICKS(TLV320_RESET_SETTLE_MS));

    // The software reset restores the codec's page-select state, so clear
    // the cached page before continuing with normal register programming.
    invalidate_page_cache();

    // ---- Phase 2: configure clocks / PLL path -------------------
    err = write_regs(0x00,
                     clocking_init,
                     sizeof(clocking_init) / sizeof(clocking_init[0]));
    if (err != ESP_OK)
    {
        goto init_fail;
    }

#if CODEC_MCLK_GPIO < 0
    // Give the internal PLL time to lock onto BCLK before we start
    // programming the DAC datapath.  Datasheet typical lock time is a few
    // ms; 15 ms is a comfortable margin.
    vTaskDelay(pdMS_TO_TICKS(15));
#endif

    // ---- Phase 3: configure the audio interface -----------------
    err = write_regs(0x00,
                     audio_interface_init,
                     sizeof(audio_interface_init) / sizeof(audio_interface_init[0]));
    if (err != ESP_OK)
    {
        goto init_fail;
    }

    // ---- Phase 4: configure DAC processing block ----------------
    err = write_regs(0x00,
                     dac_processing_init,
                     sizeof(dac_processing_init) / sizeof(dac_processing_init[0]));
    if (err != ESP_OK)
    {
        goto init_fail;
    }

    // ---- Keep DAC muted until the final step --------------------
    err = write_digital_volume(TLV320_MUTED_REG_VALUE);
    if (err != ESP_OK)
    {
        goto init_fail;
    }

    // ---- Phase 5: route DAC to the analog output paths ----------
    // Reapply the fixed DAC->mixer routing so each profile transition starts
    // from the same known analog signal path before output drivers change.
    err = write_regs(0x01,
                     output_routing_init,
                     sizeof(output_routing_init) / sizeof(output_routing_init[0]));
    if (err != ESP_OK)
    {
        goto init_fail;
    }

    // ---- Phase 6: configure analog output drivers ---------------
    err = write_regs(0x01,
                     analog_driver_init,
                     sizeof(analog_driver_init) / sizeof(analog_driver_init[0]));
    if (err != ESP_OK)
    {
        goto init_fail;
    }

    // ---- Phase 7: configure interrupt routing -------------------
    err = write_regs(0x00,
                     interrupt_routing_init,
                     sizeof(interrupt_routing_init) / sizeof(interrupt_routing_init[0]));
    if (err != ESP_OK)
    {
        goto init_fail;
    }

    err = tlv320_gpio1_set_mode(
        (CODEC_INT_GPIO >= 0)
            ? TLV320_GPIO1_MODE_INT1
            : TLV320_GPIO1_MODE_DISABLED);
    if (err != ESP_OK)
    {
        goto init_fail;
    }
    if (CODEC_INT_GPIO >= 0)
    {
        ESP_LOGI(TAG, "Codec GPIO1 routed as INT1 on GPIO %d", CODEC_INT_GPIO);
    }

    // ---- Enable headset detection before profile selection ------
    if (tlv320_headset_events_enabled())
    {
        err = write_regs(0x00,
                         headset_detect_init,
                         sizeof(headset_detect_init) / sizeof(headset_detect_init[0]));
        if (err != ESP_OK)
        {
            goto init_fail;
        }
    }

    // ---- Phases 8-9: apply the configured startup profile, gains, and EQ --
    s_apply_profile_volume_default = true;
    err = tlv320dac3100_set_profile(default_profile);
    if (err != ESP_OK)
    {
        goto init_fail;
    }
    s_apply_profile_volume_default = false;

    if (tlv320_headset_events_enabled())
    {
        err = tlv320dac3100_check_headset();
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "Initial headset detection failed: %s",
                     esp_err_to_name(err));
        }
    }

    if (tlv320_headset_events_enabled())
    {
        err = tlv320_start_event_handling();
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to start codec event handling: %s",
                     esp_err_to_name(err));
            goto init_fail;
        }
    }

    // ---- Phase 10: unmute at end --------------------------------
    err = tlv320dac3100_mute(false);
    if (err != ESP_OK)
    {
        goto init_fail;
    }

    err = select_page(0x00);
    if (err != ESP_OK)
    {
        goto init_fail;
    }

    ESP_LOGI(TAG, "TLV320DAC3100 initialized successfully");
    ESP_LOGI(TAG, "Startup profile: %s",
             (default_profile == TLV320_PROFILE_HEADPHONE)
                ? "headphone"
                : "speaker");
    ESP_LOGI(TAG, "Startup volume level: %u (%.1f dB)",
             s_volume, s_volume_db);
    ESP_LOGI(TAG, "Headset auto-switch: %s",
             tlv320_headset_events_enabled() ? "enabled" : "disabled");
    return ESP_OK;

init_fail:
    s_apply_profile_volume_default = false;
    tlv320_full_reset();
    return err;
}


esp_err_t tlv320dac3100_check_headset(void)
{
    // Read headset detection status from bits 6:5 of REG_HEADSET_DETECT
    // (Page 0).
    uint8_t reg_val = 0;
    esp_err_t err = read_reg(0x00, REG_HEADSET_DETECT, &reg_val);
    if (err != ESP_OK)
    {
        return err;
    }

    return tlv320_handle_headset_status(reg_val, false);
}


void tlv320dac3100_poll_headset(void)
{
    // Ignores return value since this is called from a timer callback
    tlv320dac3100_check_headset();
}


void tlv320dac3100_set_volume(uint8_t level)
{
    if (level > MAX_VOLUME)
    {
        level = MAX_VOLUME;
    }

    // Delegate to the dB-based setter using the lookup table.
    tlv320dac3100_set_volume_db(vol_db_table[level]);
    s_volume = level;
}


uint8_t tlv320dac3100_get_volume(void)
{
    return s_volume;
}

void tlv320dac3100_set_volume_db(float db)
{
    // Clamp to the hardware-representable range.
    if (db > 0.0f)
    {
        db = 0.0f;
    }
    if (db < -63.5f)
    {
        db = -63.5f;
    }

    s_volume_db = db;

    // Convert dB to the codec register value.  The register uses two's-
    // complement in 0.5 dB steps:  0x00 = 0 dB, 0xFF = -0.5 dB, …,
    // 0x81 = -63.5 dB.  So reg = (int8_t)(db * 2.0f) cast to uint8_t.
    int8_t reg_signed = (int8_t)(db * 2.0f);
    s_digital_volume_reg = (uint8_t)reg_signed;

    // Reverse-map the closest level for get_volume().
    // Find the nearest entry in vol_db_table (linear scan is fine for 10).
    uint8_t best = 0;
    float best_diff = 100.0f; // larger than any possible table delta
    for (int i = 0; i <= MAX_VOLUME; i++)
    {
        float diff = db - vol_db_table[i];
        if (diff < 0.0f)
        {
            diff = -diff;
        }
        if (diff < best_diff)
        {
            best_diff = diff;
            best = (uint8_t)i;
        }
    }
    s_volume = best;

    esp_err_t err = write_digital_volume(get_effective_volume_reg());
    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "Volume set to %.1f dB (level %u, reg 0x%02X)",
                 s_volume_db, s_volume, s_digital_volume_reg);
    }
    else
    {
        ESP_LOGE(TAG, "Failed to set volume %.1f dB: %s",
                 db, esp_err_to_name(err));
    }
}

float tlv320dac3100_get_volume_db(void)
{
    return s_volume_db;
}

esp_err_t tlv320dac3100_set_profile(tlv320_profile_t profile)
{
    if (s_dev == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (profile != TLV320_PROFILE_SPEAKER &&
        profile != TLV320_PROFILE_HEADPHONE)
    {
        return ESP_ERR_INVALID_ARG;
    }

    bool was_muted = s_muted;
    esp_err_t err = tlv320dac3100_mute(true);
    if (err != ESP_OK)
    {
        return err;
    }

    err = write_regs(0x01,
                     output_routing_init,
                     sizeof(output_routing_init) / sizeof(output_routing_init[0]));
    if (err != ESP_OK)
    {
        return err;
    }

    err = configure_profile_outputs(profile);
    if (err != ESP_OK)
    {
        return err;
    }

    err = tlv320_apply_gain_defaults(profile, s_apply_profile_volume_default);
    if (err != ESP_OK)
    {
        return err;
    }

    ESP_LOGI(TAG, "%s %s profile",
             (profile != s_profile)
                ? "Switched to"
                : "Reapplied",
             (profile == TLV320_PROFILE_HEADPHONE)
                ? "headphone"
                : "speaker");

    s_profile = profile;
    s_hp_active = (profile == TLV320_PROFILE_HEADPHONE);

    if (!was_muted)
    {
        return tlv320dac3100_mute(false);
    }

    return ESP_OK;
}

esp_err_t tlv320dac3100_mute(bool enable)
{
    if (s_dev == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    // Lightweight soft mute: force the DAC digital volume to a very low level
    // instead of toggling a dedicated codec hardware mute bit.
    s_muted = enable;
    return write_digital_volume(get_effective_volume_reg());
}

void tlv320dac3100_set_autoswitch(bool enable)
{
    if (s_autoswitch == enable)
    {
        return;
    }
    s_autoswitch = enable;
    ESP_LOGI(TAG, "Headset autoswitch %s", enable ? "enabled" : "disabled");
}

bool tlv320dac3100_get_autoswitch(void)
{
    return s_autoswitch;
}

tlv320_profile_t tlv320dac3100_get_profile(void)
{
    return s_profile;
}
