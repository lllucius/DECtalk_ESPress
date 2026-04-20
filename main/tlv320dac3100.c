// SPDX-License-Identifier: MIT
// Copyright (c) 2025 Leland Lucius
// ----------------------------------------------------------------
// TLV320DAC3100 I2C initialisation for the Adafruit breakout board
//
// Configures the TI TLV320DAC3100 stereo DAC as an I2S slave with
// CODEC_CLKIN = BCLK.  No PLL or MCLK is used.  Startup profile,
// startup volume, optional reset GPIO, and deferred codec event handling
// are controlled by Kconfig.
//
// Register addresses and bit layouts are taken from the
// TLV320DAC3100 datasheet (SLAS833) and cross-referenced against
// the Adafruit TLV320_I2S Arduino library.
// ----------------------------------------------------------------

#include "sdkconfig.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <math.h>
#include <stdint.h>
#include "tlv320dac3100.h"

static const char *TAG = "TLV320DAC3100";

#define TLV320_I2C_ADDR     CONFIG_DECTALK_TLV320_I2C_ADDR
#define I2C_SDA_IO          CONFIG_DECTALK_I2C_SDA_GPIO
#define I2C_SCL_IO          CONFIG_DECTALK_I2C_SCL_GPIO
#define CODEC_INT_GPIO      CONFIG_DECTALK_CODEC_INT_GPIO
#define CODEC_RESET_GPIO    CONFIG_DECTALK_CODEC_RESET_GPIO

// ---- Page 0 registers -------------------------------------------
#define REG_PAGE_SELECT     0x00
#define REG_RESET           0x01
#define REG_CLOCK_MUX       0x04    // CODEC_CLKIN source
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
#define TLV320_MIN_VOLUME_DB        (-60.0f)
#define TLV320_MAX_VOLUME_DB        (0.0f)
#define TLV320_MUTED_REG_VALUE      0x81
#define TLV320_INVALID_PAGE         0xFF
#define TLV320_EQ_BIQUAD_COUNT      6
#define TLV320_EQ_BYTES_PER_BIQUAD  10
#define TLV320_EQ_PAGE_SPEAKER      0x08
#define TLV320_EQ_PAGE_HEADPHONE    0x0C
#define TLV320_EQ_START_REG         0x02
#define TLV320_EQ_COEFFICIENT_WRITES_VERIFIED 0
#define TLV320_Q15_POSITIVE_SCALE   32767.0f
#define TLV320_Q15_NEGATIVE_SCALE   32768.0f
#define TLV320_PI                   3.14159265358979323846f
#define TLV320_VOLUME_STEPS_PER_DB  2.0f
#define TLV320_SAMPLE_RATE_HZ  11025.0f
#define TLV320_HP_ANALOG_VOL_BRINGUP 0x8A
#define TLV320_HP_DRIVERS_ENABLED   0xC4
#define TLV320_HP_DRIVER_6DB        0x34
#define TLV320_EVENT_QUEUE_LEN      8
#define TLV320_EVENT_TASK_STACK     3072
#define TLV320_EVENT_IRQ            TLV320_BIT(0)
#define TLV320_EVENT_POLL           TLV320_BIT(1)
#define TLV320_HEADSET_POLL_US      500000ULL
#define TLV320_RESET_ASSERT_MS      1
#define TLV320_RESET_SETTLE_MS      10
#define TLV320_STARTUP_VOLUME_LEVEL CONFIG_DECTALK_TLV320_STARTUP_VOLUME
#define TLV320_STARTUP_VOLUME_DB    \
    ((TLV320_STARTUP_VOLUME_LEVEL == 0) ? -60.0f : \
    (TLV320_STARTUP_VOLUME_LEVEL == 1) ? -32.0f : \
    (TLV320_STARTUP_VOLUME_LEVEL == 2) ? -28.0f : \
    (TLV320_STARTUP_VOLUME_LEVEL == 3) ? -24.0f : \
    (TLV320_STARTUP_VOLUME_LEVEL == 4) ? -20.0f : \
    (TLV320_STARTUP_VOLUME_LEVEL == 5) ? -16.0f : \
    (TLV320_STARTUP_VOLUME_LEVEL == 6) ? -12.0f : \
    (TLV320_STARTUP_VOLUME_LEVEL == 7) ? -8.0f : \
    (TLV320_STARTUP_VOLUME_LEVEL == 8) ? -4.0f : 0.0f)
