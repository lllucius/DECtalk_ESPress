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
//   DTESP_JOB_SPEAK_TEXT  – text to pass to TextToSpeechSpeak()
//   DTESP_JOB_ACTION      – firmware-side action (GPIO, etc.)
//   DTESP_JOB_FLUSH       – cancel pending speech, drain queue
// ----------------------------------------------------------------

#ifndef DTESP_JOBS_H
#define DTESP_JOBS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ----------------------------------------------------------------
// Job types
// ----------------------------------------------------------------
typedef enum
{
    DTESP_JOB_SPEAK_TEXT = 0, // Text for DECtalk synthesis
    DTESP_JOB_ACTION,         // Firmware-side custom action
    DTESP_JOB_FLUSH,          // Flush/cancel/drain signal
} dtesp_job_type_t;

// ----------------------------------------------------------------
// Action context: holds the parsed custom command and a function
// pointer to execute it.  The free_ctx callback releases any
// resources owned by the ctx pointer.
// ----------------------------------------------------------------
typedef void (*dtesp_action_fn)(void *ctx);
typedef void (*dtesp_action_free_fn)(void *ctx);

typedef struct
{
    dtesp_action_fn      execute;  // Called on the speech task
    void                  *ctx;      // Opaque context for execute()
    dtesp_action_free_fn free_ctx; // Frees ctx (may be NULL)
} dtesp_action_t;

// ----------------------------------------------------------------
// Job object: heap-allocated, queued by pointer.
// ----------------------------------------------------------------
typedef struct
{
    dtesp_job_type_t type;
    union
    {
        char            *text;   // DTESP_JOB_SPEAK_TEXT (heap-allocated)
        dtesp_action_t action; // DTESP_JOB_ACTION
    };
} dtesp_job_t;

// ----------------------------------------------------------------
// Allocation helpers.  All return NULL on allocation failure.
// ----------------------------------------------------------------

// Allocate a SPEAK_TEXT job.  Copies `len` bytes from `text` into
// an internal heap buffer (NUL-terminated).
dtesp_job_t *dtesp_job_alloc_text(const char *text, int len);

// Allocate an ACTION job with the given callbacks.
dtesp_job_t *dtesp_job_alloc_action(dtesp_action_fn execute,
                                        void *ctx,
                                        dtesp_action_free_fn free_ctx);

// Allocate a FLUSH job (no payload).
dtesp_job_t *dtesp_job_alloc_flush(void);

// Free a job and any resources it owns.
void dtesp_job_free(dtesp_job_t *job);

#ifdef __cplusplus
}
#endif

#endif // DTESP_JOBS_H
