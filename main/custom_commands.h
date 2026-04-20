// SPDX-License-Identifier: MIT
// Copyright (c) 2025 Leland Lucius
// ----------------------------------------------------------------
// Custom Command Parser / Dispatcher
//
// Scans a text buffer for bracket-colon tokens of the form
// [:fw ...] and splits the buffer into an ordered sequence of
// SPEAK_TEXT and ACTION jobs.
//
// Design notes:
//   - Only tokens whose namespace matches the configured prefix
//     (default "fw") are intercepted.  All other [:...] tokens
//     are native DECtalk inline commands and are left in the text
//     so that TextToSpeechSpeak() processes them normally.
//   - Ordering is preserved: text before a custom command becomes
//     one SPEAK_TEXT job, the command becomes an ACTION job, and
//     text after becomes another SPEAK_TEXT job.
//   - Parsing is bounded: fixed max token length, fixed max arg
//     count, no recursion.
//   - Unknown [:fw ...] commands are consumed and logged, never
//     passed to DECtalk.
// ----------------------------------------------------------------

#ifndef CUSTOM_COMMANDS_H
#define CUSTOM_COMMANDS_H

#include "espress_jobs.h"

#ifdef __cplusplus
extern "C" {
#endif

// ----------------------------------------------------------------
// Configuration defaults (overridden by Kconfig when available)
// ----------------------------------------------------------------
#ifdef CONFIG_ESPRESS_FW_CMD_NAMESPACE
#define ESPRESS_FW_CMD_NAMESPACE  CONFIG_ESPRESS_FW_CMD_NAMESPACE
#else
#define ESPRESS_FW_CMD_NAMESPACE  "fw"
#endif

#ifdef CONFIG_ESPRESS_FW_CMD_MAX_TOKEN_LEN
#define ESPRESS_FW_CMD_MAX_TOKEN_LEN  CONFIG_ESPRESS_FW_CMD_MAX_TOKEN_LEN
#else
#define ESPRESS_FW_CMD_MAX_TOKEN_LEN  128
#endif

#ifdef CONFIG_ESPRESS_FW_CMD_MAX_ARGS
#define ESPRESS_FW_CMD_MAX_ARGS  CONFIG_ESPRESS_FW_CMD_MAX_ARGS
#else
#define ESPRESS_FW_CMD_MAX_ARGS  8
#endif

// ----------------------------------------------------------------
// Job list: dynamically grown array of job pointers returned by
// the tokenizer.
// ----------------------------------------------------------------
typedef struct
{
    espress_job_t **jobs;
    int             count;
    int             capacity;
} espress_job_list_t;

// Initialise an empty job list.
void espress_job_list_init(espress_job_list_t *list);

// Append a job to the list (grows capacity as needed).
// Returns 0 on success, -1 on allocation failure.
int espress_job_list_append(espress_job_list_t *list, espress_job_t *job);

// Free all jobs in the list and the list storage itself.
void espress_job_list_free(espress_job_list_t *list);

// ----------------------------------------------------------------
// Tokenizer / dispatcher
//
// Scans `text` (NUL-terminated) and populates `out` with an
// ordered sequence of jobs.  Returns 0 on success.
//
// The caller must eventually call espress_job_list_free() on `out`
// if it does not consume all jobs.
// ----------------------------------------------------------------
int custom_commands_tokenize(const char *text, espress_job_list_t *out);

// ----------------------------------------------------------------
// Command dispatch table entry
// ----------------------------------------------------------------
typedef struct
{
    const char *name;
    // Handler receives argc/argv (argv[0] is the sub-command name).
    // Returns an ACTION job, or NULL if the command is a no-op.
    espress_job_t *(*handler)(int argc, const char **argv);
} custom_cmd_entry_t;

// ----------------------------------------------------------------
// Initialise custom command subsystem (register handlers, etc.)
// Called once at startup.
// ----------------------------------------------------------------
void custom_commands_init(void);

// ----------------------------------------------------------------
// Reset any session-level custom command state (e.g. stored voice
// prefix).  Called on reconnect when configured.
// ----------------------------------------------------------------
void custom_commands_reset_session(void);

#ifdef __cplusplus
}
#endif

#endif // CUSTOM_COMMANDS_H
