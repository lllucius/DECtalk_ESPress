// SPDX-License-Identifier: MIT
// Copyright (c) 2025 Leland Lucius
// ----------------------------------------------------------------
// TLV320DAC3100 I2C initialisation for the Adafruit breakout board
//
// Configures the TI TLV320DAC3100 stereo DAC as an I2S slave with
// CODEC_CLKIN = BCLK.  No PLL or MCLK is used.  Startup profile,
// startup volume, and headset auto-switching are controlled by Kconfig.
//
// Register addresses and bit layouts are taken from the
// TLV320DAC3100 datasheet (SLAS833) and cross-referenced against
// the Adafruit TLV320_I2S Arduino library.
// ----------------------------------------------------------------

#include "sdkconfig.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>
#include <stdint.h>
#include "tlv320dac3100.h"

static const char *TAG = "TLV320DAC3100";

#define TLV320_I2C_ADDR     CONFIG_DECTALK_TLV320_I2C_ADDR
#define I2C_SDA_IO          CONFIG_DECTALK_I2C_SDA_GPIO
#define I2C_SCL_IO          CONFIG_DECTALK_I2C_SCL_GPIO

// ---- Page 0 registers -------------------------------------------
#define REG_PAGE_SELECT     0x00
#define REG_RESET           0x01
#define REG_CLOCK_MUX       0x04    // CODEC_CLKIN source
#define REG_NDAC            0x0B    // NDAC divider
#define REG_MDAC            0x0C    // MDAC divider
#define REG_DOSR_MSB        0x0D    // DOSR upper byte
#define REG_DOSR_LSB        0x0E    // DOSR lower byte
#define REG_CODEC_IF        0x1B    // Audio interface control
#define REG_STICKY_FLAG1    0x2C    // Sticky interrupt flags (faults)
#define REG_STICKY_FLAG2    0x2E    // Sticky interrupt flags (UI events)
#define REG_INT1_CTRL       0x30    // INT1 routing / pulse control
#define REG_INT2_CTRL       0x31    // INT2 routing / pulse control
#define REG_DAC_PRB         0x3C    // DAC processing block
#define REG_DAC_DATAPATH    0x3F    // DAC data-path setup
#define REG_DAC_VOL_CTRL    0x40    // DAC volume / mute control
#define REG_DAC_LVOL        0x41    // Left DAC digital volume
#define REG_DAC_RVOL        0x42    // Right DAC digital volume
#define REG_HEADSET_DETECT  0x43    // Headset detection configuration

// ---- Interrupt control bit masks (REG_INT1_CTRL / REG_INT2_CTRL) ----
//
// Each bit routes the corresponding event to INTx when set.  Bit 1
// selects the pulse mode: 0 = single pulse, 1 = repeated pulses.
// Reference: TLV320DAC3100 datasheet (SLAS833), page 0 registers 48-49.
#define TLV320_INT_HEADSET       0x80    // Headset insertion/removal
#define TLV320_INT_BUTTON        0x40    // Button press
#define TLV320_INT_DRC_THRESH    0x20    // DRC signal-power threshold
#define TLV320_INT_SHORT_CIRCUIT 0x10    // Over-current / short circuit
#define TLV320_INT_DAC_OVERFLOW  0x08    // DAC / engine overflow
#define TLV320_INT_REPEAT        0x02    // Pulse mode: repeated pulses

// ---- Sticky flag 1 bit masks (REG_STICKY_FLAG1) ---------------------
//
// Reading REG_STICKY_FLAG1 latches and clears the sticky condition bits,
// which also deasserts a repeated-pulse interrupt if the underlying
// condition has cleared.
//
// Assumption: the short-circuit and overflow sticky bits map to the same
// relative positions as in the INT control register.  Verified against
// the SLAS833 register table; document any deviation if hardware
// behaviour differs.
#define TLV320_SFLAG1_SHORT_CIRCUIT  0x10
#define TLV320_SFLAG1_DAC_OVERFLOW   0x08

// ---- Sticky flag 2 bit masks (REG_STICKY_FLAG2) ---------------------
//
// Reading REG_STICKY_FLAG2 latches and clears the headset/button sticky
// bits.  For single-pulse INT2 the interrupt is a one-shot; re-reading
// this register is sufficient to acknowledge it.
#define TLV320_SFLAG2_HEADSET        0x20
#define TLV320_SFLAG2_BUTTON         0x10

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

