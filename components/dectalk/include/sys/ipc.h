// SPDX-License-Identifier: MIT
// Copyright (c) 2025 Leland Lucius
// ----------------------------------------------------------------
// Minimal sys/ipc.h for ESP32
//
// Provides the IPC flag constants required by the upstream DECtalk
// shmget() calls.  Only the values actually referenced by the
// library are defined.
// ----------------------------------------------------------------

#ifndef DECTALK_ESP32_SYS_IPC_H
#define DECTALK_ESP32_SYS_IPC_H

#define IPC_CREAT 01000
#define IPC_EXCL 02000
#define IPC_RMID 0

#endif
