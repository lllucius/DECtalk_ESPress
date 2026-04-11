<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2025 Leland Lucius -->

# DECtalk ESP-IDF Component — Build Process & Porting Notes

This document describes the internal build process and porting details of
the DECtalk ESP-IDF component.  For the firmware-level build process
(project bootstrapping, partition table, sdkconfig, build commands), see
the [project-level BUILD.md](../../BUILD.md).

---

## Source Resolution

The component resolves the upstream DECtalk source tree (`src/dapi/src/`)
in the following order:

1. **Configured path** — the value of *Path to DECtalk library source tree*
   in menuconfig (`CONFIG_DECTALK_SRC_ROOT`).
2. **Auto-detection** — if the component is still inside a DECtalk checkout
   (`../../..` from the component is the repo root with `src/dapi/src`).
3. **FetchContent** — if enabled in menuconfig (default: **enabled**), CMake
   clones `https://github.com/dectalk/dectalk` into the build directory.
   This is the mechanism used when the component is installed via the
   ESP Component Registry.

The path may point at the DECtalk repository root (containing
`src/dapi/src`) or directly at the library source tree (containing
`dapi/src`).

### Registry-Installed Usage

When the component is installed via the ESP Component Registry
(`idf.py add-dependency "lllucius__dectalk>=1.0.0"`), it lands in the
project's `managed_components/lllucius__dectalk/` directory.  In that
location auto-detection (step 2) will not find a DECtalk checkout, so CMake
falls through to the FetchContent step.  Because `DECTALK_FETCH_SOURCE`
defaults to `y`, the upstream source tree is cloned automatically on the
first configure run.  Subsequent builds reuse the cached checkout from the
CMake build directory.

---

## Dictionary Build (Host-Side Cross-Compilation)

The DECtalk dictionary (`.dic`) is a binary file generated from a text
source (`src/dapi/src/dic/Dic_<lang>.txt`) by a host-native tool compiled
from `src/dapi/src/dic/dic.c`.

This happens at **CMake configure time** (before any cross-compilation) via
`execute_process()`:

1. The build system locates a host C compiler (`cc` or `gcc`) — explicitly
   _not_ `CMAKE_C_COMPILER` which is the ESP32 cross-compiler.
2. It compiles `dic.c` with the same language defines, producing
   `build/dicts/dic_<lang>`.
3. It runs the compiler: `dic_<lang> Dic_<lang>.txt dtalk_<lang>.dic` in the
   `build/dicts/` working directory.
4. The resulting `.dic` file is consumed according to the dictionary storage
   mode selected in Kconfig (see [README.md](README.md)).

> The `dic.c` tool interprets paths starting with `/` as option flags, so
> the output filename is relative and `WORKING_DIRECTORY` is used.

---

## Cross-Compiling the dapi Sources

After the dictionary is prepared, `idf_component_register()` compiles
approximately 70 upstream C source files from `src/dapi/src/` covering:

| Subsystem | Directory | Purpose |
|-----------|-----------|---------|
| **api** | `src/dapi/src/api/` | Public TTS API (`TextToSpeechStartup`, `Speak`, `Sync`, `Reset`, …) |
| **cmd** | `src/dapi/src/cmd/` | Command parser (inline `[:commands]`, phoneme mode) |
| **lts** | `src/dapi/src/lts/` | Letter-to-sound rules, dictionary lookup, suffix handling |
| **ph** | `src/dapi/src/ph/` | Phonetics, timing, intonation, syllable structure |
| **kernel** | `src/dapi/src/kernel/` | Internal services, language initialisation |
| **nt** | `src/dapi/src/nt/` | Threading primitives (`opthread.c`), SPC pipe, IPC |
| **osf** | `src/dapi/src/osf/` | Play stub, MMIO stubs |
| **vtm** | `src/dapi/src/vtm/` | Vocal-tract model (Klatt synthesiser), waveform generation |
| **hlsyn** | `src/dapi/src/hlsyn/` | High-level synthesis model (Brent solver, circuit model) |

