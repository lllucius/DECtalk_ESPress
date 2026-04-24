// SPDX-License-Identifier: MIT
// Copyright (c) 2025 Leland Lucius
// ----------------------------------------------------------------
// Custom Action Handlers — Implementation
//
// Provides the firmware-side action implementations for [:fw ...]
// commands.  Each handler returns an ACTION job that is executed
// in order on the speech task thread.
// ----------------------------------------------------------------

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdint.h>
#include <stdbool.h>

#include "custom_actions.h"
#include "custom_commands.h"

// ----------------------------------------------------------------
// Platform abstraction
// ----------------------------------------------------------------
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#if CONFIG_DTESP_DAC_TLV320
#include "tlv320.h"
#include "fw_settings.h"
#include "volume_knob.h"
#endif
static const char *TAG = "custom_act";
#define LOG_I(fmt, ...) ESP_LOGI(TAG, fmt, ##__VA_ARGS__)
#define LOG_W(fmt, ...) ESP_LOGW(TAG, fmt, ##__VA_ARGS__)
#define LOG_E(fmt, ...) ESP_LOGE(TAG, fmt, ##__VA_ARGS__)
#else
#include <stdio.h>
#define LOG_I(fmt, ...) fprintf(stderr, "[I] " fmt "\n", ##__VA_ARGS__)
#define LOG_W(fmt, ...) fprintf(stderr, "[W] " fmt "\n", ##__VA_ARGS__)
#define LOG_E(fmt, ...) fprintf(stderr, "[E] " fmt "\n", ##__VA_ARGS__)
#endif

// ----------------------------------------------------------------
// Session state: voice and rate prefixes that are prepended to
// subsequent SPEAK_TEXT jobs by the main protocol layer.
// ----------------------------------------------------------------
static char voice_prefix[32] = {0};
static char rate_prefix[32]  = {0};

const char *custom_action_get_voice_prefix(void)
{
    return voice_prefix[0] ? voice_prefix : NULL;
}

const char *custom_action_get_rate_prefix(void)
{
    return rate_prefix[0] ? rate_prefix : NULL;
}

void custom_actions_reset_session(void)
{
    voice_prefix[0] = '\0';
    rate_prefix[0] = '\0';
    LOG_I("session action state reset");
}

// ================================================================
// GPIO handler: [:fw gpio <pin> <on|off|0|1>]
// ================================================================

typedef struct
{
    int pin;
    int level;
} gpio_action_ctx_t;

static void gpio_execute(void *ctx)
{
    gpio_action_ctx_t *g = (gpio_action_ctx_t *)ctx;
    LOG_I("GPIO action: pin=%d level=%d", g->pin, g->level);
#if defined(ESP_PLATFORM) && \
    (!defined(CONFIG_DTESP_FW_CMD_ENABLE) || defined(CONFIG_DTESP_FW_CMD_GPIO_ENABLE))
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << g->pin,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    gpio_set_level(g->pin, g->level);
#endif
}

