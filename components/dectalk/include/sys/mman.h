// SPDX-License-Identifier: MIT
// Copyright (c) 2025 Leland Lucius
// ----------------------------------------------------------------
// Minimal sys/mman.h for ESP32
//
// Declares mmap() / munmap() prototypes and the associated flag
// constants so the upstream DECtalk source compiles without
// modification.  The actual implementations are no-ops or stubs
// in libc_stubs.c.
// ----------------------------------------------------------------

#ifndef DECTALK_ESP32_SYS_MMAN_H
#define DECTALK_ESP32_SYS_MMAN_H

#include <stddef.h>
#include <sys/types.h>

#define PROT_READ 0x1
#define PROT_WRITE 0x2
#define MAP_SHARED 0x01
#define MAP_PRIVATE 0x02
#define MAP_FAILED ((void *)-1)

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
int munmap(void *addr, size_t length);

#endif