Plus two local source files:

- **`libc_stubs.c`** — implements POSIX APIs not available on ESP-IDF's
  newlib: `shmget`/`shmat`/`shmdt`/`shmctl` (single-slot shared memory
  emulation), `nanosleep` (delegates to FreeRTOS `vTaskDelay` +
  `esp_rom_delay_us`), `readlink` (returns `ENOSYS`), `dirname`.
- **`loaddict_wrappers.c`** — GNU ld `--wrap` overrides for
  `load_dictionary()` / `unload_dictionary()` that replace the upstream
  file-based dictionary loader with ESP32-specific paths: reading from an
  embedded symbol, memory-mapping a flash partition, or opening a SPIFFS
  file.

### Compile Definitions

Key compile definitions applied to all dapi sources:

| Define | Value | Purpose |
|--------|-------|---------|
| `__linux__` | `1` | Activates POSIX code paths in upstream sources |
| `ANSI` | `1` | ANSI C mode |
| `BLD_DECTALK_DLL` | `1` | Builds the API as a shared-library-style interface |
| `LTSSIM` / `TTSSIM` | `1` | Enables the letter-to-sound and TTS simulation modes |
| `TYPING_MODE` | `1` | Enables interactive typing-mode support |

### Linker Wrapping

The component is linked with:
```
-Wl,--wrap=load_dictionary
-Wl,--wrap=unload_dictionary
```
so that upstream calls to `load_dictionary()` are redirected to the
ESP32-specific `__wrap_load_dictionary()` at link time.

Many GCC warnings are suppressed (`-Wno-*`) because the upstream dapi
sources are legacy C code that predates modern compiler strictness.

---

## Porting Notes

### What was Adapted from Upstream

The dapi library (`src/dapi/src/`) is compiled **unmodified** — no source
patches.  All ESP32 adaptations are achieved through:

1. **Compile definitions** (`__linux__=1`, etc.) that steer `#ifdef` branches
   in the upstream code toward POSIX paths.
2. **Header shims** (`include/sys/mman.h`, `include/sys/ipc.h`,
   `include/sys/shm.h`) that provide minimal type/constant definitions
   expected by the upstream headers.
3. **libc stubs** (`libc_stubs.c`) that implement the small subset of POSIX
   APIs actually called at runtime (shared memory, `nanosleep`).
4. **Linker wrapping** (`--wrap=load_dictionary`,
   `--wrap=unload_dictionary`) that redirects dictionary I/O to
   ESP32-specific flash/partition/embed paths without touching upstream
   code.
5. **Warning suppression** — the extensive `-Wno-*` list silences
   diagnostics from the legacy C codebase without modifying it.

### ESP32-Specific Considerations

- **No dynamic linking** — ESP-IDF uses static linking.
  `BLD_DECTALK_DLL` is defined to satisfy API export macros but produces
  no actual DLL.
- **Single TTS instance** — the shared-memory stubs support only one
  slot, matching the single-instance usage.
- **No audio device** — `TextToSpeechStartup()` is called with
  `DO_NOT_USE_AUDIO_DEVICE`; audio reaches the speaker through an
  in-memory callback (implemented by the firmware application).
- **Thread safety** — the speech thread and protocol loop communicate
  exclusively through a FreeRTOS queue (managed by the firmware
  application).

---

## Partition Management

`project_include.cmake` registers the custom `udict` partition subtype
(`0x40`) with ESP-IDF and manages dynamic partition table extension when
dictionary partitions are enabled.

---

## References

- [README.md](README.md) — component overview, Kconfig settings, dictionary
  storage modes
- [Project README](../../README.md) — firmware overview and quick-start
- [Project BUILD.md](../../BUILD.md) — firmware build process, architecture,
  and build commands