dtesp_job_t *custom_action_gpio(int argc, const char **argv)
{
    // argv[0]="gpio" argv[1]=pin argv[2]=on|off|0|1
    if (argc < 3)
    {
        LOG_W("gpio: need <pin> <on|off|0|1>");
        return NULL;
    }

    int pin = atoi(argv[1]);
    if (pin < 0 || pin > 48)
    {
        LOG_W("gpio: invalid pin %d", pin);
        return NULL;
    }

#ifdef ESP_PLATFORM
    // Reject pins that are reserved by firmware-configured peripherals
    // (I2S, I2C, codec reset/interrupt, RGB LED) so host code cannot
    // accidentally clobber them via [:fw gpio <n> on].
    static const int reserved[] =
    {
# ifdef CONFIG_DTESP_I2S_BCK_GPIO
        CONFIG_DTESP_I2S_BCK_GPIO,
# endif
# ifdef CONFIG_DTESP_I2S_WS_GPIO
        CONFIG_DTESP_I2S_WS_GPIO,
# endif
# ifdef CONFIG_DTESP_I2S_DO_GPIO
        CONFIG_DTESP_I2S_DO_GPIO,
# endif
# ifdef CONFIG_DTESP_I2S_MCLK_GPIO
        CONFIG_DTESP_I2S_MCLK_GPIO,
# endif
# ifdef CONFIG_DTESP_I2C_SDA_GPIO
        CONFIG_DTESP_I2C_SDA_GPIO,
# endif
# ifdef CONFIG_DTESP_I2C_SCL_GPIO
        CONFIG_DTESP_I2C_SCL_GPIO,
# endif
# ifdef CONFIG_DTESP_CODEC_INT_GPIO
        CONFIG_DTESP_CODEC_INT_GPIO,
# endif
# ifdef CONFIG_DTESP_CODEC_RESET_GPIO
        CONFIG_DTESP_CODEC_RESET_GPIO,
# endif
# ifdef CONFIG_DTESP_RGB_LED_GPIO
        CONFIG_DTESP_RGB_LED_GPIO,
# endif
    };
    for (size_t i = 0; i < sizeof(reserved) / sizeof(reserved[0]); i++)
    {
        if (reserved[i] == pin)
        {
            LOG_W("gpio: pin %d is reserved by firmware; rejecting", pin);
            return NULL;
        }
    }
#endif

    int level;
    if (strcasecmp(argv[2], "on") == 0 || strcmp(argv[2], "1") == 0)
    {
        level = 1;
    }
    else if (strcasecmp(argv[2], "off") == 0 || strcmp(argv[2], "0") == 0)
    {
        level = 0;
    }
    else
    {
        LOG_W("gpio: invalid level '%s'", argv[2]);
        return NULL;
    }

    gpio_action_ctx_t *ctx = malloc(sizeof(gpio_action_ctx_t));
    if (!ctx)
    {
        return NULL;
    }
    ctx->pin = pin;
    ctx->level = level;

    return dtesp_job_alloc_action(gpio_execute, ctx, free);
}

// ================================================================
// Voice handler: [:fw voice <name>]
//
// Translates the voice name to a native DECtalk inline command
// and stores it as a session prefix.  The prefix is prepended to
// subsequent SPEAK_TEXT jobs by the speech task, so the DECtalk
// library receives the voice-change command in-band.  The library
// itself is NOT modified.
//
// Because the session prefix must update in order with respect to
// interleaved text, the prefix is set by the ACTION job's execute
// callback (which runs on the speech task in submission order),
// not at tokenization time.
// ================================================================

typedef struct
{
    const char *cmd; // inline command string, e.g. "[:nb]"
} voice_action_ctx_t;

typedef struct
{
    const char *name;
    const char *cmd;
} voice_map_entry_t;

// Canonical voice table.  The single source of truth for voice names
// and inline command codes is voices.json in the repository root.
// Keep this table in sync with that file.
static const voice_map_entry_t voice_map[] =
{
    { "paul",    "[:np]"  },
    { "betty",   "[:nb]"  },
    { "harry",   "[:nh]"  },
    { "frank",   "[:nf]"  },
    { "dennis",  "[:nd]"  },
    { "kit",     "[:nk]"  },
    { "ursula",  "[:nu]"  },
    { "rita",    "[:nr]"  },
    { "wendy",   "[:nw]"  },
    { NULL,      NULL     },
};

static void voice_execute(void *ctx)
{
    voice_action_ctx_t *v = (voice_action_ctx_t *)ctx;
    if (v && v->cmd)
    {
        snprintf(voice_prefix, sizeof(voice_prefix), "%s", v->cmd);
        LOG_I("voice action applied (prefix now: %s)", voice_prefix);
    }
}

dtesp_job_t *custom_action_voice(int argc, const char **argv)
{
    if (argc < 2)
    {
        LOG_W("voice: need <name>");
        return NULL;
    }

    const voice_map_entry_t *entry = voice_map;
    while (entry->name)
    {
        if (strcasecmp(argv[1], entry->name) == 0)
        {
            voice_action_ctx_t *ctx = malloc(sizeof(voice_action_ctx_t));
            if (!ctx)
            {
                return NULL;
            }
            ctx->cmd = entry->cmd; // static string, no copy needed
            return dtesp_job_alloc_action(voice_execute, ctx, free);
        }
        entry++;
    }

    LOG_W("voice: unknown voice '%s'", argv[1]);
    return NULL;
}

// ================================================================
// Rate handler: [:fw rate <75..600>]
//
// Translates to [:ra <value>] and stores as a session prefix via
// the action's execute callback (see voice handler rationale).
// ================================================================

typedef struct
{
    int rate;
} rate_action_ctx_t;

