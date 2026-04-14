// SPDX-License-Identifier: MIT
// Copyright (c) 2025 Leland Lucius
// ----------------------------------------------------------------
// GNU ld Symbol Wrapping for DECtalk Thread Naming
//
// This file is intentionally local to the ESP-IDF project so we can
// assign meaningful names to each pthread created by the DECtalk
// engine without modifying upstream sources.
//
// StartDecTalkSystemThread() is declared static in ttsapi.c, so it
// cannot be intercepted with --wrap.  Instead we wrap the non-static
// OP_CreateThread() that it calls to actually create pthreads.  The
// ThreadRoutine pointer is compared against known DECtalk thread
// entry functions to determine the thread's purpose and assign a
// descriptive name via esp_pthread_set_cfg().
//
// Requires the final link to include:
//   -Wl,--wrap=OP_CreateThread
// ----------------------------------------------------------------

#include "esp_log.h"
#include "esp_pthread.h"

#include "opthread.h"

static const char *TAG = "DECtalk Thread Wrapper";

// Forward declarations of DECtalk thread entry functions.
// Actual signatures use LPTTS_HANDLE_T, but we only need the
// addresses for comparison with the ThreadRoutine pointer.
extern void sync_main(void *);
extern void vtm_main(void *);
extern void ph_main(void *);
extern void lts_main(void *);
extern void cmd_main(void *);
// TextToSpeechThreadMain is static in ttsapi.c, so it cannot be
// referenced directly.  It is identified by elimination below.

// Provided by the linker via --wrap
extern HTHREAD_T __real_OP_CreateThread(THREAD_STACK_SIZE_T StackSize,
                                        THREAD_PROCEDURE_T ThreadRoutine,
                                        void *pThreadData);

// ----------------------------------------------------------------
// __wrap_OP_CreateThread()
//
// Wrapped replacement for the upstream OP_CreateThread().  Compares
// the ThreadRoutine pointer against the known DECtalk thread entry
// functions to determine the thread name.  When a match is found
// (or when the routine is the remaining TextToSpeechThreadMain),
// the name is set via esp_pthread_set_cfg() before calling the
// real OP_CreateThread(), then the configuration is restored.
//
// Arguments:
//   StackSize      Thread stack size (0 = default)
//   ThreadRoutine  Thread entry function pointer
//   pThreadData    Opaque data passed to the thread (typically phTTS)
//
// Returns:
//   HTHREAD_T handle, or NULL on failure (same as OP_CreateThread)
// ----------------------------------------------------------------
HTHREAD_T __wrap_OP_CreateThread(THREAD_STACK_SIZE_T StackSize,
                                 THREAD_PROCEDURE_T ThreadRoutine,
                                 void *pThreadData)
{
    // Match ThreadRoutine to known DECtalk thread functions
    const char *name = NULL;
    size_t stack = CONFIG_PTHREAD_TASK_STACK_SIZE_DEFAULT;

    if (ThreadRoutine == (THREAD_PROCEDURE_T)sync_main)
    {
        name = "dt_sync";
    }
    else if (ThreadRoutine == (THREAD_PROCEDURE_T)vtm_main)
    {
        name = "dt_vtm";
    }
    else if (ThreadRoutine == (THREAD_PROCEDURE_T)ph_main)
    {
        name = "dt_ph";
    }
    else if (ThreadRoutine == (THREAD_PROCEDURE_T)lts_main)
    {
        name = "dt_lts";
    }
    else if (ThreadRoutine == (THREAD_PROCEDURE_T)cmd_main)
    {
        name = "dt_cmd";
        stack = 65536;
    }
    else
    {
        // The only remaining caller in the compiled sources is
        // StartDecTalkSystemThread() with the static
        // TextToSpeechThreadMain, so this must be the TXT thread.
        name = "dt_txt";
    }

    ESP_LOGI(TAG, "Creating DECtalk thread: %s", name);

    // Save the current pthread configuration
    esp_pthread_cfg_t old_cfg = esp_pthread_get_default_config();
    bool had_cfg = (esp_pthread_get_cfg(&old_cfg) == ESP_OK);

    // Apply thread name via esp_pthread_set_cfg so the next
    // pthread_create() (inside __real_OP_CreateThread) picks it up
    esp_pthread_cfg_t new_cfg = esp_pthread_get_default_config();
    if (had_cfg)
    {
        new_cfg = old_cfg;
    }
    new_cfg.thread_name = name;
    new_cfg.stack_size = stack;
    esp_pthread_set_cfg(&new_cfg);

    // Create the thread with the configured name
    HTHREAD_T result = __real_OP_CreateThread(StackSize, ThreadRoutine,
                                              pThreadData);

    // Restore the previous pthread configuration
    if (had_cfg)
    {
        esp_pthread_set_cfg(&old_cfg);
    }
    else
    {
        esp_pthread_cfg_t default_cfg = esp_pthread_get_default_config();
        esp_pthread_set_cfg(&default_cfg);
    }

    return result;
}
