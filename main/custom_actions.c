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

#include "custom_actions.h"
#include "custom_commands.h"

// ----------------------------------------------------------------
// Platform abstraction
// ----------------------------------------------------------------
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#include "esp_log.h"
#include "driver/gpio.h"
#if CONFIG_DECTALK_DAC_TLV320DAC3100
#include "tlv320dac3100.h"
#include "fw_settings.h"
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
    (!defined(CONFIG_DECTALK_FW_CMD_ENABLE) || defined(CONFIG_DECTALK_FW_CMD_GPIO_ENABLE))
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

espress_job_t *custom_action_gpio(int argc, const char **argv)
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
# ifdef CONFIG_DECTALK_I2S_BCK_GPIO
        CONFIG_DECTALK_I2S_BCK_GPIO,
# endif
# ifdef CONFIG_DECTALK_I2S_WS_GPIO
        CONFIG_DECTALK_I2S_WS_GPIO,
# endif
# ifdef CONFIG_DECTALK_I2S_DO_GPIO
        CONFIG_DECTALK_I2S_DO_GPIO,
# endif
# ifdef CONFIG_DECTALK_I2S_MCLK_GPIO
        CONFIG_DECTALK_I2S_MCLK_GPIO,
# endif
# ifdef CONFIG_DECTALK_I2C_SDA_GPIO
        CONFIG_DECTALK_I2C_SDA_GPIO,
# endif
# ifdef CONFIG_DECTALK_I2C_SCL_GPIO
        CONFIG_DECTALK_I2C_SCL_GPIO,
# endif
# ifdef CONFIG_DECTALK_CODEC_INT_GPIO
        CONFIG_DECTALK_CODEC_INT_GPIO,
# endif
# ifdef CONFIG_DECTALK_CODEC_RESET_GPIO
        CONFIG_DECTALK_CODEC_RESET_GPIO,
# endif
# ifdef CONFIG_DECTALK_RGB_LED_GPIO
        CONFIG_DECTALK_RGB_LED_GPIO,
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

    return espress_job_alloc_action(gpio_execute, ctx, free);
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

espress_job_t *custom_action_voice(int argc, const char **argv)
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
            return espress_job_alloc_action(voice_execute, ctx, free);
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

espress_job_t *custom_action_rate(int argc, const char **argv)
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
    return espress_job_alloc_action(rate_execute, ctx, free);
}

// ================================================================
// Tone handler: [:fw tone <freq_hz> <duration_ms>]
//
// TODO: This is a stub.  A real implementation needs to know which
// audio path to use (I2S direct, LEDC PWM, dedicated tone GPIO,
// etc.), which is board-specific.  For now it logs the request.
// ================================================================

typedef struct
{
    int freq_hz;
    int duration_ms;
} tone_action_ctx_t;

static void tone_execute(void *ctx)
{
    tone_action_ctx_t *t = (tone_action_ctx_t *)ctx;
    LOG_I("tone action: freq=%d Hz, duration=%d ms (STUB — not implemented)",
          t->freq_hz, t->duration_ms);
    // TODO: Implement tone generation for the target hardware.
    // Options include:
    //   - LEDC PWM output to a GPIO connected to a speaker/buzzer
    //   - Mixing a sine wave into the I2S audio stream
    //   - Using the DAC peripheral if available
    // The correct approach depends on the board hardware.
}

espress_job_t *custom_action_tone(int argc, const char **argv)
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

    return espress_job_alloc_action(tone_execute, ctx, free);
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
#if defined(ESP_PLATFORM) && CONFIG_DECTALK_DAC_TLV320DAC3100
    fw_settings_set_volume(v->level);
    tlv320dac3100_set_volume(v->level);
#endif
}

espress_job_t *custom_action_volume(int argc, const char **argv)
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
    return espress_job_alloc_action(volume_execute, ctx, free);
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
#if defined(ESP_PLATFORM) && CONFIG_DECTALK_DAC_TLV320DAC3100
    tlv320_profile_t tp = (p->profile == 1)
                              ? TLV320_PROFILE_HEADPHONE
                              : TLV320_PROFILE_SPEAKER;
    fw_settings_set_profile((uint8_t)tp);
    esp_err_t err = tlv320dac3100_set_profile(tp);
    if (err != ESP_OK)
    {
        LOG_E("profile: codec set_profile failed (%d)", (int)err);
    }
#endif
}

espress_job_t *custom_action_profile(int argc, const char **argv)
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
    return espress_job_alloc_action(profile_execute, ctx, free);
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
#if defined(ESP_PLATFORM) && CONFIG_DECTALK_DAC_TLV320DAC3100
    fw_settings_set_autoswitch(a->enable ? 1 : 0);
    tlv320dac3100_set_autoswitch(a->enable ? true : false);
#endif
}

espress_job_t *custom_action_autoswitch(int argc, const char **argv)
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
    return espress_job_alloc_action(autoswitch_execute, ctx, free);
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
#if defined(ESP_PLATFORM) && CONFIG_DECTALK_DAC_TLV320DAC3100
    esp_err_t err = fw_settings_save();
    if (err != ESP_OK)
    {
        LOG_E("save: fw_settings_save failed (%d)", (int)err);
    }
#endif
}

espress_job_t *custom_action_save(int argc, const char **argv)
{
    (void)argc;
    (void)argv;
    return espress_job_alloc_action(save_execute, NULL, NULL);
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
    (defined(CONFIG_DECTALK_FW_CMD_ENABLE) && defined(CONFIG_DECTALK_FW_CMD_TONE_ENABLE))
    { "tone",       custom_action_tone       },
#endif
    { "volume",     custom_action_volume     },
    { "profile",    custom_action_profile    },
    { "autoswitch", custom_action_autoswitch },
    { "save",       custom_action_save       },
    { NULL,         NULL                     },
};

espress_job_t *custom_actions_dispatch(int argc, const char **argv)
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