static void rate_execute(void *ctx)
{
    rate_action_ctx_t *r = (rate_action_ctx_t *)ctx;
    if (r)
    {
        snprintf(rate_prefix, sizeof(rate_prefix), "[:ra %d]", r->rate);
        LOG_I("rate action applied (prefix now: %s)", rate_prefix);
    }
}

dtesp_job_t *custom_action_rate(int argc, const char **argv)
{
    if (argc < 2)
    {
        LOG_W("rate: need <75..600>");
        return NULL;
    }

    int rate = atoi(argv[1]);
    if (rate < 75 || rate > 600)
    {
        LOG_W("rate: out of range %d (75..600)", rate);
        return NULL;
    }

    rate_action_ctx_t *ctx = malloc(sizeof(rate_action_ctx_t));
    if (!ctx)
    {
        return NULL;
    }
    ctx->rate = rate;
    return dtesp_job_alloc_action(rate_execute, ctx, free);
}

// ================================================================
// Tone handler: [:fw tone <freq_hz> <duration_ms>]
//
// Generates a square-wave tone via the LEDC PWM peripheral.  The
// tone GPIO is configured via CONFIG_DTESP_TONE_GPIO (Kconfig).
// On non-ESP builds or when the tone GPIO is not configured, falls
// back to a diagnostic log.
// ================================================================

typedef struct
{
    int freq_hz;
    int duration_ms;
} tone_action_ctx_t;

static void tone_execute(void *ctx)
{
    tone_action_ctx_t *t = (tone_action_ctx_t *)ctx;
    LOG_I("tone action: freq=%d Hz, duration=%d ms", t->freq_hz, t->duration_ms);

#if defined(ESP_PLATFORM) && defined(CONFIG_DTESP_TONE_GPIO) && \
    (CONFIG_DTESP_TONE_GPIO >= 0)
    ledc_timer_config_t timer_cfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = (uint32_t)t->freq_hz,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    if (ledc_timer_config(&timer_cfg) != ESP_OK)
    {
        LOG_E("tone: LEDC timer config failed");
        return;
    }

    ledc_channel_config_t chan_cfg = {
        .gpio_num = CONFIG_DTESP_TONE_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 128, // 50% duty for square wave (8-bit resolution)
        .hpoint = 0,
    };
    if (ledc_channel_config(&chan_cfg) != ESP_OK)
    {
        LOG_E("tone: LEDC channel config failed");
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(t->duration_ms));

    // Stop the tone
    ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
#else
    (void)t;
    LOG_W("tone: no tone GPIO configured (set CONFIG_DTESP_TONE_GPIO)");
#endif
}

dtesp_job_t *custom_action_tone(int argc, const char **argv)
{
    if (argc < 3)
    {
        LOG_W("tone: need <freq_hz> <duration_ms>");
        return NULL;
    }

    int freq = atoi(argv[1]);
    int dur  = atoi(argv[2]);

    if (freq <= 0 || freq > 20000)
    {
        LOG_W("tone: invalid frequency %d", freq);
        return NULL;
    }
    if (dur <= 0 || dur > 30000)
    {
        LOG_W("tone: invalid duration %d ms", dur);
        return NULL;
    }

    tone_action_ctx_t *ctx = malloc(sizeof(tone_action_ctx_t));
    if (!ctx)
    {
        return NULL;
    }
    ctx->freq_hz = freq;
    ctx->duration_ms = dur;

    return dtesp_job_alloc_action(tone_execute, ctx, free);
}

// ================================================================
// Codec volume handler: [:fw volume <0..9>]
//
// Updates the in-memory fw setting and applies the new volume to the
// TLV320DAC3100 immediately.  Use [:fw save] to persist the change
// across reboots.  On non-TLV320 builds (or host tests), the action
// only logs and updates in-memory settings — it does not touch the
// codec driver.
// ================================================================

typedef struct
{
    uint8_t level;
} volume_action_ctx_t;

static void volume_execute(void *ctx)
{
    volume_action_ctx_t *v = (volume_action_ctx_t *)ctx;
    if (!v)
    {
        return;
    }
    LOG_I("volume action: level=%u", (unsigned)v->level);
#if defined(ESP_PLATFORM) && CONFIG_DTESP_DAC_TLV320
    fw_settings_set_volume(v->level);
    tlv320_set_volume(v->level);
    // Tell the knob a FW command just changed volume so it
    // unlatches and won't snap back to the pot's position.
    volume_knob_notify_external_volume(v->level);
#endif
}