// ---- Default runtime settings ------------------------------------
#define DEFAULT_VOLUME              CONFIG_DECTALK_TLV320_STARTUP_VOLUME
#define MAX_VOLUME                  TLV320DAC3100_MAX_VOLUME
#define TLV320_MIN_VOLUME_DB        (-60.0f)
#define TLV320_MAX_VOLUME_DB        (0.0f)
#define TLV320_SPEAKER_DB           (-16.0f)
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
    {REG_HEADSET_DETECT, 0x88},
};

// Page 0 phase: configure interrupt routing.
//
// INT1 carries fault conditions with repeated pulses so the host
// keeps being notified while the fault persists:
//   - short circuit (over-current)  → TLV320_INT_SHORT_CIRCUIT
//   - DAC / engine overflow         → TLV320_INT_DAC_OVERFLOW
//   - repeated-pulse mode           → TLV320_INT_REPEAT
//
// INT2 carries UI / accessory events with a single pulse:
//   - headset insertion/removal     → TLV320_INT_HEADSET
//   - button press                  → TLV320_INT_BUTTON
//
// DRC threshold interrupt is intentionally left disabled.
static const reg_val_t interrupt_routing_init[] =
{
    {REG_INT1_CTRL, (TLV320_INT_SHORT_CIRCUIT |
                     TLV320_INT_DAC_OVERFLOW  |
                     TLV320_INT_REPEAT)},       // 0x1A
    {REG_INT2_CTRL, (TLV320_INT_HEADSET |
                     TLV320_INT_BUTTON)},        // 0xC0
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

// ---- Codec GPIO configuration ------------------------------------
#define CODEC_RESET_GPIO    CONFIG_DECTALK_CODEC_RESET_GPIO
#define CODEC_INT_GPIO      CONFIG_DECTALK_CODEC_INT_GPIO

// Stack size for the ISR deferred handler task.  Needs room for I2C
// transactions and ESP_LOG formatting; 3 KiB gives comfortable margin.
#define TLV320_ISR_TASK_STACK   3072

// ---- Module state ------------------------------------------------
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static uint8_t s_current_page = TLV320_INVALID_PAGE;
static bool s_hp_active; // true when headphone output is active
static uint8_t s_volume = DEFAULT_VOLUME;
static float s_volume_db = TLV320_SPEAKER_DB;
static uint8_t s_digital_volume_reg = 0xEC;
static bool s_muted = true;
static tlv320_profile_t s_profile = TLV320_PROFILE_SPEAKER;
static bool s_apply_profile_volume_default;

#if CODEC_INT_GPIO >= 0
static TaskHandle_t s_isr_task;
#endif


static tlv320_profile_t tlv320_get_default_profile(void)
{
#if CONFIG_DECTALK_TLV320_DEFAULT_PROFILE_HEADPHONE
    return TLV320_PROFILE_HEADPHONE;
#else
    return TLV320_PROFILE_SPEAKER;
#endif
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
}

// ---- Hardware reset ----------------------------------------------
//
// If CODEC_RESET_GPIO is configured (>= 0), perform a hardware reset
// by driving the active-low reset pin.  The TLV320DAC3100 requires
// at least 10 ns low pulse (SLAS833); we hold it for 1 ms for margin.
// A 5 ms delay after release allows the codec to complete its internal
// power-on sequence before I2C traffic begins.
//
// If CODEC_RESET_GPIO < 0 the function is a no-op.
static void tlv320_hardware_reset(void)
{
#if CODEC_RESET_GPIO >= 0
    gpio_config_t rst_cfg =
    {
        .pin_bit_mask = 1ULL << CODEC_RESET_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&rst_cfg);

    // Assert reset (active-low)
    gpio_set_level(CODEC_RESET_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(1));

    // Release reset
    gpio_set_level(CODEC_RESET_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(5));

    ESP_LOGI(TAG, "Hardware reset complete (GPIO %d)", CODEC_RESET_GPIO);
#endif
}

// ---- Interrupt deferred handler ----------------------------------
//
// The GPIO ISR sends a task notification here.  We read the codec's
// sticky flag registers over I2C (which cannot run in ISR context)
// and dispatch events.  Fault events are handled before UI events.
#if CODEC_INT_GPIO >= 0
static void IRAM_ATTR tlv320_isr_handler(void *arg)
{
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(s_isr_task, &woken);
    portYIELD_FROM_ISR(woken);
}

static void tlv320_isr_task(void *arg)
{
    for (;;)
    {
        // Block until the GPIO ISR posts a notification.
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        // ---- Read fault sticky flags (INT1 sources) ----
        uint8_t fault_flags = 0;
        if (read_reg(0x00, REG_STICKY_FLAG1, &fault_flags) != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to read sticky flag 1");
        }

        if (fault_flags & TLV320_SFLAG1_SHORT_CIRCUIT)
        {
            ESP_LOGW(TAG, "Codec fault: short-circuit / over-current detected");
        }

        if (fault_flags & TLV320_SFLAG1_DAC_OVERFLOW)
        {
            ESP_LOGW(TAG, "Codec fault: DAC / engine overflow detected");
        }

        // ---- Read UI sticky flags (INT2 sources) ----
        uint8_t ui_flags = 0;
        if (read_reg(0x00, REG_STICKY_FLAG2, &ui_flags) != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to read sticky flag 2");
        }

        if (ui_flags & (TLV320_SFLAG2_HEADSET | TLV320_SFLAG2_BUTTON))
        {
            if (ui_flags & TLV320_SFLAG2_HEADSET)
            {
                ESP_LOGI(TAG, "Codec event: headset insertion/removal");
            }

            if (ui_flags & TLV320_SFLAG2_BUTTON)
            {
                ESP_LOGI(TAG, "Codec event: button press");
            }

            // Re-use the existing headset check logic to switch profiles.
            tlv320dac3100_check_headset();
        }
    }
}

static esp_err_t tlv320_setup_isr(void)
{
    // Create the deferred handler task before installing the ISR so the
    // notification target is valid when the first edge arrives.
    BaseType_t ok = xTaskCreate(tlv320_isr_task,
                                "tlv320_isr",
                                TLV320_ISR_TASK_STACK,
                                NULL,
                                tskIDLE_PRIORITY + 3,
                                &s_isr_task);
    if (ok != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create codec ISR task");
        return ESP_ERR_NO_MEM;
    }

    gpio_config_t int_cfg =
    {
        .pin_bit_mask = 1ULL << CODEC_INT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,     // INT pins are active-low
    };
    esp_err_t err = gpio_config(&int_cfg);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to configure interrupt GPIO %d: %s",
                 CODEC_INT_GPIO, esp_err_to_name(err));
        return err;
    }

    // Flags argument 0 = default allocation (no special options).
    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        // ESP_ERR_INVALID_STATE means the ISR service is already installed,
        // which is fine.
        ESP_LOGE(TAG, "Failed to install GPIO ISR service: %s",
                 esp_err_to_name(err));
        return err;
    }

    err = gpio_isr_handler_add(CODEC_INT_GPIO, tlv320_isr_handler, NULL);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to add ISR handler for GPIO %d: %s",
                 CODEC_INT_GPIO, esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Codec interrupt handler installed (GPIO %d, active-low)",
             CODEC_INT_GPIO);
    return ESP_OK;
}
#endif // CODEC_INT_GPIO >= 0

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

    static const reg_val_t headphone_output_cfg[] =
    {
        {REG_SPK_DRIVER, 0x00},
        {REG_SPK_AMP,    0x06},
        {REG_HP_DRIVERS, 0xC4},
        {REG_HPL_DRIVER, 0x04},
        {REG_HPR_DRIVER, 0x04},
    };

    const reg_val_t *cfg = (profile == TLV320_PROFILE_HEADPHONE)
        ? headphone_output_cfg
        : speaker_output_cfg;
    size_t count = (profile == TLV320_PROFILE_HEADPHONE)
        ? sizeof(headphone_output_cfg) / sizeof(headphone_output_cfg[0])
        : sizeof(speaker_output_cfg) / sizeof(speaker_output_cfg[0]);

    return write_regs(0x01, cfg, count);
}