#define TLV320_STARTUP_VOLUME_REG   \
    ((TLV320_STARTUP_VOLUME_LEVEL == 0) ? 0x81 : \
    (TLV320_STARTUP_VOLUME_LEVEL == 1) ? 0xC0 : \
    (TLV320_STARTUP_VOLUME_LEVEL == 2) ? 0xC8 : \
    (TLV320_STARTUP_VOLUME_LEVEL == 3) ? 0xD0 : \
    (TLV320_STARTUP_VOLUME_LEVEL == 4) ? 0xD8 : \
    (TLV320_STARTUP_VOLUME_LEVEL == 5) ? 0xE0 : \
    (TLV320_STARTUP_VOLUME_LEVEL == 6) ? 0xE8 : \
    (TLV320_STARTUP_VOLUME_LEVEL == 7) ? 0xF0 : \
    (TLV320_STARTUP_VOLUME_LEVEL == 8) ? 0xF8 : 0x00)

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

// miniDSP biquad coefficients stored in signed Q15 format:
// b0-b2 are feedforward taps, a1-a2 are feedback taps.
typedef struct
{
    int16_t b0;
    int16_t b1;
    int16_t b2;
    int16_t a1;
    int16_t a2;
} tlv320_biquad_t;

// Page 0 phase: configure clocking from BCLK without PLL or MCLK.
static const reg_val_t clocking_init[] =
{
    {REG_CLOCK_MUX,    0x01},   // CODEC_CLKIN = BCLK
    {REG_NDAC,         0x81},   // NDAC = 1, powered up
    {REG_MDAC,         0x81},   // MDAC = 1, powered up
    {REG_DOSR_MSB,     0x00},   // DOSR = 32 (0x0020)
    {REG_DOSR_LSB,     0x20},
};

// Page 0 phase: configure the I2S data interface.
static const reg_val_t audio_interface_init[] =
{
    {REG_CODEC_IF,     0x00},   // I2S, 16-bit, BCLK+WCLK inputs
                                // (slave mode)
};