dtesp_job_t *custom_action_volume(int argc, const char **argv)
{
    if (argc < 2)
    {
        LOG_W("volume: need <0..9>");
        return NULL;
    }

    // Parse strictly: digits only.
    const char *s = argv[1];
    for (const char *p = s; *p; p++)
    {
        if (!isdigit((unsigned char)*p))
        {
            LOG_W("volume: invalid level '%s'", s);
            return NULL;
        }
    }
    int level = atoi(s);
    if (level < 0 || level > 9)
    {
        LOG_W("volume: out of range %d (0..9)", level);
        return NULL;
    }

    volume_action_ctx_t *ctx = malloc(sizeof(volume_action_ctx_t));
    if (!ctx)
    {
        return NULL;
    }
    ctx->level = (uint8_t)level;
    return dtesp_job_alloc_action(volume_execute, ctx, free);
}

// ================================================================
// Codec profile handler: [:fw profile <speaker|headphone>]
// ================================================================

typedef struct
{
    int profile; // 0=speaker, 1=headphone
} profile_action_ctx_t;

static void profile_execute(void *ctx)
{
    profile_action_ctx_t *p = (profile_action_ctx_t *)ctx;
    if (!p)
    {
        return;
    }
    LOG_I("profile action: %s",
          p->profile == 0 ? "speaker" : "headphone");
#if defined(ESP_PLATFORM) && CONFIG_DTESP_DAC_TLV320
    tlv320_profile_t tp = (p->profile == 1)
                              ? TLV320_PROFILE_HEADPHONE
                              : TLV320_PROFILE_SPEAKER;
    fw_settings_set_profile((uint8_t)tp);
    esp_err_t err = tlv320_set_profile(tp);
    if (err != ESP_OK)
    {
        LOG_E("profile: codec set_profile failed (%d)", (int)err);
    }
#endif
}

dtesp_job_t *custom_action_profile(int argc, const char **argv)
{
    if (argc < 2)
    {
        LOG_W("profile: need <speaker|headphone>");
        return NULL;
    }

    int profile;
    if (strcasecmp(argv[1], "speaker") == 0 ||
        strcasecmp(argv[1], "spk") == 0)
    {
        profile = 0;
    }
    else if (strcasecmp(argv[1], "headphone") == 0 ||
             strcasecmp(argv[1], "headphones") == 0 ||
             strcasecmp(argv[1], "hp") == 0)
    {
        profile = 1;
    }
    else
    {
        LOG_W("profile: unknown '%s' (speaker|headphone)", argv[1]);
        return NULL;
    }

    profile_action_ctx_t *ctx = malloc(sizeof(profile_action_ctx_t));
    if (!ctx)
    {
        return NULL;
    }
    ctx->profile = profile;
    return dtesp_job_alloc_action(profile_execute, ctx, free);
}

// ================================================================
// Headset auto-switch handler: [:fw autoswitch <on|off|0|1>]
// ================================================================

typedef struct
{
    int enable;
} autoswitch_action_ctx_t;

static void autoswitch_execute(void *ctx)
{
    autoswitch_action_ctx_t *a = (autoswitch_action_ctx_t *)ctx;
    if (!a)
    {
        return;
    }
    LOG_I("autoswitch action: %s", a->enable ? "on" : "off");
#if defined(ESP_PLATFORM) && CONFIG_DTESP_DAC_TLV320
    fw_settings_set_autoswitch(a->enable ? 1 : 0);
    tlv320_set_autoswitch(a->enable ? true : false);
#endif
}

dtesp_job_t *custom_action_autoswitch(int argc, const char **argv)
{
    if (argc < 2)
    {
        LOG_W("autoswitch: need <on|off|0|1>");
        return NULL;
    }

    int enable;
    if (strcasecmp(argv[1], "on") == 0 || strcmp(argv[1], "1") == 0)
    {
        enable = 1;
    }
    else if (strcasecmp(argv[1], "off") == 0 || strcmp(argv[1], "0") == 0)
    {
        enable = 0;
    }
    else
    {
        LOG_W("autoswitch: invalid '%s'", argv[1]);
        return NULL;
    }

    autoswitch_action_ctx_t *ctx = malloc(sizeof(autoswitch_action_ctx_t));
    if (!ctx)
    {
        return NULL;
    }
    ctx->enable = enable;
    return dtesp_job_alloc_action(autoswitch_execute, ctx, free);
}

