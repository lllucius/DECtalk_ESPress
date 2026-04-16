// SPDX-License-Identifier: MIT
// Copyright (c) 2025 Leland Lucius
// ----------------------------------------------------------------
// Memory and Task Diagnostics - Implementation
//
// When CONFIG_DECTALK_ENABLE_DIAG_MEM is enabled a low-priority
// background task periodically logs per-task stack high-water marks
// together with heap usage and fragmentation statistics.  This is
// useful during development to size task stacks and track memory
// leaks without attaching a debugger.
//
// Requires CONFIG_FREERTOS_USE_TRACE_FACILITY=y (auto-selected by
// the Kconfig symbol).
// ----------------------------------------------------------------

#include "diag_mem.h"

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_system.h"

#if CONFIG_DECTALK_ENABLE_DIAG_MEM

static const char *TAG = "DIAG";

// Print per-task stack high-water marks using uxTaskGetSystemState().
void diag_mem_print_tasks(void)
{
    UBaseType_t task_count = uxTaskGetNumberOfTasks();

    TaskStatus_t *task_array = malloc(task_count * sizeof(TaskStatus_t));
    if (!task_array)
    {
        ESP_LOGE(TAG, "Failed to allocate task array");
        return;
    }

    uint32_t total_runtime;
    task_count = uxTaskGetSystemState(task_array, task_count, &total_runtime);

    ESP_LOGI(TAG, "---- Task Stack High Water Marks ----");

    for (UBaseType_t i = 0; i < task_count; i++)
    {
        TaskStatus_t *t = &task_array[i];

        uint32_t stack_bytes = t->usStackHighWaterMark * sizeof(StackType_t);

        ESP_LOGI(TAG,
                 "%-16s | HWM: %5u bytes | Priority: %2u",
                 t->pcTaskName,
                 (unsigned)stack_bytes,
                 (unsigned)t->uxCurrentPriority
        );
    }

    free(task_array);
}

// Print heap statistics: free/min totals, DRAM breakdown, largest
// contiguous block, and approximate fragmentation percentage.
void diag_mem_print_heap(void)
{
    uint32_t free_heap = esp_get_free_heap_size();
    uint32_t min_heap  = esp_get_minimum_free_heap_size();

    uint32_t free_8bit = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    uint32_t min_8bit  = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);

    uint32_t largest   = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);

    ESP_LOGI(TAG, "---- Heap Stats ----");
    ESP_LOGI(TAG, "Heap current/min: %u / %u bytes",
             (unsigned)free_heap,
             (unsigned)min_heap);

    ESP_LOGI(TAG, "DRAM current/min: %u / %u bytes",
             (unsigned)free_8bit,
             (unsigned)min_8bit);

    ESP_LOGI(TAG, "Largest free block: %u bytes", (unsigned)largest);

    if (free_8bit > 0)
    {
        float frag = 100.0f * (1.0f - ((float)largest / (float)free_8bit));
        ESP_LOGI(TAG, "Fragmentation (approx): %.1f%%", frag);
    }
}

// Print both task stack and heap diagnostics in one call.
void diag_mem_print_all(void)
{
    diag_mem_print_tasks();
    diag_mem_print_heap();
}

// FreeRTOS task that prints all diagnostics every 10 seconds.
void diag_mem_task(void *arg)
{
    while (1)
    {
        diag_mem_print_all();
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

// Create and start the periodic diagnostics task at priority 1.
void diag_mem_start()
{
    xTaskCreate(diag_mem_task, "diag", 4096, NULL, 1, NULL);
}

#endif
