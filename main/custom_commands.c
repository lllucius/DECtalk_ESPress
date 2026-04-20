// SPDX-License-Identifier: MIT
// Copyright (c) 2025 Leland Lucius
// ----------------------------------------------------------------
// Custom Command Parser / Dispatcher — Implementation
//
// Tokenises a text buffer into an ordered sequence of SPEAK_TEXT
// and ACTION jobs.  Only [:fw ...] tokens are intercepted; all
// other [:...] tokens pass through as normal DECtalk text.
// ----------------------------------------------------------------

#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "custom_commands.h"
#include "custom_actions.h"

// ----------------------------------------------------------------
// Platform logging abstraction.  On ESP-IDF we use esp_log.h; for
// host-native tests we fall back to printf.
// ----------------------------------------------------------------
#ifdef ESP_PLATFORM
#include "esp_log.h"
static const char *TAG = "custom_cmd";
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
// Job list helpers
// ----------------------------------------------------------------

void dtesp_job_list_init(dtesp_job_list_t *list)
{
    list->jobs = NULL;
    list->count = 0;
    list->capacity = 0;
}

int dtesp_job_list_append(dtesp_job_list_t *list, dtesp_job_t *job)
{
    if (list->count >= list->capacity)
    {
        int new_cap = (list->capacity == 0) ? 4 : list->capacity * 2;
        dtesp_job_t **new_jobs = realloc(list->jobs,
                                           (size_t)new_cap * sizeof(dtesp_job_t *));
        if (!new_jobs)
        {
            return -1;
        }
        list->jobs = new_jobs;
        list->capacity = new_cap;
    }
    list->jobs[list->count++] = job;
    return 0;
}

void dtesp_job_list_free(dtesp_job_list_t *list)
{
    for (int i = 0; i < list->count; i++)
    {
        dtesp_job_free(list->jobs[i]);
    }
    free(list->jobs);
    list->jobs = NULL;
    list->count = 0;
    list->capacity = 0;
}

// ----------------------------------------------------------------
// Job allocation helpers (definition shared with dtesp_jobs.h)
// ----------------------------------------------------------------

dtesp_job_t *dtesp_job_alloc_text(const char *text, int len)
{
    dtesp_job_t *job = calloc(1, sizeof(dtesp_job_t));
    if (!job)
    {
        return NULL;
    }
    job->type = DTESP_JOB_SPEAK_TEXT;
    job->text = malloc((size_t)len + 1);
    if (!job->text)
    {
        free(job);
        return NULL;
    }
    memcpy(job->text, text, (size_t)len);
    job->text[len] = '\0';
    return job;
}

dtesp_job_t *dtesp_job_alloc_action(dtesp_action_fn execute,
                                        void *ctx,
                                        dtesp_action_free_fn free_ctx)
{
    dtesp_job_t *job = calloc(1, sizeof(dtesp_job_t));
    if (!job)
    {
        return NULL;
    }
    job->type = DTESP_JOB_ACTION;
    job->action.execute = execute;
    job->action.ctx = ctx;
    job->action.free_ctx = free_ctx;
    return job;
}

dtesp_job_t *dtesp_job_alloc_flush(void)
{
    dtesp_job_t *job = calloc(1, sizeof(dtesp_job_t));
    if (!job)
    {
        return NULL;
    }
    job->type = DTESP_JOB_FLUSH;
    return job;
}

void dtesp_job_free(dtesp_job_t *job)
{
    if (!job)
    {
        return;
    }
    switch (job->type)
    {
    case DTESP_JOB_SPEAK_TEXT:
        free(job->text);
        break;
    case DTESP_JOB_ACTION:
        if (job->action.free_ctx && job->action.ctx)
        {
            job->action.free_ctx(job->action.ctx);
        }
        break;
    case DTESP_JOB_FLUSH:
        break;
    }
    free(job);
}

// ----------------------------------------------------------------
// Argument splitter: splits `token` (the content between [: and ])
// by whitespace into argv[].  Returns argc.
// ----------------------------------------------------------------
static int split_args(const char *token, int token_len,
                      const char **argv, int max_args,
                      char *arg_buf, int arg_buf_size)
{
    int argc = 0;
    int buf_pos = 0;
    int i = 0;

    while (i < token_len && argc < max_args)
    {
        // Skip whitespace
        while (i < token_len && isspace((unsigned char)token[i]))
        {
            i++;
        }
        if (i >= token_len)
        {
            break;
        }
        // Start of argument
        argv[argc] = &arg_buf[buf_pos];
        while (i < token_len && !isspace((unsigned char)token[i]))
        {
            if (buf_pos < arg_buf_size - 1)
            {
                arg_buf[buf_pos++] = token[i];
            }
            i++;
        }
        if (buf_pos < arg_buf_size)
        {
            arg_buf[buf_pos++] = '\0';
        }
        argc++;
    }

    return argc;
}

// ----------------------------------------------------------------
// Check if a token matches the custom command namespace.
// Token points to the content after [: (e.g. "fw gpio 2 on").
// Returns 1 if it starts with the namespace, 0 otherwise.
// ----------------------------------------------------------------
static int is_custom_namespace(const char *token, int token_len)
{
    int ns_len = (int)strlen(DTESP_FW_CMD_NAMESPACE);

    if (token_len < ns_len)
    {
        return 0;
    }

    if (strncmp(token, DTESP_FW_CMD_NAMESPACE, (size_t)ns_len) != 0)
    {
        return 0;
    }

    // Must be followed by whitespace or end of token
    if (ns_len < token_len && !isspace((unsigned char)token[ns_len]))
    {
        return 0;
    }

    return 1;
}