// ================================================================
// Save handler: [:fw save]
//
// Persists the current in-memory firmware settings (volume, profile,
// autoswitch) to NVS so they survive a reboot.
// ================================================================

static void save_execute(void *ctx)
{
    (void)ctx;
    LOG_I("save action: persisting firmware settings to NVS");
#if defined(ESP_PLATFORM) && CONFIG_DTESP_DAC_TLV320
    esp_err_t err = fw_settings_save();
    if (err != ESP_OK)
    {
        LOG_E("save: fw_settings_save failed (%d)", (int)err);
    }
#endif
}

dtesp_job_t *custom_action_save(int argc, const char **argv)
{
    (void)argc;
    (void)argv;
    return dtesp_job_alloc_action(save_execute, NULL, NULL);
}

// ================================================================
// DSP: bass / treble / eq / drc / spkgain / mute
//
// On host builds these handlers allocate an ACTION job and log only
// — no codec writes.  On ESP builds they mutate the in-memory DSP
// state (in fw_settings.c) and push it to the codec immediately;
// [:fw save] persists the state to NVS.
//
// Sub-commands:
//   [:fw bass <-12..+12>]
//   [:fw treble <-12..+12>]
//   [:fw eq <1..5> <gain_db>]
//   [:fw eq preset <flat|speech|crisp|warm>]
//   [:fw eq reset]
//   [:fw eq show]
//   [:fw drc <on|off>]
//   [:fw drc preset <soft|speech|loud>]
//   [:fw spkgain <6|12|18|24>]
//   [:fw mute <on|off>]
// ================================================================

// Parse an integer dB value and reject anything outside min..max.
// Returns true on success with *out set to the parsed value.
// Accepts an optional leading "+" and a single leading "-".
static bool parse_int_db(const char *s, int minv, int maxv, int *out)
{
    if (s == NULL || *s == '\0')
    {
        return false;
    }
    const char *p = s;
    int sign = 1;
    if (*p == '+')
    {
        p++;
    }
    else if (*p == '-')
    {
        sign = -1;
        p++;
    }
    if (*p == '\0')
    {
        return false;
    }
    int v = 0;
    for (; *p; p++)
    {
        if (!isdigit((unsigned char)*p))
        {
            return false;
        }
        v = v * 10 + (*p - '0');
        if (v > 10000)
        {
            return false;
        }
    }
    v *= sign;
    if (v < minv || v > maxv)
    {
        return false;
    }
    *out = v;
    return true;
}

// ---- Bass / Treble ---------------------------------------------
typedef struct
{
    int8_t gain_db;
    uint8_t slot; // 0 = bass, 1 = treble
} dsp_tone_action_ctx_t;

static void dsp_tone_execute(void *ctx)
{
    dsp_tone_action_ctx_t *t = (dsp_tone_action_ctx_t *)ctx;
    if (!t) return;
    LOG_I("%s action: %+d dB",
          t->slot == 0 ? "bass" : "treble", (int)t->gain_db);
#if defined(ESP_PLATFORM) && CONFIG_DTESP_DAC_TLV320
    tlv320_dsp_state_t *s = fw_settings_get_dsp();
    if (!s) return;
    if (t->slot == 0)
    {
        tlv320_dsp_state_set_bass(s, (float)t->gain_db);
    }
    else
    {
        tlv320_dsp_state_set_treble(s, (float)t->gain_db);
    }
    esp_err_t err = tlv320_apply_dsp(s);
    if (err != ESP_OK)
    {
        LOG_E("%s: apply_dsp failed (%d)",
              t->slot == 0 ? "bass" : "treble", (int)err);
    }
#endif
}

static dtesp_job_t *dsp_tone_common(int argc, const char **argv, uint8_t slot)
{
    const char *name = (slot == 0) ? "bass" : "treble";
    if (argc < 2)
    {
        LOG_W("%s: need <-12..+12>", name);
        return NULL;
    }
    int g;
    if (!parse_int_db(argv[1], -12, 12, &g))
    {
        LOG_W("%s: invalid or out-of-range '%s' (-12..+12)", name, argv[1]);
        return NULL;
    }
    dsp_tone_action_ctx_t *ctx = malloc(sizeof(*ctx));
    if (!ctx) return NULL;
    ctx->gain_db = (int8_t)g;
    ctx->slot = slot;
    return dtesp_job_alloc_action(dsp_tone_execute, ctx, free);
}

