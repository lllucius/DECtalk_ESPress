// SPDX-License-Identifier: MIT
// Copyright (c) 2025 Leland Lucius
// ----------------------------------------------------------------
// ESPress Job Queue Types
//
// Typed job objects that replace the raw char* + sentinel queue
// payload.  The speech task processes jobs in FIFO order, so the
// original ordering of speech text and firmware actions is preserved.
//
// Job types:
//   ESPRESS_JOB_SPEAK_TEXT  – text to pass to TextToSpeechSpeak()
//   ESPRESS_JOB_ACTION      – firmware-side action (GPIO, etc.)
//   ESPRESS_JOB_FLUSH       – cancel pending speech, drain queue
// ----------------------------------------------------------------

#ifndef ESPRESS_JOBS_H
#define ESPRESS_JOBS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ----------------------------------------------------------------
// Job types
// ----------------------------------------------------------------
typedef enum
{
    ESPRESS_JOB_SPEAK_TEXT = 0, // Text for DECtalk synthesis
    ESPRESS_JOB_ACTION,         // Firmware-side custom action
    ESPRESS_JOB_FLUSH,          // Flush/cancel/drain signal
} espress_job_type_t;

// ----------------------------------------------------------------
// Action context: holds the parsed custom command and a function
// pointer to execute it.  The free_ctx callback releases any
// resources owned by the ctx pointer.
// ----------------------------------------------------------------
typedef void (*espress_action_fn)(void *ctx);
typedef void (*espress_action_free_fn)(void *ctx);

typedef struct
{
    espress_action_fn      execute;  // Called on the speech task
    void                  *ctx;      // Opaque context for execute()
    espress_action_free_fn free_ctx; // Frees ctx (may be NULL)
} espress_action_t;

// ----------------------------------------------------------------
// Job object: heap-allocated, queued by pointer.
// ----------------------------------------------------------------
typedef struct
{
    espress_job_type_t type;
    union
    {
        char            *text;   // ESPRESS_JOB_SPEAK_TEXT (heap-allocated)
        espress_action_t action; // ESPRESS_JOB_ACTION
    };
} espress_job_t;

// ----------------------------------------------------------------
// Allocation helpers.  All return NULL on allocation failure.
// ----------------------------------------------------------------

// Allocate a SPEAK_TEXT job.  Copies `len` bytes from `text` into
// an internal heap buffer (NUL-terminated).
espress_job_t *espress_job_alloc_text(const char *text, int len);

// Allocate an ACTION job with the given callbacks.
espress_job_t *espress_job_alloc_action(espress_action_fn execute,
                                        void *ctx,
                                        espress_action_free_fn free_ctx);

// Allocate a FLUSH job (no payload).
espress_job_t *espress_job_alloc_flush(void);

// Free a job and any resources it owns.
void espress_job_free(espress_job_t *job);

#ifdef __cplusplus
}
#endif

#endif // ESPRESS_JOBS_H
