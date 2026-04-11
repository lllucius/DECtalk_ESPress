---
title: DECtalk ESPress Firmware
---

<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2025 Leland Lucius -->

# DECtalk ESPress Firmware

An ESP32-S3 firmware that turns a microcontroller into a standalone DECtalk
text-to-speech device.  The firmware boots directly into the DECtalk ESPress
serial protocol over USB CDC-ACM, allowing a host computer to send text and
receive status exactly as it would with a vintage DECtalk Express hardware
unit.

The speech synthesis itself is provided by the **DECtalk component**
(`components/dectalk/`) which cross-compiles the upstream `dapi` library as
a reusable ESP-IDF component.  See the
[component README](components/dectalk/README.md) for component-specific
documentation (language selection, dictionary storage modes, source
resolution, porting notes).

For detailed build instructions, toolchain setup, and architecture notes see
**[BUILD.md](BUILD.md)**.

## Key Features

- **ESPress serial protocol** — drop-in replacement for a real DECtalk
  Express: plain-ASCII text input, ETX flush, ENQ status query, SO/SI
  pause/resume, DLE command sequences, XON/XOFF flow control
- **USB CDC-ACM host transport** — the native USB port appears as a standard
  serial (COM / ttyACM) port to the host; no external UART adapter needed
- **I2S audio output** — 11.025 kHz, 16-bit mono via I2S to an external DAC
  (PCM5102, MAX98357A, etc.)
- **Configurable via `menuconfig`** — I2S pins, sample rate, DMA tuning,
  flow-control thresholds, task pinning, and more; no source edits needed
- **Python host tools** — a serial API module (`dectalk_serial.py`) and
  GUI applications for controlling the device from a PC
- **Memory diagnostics** — optional runtime task that logs per-task stack
  high-water marks and heap fragmentation statistics
- **Full DECtalk speech synthesis** — provided by the
  [DECtalk component](components/dectalk/README.md), supporting six
  languages and three dictionary storage modes

## Hardware Requirements

| Component | Details |
|-----------|---------|
| MCU board | ESP32 development board with **two USB data ports**: one UART/serial port for flashing and logs, plus one native USB-OTG/device port for ESPress protocol communication |
| Flash | 8 MB (configured in `sdkconfig.defaults`) |
| PSRAM | Optional for embedded/partition dictionary modes; recommended when loading the dictionary from SPIFFS (around 2 MB is a practical minimum) |
| I2S DAC | PCM5102, MAX98357A, or any I2S-compatible DAC/amplifier |
| USB cables | Two USB data-capable connections are recommended in practice: UART USB for flashing/debugging and native USB for CDC-ACM host communication |

> **Board selection note:** An Espressif **ESP32-S3** dev board is the
> recommended and currently configured option, but the important requirement is
> a chip/board combination that provides **both** a UART flashing/debug path
> and a separate native USB-OTG/device port for DECtalk ESPress communication.
> ESP32-S2 and ESP32-P4 based boards should also be viable in principle, but
> they are untested here and would require `sdkconfig` updates (the defaults
> currently set `CONFIG_IDF_TARGET="esp32s3"`).
>
> **Port usage:** The UART-side USB connection is for reflashing and console
> logs.  General users will typically only need it when using the `flasher`
> tool or `idf.py flash`, while developers/tinkerers will also use it for
> debugging.  The native USB CDC port is reserved for normal runtime
> host↔ESPress protocol communication.
>
> **Memory note:** PSRAM is not strictly required to run the firmware because
> the ESPress application itself fits within the base 512 KB RAM budget.
> However, dictionary storage mode matters: embedded dictionaries and dedicated
> dictionary partitions can work without PSRAM, while loading the dictionary
> from a SPIFFS file uses additional heap, so a modest PSRAM size (roughly
> 2 MB, depending on filesystem size and usage) is recommended.

## Wiring

Default I2S pin assignments (configurable via `idf.py menuconfig` →
*DECtalk ESPress Firmware → Audio output*):