// Page 0 phase: select a simple DAC processing block and mono data path.
static const reg_val_t dac_processing_init[] =
{
    {REG_DAC_PRB,      0x01},   // Processing block PRB_P1
    {REG_DAC_DATAPATH, 0xD8},   // L DAC on, R DAC on,
                                // L path = normal (left data),
                                // R path = swapped (left data)
                                //   → mono: both outputs play the
                                //     same audio from the L I2S slot;
                                // soft-step = 1 step/sample
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
static float s_volume_db = TLV320_STARTUP_VOLUME_DB;
static uint8_t s_digital_volume_reg = TLV320_STARTUP_VOLUME_REG;
static bool s_muted = true;
static tlv320_profile_t s_profile = TLV320_PROFILE_SPEAKER;
static bool s_apply_profile_volume_default;
static QueueHandle_t s_event_queue = NULL;
static TaskHandle_t s_event_task = NULL;
static esp_timer_handle_t s_poll_timer = NULL;
static bool s_irq_handler_registered = false;
static int s_irq_gpio = -1;


static tlv320_profile_t tlv320_get_default_profile(void)
{
#if CONFIG_DECTALK_TLV320_DEFAULT_PROFILE_HEADPHONE
    return TLV320_PROFILE_HEADPHONE;
#else
    return TLV320_PROFILE_SPEAKER;
#endif
}

static bool tlv320_headset_events_enabled(void)
{
#if CONFIG_DECTALK_TLV320_HEADSET_AUTOSWITCH
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
    return i2c_master_transmit(dev, buf, sizeof(buf), -1);
}

static esp_err_t read_reg_raw(i2c_master_dev_handle_t dev,
                              uint8_t reg,
                              uint8_t *val)
{
    esp_err_t err = i2c_master_transmit(dev, &reg, 1, -1);
    if (err != ESP_OK)
    {
        return err;
    }

    return i2c_master_receive(dev, val, 1, -1);
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

static float clamp_volume_db(float db)
{
    if (db < TLV320_MIN_VOLUME_DB)
    {
        return TLV320_MIN_VOLUME_DB;
    }

    if (db > TLV320_MAX_VOLUME_DB)
    {
        return TLV320_MAX_VOLUME_DB;
    }

    return db;
}

static float clamp_cutoff_hz(float cutoff_hz)
{
    float nyquist_margin_hz = TLV320_SAMPLE_RATE_HZ * 0.45f;

    if (cutoff_hz < 20.0f)
    {
        return 20.0f;
    }

    if (cutoff_hz > nyquist_margin_hz)
    {
        return nyquist_margin_hz;
    }

    return cutoff_hz;
}

static int16_t float_to_q15(float value)
{
    if (value >= 1.0f)
    {
        return INT16_MAX;
    }

    if (value <= -1.0f)
    {
        return INT16_MIN;
    }

    // Use symmetrical round-to-nearest conversion for signed Q15 output.
    float scaled = (value >= 0.0f)
        ? (value * TLV320_Q15_POSITIVE_SCALE) + 0.5f
        : (value * TLV320_Q15_NEGATIVE_SCALE) - 0.5f;

    return (int16_t)scaled;
}

static tlv320_biquad_t tlv320_make_identity_biquad(void)
{
    return (tlv320_biquad_t)
    {
        .b0 = float_to_q15(1.0f),
        .b1 = 0,
        .b2 = 0,
        .a1 = 0,
        .a2 = 0,
    };
}

static tlv320_biquad_t tlv320_make_highpass_biquad(float cutoff_hz)
{
    // First-order high-pass: H(z) = g * (1 - z^-1) / (1 - p z^-1),
    // with p derived from the requested cutoff and g chosen for unity HF gain.
    float pole =
        expf((-2.0f * TLV320_PI * clamp_cutoff_hz(cutoff_hz)) / TLV320_SAMPLE_RATE_HZ);
    float gain = (1.0f + pole) * 0.5f;

    return (tlv320_biquad_t)
    {
        .b0 = float_to_q15(gain),
        .b1 = float_to_q15(-gain),
        .b2 = 0,
        .a1 = float_to_q15(-pole),
        .a2 = 0,
    };
}

static tlv320_biquad_t tlv320_make_lowpass_biquad(float cutoff_hz)
{
    // First-order low-pass: H(z) = (1 - p) / (1 - p z^-1), where the pole
    // position sets the cutoff and keeps the section stable for all profiles.
    float pole =
        expf((-2.0f * TLV320_PI * clamp_cutoff_hz(cutoff_hz)) / TLV320_SAMPLE_RATE_HZ);
    float feedforward = 1.0f - pole;

    return (tlv320_biquad_t)
    {
        .b0 = float_to_q15(feedforward),
        .b1 = 0,
        .b2 = 0,
        .a1 = float_to_q15(-pole),
        .a2 = 0,
    };
}

static tlv320_biquad_t tlv320_make_preemphasis_biquad(float alpha)
{
    if (alpha < 0.0f)
    {
        alpha = 0.0f;
    }
    else if (alpha > 0.95f)
    {
        alpha = 0.95f;
    }

    return (tlv320_biquad_t)
    {
        .b0 = float_to_q15(1.0f),
        .b1 = float_to_q15(-alpha),
        .b2 = 0,
        .a1 = 0,
        .a2 = 0,
    };
}

static uint8_t db_to_reg(float db)
{
    // The codec stores digital gain as a signed 8-bit two's complement
    // attenuation value in 0.5 dB steps, so -10.0 dB becomes -20 steps.
    float attenuation_db = -clamp_volume_db(db);
    int steps = (int)((attenuation_db * TLV320_VOLUME_STEPS_PER_DB) + 0.5f);

    return (uint8_t)((int8_t)(-steps));
}

static uint8_t db_to_level(float db)
{
    uint8_t best_level = 0;
    float best_diff = 1000.0f;

    for (uint8_t level = 0; level <= MAX_VOLUME; level++)
    {
        float diff = db - vol_db_table[level];
        if (diff < 0.0f)
        {
            diff = -diff;
        }

        if (diff < best_diff)
        {
            best_diff = diff;
            best_level = level;
        }
    }

    return best_level;
}

static void tlv320_reset_state(void)
{
    tlv320_profile_t default_profile = tlv320_get_default_profile();

    s_current_page = TLV320_INVALID_PAGE;
    s_hp_active = (default_profile == TLV320_PROFILE_HEADPHONE);
    s_headset_present = false;
    s_volume = CONFIG_DECTALK_TLV320_STARTUP_VOLUME;
    s_volume_db = vol_db_table[CONFIG_DECTALK_TLV320_STARTUP_VOLUME];
    s_digital_volume_reg = vol_table[CONFIG_DECTALK_TLV320_STARTUP_VOLUME];
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

    s_current_page = TLV320_INVALID_PAGE;
    s_headset_present = false;
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

#if CONFIG_DECTALK_TLV320_HEADSET_AUTOSWITCH
    if (headset_present && !s_hp_active)
    {
        err = tlv320dac3100_set_profile(TLV320_PROFILE_HEADPHONE);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to switch to headphone profile: %s",
                     esp_err_to_name(err));
        }
    }
    else if (!headset_present && s_hp_active)
    {
        err = tlv320dac3100_set_profile(TLV320_PROFILE_SPEAKER);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to switch to speaker profile: %s",
                     esp_err_to_name(err));
        }
    }
