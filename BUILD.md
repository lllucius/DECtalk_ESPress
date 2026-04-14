<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2025 Leland Lucius -->

# DECtalk ESPress Firmware — Build Process & Architecture

This document describes the build system, source layout, and internal
architecture of the DECtalk ESPress firmware.

For the DECtalk component build process (dapi source compilation, dictionary
cross-compilation, porting notes), see the
[component BUILD.md](components/dectalk/BUILD.md).

## Table of Contents

- [Prerequisites](#prerequisites)
  - [Installing ESP-IDF](#installing-esp-idf)
  - [Cloning the Repository](#cloning-the-repository)
- [Directory Layout](#directory-layout)
- [How the Build Works](#how-the-build-works)
  - [1. Project Bootstrapping](#1-project-bootstrapping)
  - [2. Component: `dectalk` (the TTS library)](#2-component-dectalk-the-tts-library)
  - [3. Component: `main` (Firmware Application)](#3-component-main-firmware-application)
  - [4. Partition Table](#4-partition-table)
  - [5. `sdkconfig.defaults`](#5-sdkconfigdefaults)
  - [6. `sdkconfig.devel` (Development Overrides)](#6-sdkconfigdevel-development-overrides)
  - [7. Combining sdkconfig Files](#7-combining-sdkconfig-files)
- [Firmware Architecture](#firmware-architecture)
  - [Thread Model](#thread-model)
  - [Data Flow](#data-flow)
  - [TTS In-Memory Mode](#tts-in-memory-mode)
  - [USB CDC Transport](#usb-cdc-transport)
  - [Dictionary Loading](#dictionary-loading)
  - [Flow Control](#flow-control)
  - [Idle Flush](#idle-flush)
- [Build Commands Reference](#build-commands-reference)
  - [Changing Language](#changing-language)
  - [Changing Dictionary Storage Mode](#changing-dictionary-storage-mode)
- [Porting Notes](#porting-notes)

---

## Prerequisites

| Requirement | Version | Notes |
|-------------|---------|-------|
| **ESP-IDF** | v6.0+ (tested with v6.0) | The `sdkconfig.defaults` header references ESP-IDF 6.0 |
| **Python** | 3.8+ | Required by ESP-IDF tools |
| **Host C compiler** | `cc` or `gcc` | Used at build time to compile the dictionary compiler that runs on the host |
| **CMake** | 3.5+ | Bundled with ESP-IDF |
| **Ninja** | any | Bundled with ESP-IDF |

### Installing ESP-IDF

#### Linux / macOS

```bash
# Install system dependencies (Ubuntu/Debian)
sudo apt-get install git wget flex bison gperf python3 python3-pip \
    python3-venv cmake ninja-build ccache libffi-dev libssl-dev \
    dfu-util libusb-1.0-0

# Clone ESP-IDF
mkdir -p ~/esp && cd ~/esp
git clone --recursive https://github.com/espressif/esp-idf.git
cd esp-idf
git checkout v6.0   # or latest stable release

# Install toolchains (ESP32-S3 target)
./install.sh esp32s3

# Activate the environment (run in every new shell, or add to .bashrc)
. ~/esp/esp-idf/export.sh
```

#### Windows

Use the [ESP-IDF Windows Installer](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/windows-setup.html)
which bundles Git, Python, CMake, Ninja, and the Xtensa/RISC-V toolchains.

### Cloning the Repository

The upstream DECtalk source tree is included as a Git submodule at
`components/dectalk/dectalk`.  You must initialise it when you clone:

```bash
git clone --recursive https://github.com/lllucius/DECtalk_ESPress.git
cd DECtalk_ESPress
```

If you already cloned without `--recursive`, pull the submodule manually:

```bash
git submodule update --init --recursive
```

---

## Directory Layout

```
DECtalk_ESPress/
├── CMakeLists.txt                  # Top-level ESP-IDF project file
├── sdkconfig.defaults              # Default Kconfig values (target, flash, PSRAM, TinyUSB…)
├── sdkconfig.devel                 # Optional development overrides (diagnostics, PSRAM, debugging)
├── partitions.csv                  # Custom partition table
├── BUILD.md                        # ← this file (firmware build & architecture)
├── README.md                       # Firmware overview and quick-start
│
├── components/
│   └── dectalk/                    # ESP-IDF component wrapping the upstream dapi library
│       ├── CMakeLists.txt          # Compiles all dapi sources + local stubs; builds dictionary
│       ├── Kconfig.projbuild       # menuconfig: language, dict storage, source path (DECtalk menu)
│       ├── README.md               # Component overview, Kconfig settings, dictionary modes
│       ├── BUILD.md                # Component build process, dapi compilation, porting notes
│       ├── project_include.cmake   # Registers custom partition subtypes; manages partition CSV
│       ├── include/
│       │   ├── config.h            # Maps Kconfig DECTALK_DICT_ROOT → DECTALK_INSTALL_PREFIX
│       │   └── sys/
│       │       ├── ipc.h           # Minimal IPC_CREAT / IPC_RMID stubs
│       │       ├── mman.h          # mmap/munmap prototypes + MAP_FAILED constant
│       │       └── shm.h           # shmget/shmat/shmdt/shmctl prototypes
│       └── src/
│           ├── libc_stubs.c        # shmget/shmat/shmdt/shmctl, nanosleep, readlink, dirname
│           └── loaddict_wrappers.c # __wrap_load_dictionary / __wrap_unload_dictionary
│
├── main/                           # Main application component
│   ├── CMakeLists.txt              # Registers main sources; depends on dectalk, driver, pthread…
│   ├── Kconfig.projbuild           # menuconfig: audio, tuning, CDC, diagnostics (ESPress menu)
│   ├── idf_component.yml           # IDF component manager dep: espressif/esp_tinyusb ≥ 2.0.0
│   ├── dectalk_espress.c           # Entry point (app_main), I2S init, threads, ESPress protocol
│   ├── dectalk_espress.h           # Protocol constants, DLE encode/decode, public API
│   ├── usb_cdc_transport.c         # USB CDC-ACM transport layer (TinyUSB wrapper)
│   ├── usb_cdc_transport.h         # Transport API: init, read, write, connected, reconnect
│   ├── diag_mem.c                  # Optional heap/stack diagnostics task
│   └── diag_mem.h                  # Diagnostics API
│
└── host/                           # Python host-side tools
    ├── README.md                   # Host tools documentation
    ├── dectalk_serial.py           # DECtalkESPressSerial class (serial protocol API)
    └── dectalk_espress_gui.py      # Tkinter GUI for voice control, status, pause/resume
```

---

## How the Build Works

### 1. Project Bootstrapping

`CMakeLists.txt` is a minimal ESP-IDF project file:

```cmake
cmake_minimum_required(VERSION 3.5)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(dectalk_espress)
```

ESP-IDF discovers the `components/dectalk/` and `main/` components
automatically.

### 2. Component: `dectalk` (the TTS library)

The DECtalk component handles source resolution, language selection,
dictionary cross-compilation, and dapi library compilation.  For full
details see the
[component BUILD.md](components/dectalk/BUILD.md).

### 3. Component: `main` (Firmware Application)

The `main/` component contains the application logic:

| File | Role |
|------|------|
| `dectalk_espress.c` | Entry point (`app_main`), I2S initialisation, thread creation, ESPress protocol loop, speech task, TTS callback |
| `usb_cdc_transport.c` | TinyUSB CDC-ACM driver: RX stream buffer, DTR-based connection tracking, reconnection detection |
| `diag_mem.c` | Optional diagnostic task enabled from `idf.py menuconfig` that logs stack HWM and heap stats every 10 s |

Dependencies declared in `CMakeLists.txt`:
- `dectalk` — the TTS library component
- `driver` — ESP-IDF I2S driver
- `pthread` — POSIX threading
- `spiffs` — SPIFFS file system (for dictionary-from-filesystem mode)
- `esp_timer` — High-resolution timer

External dependency via `idf_component.yml`:
- `espressif/esp_tinyusb ≥ 2.0.0` — TinyUSB CDC-ACM for native USB

### 4. Partition Table

`partitions.csv` defines a custom layout:

| Name | Type | SubType | Size | Purpose |
|------|------|---------|------|---------|
| `nvs` | data | nvs | 24 KB | Non-volatile storage |
| `phy_init` | data | phy | 4 KB | PHY calibration data |
| `factory` | app | factory | 2 MB | Application firmware |

Additional partitions can be added for dictionary storage:
- A `udict` data partition (subtype `0x40`) when using partition-based
  dictionary storage — this can be created automatically via
  `CONFIG_DECTALK_AUTOCREATE_PARTITIONS`.
- A `storage` SPIFFS partition when using file-system-based dictionary
  loading (commented out by default).

The `project_include.cmake` file registers the custom `udict` subtype
(`0x40`) with ESP-IDF and manages dynamic partition table extension.

### 5. `sdkconfig.defaults`

These are the minimal settings required for the project.  ESP-IDF applies
them automatically whenever the `sdkconfig` file is created or recreated
(e.g. after `idf.py fullclean` or `idf.py set-target`):

| Setting | Value | Rationale |
|---------|-------|-----------|
| `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240` | `y` | Maximum CPU clock for synthesis performance |
| `CONFIG_ESP_TASK_WDT_EN` | `n` | Task watchdog disabled (speech synthesis is CPU-intensive) |
| `CONFIG_IDF_TARGET` | `esp32s3` | Target SoC |
| `CONFIG_PARTITION_TABLE_CUSTOM` | `y` | Use the project's `partitions.csv` |
| `CONFIG_PTHREAD_TASK_STACK_SIZE_DEFAULT` | `8192` | Default pthread stack (8 KB) |
| `CONFIG_TINYUSB_CDC_ENABLED` | `y` | Enable TinyUSB CDC-ACM for host protocol |

### 6. `sdkconfig.devel` (Development Overrides)

`sdkconfig.devel` contains additional settings useful during development
and debugging.  These are **not** applied automatically — you must
explicitly combine them with `sdkconfig.defaults` (see
[Combining sdkconfig Files](#7-combining-sdkconfig-files) below).

| Setting | Value | Rationale |
|---------|-------|-----------|
| `CONFIG_COMPILER_STACK_CHECK_MODE_STRONG` | `y` | Strong stack-smashing detection |
| `CONFIG_DECTALK_ENABLE_DIAG_MEM` | `y` | Enable heap/stack diagnostics task |
| `CONFIG_DECTALK_LOG_LEVEL_VERBOSE` | `y` | Verbose ESP_LOG output |
| `CONFIG_ESPTOOLPY_FLASHSIZE_8MB` | `y` | 8 MB flash for firmware + dictionary |
| `CONFIG_ESPTOOLPY_HEADER_FLASHSIZE_UPDATE` | `y` | Auto-update flash size in binary header |
| `CONFIG_ESP_SYSTEM_PANIC_PRINT_HALT` | `y` | Print backtrace and halt on panic |
| `CONFIG_FREERTOS_USE_TRACE_FACILITY` | `y` | Enable FreeRTOS task trace (for diagnostics) |
| `CONFIG_HEAP_ABORT_WHEN_ALLOCATION_FAILS` | `y` | Hard-fail on OOM for easier debugging |
| `CONFIG_HEAP_POISONING_COMPREHENSIVE` | `y` | Full heap poisoning for corruption detection |
| `CONFIG_SPIRAM` | `y` | Enable PSRAM |
| `CONFIG_SPIRAM_MODE_OCTAL` | `y` | Octal SPI PSRAM |

### 7. Combining sdkconfig Files

ESP-IDF can merge multiple defaults files at configuration time using the
`-D SDKCONFIG_DEFAULTS` CMake variable.  This is useful for layering the
development overrides on top of the base defaults:

```bash
# Create (or recreate) sdkconfig with both base and devel settings:
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.devel" build
```

Settings in later files override earlier ones, so `sdkconfig.devel` values
take precedence over `sdkconfig.defaults`.

> **When do you need to do this?**  Only when the `sdkconfig` file needs to
> be created or recreated — for example after `idf.py fullclean`,
> `idf.py set-target`, or when cloning the project for the first time.
> Once `sdkconfig` exists, subsequent `idf.py build` commands reuse it and
> you do not need to pass `-D SDKCONFIG_DEFAULTS` again.  You can also
> make further changes interactively with `idf.py menuconfig` at any time.

---

## Firmware Architecture

### Thread Model

`app_main()` creates two pthreads and then returns (freeing the default
FreeRTOS task):

| Thread | Core | Stack | Role |
|--------|------|-------|------|
| `speech_thread` | CPU 1 (configurable) | default (8 KB) | Dequeues text from `speech_queue`, calls `TextToSpeechSpeak()` + `Sync()` |
| `main_thread` | any | 12 KB (configurable) | Runs the ESPress protocol loop: USB CDC reads, DLE state machine, flow control |

CPU pinning is important: the speech synthesis in `TextToSpeechSpeak()` is
compute-intensive and does not yield to the scheduler.  Pinning it to CPU 1
keeps CPU 0 free so the IDLE0 task can service the Task Watchdog Timer (even
though the watchdog is disabled in the defaults, this is defensive).

### Data Flow

```
  Host (PC)                         ESP32-S3
  ─────────                         ────────
  Serial terminal / GUI             USB CDC-ACM
       │                                │
       │  ASCII text, control chars     │
       │  DLE command sequences         │
       ├───────────────────────────────►│
       │                                ▼
       │                         main_thread (protocol loop)
       │                           ├── DLE state machine
       │                           ├── Control char handlers
       │                           ├── Text accumulation buffer
       │                           └── XON/XOFF flow control
       │                                │
       │                                │ strdup'd text chunks
       │                                ▼
       │                         speech_queue (FreeRTOS queue)
       │                                │
       │                                ▼
       │                         speech_thread
       │                           ├── TextToSpeechSpeak()
       │                           ├── TextToSpeechSync()
       │                           └── Flush / drain
       │                                │
       │                                │ TTS_MSG_BUFFER callback
       │                                ▼
       │                         espress_tts_callback()
       │                           ├── Audio samples → I2S DMA
       │                           └── Index markers → DLE INDEX
       │                                │
       │  DLE STATUS, INDEX, XON/XOFF   │
       │◄───────────────────────────────┤
       │                                │
       │                                ▼
       │                         I2S peripheral → DAC → speaker
```

### TTS In-Memory Mode

The firmware uses `TextToSpeechOpenInMemory()` with three rotating audio
buffers (16 KB each, 8192 16-bit samples per buffer).  Each buffer also
carries up to 8 index-mark slots.  When a buffer is filled, the
`espress_tts_callback()` is invoked with `TTS_MSG_BUFFER`:

1. Any embedded index marks are extracted and sent to the host as DLE INDEX
   sequences.
2. Audio samples are written to the I2S DMA ring buffer via
   `i2s_channel_write()`.
3. If speech is paused (SO received), samples are zeroed before writing.
4. The buffer is reset and re-queued with `TextToSpeechAddBuffer()`.

### USB CDC Transport

`usb_cdc_transport.c` wraps the `espressif/esp_tinyusb` CDC-ACM interface:

- **RX path:** A TinyUSB callback drains 64-byte USB bulk packets into a
  FreeRTOS stream buffer (default 4 KB).  The protocol loop calls
  `usb_cdc_transport_read()` which blocks on the stream buffer with a
  configurable timeout.
- **TX path:** `usb_cdc_transport_write()` queues data via
  `tinyusb_cdcacm_write_queue()` and flushes with a 50 ms timeout.  Writes
  are silently dropped when no host is connected (`cdc_connected == false`).
- **Connection tracking:** The line-state callback monitors DTR.  When DTR
  transitions low→high (host opens the port), a reconnection counter is
  incremented.  The protocol loop polls `usb_cdc_transport_check_reconnected()`
  each iteration and resets all protocol state on reconnection.
- **Initial boot:** `cdc_had_disconnect` starts as `true` so the first host
  connection after power-on is treated as a reconnection, triggering XON.

### Dictionary Loading

Dictionary loading is handled by the DECtalk component.  See the
[component README](components/dectalk/README.md) for dictionary storage
modes and the [component BUILD.md](components/dectalk/BUILD.md) for the
dictionary build pipeline and `__wrap_load_dictionary()` implementation.

### Flow Control

The protocol loop implements two-tier XON/XOFF flow control:

1. **Text buffer level** — XOFF at 2/3 full, XON at 1/3 full.
2. **Speech queue depth** — XOFF at 3/4 full, XON at 1/4 full.

XOFF is sent when _either_ threshold is exceeded (aggressive).  XON requires
_both_ to be below their respective thresholds (conservative) to prevent
rapid oscillation.

### Idle Flush

If no new characters arrive for `CONFIG_DECTALK_TEXT_IDLE_TIMEOUT_MS`
(default 200 ms), any buffered text is automatically flushed to the speech
queue.  This handles the case where the host sends text without a trailing
CR.

---

## Build Commands Reference

```bash
# Full clean build
idf.py fullclean && idf.py build

# Full clean build with development overrides
idf.py fullclean && idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.devel" build

# Build only
idf.py build

# Flash (replace /dev/ttyUSB0 with your UART port)
idf.py -p /dev/ttyUSB0 flash

# Flash dictionary partition separately (when using partition mode)
idf.py -p /dev/ttyUSB0 udict-flash

# Monitor console output (UART0)
idf.py -p /dev/ttyUSB0 monitor

# Open menuconfig
idf.py menuconfig

# Set target (only needed once, or after fullclean)
idf.py set-target esp32s3
```

### Changing Language

```bash
idf.py menuconfig
# Navigate to: DECtalk → DECtalk Language
# Select desired language, save, exit

idf.py build
idf.py -p /dev/ttyUSB0 flash
```

See the [component README](components/dectalk/README.md) for supported
languages and compile definitions.

### Changing Dictionary Storage Mode

```bash
idf.py menuconfig
# Navigate to: DECtalk → Dictionary location
# Choose: Embedded in firmware / Dedicated partition / File system
```

See the [component README](components/dectalk/README.md) for detailed
descriptions of each storage mode and its sub-options.

---

## Porting Notes

For details on how the upstream dapi library was adapted for ESP32 (compile
definitions, header shims, libc stubs, linker wrapping, warning
suppression), see the
[component BUILD.md](components/dectalk/BUILD.md).
