<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2025 Leland Lucius -->

# DECtalk ESP-IDF Component

An ESP-IDF component that wraps the upstream DECtalk `dapi` text-to-speech
library for use on ESP32 targets.  This component handles source resolution,
language selection, dictionary compilation, cross-compilation of the
approximately 70 upstream C source files, and dictionary loading at runtime.

For details on how the firmware application (ESPress protocol, I2S audio,
USB CDC transport) uses this component, see the
[project-level README](../../README.md) and [BUILD.md](../../BUILD.md).

## Features

- **Full DECtalk TTS engine** — the complete `dapi` pipeline
  (letter-to-sound, phonetics, vocal-tract model, Klatt synthesiser)
  compiled as an ESP-IDF component
- **Six languages** — US English (default), UK English, Spanish, German,
  Latin American Spanish, French; selected at build time via Kconfig
- **Three dictionary storage modes** — embedded in firmware binary, stored
  on a dedicated flash partition, or loaded from a SPIFFS file system
- **Zero upstream patches** — all adaptations achieved through compile
  definitions, header shims, libc stubs, and linker wrapping
- **Standalone component** — works as a standalone project, installed via
  the ESP Component Registry, or with a local DECtalk source path / FetchContent
  from GitHub
- **Configurable via `menuconfig`** — language, dictionary location, source
  paths; all under the **DECtalk** menu

## ESP Component Registry Installation

