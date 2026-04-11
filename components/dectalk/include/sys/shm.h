// SPDX-License-Identifier: MIT
// Copyright (c) 2025 Leland Lucius
// ----------------------------------------------------------------
// Minimal sys/shm.h for ESP32
//
// Declares the SysV shared memory API used by the upstream DECtalk
// kernel to share data between the API caller and synthesis threads.
// The implementations in libc_stubs.c use a single heap allocation.
// ----------------------------------------------------------------

#ifndef DECTALK_ESP32_SYS_SHM_H
#define DECTALK_ESP32_SYS_SHM_H

#include <sys/types.h>

int shmget(key_t key, size_t size, int shmflg);
void *shmat(int shmid, const void *shmaddr, int shmflg);
int shmdt(const void *shmaddr);
int shmctl(int shmid, int cmd, void *buf);

#endif