static esp_err_t tlv320_apply_gain_defaults(bool apply_startup_volume)
{
    static const reg_val_t analog_gain_cfg[] =
    {
        {REG_HPL_VOL, 0x80},
        {REG_HPR_VOL, 0x80},
        {REG_SPK_VOL, 0x80},
    };

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

    // ---- Hardware reset (if configured) -------------------------
    tlv320_hardware_reset();

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
    vTaskDelay(pdMS_TO_TICKS(10));

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

    // ---- Enable headset detection before profile selection ------
#if CONFIG_DECTALK_TLV320_HEADSET_AUTOSWITCH
    err = write_regs(0x00,
                     headset_detect_init,
                     sizeof(headset_detect_init) / sizeof(headset_detect_init[0]));
    if (err != ESP_OK)
    {
        goto init_fail;
    }
#endif

    // ---- Configure interrupt routing ----------------------------
    //
    // INT1: fault conditions (short circuit + DAC overflow), repeated pulses
    // INT2: UI events (headset detect + button press), single pulse
    //
    // This is written unconditionally so the codec's INT pins reflect the
    // correct events even when the host GPIO is not wired.  If
    // CODEC_INT_GPIO < 0 the GPIO ISR is simply not installed.
    err = write_regs(0x00,
                     interrupt_routing_init,
                     sizeof(interrupt_routing_init) / sizeof(interrupt_routing_init[0]));
    if (err != ESP_OK)
    {
        goto init_fail;
    }

    // ---- Phases 7-8: apply the configured startup profile, gains, and EQ --
    s_apply_profile_volume_default = true;
    err = tlv320dac3100_set_profile(default_profile);
    if (err != ESP_OK)
    {
        goto init_fail;
    }
    s_apply_profile_volume_default = false;

    // Perform an initial headset check so we start in the correct mode.
#if CONFIG_DECTALK_TLV320_HEADSET_AUTOSWITCH
    err = tlv320dac3100_check_headset();
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Initial headset detection failed: %s",
                 esp_err_to_name(err));
    }