The component is published on the
[ESP Component Registry](https://components.espressif.com/) as
**`lllucius/dectalk`** (the ESP-IDF component wrapper published from this
repository).

### Add to a Project

```bash
idf.py add-dependency "lllucius__dectalk>=1.0.0"
```

This downloads the component to your project's `managed_components/`
directory.  On the first `idf.py build` (or `idf.py reconfigure`), CMake
will automatically clone the upstream DECtalk source tree from GitHub via
FetchContent — no manual source-tree configuration required.

> **Internet access required on first build.** CMake fetches
> `https://github.com/dectalk/dectalk` (branch `develop`) into the build
> directory.  Subsequent builds reuse the cached checkout.

### Offline / Air-Gapped Builds

If network access is unavailable, download the DECtalk upstream source tree
manually and point the component at it:

1. Clone the upstream DECtalk source: `https://github.com/dectalk/dectalk`
   (this is the TTS engine source, separate from this project's repository
   `https://github.com/lllucius/DECtalk_ESPress`).
2. In `idf.py menuconfig`, navigate to
   *DECtalk → DECtalk distribution → Path to DECtalk library source tree*
   and enter the path to your local clone.
3. Disable *Fetch DECtalk source from GitHub if local path is unavailable*
   (optional, but avoids a network attempt).

```bash
idf.py menuconfig   # set source path, optionally disable FetchContent
idf.py build
```

## Kconfig Settings

All component settings appear under **DECtalk** in `idf.py menuconfig`:

| Menu Path | Key Settings |
|-----------|-------------|
| *DECtalk distribution* | Local DECtalk source path; optional GitHub FetchContent fallback (enabled by default) and Git ref |
| *DECtalk Language* | US English (default), UK English, Spanish, German, Latin American Spanish, French |
| *Dictionary location* | Embedded in firmware, dedicated partition, or SPIFFS file system |

These settings are defined in `Kconfig.projbuild` within this component
directory.  Firmware-specific settings (audio, runtime tuning, transport,
diagnostics) are defined separately by the application — see the
[project-level README](../../README.md).

## Directory Layout

```
components/dectalk/
├── CMakeLists.txt          # Compiles all dapi sources + local stubs; builds dictionary
├── Kconfig.projbuild       # menuconfig entries (language, dict storage, source path)
├── idf_component.yml       # ESP Component Registry metadata (name, version, dependencies)
├── BUILD.md                # Detailed build process and porting notes
├── README.md               # ← this file
├── project_include.cmake   # Registers custom partition subtypes; manages partition CSV
├── include/
│   ├── config.h            # Maps Kconfig DECTALK_DICT_ROOT → DECTALK_INSTALL_PREFIX
│   └── sys/
│       ├── ipc.h           # Minimal IPC_CREAT / IPC_RMID stubs
│       ├── mman.h          # mmap/munmap prototypes + MAP_FAILED constant
│       └── shm.h           # shmget/shmat/shmdt/shmctl prototypes
└── src/
    ├── libc_stubs.c        # shmget/shmat/shmdt/shmctl, nanosleep, readlink, dirname
    └── loaddict_wrappers.c # __wrap_load_dictionary / __wrap_unload_dictionary
```

## Quick Start

### From the Project Root

```bash
idf.py build   # fetches upstream source automatically on first run
```

### Setting a Local DECtalk Source Path

If you have a local checkout of the DECtalk source tree:

1. **Set a local path** — in `idf.py menuconfig`, navigate to
   *DECtalk → DECtalk distribution → Path to DECtalk library source tree*
   and point it at your local DECtalk checkout.

2. **Or rely on FetchContent** — FetchContent is enabled by default
   (`DECTALK_FETCH_SOURCE=y`).  If no local path is configured, CMake will
   automatically clone the upstream source tree from GitHub into the build
   directory.

## Language Selection

```bash
idf.py menuconfig
# Navigate to: DECtalk → DECtalk Language
# Select desired language, save, exit
idf.py build
```

| Kconfig Symbol | Lang Code | Compile Definitions |
|----------------|-----------|---------------------|
| `DECTALK_LANG_US` | `us` | `ENGLISH ENGLISH_US ACNA` |
| `DECTALK_LANG_UK` | `uk` | `ENGLISH ENGLISH_UK` |
| `DECTALK_LANG_SP` | `sp` | `SPANISH SPANISH_SP` |
| `DECTALK_LANG_GR` | `gr` | `GERMAN` |
| `DECTALK_LANG_LA` | `la` | `SPANISH SPANISH_LA` |
| `DECTALK_LANG_FR` | `fr` | `FRENCH` |

## Dictionary Storage Modes

```bash
idf.py menuconfig
# Navigate to: DECtalk → Dictionary location
# Choose: Embedded in firmware / Dedicated partition / File system
```

| Mode | Kconfig Symbol | How it Works |
|------|---------------|--------------|
| **Embedded** | `DECTALK_DICT_EMBED` | Dictionary binary is linked into the firmware via ESP-IDF's `EMBED_FILES`.  Accessed through the linker symbol `_binary_dtalk_<lang>_dic_start`.  Zero-copy. |
| **Partition** | `DECTALK_DICT_PART` | Dictionary is flashed to a `udict` data partition.  Mapped into the address space via `esp_partition_mmap()`.  Zero-copy. |
| **File system** | `DECTALK_DICT_FILE` | Dictionary is loaded from a SPIFFS mount at runtime using `fopen()`/`fread()`.  Requires separate heap allocation. |

**Partition mode options:**
- *Automatically add dictionary partitions* — adds the `udict` partition to
  the table at build time.
- *Automatically flash dictionary partitions with app* — includes the
  dictionary in the normal `idf.py flash` target.
- Or flash separately: `idf.py -p <PORT> udict-flash`.

**File system mode options:**
- *Automatically add SPIFFS dictionary partition* — adds a `dictionary`
  SPIFFS partition to the table.
- *Automatically flash dictionary SPIFFS image with app* — includes the
  SPIFFS image in `idf.py flash`.
- *Path to SPIFFS image source directory* — defaults to the build directory.
- *Path to directory containing dictionary file* — the SPIFFS mount point
  (default `/dict`).

## Build Details

For detailed build process documentation including the dictionary
cross-compilation pipeline, dapi source compilation, porting notes, and
linker wrapping, see **[BUILD.md](BUILD.md)**.

## License

This component wrapper (the files in this directory: `idf_component.yml`,
`CMakeLists.txt`, `Kconfig.projbuild`, `project_include.cmake`, and files
under `include/` and `src/`) is licensed under the MIT License — see
[LICENSE](../../LICENSE).

The upstream DECtalk TTS engine source (fetched from
`https://github.com/dectalk/dectalk`, the upstream source repository) is
covered by the FONIX proprietary license (see the `LICENCE` file in that
repository).
