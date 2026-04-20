// SPDX-License-Identifier: MIT
// Copyright (c) 2025 Leland Lucius
// ----------------------------------------------------------------
// Memory and Task Diagnostics
//
// Optional runtime diagnostic helpers that periodically log per-task
// stack high-water marks, heap usage, and fragmentation statistics.
// Guarded by CONFIG_DTESP_ENABLE_DIAG_MEM (Kconfig).  When the
// option is disabled the functions are still declared but compile to
// empty stubs so call sites do not need #ifdefs.
// ----------------------------------------------------------------

#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C"
{
#endif

// Print per-task stack high-water marks to the console via ESP_LOG.
void diag_mem_print_tasks(void);

// Print heap statistics (free, minimum, largest block, fragmentation).
void diag_mem_print_heap(void);

// Print both task stack and heap diagnostics in one call.
void diag_mem_print_all(void);

// FreeRTOS task function that calls diag_mem_print_all() every 10 s.
void diag_mem_task(void *arg);

// Create and start the periodic diagnostics task.
void diag_mem_start();

#ifdef __cplusplus
}
#endif