dtesp_job_t *custom_action_bass(int argc, const char **argv)
{
    return dsp_tone_common(argc, argv, 0);
}

dtesp_job_t *custom_action_treble(int argc, const char **argv)
{
    return dsp_tone_common(argc, argv, 1);
}

// ---- 5-band EQ (band / reset / show / preset) -------------------
typedef enum
{
    EQ_OP_BAND,
    EQ_OP_RESET,
    EQ_OP_SHOW,
    EQ_OP_PRESET,
} eq_op_t;

typedef struct
{
    eq_op_t op;
    uint8_t band_index; // 0..4
    int8_t  gain_db;
    char    preset_name[16];
} eq_action_ctx_t;

static void eq_execute(void *ctx)
{
    eq_action_ctx_t *e = (eq_action_ctx_t *)ctx;
    if (!e) return;

    switch (e->op)
    {
    case EQ_OP_BAND:
        LOG_I("eq action: band %u = %+d dB",
              (unsigned)(e->band_index + 1), (int)e->gain_db);
        break;
    case EQ_OP_RESET:
        LOG_I("eq action: reset");
        break;
    case EQ_OP_SHOW:
        LOG_I("eq action: show");
        break;
    case EQ_OP_PRESET:
        LOG_I("eq action: preset '%s'", e->preset_name);
        break;
    }

#if defined(ESP_PLATFORM) && CONFIG_DTESP_DAC_TLV320
    tlv320_dsp_state_t *s = fw_settings_get_dsp();
    if (!s) return;

    switch (e->op)
    {
    case EQ_OP_BAND:
        tlv320_dsp_state_set_eq_band(s, (int)e->band_index, (float)e->gain_db);
        break;
    case EQ_OP_RESET:
        tlv320_dsp_state_reset_eq(s);
        break;
    case EQ_OP_PRESET:
        if (!tlv320_dsp_state_apply_preset(s, e->preset_name))
        {
            LOG_W("eq: unknown preset '%s'", e->preset_name);
            return;
        }
        break;
    case EQ_OP_SHOW:
    {
        char buf[160];
        tlv320_dsp_state_format(s, buf, sizeof(buf));
        LOG_I("eq state: %s", buf);
        return; // show is read-only
    }
    }

    esp_err_t err = tlv320_apply_dsp(s);
    if (err != ESP_OK)
    {
        LOG_E("eq: apply_dsp failed (%d)", (int)err);
    }
#endif
}

dtesp_job_t *custom_action_eq(int argc, const char **argv)
{
    if (argc < 2)
    {
        LOG_W("eq: need <1..5> <dB> | reset | show | preset <name>");
        return NULL;
    }

    eq_action_ctx_t *ctx = malloc(sizeof(*ctx));
    if (!ctx) return NULL;
    memset(ctx, 0, sizeof(*ctx));

    if (strcasecmp(argv[1], "reset") == 0)
    {
        ctx->op = EQ_OP_RESET;
    }
    else if (strcasecmp(argv[1], "show") == 0)
    {
        ctx->op = EQ_OP_SHOW;
    }
    else if (strcasecmp(argv[1], "preset") == 0)
    {
        if (argc < 3)
        {
            LOG_W("eq preset: need <flat|speech|crisp|warm>");
            free(ctx);
            return NULL;
        }
        // Validate up front so a bad name is rejected at parse time.
        const char *name = argv[2];
        if (strcasecmp(name, "flat")   != 0 &&
            strcasecmp(name, "speech") != 0 &&
            strcasecmp(name, "crisp")  != 0 &&
            strcasecmp(name, "warm")   != 0)
        {
            LOG_W("eq preset: unknown '%s' (flat|speech|crisp|warm)", name);
            free(ctx);
            return NULL;
        }
        ctx->op = EQ_OP_PRESET;
        strncpy(ctx->preset_name, name, sizeof(ctx->preset_name) - 1);
    }
    else
    {
        // Numeric band index: 1..5
        const char *b = argv[1];
        if (!(b[0] >= '1' && b[0] <= '5' && b[1] == '\0'))
        {
            LOG_W("eq: band must be 1..5 (got '%s')", b);
            free(ctx);
            return NULL;
        }
        if (argc < 3)
        {
            LOG_W("eq %s: need gain_db (-12..+12)", b);
            free(ctx);
            return NULL;
        }
        int g;
        if (!parse_int_db(argv[2], -12, 12, &g))
        {
            LOG_W("eq %s: invalid or out-of-range '%s' (-12..+12)",
                  b, argv[2]);
            free(ctx);
            return NULL;
        }
        ctx->op = EQ_OP_BAND;
        ctx->band_index = (uint8_t)(b[0] - '0' - 1);
        ctx->gain_db    = (int8_t)g;
    }

    return dtesp_job_alloc_action(eq_execute, ctx, free);
}

