// SPDX-License-Identifier: MIT
// Copyright (c) 2025 Leland Lucius
// ----------------------------------------------------------------
// POSIX / libc Stubs for ESP32
//
// The upstream DECtalk library expects several POSIX APIs that are
// not provided by the ESP-IDF newlib port.  This file supplies
// minimal implementations so the library links without modification:
//
//   shmget / shmat / shmdt / shmctl - single-slot shared memory
//   nanosleep                       - FreeRTOS + ROM busy-wait
//   readlink                        - always returns ENOSYS
//   dirname                         - basic POSIX path helper
// ----------------------------------------------------------------

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if defined(CONFIG_DECTALK_DICT_PART)
#include "esp_partition.h"
#endif

// ----------------------------------------------------------------
// Single-slot shared memory emulation
//
// DECtalk uses SysV shared memory to share the kernel data segment
// between the API caller and the synthesis threads.  On ESP32 all
// threads share one address space so a single heap allocation is
// sufficient.
// ----------------------------------------------------------------

typedef struct
{
    int shmid;
    void *ptr;
} dectalk_shm_slot_t;

static dectalk_shm_slot_t g_shm_slot;
static int g_next_shmid = 1;

// Allocate a shared memory segment.  Only one segment is supported.
int shmget(key_t key, size_t size, int shmflg)
{
    (void)key;
    (void)shmflg;

    if (g_shm_slot.ptr == NULL)
    {
        g_shm_slot.ptr = calloc(1, size);
        if (g_shm_slot.ptr == NULL)
        {
            errno = ENOMEM;
            return -1;
        }
        g_shm_slot.shmid = g_next_shmid++;
    }

    return g_shm_slot.shmid;
}

// Attach to the shared memory segment and return a pointer.
void *shmat(int shmid, const void *shmaddr, int shmflg)
{
    (void)shmaddr;
    (void)shmflg;

    if (g_shm_slot.ptr == NULL || shmid != g_shm_slot.shmid)
    {
        errno = EINVAL;
        return (void *)-1;
    }

    return g_shm_slot.ptr;
}

// Detach from the shared memory segment (no-op; memory stays valid).
int shmdt(const void *shmaddr)
{
    if (g_shm_slot.ptr == NULL || shmaddr != g_shm_slot.ptr)
    {
        errno = EINVAL;
        return -1;
    }

    return 0;
}

// Destroy the shared memory segment and free the backing allocation.
int shmctl(int shmid, int cmd, void *buf)
{
    (void)cmd;
    (void)buf;

    if (g_shm_slot.ptr == NULL || shmid != g_shm_slot.shmid)
    {
        errno = EINVAL;
        return -1;
    }

    free(g_shm_slot.ptr);
    g_shm_slot.ptr = NULL;
    g_shm_slot.shmid = 0;
    return 0;
}

// ----------------------------------------------------------------
// nanosleep - sleep for the specified duration
//
// For delays >= 1 ms when the FreeRTOS scheduler is running, the
// bulk of the wait is handled by vTaskDelay() to avoid busy-waiting.
// Any remaining sub-tick remainder is consumed by the ROM busy-wait
// helper esp_rom_delay_us().
// ----------------------------------------------------------------
int nanosleep(const struct timespec *req, struct timespec *rem)
{
    if (req == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    if (req->tv_sec < 0 || req->tv_nsec < 0 || req->tv_nsec >= 1000000000L)
    {
        errno = EINVAL;
        return -1;
    }

    uint64_t total_us = (uint64_t)req->tv_nsec / 1000ULL;
    if ((uint64_t)req->tv_sec > (UINT64_MAX - total_us) / 1000000ULL)
    {
        total_us = UINT64_MAX;
    }
    else
    {
        total_us += (uint64_t)req->tv_sec * 1000000ULL;
    }

    if (total_us >= 1000ULL && xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED)
    {
        TickType_t ticks = pdMS_TO_TICKS((uint32_t)(total_us / 1000ULL));
        if (ticks > 0)
        {
            vTaskDelay(ticks);
            total_us %= 1000ULL;
        }
    }

    if (total_us > 0)
    {
        esp_rom_delay_us((uint32_t)total_us);
    }

    if (rem != NULL)
    {
        rem->tv_sec = 0;
        rem->tv_nsec = 0;
    }
    return 0;
}

// readlink - stub that always fails (no symlinks on ESP32).
ssize_t readlink(const char *path, char *buf, size_t bufsiz)
{
    (void)path;
    (void)buf;
    (void)bufsiz;
    errno = ENOSYS;
    return -1;
}

// dirname - return the directory portion of a path.
char *dirname(char *path)
{
    if (path == NULL || *path == '\0')
    {
        return ".";
    }

    char *last_slash = strrchr(path, '/');
    if (last_slash == NULL)
    {
        return ".";
    }

    if (last_slash == path)
    {
        last_slash[1] = '\0';
        return path;
    }

    *last_slash = '\0';
    return path;
}