#endif

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

    if (s_irq_handler_registered && (CODEC_INT_GPIO < 0 || s_irq_gpio != CODEC_INT_GPIO) && s_irq_gpio >= 0)
    {
        esp_err_t err = gpio_isr_handler_remove(s_irq_gpio);
        if (err != ESP_OK)
        {
            return err;
        }
        s_irq_handler_registered = false;
        s_irq_gpio = -1;
    }

    if (CODEC_INT_GPIO >= 0)
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

        if (s_irq_handler_registered && s_irq_gpio >= 0)
        {
            err = gpio_isr_handler_remove(s_irq_gpio);
            if (err != ESP_OK)
            {
                return err;
            }
            s_irq_handler_registered = false;
            s_irq_gpio = -1;
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

static esp_err_t write_sequential_bytes(uint8_t page,
                                        uint8_t start_reg,
                                        const uint8_t *data,
                                        size_t count)
{
    esp_err_t err = select_page(page);
    if (err != ESP_OK)
    {
        return err;
    }

    for (size_t i = 0; i < count; i++)
    {
        err = write_reg_raw(s_dev, (uint8_t)(start_reg + i), data[i]);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "I2C write failed: page 0x%02X reg 0x%02X val 0x%02X (%s)",
                     page,
                     (uint8_t)(start_reg + i),
                     data[i],
                     esp_err_to_name(err));
            return err;
        }
    }

    return ESP_OK;
}

