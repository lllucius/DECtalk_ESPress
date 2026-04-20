// SPDX-License-Identifier: MIT
// Copyright (c) 2025 Leland Lucius
// ----------------------------------------------------------------
// Job Pool Allocator
//
// Pre-allocates a fixed number of espress_job_t objects with
// embedded text buffers to eliminate per-job heap allocation from
// the hot path.  When the pool is exhausted, falls back to malloc.
//
// The pool is lock-free using a FreeRTOS queue of pointers (a
// bounded LIFO free-list).  Jobs are returned to the pool by
// espress_job_free() when they came from the pool.
// ----------------------------------------------------------------

#include "espress_job_pool.h"

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "job_pool";

// Pool entry: job struct + inline text buffer.
typedef struct
{
    espress_job_t job;
    char text_buf[ESPRESS_JOB_POOL_TEXT_SIZE];
    uint8_t from_pool; // 1 if this entry came from the pool
} pool_entry_t;

static pool_entry_t s_pool[ESPRESS_JOB_POOL_SIZE];
static QueueHandle_t s_free_list;

void espress_job_pool_init(void)
{
    s_free_list = xQueueCreate(ESPRESS_JOB_POOL_SIZE, sizeof(pool_entry_t *));
    if (s_free_list == NULL)
    {
        ESP_LOGE(TAG, "Failed to create free list queue");
        return;
    }

    for (int i = 0; i < ESPRESS_JOB_POOL_SIZE; i++)
    {
        s_pool[i].from_pool = 1;
        pool_entry_t *p = &s_pool[i];
        xQueueSend(s_free_list, &p, 0);
    }

    ESP_LOGI(TAG, "Job pool initialized: %d entries, %d bytes text each",
             ESPRESS_JOB_POOL_SIZE, ESPRESS_JOB_POOL_TEXT_SIZE);
}

espress_job_t *espress_job_pool_alloc_text(const char *text, int len)
{
    // Try the pool first for short texts that fit in the inline buffer.
    if (len < ESPRESS_JOB_POOL_TEXT_SIZE && s_free_list != NULL)
    {
        pool_entry_t *entry = NULL;
        if (xQueueReceive(s_free_list, &entry, 0) == pdTRUE)
        {
            entry->job.type = ESPRESS_JOB_SPEAK_TEXT;
            memcpy(entry->text_buf, text, (size_t)len);
            entry->text_buf[len] = '\0';
            entry->job.text = entry->text_buf;
            return &entry->job;
        }
    }

    // Fallback to heap allocation
    return espress_job_alloc_text(text, len);
}

espress_job_t *espress_job_pool_alloc_action(espress_action_fn execute,
                                             void *ctx,
                                             espress_action_free_fn free_ctx)
{
    // Actions use the pool entry without the text buffer.
    if (s_free_list != NULL)
    {
        pool_entry_t *entry = NULL;
        if (xQueueReceive(s_free_list, &entry, 0) == pdTRUE)
        {
            entry->job.type = ESPRESS_JOB_ACTION;
            entry->job.action.execute = execute;
            entry->job.action.ctx = ctx;
            entry->job.action.free_ctx = free_ctx;
            return &entry->job;
        }
    }

    return espress_job_alloc_action(execute, ctx, free_ctx);
}

espress_job_t *espress_job_pool_alloc_flush(void)
{
    if (s_free_list != NULL)
    {
        pool_entry_t *entry = NULL;
        if (xQueueReceive(s_free_list, &entry, 0) == pdTRUE)
        {
            entry->job.type = ESPRESS_JOB_FLUSH;
            return &entry->job;
        }
    }

    return espress_job_alloc_flush();
}

void espress_job_pool_free(espress_job_t *job)
{
    if (!job)
    {
        return;
    }

    // Check if this job came from the pool.  We detect this by checking
    // if the pointer falls within our static pool array.
    pool_entry_t *entry = (pool_entry_t *)((char *)job - offsetof(pool_entry_t, job));
    if ((char *)entry >= (char *)&s_pool[0] &&
        (char *)entry < (char *)&s_pool[ESPRESS_JOB_POOL_SIZE])
    {
        // For ACTION jobs, free the context before returning to pool.
        if (job->type == ESPRESS_JOB_ACTION &&
            job->action.free_ctx && job->action.ctx)
        {
            job->action.free_ctx(job->action.ctx);
        }
        // Return to pool (best-effort; if queue is full, leak is impossible
        // since pool size == queue size).
        xQueueSend(s_free_list, &entry, 0);
        return;
    }

    // Not from pool — use standard free.
    espress_job_free(job);
}

#endif // ESP_PLATFORM