// ---- DRC --------------------------------------------------------
typedef enum
{
    DRC_OP_ON,
    DRC_OP_OFF,
    DRC_OP_PRESET,
} drc_op_t;

typedef struct
{
    drc_op_t op;
    char     preset_name[16];
} drc_action_ctx_t;

static void drc_execute(void *ctx)
{
    drc_action_ctx_t *d = (drc_action_ctx_t *)ctx;
    if (!d) return;

    switch (d->op)
    {
    case DRC_OP_ON:     LOG_I("drc action: on");                    break;
    case DRC_OP_OFF:    LOG_I("drc action: off");                   break;
    case DRC_OP_PRESET: LOG_I("drc action: preset '%s'",
                              d->preset_name);                      break;
    }

#if defined(ESP_PLATFORM) && CONFIG_DTESP_DAC_TLV320
    tlv320_dsp_state_t *s = fw_settings_get_dsp();
    if (!s) return;

    switch (d->op)
    {
    case DRC_OP_ON:
        s->drc_enabled = true;
        break;
    case DRC_OP_OFF:
        s->drc_enabled = false;
        break;
    case DRC_OP_PRESET:
    {
        tlv320_dsp_drc_preset_t p;
        if (!tlv320_dsp_drc_preset_from_name(d->preset_name, &p))
        {
            LOG_W("drc: unknown preset '%s'", d->preset_name);
            return;
        }
        s->drc_preset = p;
        s->drc_enabled = true;
        break;
    }
    }

    esp_err_t err = tlv320_apply_dsp(s);
    if (err != ESP_OK)
    {
        LOG_E("drc: apply_dsp failed (%d)", (int)err);
    }
#endif
}

dtesp_job_t *custom_action_drc(int argc, const char **argv)
{
    if (argc < 2)
    {
        LOG_W("drc: need <on|off> | preset <soft|speech|loud>");
        return NULL;
    }

    drc_action_ctx_t *ctx = malloc(sizeof(*ctx));
    if (!ctx) return NULL;
    memset(ctx, 0, sizeof(*ctx));

    if (strcasecmp(argv[1], "on") == 0 || strcmp(argv[1], "1") == 0)
    {
        ctx->op = DRC_OP_ON;
    }
    else if (strcasecmp(argv[1], "off") == 0 || strcmp(argv[1], "0") == 0)
    {
        ctx->op = DRC_OP_OFF;
    }
    else if (strcasecmp(argv[1], "preset") == 0)
    {
        if (argc < 3)
        {
            LOG_W("drc preset: need <soft|speech|loud>");
            free(ctx);
            return NULL;
        }
        const char *name = argv[2];
        if (strcasecmp(name, "soft")   != 0 &&
            strcasecmp(name, "speech") != 0 &&
            strcasecmp(name, "loud")   != 0)
        {
            LOG_W("drc preset: unknown '%s' (soft|speech|loud)", name);
            free(ctx);
            return NULL;
        }
        ctx->op = DRC_OP_PRESET;
        strncpy(ctx->preset_name, name, sizeof(ctx->preset_name) - 1);
    }
    else
    {
        LOG_W("drc: invalid '%s'", argv[1]);
        free(ctx);
        return NULL;
    }

    return dtesp_job_alloc_action(drc_execute, ctx, free);
}

// ---- Speaker analog gain ---------------------------------------
typedef struct
{
    uint8_t gain_db;
} spkgain_action_ctx_t;

static void spkgain_execute(void *ctx)
{
    spkgain_action_ctx_t *s = (spkgain_action_ctx_t *)ctx;
    if (!s) return;
    LOG_I("spkgain action: %u dB", (unsigned)s->gain_db);
#if defined(ESP_PLATFORM) && CONFIG_DTESP_DAC_TLV320
    fw_settings_set_spk_gain_db(s->gain_db);
    esp_err_t err = tlv320_set_speaker_gain_db(s->gain_db);
    if (err != ESP_OK)
    {
        LOG_E("spkgain: set failed (%d)", (int)err);
    }
#endif
}

