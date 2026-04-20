<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2025 Leland Lucius -->

# DECtalk ESPress Firmware

An ESP32 firmware for **ESP32-S3** and **ESP32-C6** boards that turns a
microcontroller into a standalone DECtalk text-to-speech device.  The firmware
boots directly into the DECtalk ESPress serial protocol, allowing a host
computer to send text and receive status exactly as it would with a vintage
DECtalk Express hardware unit.  On ESP32-S3 the host link uses **USB
CDC-ACM**; on ESP32-C6 it uses the built-in **USB Serial/JTAG** interface.

The speech synthesis itself is provided by the **DECtalk component**
(`components/dectalk/`) which cross-compiles the upstream `dapi` library as
a reusable ESP-IDF component.  See the
[component README](components/dectalk/README.md) for component-specific
documentation (language selection, dictionary storage modes, source
resolution, porting notes).

For detailed build instructions, toolchain setup, and architecture notes see
**[BUILD.md](BUILD.md)**.

## Acknowledgments

This project wouldn't exist without the efforts of the
[dectalk/dectalk](https://github.com/dectalk/dectalk) project and the
[DECtalk community](https://dectalk.de/).  Thank you for preserving and
advancing DECtalk for everyone.

## Table of Contents

- [Key Features](#key-features)
- [Hardware Requirements](#hardware-requirements)
- [Wiring](#wiring)
- [Quick Start](#quick-start)
- [Web Flasher](#web-flasher)
- [Release Build Workflow](#release-build-workflow)
- [Serial Interfaces](#serial-interfaces)
- [ESPress Protocol Summary](#espress-protocol-summary)
- [Host Tools](#host-tools)
- [Configuration (`menuconfig`)](#configuration-menuconfig)
  - [DECtalk (component settings)](#dectalk-component-settings)
  - [DECtalk ESPress Firmware (application settings)](#dectalk-espress-firmware-application-settings)
- [Troubleshooting](#troubleshooting)
- [License](#license)
- [References](#references)

## Key Features

- **ESPress serial protocol** — drop-in replacement for a real DECtalk
  Express: plain-ASCII text input, ETX flush, ENQ status query, SO/SI
  pause/resume, DLE command sequences, XON/XOFF flow control
- **Native USB host transport** — ESP32-S3 uses TinyUSB CDC-ACM and
  ESP32-C6 uses USB Serial/JTAG, both appearing as standard serial
  (COM / ttyACM) ports to the host; no external UART adapter needed
- **I2S audio output** — 11.025 kHz, 16-bit mono via I2S to an external DAC
  (PCM5102, MAX98357A, etc.)
- **Configurable via `menuconfig`** — I2S pins, DMA tuning,
  flow-control thresholds, task pinning, and more; no source edits needed
- **Python host tools** — a serial API module (`espress_serial.py`) and
  GUI applications for controlling the device from a PC
- **Memory diagnostics** — optional runtime task that logs per-task stack
  high-water marks and heap fragmentation statistics
- **Full DECtalk speech synthesis** — provided by the
  [DECtalk component](components/dectalk/README.md), supporting six
  languages and three dictionary storage modes

## Hardware Requirements

| Component | Details |
|-----------|---------|
| MCU board | **ESP32-S3** development board with separate UART + native USB ports, or an **ESP32-C6** board with a native USB Serial/JTAG port for ESPress protocol communication |
| Flash | 8 MB (configured in `sdkconfig.defaults`) |
| PSRAM | Optional for embedded/partition dictionary modes; recommended when loading the dictionary from SPIFFS (around 2 MB is a practical minimum) |
| I2S DAC | PCM5102, MAX98357A, Adafruit TLV320DAC3100, or any I2S-compatible DAC/amplifier |
| USB cables | **ESP32-S3:** two USB data-capable connections are recommended (UART USB for flashing/debugging plus native USB for ESPress). **ESP32-C6:** one data-capable native USB connection is sufficient for host communication |

> **Board selection note:** The firmware currently supports **ESP32-S3** and
> **ESP32-C6**.  `sdkconfig.defaults` sets `CONFIG_IDF_TARGET="esp32s3"` by
> default; run `idf.py set-target esp32c6` when building for ESP32-C6 so
> ESP-IDF also applies `sdkconfig.defaults.esp32c6`.
>
> **Port usage:** On **ESP32-S3**, the UART-side USB connection is for
> reflashing and console logs while the native USB CDC port is reserved for
> normal runtime host↔ESPress protocol communication.  On **ESP32-C6**, the
> built-in USB Serial/JTAG port is now supported for host communication; the
> firmware disables the RTS-triggered reset so opening the port does not reboot
> the device.
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

**TLV320DAC3100 (Adafruit breakout) notes** — the TLV320DAC3100 requires
two extra I2C connections beyond the basic I2S pins:

| ESP32-S3 GPIO | TLV320DAC3100 Pin | Function |
|---------------|-------------------|----------|
| GPIO 9 | MCLK | Master Clock (256 × Fs) |
| GPIO 1 | SDA | I2C Data |
| GPIO 2 | SCL | I2C Clock |

All GPIOs above are configurable via `idf.py menuconfig` → *DECtalk ESPress
Firmware → Audio output*.  Select **Adafruit TLV320DAC3100 breakout** as the
Audio DAC to expose the codec I2C, reset, and interrupt GPIO settings.

## Quick Start

```bash
# 1. Source the ESP-IDF environment
. ~/esp/esp-idf/export.sh

# 2. Navigate to the project directory
cd DECtalk_ESPress

# 3. Select the target if needed
# idf.py set-target esp32c6

# 4. Build (dictionary is compiled automatically)
idf.py build

# 5. Flash firmware
idf.py -p /dev/ttyUSB0 flash

# 6. Monitor console logs (UART0 or your board's console port)
idf.py -p /dev/ttyUSB0 monitor
```

For **ESP32-S3**, the runtime host port is the board's native **USB CDC-ACM**
device (typically `/dev/ttyACM0` on Linux).  For **ESP32-C6**, use the native
**USB Serial/JTAG** port exposed by the board.  Open the port at any baud rate
and start sending text.

See [BUILD.md](BUILD.md) for full prerequisites and configuration options.

## Web Flasher

The published project site includes a browser-based flasher for installing
released firmware without setting up ESP-IDF locally.

1. Open the project site and click **Web Flasher** in the top navigation.
2. Use **Chrome** or **Edge** (version 89 or newer) so the page can access the
   board over the browser's Web Serial API.
3. Connect the board's **UART flashing/debug USB port** to your computer, click
   **Connect**, and choose the serial device for that port.
4. Select the desired **Release** and **Language**, then click
   **Flash Firmware**.
5. If you already downloaded a release archive from GitHub, click
   **Flash from File…** and select the `.tar.gz` / `.tgz` file instead.

After flashing finishes, unplug or close the flashing connection if needed and
use the board's runtime host port: **native USB CDC** on ESP32-S3 or **USB
Serial/JTAG** on ESP32-C6.

## Release Build Workflow

GitHub Actions includes a release-build workflow at
`.github/workflows/release-builds.yml`.  It builds firmware binaries and the
matching dictionary for all six DECtalk languages, uploads each language as a
workflow artifact, and attaches the packaged artifacts to published GitHub
releases.

## Serial Interfaces

The firmware's host transport depends on the target:

| Interface | Purpose | How to access |
|-----------|---------|---------------|
| **UART0** (`CONFIG_ESP_CONSOLE_UART_DEFAULT`) | ESP-IDF console, `ESP_LOG` output, boot messages | Connect via the board's UART USB bridge (`/dev/ttyUSB0` or similar); use `idf.py monitor` |
| **USB CDC-ACM** (TinyUSB, **ESP32-S3**) | ESPress protocol data — host ↔ device text, control chars, DLE sequences | Connect via the native USB port (`/dev/ttyACM0` or similar); use `host/espress_serial.py` or any serial terminal |
| **USB Serial/JTAG** (**ESP32-C6**) | ESPress protocol data — host ↔ device text, control chars, DLE sequences | Connect via the native USB Serial/JTAG port exposed by the board; use `host/espress_serial.py` or any serial terminal |

On **ESP32-S3**, this separation means log output never corrupts the ESPress
byte stream and protocol debugging is straightforward.  On **ESP32-C6**, the
built-in USB Serial/JTAG interface is used for host communications instead.

> **Transport note:** **ESP32-S3** keeps using TinyUSB CDC-ACM because its
> built-in USB Serial/JTAG peripheral reboots the chip when the host toggles
> DTR.  **ESP32-C6** now uses USB Serial/JTAG for host communication, with the
> RTS-triggered reset explicitly disabled in firmware so opening the port does
> not reboot the chip.

## ESPress Protocol Summary

The firmware boots directly into ESPress protocol mode — no handshake or
mode-switch command is needed.  When the host opens the transport port, the
firmware detects the new connection and responds with a protocol-state reset
and XON.

| Feature | Details |
|---------|---------|
| Transport | USB CDC-ACM on ESP32-S3; USB Serial/JTAG on ESP32-C6 |
| Text input | Plain ASCII + CR for clause boundaries |
| Flush / cancel | ETX (`0x03`) — cancels all pending speech |
| Status query | ENQ (`0x05`) → 4-byte DLE status response |
| Pause / Resume | SO (`0x0E`) / SI (`0x0F`) |
| Flow control | XON (`0x11`) / XOFF (`0x13`), application-level, in-band |
| Flush-with-ack | `]` + ETX + XON → XON + SOH (TSR FLUSH_TEXT sequence) |
| DLE sequences | 4-byte packets: DLE + type + param1 + param2 |
| Index markers | DLE INDEX (`0x50`) followed by DLE STATUS (`0x40`) |
| Device ready | XON sent on power-up and after each host reconnection |

See `main/espress.h` for the full protocol constant definitions and
encoding/decoding helpers.

## Host Tools

The `host/` directory contains Python-based host software.  See
**[host/README.md](host/README.md)** for full details.

- **`espress_serial.py`** — `DECtalkESPressSerial` class implementing the
  ESPress protocol: connect, speak, flush, pause/resume, status query,
  device detection.
- **`espress_gui_qt.py`** — Tkinter GUI with voice/rate/pitch controls,
  pause/resume/flush buttons, device status panel, and a communications log.

```python
from espress_serial import DECtalkESPressSerial

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
| *DECtalk distribution* | Local DECtalk source path (overrides bundled submodule) |
| *DECtalk Language* | US English (default), UK English, Spanish, German, Latin American Spanish, French |
| *Dictionary location* | Embedded in firmware, dedicated partition, or SPIFFS file system |

### DECtalk ESPress Firmware (application settings)

These settings are defined by the firmware application and control the
ESPress protocol emulation, hardware interfaces, and runtime behaviour.

| Menu Path | Key Settings |
|-----------|-------------|
| *Audio output* | Audio DAC selection (generic or TLV320DAC3100), I2S BCK/WS/DO GPIO pins, and TLV320DAC3100 I2C/reset/interrupt GPIOs; sample rate is hardcoded at 11.025 kHz |
| *Audio output → Advanced audio tuning* | I2S DMA descriptor count, DMA frame count |
| *Runtime tuning* | Text buffer size, speech queue depth, RX timeout, idle flush timeout |
| *Runtime tuning → Advanced task tuning* | Speech task core affinity, main ESPress thread stack size |
| *USB CDC transport* / *JTAG serial transport* | Target-specific host transport buffer sizing |
| *Diagnostics and logging* | Enable/disable heap and stack diagnostics; choose the DECtalk firmware log level |

## Troubleshooting

| Symptom | Things to check |
|---------|-----------------|
| **No audio** | I2S wiring; DAC power; speaker connections; amplifier gain |
| **Garbled audio** | I2S pin config matches wiring; attached DAC supports 11.025 kHz I2S audio |
| **Build fails** | ESP-IDF environment sourced; host C compiler available for dictionary build; try `idf.py fullclean` |
| **No host serial port appears** | USB cable is data-capable (not charge-only); board has the expected native USB connector; for ESP32-S3 verify TinyUSB is enabled; for ESP32-C6 verify the board exposes USB Serial/JTAG |
| **Device not responding** | Check UART0 or the board's console logs for boot/crash output; verify the runtime host port is the correct one for the target; try resetting board |
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
- [ESP-IDF USB Serial/JTAG](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c6/api-reference/peripherals/usb_serial_jtag.html)
