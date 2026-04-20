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

#include "custom_actions.h"

// ----------------------------------------------------------------
// Platform abstraction
// ----------------------------------------------------------------
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#include "esp_log.h"
#include "driver/gpio.h"
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
    (!defined(CONFIG_CUSTOM_CMD_ENABLE) || defined(CONFIG_CUSTOM_CMD_GPIO_ENABLE))
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