// ----------------------------------------------------------------
// Dispatch a parsed custom command.
// argv[0] = namespace (e.g. "fw")
// argv[1] = sub-command (e.g. "gpio")
// argv[2..] = arguments
// Returns an ACTION job, or NULL if unknown/invalid.
// ----------------------------------------------------------------
static dtesp_job_t *dispatch_custom_command(int argc, const char **argv)
{
    if (argc < 2)
    {
        LOG_W("custom command with no sub-command");
        return NULL;
    }

    // Pass argc-1, argv+1 so handlers see their own name as argv[0].
    dtesp_job_t *job = custom_actions_dispatch(argc - 1, argv + 1);
    if (!job)
    {
        LOG_W("unknown or invalid custom command: [:%.16s ...]", argv[1]);
    }
    return job;
}

// ----------------------------------------------------------------
// Tokenizer: scan text for [:...] tokens, split into jobs.
//
// Native DECtalk inline commands ([:nb], [:ra 200], [:dv ...],
// [:phoneme ...], etc.) are NOT intercepted.  Only tokens whose
// content starts with the configured namespace (default "fw") are
// extracted as ACTION jobs.  This is intentional: the DECtalk
// library must continue to see its own inline commands so that
// voice, rate, phoneme, and all other features work exactly as
// they always have.
// ----------------------------------------------------------------
int custom_commands_tokenize(const char *text, dtesp_job_list_t *out)
{
    dtesp_job_list_init(out);

    // If custom commands are compiled out, just produce a single
    // text job.
#if defined(ESP_PLATFORM) && !defined(CONFIG_DTESP_FW_CMD_ENABLE)
    {
        int len = (int)strlen(text);
        if (len > 0)
        {
            dtesp_job_t *job = dtesp_job_alloc_text(text, len);
            if (!job)
            {
                return -1;
            }
            dtesp_job_list_append(out, job);
        }
        return 0;
    }
#endif

    int text_len = (int)strlen(text);
    int pos = 0;       // Current scan position
    int seg_start = 0; // Start of current plain-text segment

    while (pos < text_len)
    {
        // Look for "[:"
        if (text[pos] == '[' && pos + 1 < text_len && text[pos + 1] == ':')
        {
            // Find matching ']'
            int bracket_start = pos;
            int content_start = pos + 2; // After "[:"
            int close = -1;

            for (int j = content_start; j < text_len; j++)
            {
                if (text[j] == ']')
                {
                    close = j;
                    break;
                }
            }

            if (close < 0)
            {
                // No closing bracket found — treat as plain text.
                // This handles malformed input gracefully.
                pos++;
                continue;
            }

            int token_len = close - content_start;

            // Check if this is a custom namespace command
            if (token_len > 0 &&
                is_custom_namespace(&text[content_start], token_len))
            {
                // Enforce max token length
                if (token_len > DTESP_FW_CMD_MAX_TOKEN_LEN)
                {
                    LOG_W("custom command token too long (%d > %d), skipping",
                          token_len, DTESP_FW_CMD_MAX_TOKEN_LEN);
                    // Consume the token (don't pass to DECtalk)
                    if (bracket_start > seg_start)
                    {
                        dtesp_job_t *tj = dtesp_job_alloc_text(
                            &text[seg_start], bracket_start - seg_start);
                        if (tj)
                        {
                            dtesp_job_list_append(out, tj);
                        }
                    }
                    pos = close + 1;
                    seg_start = pos;
                    continue;
                }

                // Emit any plain text accumulated before this command
                if (bracket_start > seg_start)
                {
                    dtesp_job_t *tj = dtesp_job_alloc_text(
                        &text[seg_start], bracket_start - seg_start);
                    if (tj)
                    {
                        dtesp_job_list_append(out, tj);
                    }
                }

                // Parse and dispatch the custom command
                const char *argv[DTESP_FW_CMD_MAX_ARGS];
                char arg_buf[DTESP_FW_CMD_MAX_TOKEN_LEN + 1];
                int argc = split_args(&text[content_start], token_len,
                                      argv, DTESP_FW_CMD_MAX_ARGS,
                                      arg_buf, (int)sizeof(arg_buf));

                dtesp_job_t *action = dispatch_custom_command(argc, argv);
                if (action)
                {
                    dtesp_job_list_append(out, action);
                }
                // If dispatch returned NULL (unknown cmd), the token
                // is silently consumed — it never reaches DECtalk.

                pos = close + 1;
                seg_start = pos;
                continue;
            }

            // Not a custom command — this is a native DECtalk inline
            // command (e.g. [:nb], [:ra 200]).  Leave it in the text
            // so TextToSpeechSpeak() handles it normally.
            pos = close + 1;
            continue;
        }

        pos++;
    }

    // Emit any remaining plain text
    if (seg_start < text_len)
    {
        dtesp_job_t *tj = dtesp_job_alloc_text(
            &text[seg_start], text_len - seg_start);
        if (tj)
        {
            dtesp_job_list_append(out, tj);
        }
    }

    return 0;
}

// ----------------------------------------------------------------
// Init / session reset
// ----------------------------------------------------------------

void custom_commands_init(void)
{
    LOG_I("custom commands initialised (namespace=\"%s\", "
          "max_token=%d, max_args=%d)",
          DTESP_FW_CMD_NAMESPACE,
          DTESP_FW_CMD_MAX_TOKEN_LEN,
          DTESP_FW_CMD_MAX_ARGS);
}

void custom_commands_reset_session(void)
{
    custom_actions_reset_session();
    LOG_I("custom command session state reset");
}
