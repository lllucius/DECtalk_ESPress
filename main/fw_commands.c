// SPDX-License-Identifier: MIT
// Copyright (c) 2025 Leland Lucius
// ----------------------------------------------------------------
// Firmware Commands — [:fw ...] command parser and dispatch
//
// New commands are added by writing a handler function and appending
// an entry to the fw_cmd_table[] array.
// ----------------------------------------------------------------

#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "fw_commands.h"
#include "fw_settings.h"

#if CONFIG_DECTALK_DAC_TLV320DAC3100
#include "tlv320dac3100.h"
#endif

static const char *TAG = "FW Cmd";

// ---- Helper: skip leading whitespace ------------------------------

static const char *skip_ws(const char *s)
{
    while (*s == ' ' || *s == '\t')
    {
        s++;
    }

    return s;
}

// ---- Command handlers ---------------------------------------------
//
// Each handler receives a pointer to the argument string that follows
// the subcommand name (leading whitespace already skipped by the
// dispatcher).  The string is NUL-terminated at the closing ']'.

static void cmd_volume(const char *args)
{
    args = skip_ws(args);

    if (*args == '\0')
    {
        ESP_LOGW(TAG, "volume: missing level argument");
        return;
    }

    int level = atoi(args);
    if (level < 0)
    {
        level = 0;
    }
    else if (level > 9)
    {
        level = 9;
    }

    fw_settings_set_volume((uint8_t)level);

#if CONFIG_DECTALK_DAC_TLV320DAC3100
    tlv320dac3100_set_volume((uint8_t)level);
#endif

    ESP_LOGI(TAG, "volume set to %d", level);
}

static void cmd_profile(const char *args)
{
    args = skip_ws(args);

    if (*args == '\0')
    {
        ESP_LOGW(TAG, "profile: missing argument (speaker|headphone)");
        return;
    }

    int profile;
    if (strncasecmp(args, "headphone", 9) == 0)
    {
        profile = 1;
    }
    else if (strncasecmp(args, "speaker", 7) == 0)
    {
        profile = 0;
    }
    else
    {
        ESP_LOGW(TAG, "profile: unrecognised argument '%s'", args);
        return;
    }

    fw_settings_set_profile(profile);

#if CONFIG_DECTALK_DAC_TLV320DAC3100
    tlv320dac3100_set_profile(profile == 1
        ? TLV320_PROFILE_HEADPHONE
        : TLV320_PROFILE_SPEAKER);
#endif

    ESP_LOGI(TAG, "profile set to %s", profile ? "headphone" : "speaker");
}

static void cmd_autoswitch(const char *args)
{
    args = skip_ws(args);

    if (*args == '\0')
    {
        ESP_LOGW(TAG, "autoswitch: missing argument (on|off)");
        return;
    }

    bool enabled;
    if (strncasecmp(args, "on", 2) == 0)
    {
        enabled = true;
    }
    else if (strncasecmp(args, "off", 3) == 0)
    {
        enabled = false;
    }
    else
    {
        ESP_LOGW(TAG, "autoswitch: unrecognised argument '%s'", args);
        return;
    }

    fw_settings_set_autoswitch(enabled);

#if CONFIG_DECTALK_DAC_TLV320DAC3100
    tlv320dac3100_set_autoswitch(enabled);
#endif

    ESP_LOGI(TAG, "autoswitch %s", enabled ? "on" : "off");
}

static void cmd_save(const char *args)
{
    (void)args;
    fw_settings_save();
}

static void cmd_reset(const char *args)
{
    (void)args;
    fw_settings_reset();

    // Re-apply Kconfig defaults to the codec.
#if CONFIG_DECTALK_DAC_TLV320DAC3100
    tlv320dac3100_set_volume(fw_settings_get_volume());
    tlv320dac3100_set_profile(fw_settings_get_profile() == 1
        ? TLV320_PROFILE_HEADPHONE
        : TLV320_PROFILE_SPEAKER);
    tlv320dac3100_set_autoswitch(fw_settings_get_autoswitch());
#endif

    ESP_LOGI(TAG, "settings reset to defaults");
}

// ---- Dispatch table -----------------------------------------------
//
// Add new commands here.  The dispatcher matches the first token after
// "[:fw " against the name field (case-insensitive).

typedef struct
{
    const char *name;
    void (*handler)(const char *args);
} fw_cmd_entry_t;

static const fw_cmd_entry_t fw_cmd_table[] =
{
    { "volume",     cmd_volume     },
    { "profile",    cmd_profile    },
    { "autoswitch", cmd_autoswitch },
    { "save",       cmd_save       },
    { "reset",      cmd_reset      },
    { NULL,         NULL           },
};

// ---- Dispatcher ---------------------------------------------------

static void fw_dispatch(const char *cmd)
{
    cmd = skip_ws(cmd);

    if (*cmd == '\0')
    {
        ESP_LOGW(TAG, "empty fw command");
        return;
    }

    for (const fw_cmd_entry_t *entry = fw_cmd_table; entry->name; entry++)
    {
        size_t len = strlen(entry->name);
        if (strncasecmp(cmd, entry->name, len) == 0 &&
            (cmd[len] == ' ' || cmd[len] == '\t' || cmd[len] == '\0'))
        {
            entry->handler(cmd + len);
            return;
        }
    }

    ESP_LOGW(TAG, "unknown fw command: %s", cmd);
}

// ---- Public API ---------------------------------------------------

void fw_commands_init(void)
{
    ESP_LOGI(TAG, "fw command subsystem ready");
}

bool fw_commands_process_text(char *text)
{
    if (text == NULL || *text == '\0')
    {
        return false;
    }

    bool found = false;
    char *read = text;
    char *write = text;

    while (*read)
    {
        // Look for the "[:fw " (or "[:FW ") prefix.
        if (read[0] == '[' && read[1] == ':' &&
            (read[2] == 'f' || read[2] == 'F') &&
            (read[3] == 'w' || read[3] == 'W') &&
            (read[4] == ' ' || read[4] == '\t'))
        {
            // Find the closing ']'.
            char *end = strchr(read + 5, ']');
            if (end)
            {
                *end = '\0';
                fw_dispatch(read + 5);
                found = true;
                read = end + 1;
                continue;
            }
        }

        *write++ = *read++;
    }

    *write = '\0';

    return found;
}