#endif

    // ---- Phase 9: unmute at end ---------------------------------
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

    // ---- Install interrupt handler (if GPIO configured) ---------
#if CODEC_INT_GPIO >= 0
    err = tlv320_setup_isr();
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Codec interrupt setup failed; falling back to polling");
    }
#endif

    return ESP_OK;

init_fail:
    s_apply_profile_volume_default = false;
    tlv320_release_i2c_resources();
    tlv320_reset_state();
    return err;
}


esp_err_t tlv320dac3100_check_headset(void)
{
#if !CONFIG_DECTALK_TLV320_HEADSET_AUTOSWITCH
    return ESP_OK;
#else
    // Read headset detection status from bits 6:5 of REG_HEADSET_DETECT
    // (Page 0).
    uint8_t reg_val = 0;
    esp_err_t err = read_reg(0x00, REG_HEADSET_DETECT, &reg_val);
    if (err != ESP_OK)
    {
        return err;
    }

    uint8_t status = (reg_val >> 5) & 0x03;
    bool hp_detected =
        (status == HEADSET_WITHOUT_MIC || status == HEADSET_WITH_MIC);

    if (hp_detected && !s_hp_active)
    {
        ESP_LOGI(TAG, "Headphone inserted - switching to headphone profile");
        err = tlv320dac3100_set_profile(TLV320_PROFILE_HEADPHONE);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to switch to headphone profile: %s",
                     esp_err_to_name(err));
            return err;
        }
    }
    else if (!hp_detected && s_hp_active)
    {
        ESP_LOGI(TAG, "Headphone removed - switching to speaker profile");
        err = tlv320dac3100_set_profile(TLV320_PROFILE_SPEAKER);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to switch to speaker profile: %s",
                     esp_err_to_name(err));
            return err;
        }
    }

    return ESP_OK;
#endif
}


void tlv320dac3100_poll_headset(void)
{
    // Ignores return value since this is called from a timer callback
    tlv320dac3100_check_headset();
}


bool tlv320dac3100_irq_enabled(void)
{
#if CODEC_INT_GPIO >= 0
    return s_isr_task != NULL;
#else
    return false;
#endif
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

    err = tlv320_apply_gain_defaults(s_apply_profile_volume_default);
    if (err != ESP_OK)
    {
        return err;
    }

    err = tlv320_apply_speech_eq(profile);
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