static esp_err_t write_biquad_bank(uint8_t page,
                                   const tlv320_biquad_t *biquads,
                                   size_t biquad_count)
{
    uint8_t bank_bytes[TLV320_EQ_BIQUAD_COUNT * TLV320_EQ_BYTES_PER_BIQUAD] = {0};

    if (biquad_count > TLV320_EQ_BIQUAD_COUNT)
    {
        biquad_count = TLV320_EQ_BIQUAD_COUNT;
    }

    for (size_t i = 0; i < biquad_count; i++)
    {
        const int16_t coeffs[5] =
        {
            biquads[i].b0,
            biquads[i].b1,
            biquads[i].b2,
            biquads[i].a1,
            biquads[i].a2,
        };

        for (size_t j = 0; j < 5; j++)
        {
            size_t offset = (i * TLV320_EQ_BYTES_PER_BIQUAD) + (j * 2);

            // The codec expects each Q15 coefficient MSB first on I2C.
            bank_bytes[offset] = (uint8_t)(((uint16_t)coeffs[j]) >> 8);
            bank_bytes[offset + 1] = (uint8_t)((uint16_t)coeffs[j] & 0xFF);
        }
    }

    return write_sequential_bytes(page,
                                  TLV320_EQ_START_REG,
                                  bank_bytes,
                                  sizeof(bank_bytes));
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

    // Initial hardware bring-up tuning for low headphone output; may be reduced
    // later.
    static const reg_val_t headphone_output_cfg[] =
    {
        {REG_SPK_DRIVER, 0x00},
        {REG_SPK_AMP,    0x06},
        {REG_HP_DRIVERS, TLV320_HP_DRIVERS_ENABLED},
        {REG_HPL_DRIVER, TLV320_HP_DRIVER_6DB},
        {REG_HPR_DRIVER, TLV320_HP_DRIVER_6DB},
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
    // Speaker profile: existing conservative analog gain defaults.
    static const reg_val_t speaker_analog_gain_cfg[] =
    {
        {REG_HPL_VOL, 0x80},   // HPL routed, analog gain = 0 dB
        {REG_HPR_VOL, 0x80},   // HPR routed, analog gain = 0 dB
        {REG_SPK_VOL, 0x80},   // SPK routed, analog gain = 0 dB
    };

    // Initial hardware bring-up tuning for low headphone output; may be reduced
    // later.
    static const reg_val_t headphone_analog_gain_cfg[] =
    {
        {REG_HPL_VOL, TLV320_HP_ANALOG_VOL_BRINGUP},
        {REG_HPR_VOL, TLV320_HP_ANALOG_VOL_BRINGUP},
        {REG_SPK_VOL, 0x80},   // SPK routed, analog gain = 0 dB
    };

    const reg_val_t *cfg = (profile == TLV320_PROFILE_HEADPHONE)
        ? headphone_analog_gain_cfg
        : speaker_analog_gain_cfg;
    size_t count = (profile == TLV320_PROFILE_HEADPHONE)
        ? sizeof(headphone_analog_gain_cfg) / sizeof(headphone_analog_gain_cfg[0])
        : sizeof(speaker_analog_gain_cfg) / sizeof(speaker_analog_gain_cfg[0]);

    esp_err_t err = write_regs(0x01, cfg, count);
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

static void tlv320_log_headphone_profile_registers(void)
{
    uint8_t hp_drivers = 0;
    uint8_t hpl_vol = 0;
    uint8_t hpr_vol = 0;
    uint8_t hpl_driver = 0;
    uint8_t hpr_driver = 0;

    esp_err_t err = read_reg(0x01, REG_HP_DRIVERS, &hp_drivers);
    if (err == ESP_OK)
    {
        err = read_reg(0x01, REG_HPL_VOL, &hpl_vol);
    }
    if (err == ESP_OK)
    {
        err = read_reg(0x01, REG_HPR_VOL, &hpr_vol);
    }
    if (err == ESP_OK)
    {
        err = read_reg(0x01, REG_HPL_DRIVER, &hpl_driver);
    }
    if (err == ESP_OK)
    {
        err = read_reg(0x01, REG_HPR_DRIVER, &hpr_driver);
    }
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Headphone regs readback failed: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG,
             "Headphone regs: HP_DRV=0x%02X HPL_VOL=0x%02X HPR_VOL=0x%02X HPL_DRV=0x%02X HPR_DRV=0x%02X",
             hp_drivers,
             hpl_vol,
             hpr_vol,
             hpl_driver,
             hpr_driver);
}

static esp_err_t tlv320_apply_speech_eq(tlv320_profile_t profile)
{
    tlv320_biquad_t biquads[TLV320_EQ_BIQUAD_COUNT];
    uint8_t eq_page = (profile == TLV320_PROFILE_HEADPHONE)
        ? TLV320_EQ_PAGE_HEADPHONE
        : TLV320_EQ_PAGE_SPEAKER;

    for (size_t i = 0; i < TLV320_EQ_BIQUAD_COUNT; i++)
    {
        biquads[i] = tlv320_make_identity_biquad();
    }

    if (profile == TLV320_PROFILE_HEADPHONE)
    {
        // Headphones stay closer to flat: a gentle low-cut for rumble plus
        // light pre-emphasis to keep consonants forward without sounding sharp.
        biquads[0] = tlv320_make_highpass_biquad(70.0f);
        biquads[1] = tlv320_make_preemphasis_biquad(0.18f);
    }
    else
    {
        // Speakers get stronger speech shaping for a small enclosure:
        // low-cut to reduce boom, pre-emphasis for presence, then a mild
        // treble rolloff to avoid harshness at the top of the band.
        biquads[0] = tlv320_make_highpass_biquad(150.0f);
        biquads[1] = tlv320_make_preemphasis_biquad(0.32f);
        biquads[2] = tlv320_make_lowpass_biquad(3600.0f);
    }

    // Keep the EQ scaffold in place, but defer coefficient-bank programming
    // until the selected DAC processing block and coefficient-page mapping are
    // verified against the TLV320DAC3100 datasheet for this driver setup.
    if (TLV320_EQ_COEFFICIENT_WRITES_VERIFIED)
    {
        return write_biquad_bank(eq_page, biquads, TLV320_EQ_BIQUAD_COUNT);
    }

    ESP_LOGD(TAG, "Speech EQ coefficient writes deferred pending verified TLV320DAC3100 processing-block mapping");
    return ESP_OK;
}


esp_err_t tlv320dac3100_init(void)
{
    esp_err_t err;
    tlv320_profile_t default_profile = tlv320_get_default_profile();

    ESP_LOGI(TAG, "Initializing TLV320DAC3100 (I2C addr 0x%02X)...",
             TLV320_I2C_ADDR);

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

    tlv320_release_i2c_resources();
    tlv320_reset_state();

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
    s_current_page = TLV320_INVALID_PAGE;

    // ---- Phase 2: configure clocks / PLL path -------------------
    err = write_regs(0x00,
                     clocking_init,
                     sizeof(clocking_init) / sizeof(clocking_init[0]));
    if (err != ESP_OK)
    {
        goto init_fail;
    }

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
    tlv320_release_i2c_resources();
    tlv320_reset_state();
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

    s_volume = level;
    s_volume_db = vol_db_table[level];
    s_digital_volume_reg = vol_table[level];
    esp_err_t err = write_digital_volume(get_effective_volume_reg());
    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "Volume set to %u (reg 0x%02X)", level, s_digital_volume_reg);
    }
    else
    {
        ESP_LOGE(TAG, "Failed to set volume level %u: %s",
                 level, esp_err_to_name(err));
    }
}


uint8_t tlv320dac3100_get_volume(void)
{
    return s_volume;
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

    err = tlv320_apply_speech_eq(profile);
    if (err != ESP_OK)
    {
        return err;
    }

    if (profile == TLV320_PROFILE_HEADPHONE)
    {
        tlv320_log_headphone_profile_registers();
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

esp_err_t tlv320dac3100_set_volume_db(float db)
{
    if (s_dev == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    db = clamp_volume_db(db);

    s_volume_db = db;
    s_digital_volume_reg = db_to_reg(db);
    s_volume = db_to_level(db);

    esp_err_t err = write_digital_volume(get_effective_volume_reg());
    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "Volume set to %.1f dB", db);
    }

    return err;
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
