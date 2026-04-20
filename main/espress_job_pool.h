// SPDX-License-Identifier: MIT
// Copyright (c) 2025 Leland Lucius
// ----------------------------------------------------------------
// Job Pool Allocator — Public API
//
// Pre-allocates a fixed pool of job objects with inline text
// buffers.  Falls back to malloc when the pool is exhausted.
// Call espress_job_pool_init() once at startup, then use the
// pool_alloc / pool_free functions in place of the heap-based
// espress_job_alloc_* / espress_job_free().
// ----------------------------------------------------------------

#ifndef ESPRESS_JOB_POOL_H
#define ESPRESS_JOB_POOL_H

#include "espress_jobs.h"

#ifdef __cplusplus
extern "C" {
#endif

// Pool size: number of pre-allocated job entries.
#ifndef ESPRESS_JOB_POOL_SIZE
#define ESPRESS_JOB_POOL_SIZE 16
#endif

// Maximum text length that fits in the pool's inline buffer.
// Texts longer than this are heap-allocated (fallback).
#ifndef ESPRESS_JOB_POOL_TEXT_SIZE
#define ESPRESS_JOB_POOL_TEXT_SIZE 512
#endif

/**
 * @brief One-time pool initialization.  Call before any alloc/free.
 */
void espress_job_pool_init(void);

/**
 * @brief Allocate a SPEAK_TEXT job, preferring the pool.
 *
 * Copies `len` bytes from `text` into the job's inline buffer if it
 * fits, otherwise falls back to heap allocation.
 */
espress_job_t *espress_job_pool_alloc_text(const char *text, int len);

/**
 * @brief Allocate an ACTION job, preferring the pool.
 */
espress_job_t *espress_job_pool_alloc_action(espress_action_fn execute,
                                             void *ctx,
                                             espress_action_free_fn free_ctx);

/**
 * @brief Allocate a FLUSH job, preferring the pool.
 */
espress_job_t *espress_job_pool_alloc_flush(void);

/**
 * @brief Free a job, returning it to the pool if it originated there.
 *
 * For ACTION jobs the context free function is still called.
 * For heap-allocated jobs, delegates to espress_job_free().
 */
void espress_job_pool_free(espress_job_t *job);

#ifdef __cplusplus
}
#endif

#endif // ESPRESS_JOB_POOL_H
