// SPDX-License-Identifier: MIT
// Copyright (c) 2025 Leland Lucius
// ----------------------------------------------------------------
// DECtalk ESP32 Build Configuration
//
// Maps Kconfig symbols to the compile-time defines expected by the
// upstream DECtalk source tree.  Currently handles:
//
//   DECTALK_INSTALL_PREFIX - root path used by the dictionary loader
//                            to locate .dic files at runtime.
// ----------------------------------------------------------------

#ifndef DECTALK_ESP32_CONFIG_H
#define DECTALK_ESP32_CONFIG_H

#include "sdkconfig.h"

#ifdef CONFIG_DECTALK_DICT_ROOT
#define DECTALK_INSTALL_PREFIX CONFIG_DECTALK_DICT_ROOT
#else
#define DECTALK_INSTALL_PREFIX "/dict"
#endif

#include <limits.h>

#endif