dtesp_job_t *custom_action_spkgain(int argc, const char **argv)
{
    if (argc < 2)
    {
        LOG_W("spkgain: need <6|12|18|24>");
        return NULL;
    }
    int g;
    if (!parse_int_db(argv[1], 0, 24, &g) ||
        !(g == 6 || g == 12 || g == 18 || g == 24))
    {
        LOG_W("spkgain: invalid '%s' (allowed: 6, 12, 18, 24)", argv[1]);
        return NULL;
    }
    spkgain_action_ctx_t *ctx = malloc(sizeof(*ctx));
    if (!ctx) return NULL;
    ctx->gain_db = (uint8_t)g;
    return dtesp_job_alloc_action(spkgain_execute, ctx, free);
}

// ---- Mute -------------------------------------------------------
typedef struct
{
    uint8_t enable;
} mute_action_ctx_t;

static void mute_execute(void *ctx)
{
    mute_action_ctx_t *m = (mute_action_ctx_t *)ctx;
    if (!m) return;
    LOG_I("mute action: %s", m->enable ? "on" : "off");
#if defined(ESP_PLATFORM) && CONFIG_DTESP_DAC_TLV320
    esp_err_t err = tlv320_mute(m->enable != 0);
    if (err != ESP_OK)
    {
        LOG_E("mute: set failed (%d)", (int)err);
    }
#endif
}

dtesp_job_t *custom_action_mute(int argc, const char **argv)
{
    if (argc < 2)
    {
        LOG_W("mute: need <on|off|0|1>");
        return NULL;
    }
    int enable;
    if (strcasecmp(argv[1], "on") == 0 || strcmp(argv[1], "1") == 0)
    {
        enable = 1;
    }
    else if (strcasecmp(argv[1], "off") == 0 || strcmp(argv[1], "0") == 0)
    {
        enable = 0;
    }
    else
    {
        LOG_W("mute: invalid '%s'", argv[1]);
        return NULL;
    }
    mute_action_ctx_t *ctx = malloc(sizeof(*ctx));
    if (!ctx) return NULL;
    ctx->enable = (uint8_t)enable;
    return dtesp_job_alloc_action(mute_execute, ctx, free);
}

// ================================================================
// Action dispatch table.
//
// Extending [:fw ...]:  add a custom_action_<name>() above, then add
// one row below.  The tokenizer in custom_commands.c never needs to
// change.
// ================================================================

static const custom_cmd_entry_t action_table[] =
{
    { "gpio",       custom_action_gpio       },
    { "voice",      custom_action_voice      },
    { "rate",       custom_action_rate       },
#if !defined(ESP_PLATFORM) || \
    (defined(CONFIG_DTESP_FW_CMD_ENABLE) && defined(CONFIG_DTESP_FW_CMD_TONE_ENABLE))
    { "tone",       custom_action_tone       },
#endif
    { "volume",     custom_action_volume     },
    { "profile",    custom_action_profile    },
    { "autoswitch", custom_action_autoswitch },
    { "save",       custom_action_save       },
#if !defined(ESP_PLATFORM) || \
    (defined(CONFIG_DTESP_FW_CMD_ENABLE) && \
     (!defined(CONFIG_DTESP_FW_CMD_DSP_ENABLE) || CONFIG_DTESP_FW_CMD_DSP_ENABLE))
    // DSP / EQ / DRC / speaker-gain / mute handlers.  Per-command
    // Kconfig toggles are all defaulted-enabled; on non-ESP (host
    // test) builds every handler is compiled in so the parser test
    // suite can exercise them without a Kconfig.
    { "bass",       custom_action_bass       },
    { "treble",     custom_action_treble     },
    { "eq",         custom_action_eq         },
    { "drc",        custom_action_drc        },
    { "spkgain",    custom_action_spkgain    },
    { "mute",       custom_action_mute       },
#endif
    { NULL,         NULL                     },
};

dtesp_job_t *custom_actions_dispatch(int argc, const char **argv)
{
    if (argc < 1 || argv == NULL || argv[0] == NULL)
    {
        return NULL;
    }
    const char *name = argv[0];
    for (const custom_cmd_entry_t *e = action_table; e->name; e++)
    {
        if (strcasecmp(e->name, name) == 0)
        {
            return e->handler(argc, argv);
        }
    }
    return NULL;
}