| ESP32-S3 GPIO | I2S DAC Pin | Function |
|---------------|-------------|----------|
| GPIO 8 | BCK | Bit Clock |
| GPIO 3 | WS / LRCK | Word Select |
| GPIO 18 | DIN / DATA | Serial Data |
| GND | GND | Ground |
| 3.3 V / 5 V | VCC | Power (check your DAC's requirement) |

**PCM5102 notes** — short SCK→GND (internal PLL), FLT→GND, DEMP→GND,
FMT→GND.

**MAX98357A notes** — connect a 4–8 Ω speaker directly to the amplifier
output terminals.

## Quick Start

```bash
# 1. Source the ESP-IDF environment
. ~/esp/esp-idf/export.sh

# 2. Navigate to the project directory
cd DECtalk_ESPress

# 3. Build (dictionary is compiled automatically)
idf.py build

# 4. Flash firmware
idf.py -p /dev/ttyUSB0 flash

# 5. Monitor console logs (UART0)
idf.py -p /dev/ttyUSB0 monitor
```

The device's native USB port will appear as `/dev/ttyACM0` (Linux) or a COM
port (Windows).  Open it at any baud rate — the CDC-ACM link ignores baud —
and start sending text.

See [BUILD.md](BUILD.md) for full prerequisites and configuration options.

## Release Build Workflow

GitHub Actions includes a release-build workflow at
`.github/workflows/release-builds.yml`.  It builds firmware binaries and the
matching dictionary for all six DECtalk languages, uploads each language as a
workflow artifact, and attaches the packaged artifacts to published GitHub
releases.

## Serial Interfaces

The firmware uses **two** serial paths simultaneously:

| Interface | Purpose | How to access |
|-----------|---------|---------------|
| **UART0** (`CONFIG_ESP_CONSOLE_UART_DEFAULT`) | ESP-IDF console, `ESP_LOG` output, boot messages | Connect via the board's UART USB bridge (`/dev/ttyUSB0` or similar); use `idf.py monitor` |
| **USB CDC-ACM** (TinyUSB) | ESPress protocol data — host ↔ device text, control chars, DLE sequences | Connect via the native USB port (`/dev/ttyACM0` or similar); use `host/dectalk_serial.py` or any serial terminal |

This separation means log output never corrupts the ESPress byte stream and
protocol debugging is straightforward.

## ESPress Protocol Summary

The firmware boots directly into ESPress protocol mode — no handshake or
mode-switch command is needed.  Opening the USB CDC port asserts DTR, which
the firmware detects and responds to with a protocol-state reset and XON.

| Feature | Details |
|---------|---------|
| Transport | USB CDC-ACM (appears as COM / ttyACM) |
| Text input | Plain ASCII + CR for clause boundaries |
| Flush / cancel | ETX (`0x03`) — cancels all pending speech |
| Status query | ENQ (`0x05`) → 4-byte DLE status response |
| Pause / Resume | SO (`0x0E`) / SI (`0x0F`) |
| Flow control | XON (`0x11`) / XOFF (`0x13`), application-level, in-band |
| Flush-with-ack | `]` + ETX + XON → XON + SOH (TSR FLUSH_TEXT sequence) |
| DLE sequences | 4-byte packets: DLE + type + param1 + param2 |
| Index markers | DLE INDEX (`0x50`) followed by DLE STATUS (`0x40`) |
| Device ready | XON sent on power-up and after each host reconnection |

See `main/dectalk_espress.h` for the full protocol constant definitions and
encoding/decoding helpers.

## Host Tools

The `host/` directory contains Python-based host software.  See
**[host/README.md](host/README.md)** for full details.

- **`dectalk_serial.py`** — `DECtalkESPressSerial` class implementing the
  ESPress protocol: connect, speak, flush, pause/resume, status query,
  device detection.
- **`dectalk_espress_gui.py`** — Tkinter GUI with voice/rate/pitch controls,
  pause/resume/flush buttons, device status panel, and a communications log.

```python
from dectalk_serial import DECtalkESPressSerial

dt = DECtalkESPressSerial()
dt.connect("/dev/ttyACM0")
dt.speak("Hello from DECtalk on ESP32.", voice="Betty", rate=180)
dt.disconnect()
```

## Configuration (`menuconfig`)

Settings are split across two menus in `idf.py menuconfig`:

### DECtalk (component settings)

These settings are defined by the DECtalk component and control the
text-to-speech library itself.  See the
[component README](components/dectalk/README.md) for details.

| Menu Path | Key Settings |
|-----------|-------------|
| *DECtalk distribution* | Local DECtalk source path; optional GitHub FetchContent fallback and Git ref |
| *DECtalk Language* | US English (default), UK English, Spanish, German, Latin American Spanish, French |
| *Dictionary location* | Embedded in firmware, dedicated partition, or SPIFFS file system |

### DECtalk ESPress Firmware (application settings)

These settings are defined by the firmware application and control the
ESPress protocol emulation, hardware interfaces, and runtime behaviour.

| Menu Path | Key Settings |
|-----------|-------------|
| *Audio output* | I2S BCK/WS/DO GPIO pins, sample rate (8000–48000 Hz, default 11025) |
| *Audio output → Advanced audio tuning* | I2S DMA descriptor count, DMA frame count |
| *Runtime tuning* | Text buffer size, speech queue depth, RX timeout, idle flush timeout |
| *Runtime tuning → Advanced task tuning* | Speech task core affinity, main ESPress thread stack size |
| *USB CDC transport* | CDC RX stream buffer size |
| *Diagnostics and logging* | Enable/disable heap and stack diagnostics; choose the DECtalk firmware log level |

## Troubleshooting

| Symptom | Things to check |
|---------|-----------------|
| **No audio** | I2S wiring; DAC power; speaker connections; amplifier gain |
| **Garbled audio** | I2S pin config matches wiring; sample rate appropriate for DAC |
| **Build fails** | ESP-IDF environment sourced; host C compiler available for dictionary build; try `idf.py fullclean` |
| **No CDC port on host** | USB cable is data-capable (not charge-only); board has native USB connector; TinyUSB enabled in sdkconfig |
| **Device not responding** | Check UART0 console for boot/crash logs; verify CDC DTR is asserted; try resetting board |
| **WDT timeout** | Speech task must be pinned to CPU 1 (default); ensure `CONFIG_ESP_TASK_WDT_EN=n` in sdkconfig |

## License

This firmware is licensed under the MIT License — see [LICENSE](LICENSE).

## References

- [BUILD.md](BUILD.md) — detailed firmware build process and architecture
- [host/README.md](host/README.md) — Python host tools documentation
- [components/dectalk/README.md](components/dectalk/README.md) — DECtalk
  component: language, dictionary, source resolution
- [components/dectalk/BUILD.md](components/dectalk/BUILD.md) — DECtalk
  component: build process, porting notes
- [ESP-IDF Programming Guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/index.html)
- [ESP-IDF I2S Driver](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/peripherals/i2s.html)
- [TinyUSB CDC-ACM](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/usb_device.html)
