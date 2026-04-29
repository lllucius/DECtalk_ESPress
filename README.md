<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2025 Leland Lucius -->

# DECtalk ESPress Firmware

An ESP32 firmware for **ESP32-S3** and **ESP32-C6** boards that turns a
microcontroller into a standalone DECtalk text-to-speech device.  The firmware
boots directly into the DECtalk ESPress serial protocol, allowing a host
computer to send text and receive status exactly as it would with a vintage
DECtalk Express hardware unit.  On ESP32-S3 the host link uses **USB
CDC-ACM**; on ESP32-C6 it uses the built-in **USB Serial/JTAG** interface.
The documented photographed hardware build in **[HARDWARE.md](HARDWARE.md)** is
an **ESP32-C6** perfboard build; **ESP32-S3** remains a supported firmware
target, but it is not the specific physical build documented there.

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
- [Physical Device Build](#physical-device-build)
- [Quick Start](#quick-start)
- [Web Flasher](#web-flasher)
- [Web GUI](#web-gui)
- [Release Build Workflow](#release-build-workflow)
- [Serial Interfaces](#serial-interfaces)
- [ESPress Protocol Summary](#espress-protocol-summary)
- [Host Tools](#host-tools)
- [Custom `[:fw …]` Commands](#custom-fw--commands)
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
  ports to the host (for example COM, `/dev/ttyACM*`, or `/dev/ttyUSB*`
  depending on board and driver); no external UART adapter needed
- **I2S audio output** — 11.025 kHz, 16-bit mono via I2S to an external DAC
  (PCM5102, MAX98357A, etc.)
- **Configurable via `menuconfig`** — I2S pins, DMA tuning,
  flow-control thresholds, task pinning, and more; no source edits needed
- **Python host tools** — a serial API module (`dtesp_serial.py`) and
  GUI applications for controlling the device from a PC
- **Browser-based Web GUI** — control the device directly from Chrome or Edge
  via the Web Serial API; no install required
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

Current default I2S pin assignments (configurable via `idf.py menuconfig` →
*DECtalk ESPress Firmware → Audio output*):

| Default GPIO | I2S DAC Pin | Function |
|--------------|-------------|----------|
| GPIO 21 | BCK | Bit Clock |
| GPIO 20 | WS / LRCK | Word Select |
| GPIO 19 | DIN / DATA | Serial Data |
| GND | GND | Ground |
| 3.3 V / 5 V | VCC | Power (check your DAC's requirement) |

**PCM5102 notes** — short SCK→GND (internal PLL), FLT→GND, DEMP→GND,
FMT→GND.

**MAX98357A notes** — connect a 4–8 Ω speaker directly to the amplifier
output terminals.

**TLV320DAC3100 (Adafruit breakout) notes** — the TLV320DAC3100 adds I2C
control pins on top of the basic I2S wiring.  The current Kconfig defaults are:

| Default GPIO | TLV320DAC3100 Pin | Function |
|--------------|-------------------|----------|
| GPIO 18 | SDA | I2C Data |
| GPIO 9 | SCL | I2C Clock |
| GPIO 22 | RESET | Optional active-low hardware reset |
| GPIO 15 | INT | Optional interrupt output |

The optional TLV320 **MCLK** output is disabled by default (`-1`).  If you wire
MCLK, set it explicitly in `idf.py menuconfig` → *DECtalk ESPress Firmware →
Audio output*.  Select **Adafruit TLV320DAC3100 breakout** as the Audio DAC to
expose the codec I2C, reset, interrupt, MCLK, and DSP-default settings.

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

## Web GUI

The published project site includes a browser-based GUI for controlling the
device without installing any software.

1. Open the project site and click **Web GUI** in the top navigation.
2. Use **Chrome** or **Edge** (version 89 or newer) — the page requires the
   Web Serial API.
3. Connect the board's **runtime host port** to your computer, click
   **Connect**, and select the ESP32 serial port from the browser chooser.
4. Choose a voice, adjust rate and pitch with the sliders, type text in the
   text box, and click **Speak**.

The Web GUI mirrors the Qt desktop GUI feature-for-feature: voice / rate /
pitch controls, Pause / Resume / Flush / Query Status buttons, an **Audio
Settings** dialog (volume, EQ presets, DRC, speaker gain), a Device Status
panel, and a timestamped Communications Log.

> **Browser support:** the Web Serial API is available only in Chromium-based
> browsers (Chrome, Edge, Opera, …) on desktop.  Firefox and Safari are not
> supported.  On Linux, ensure your user has access to the serial device
> (typically by being in the `dialout` or `uucp` group).

See [host/README.md](host/README.md) for full Web GUI documentation.

## Release Build Workflow

GitHub Actions includes a release-build workflow at
`.github/workflows/release.yml`.  It builds firmware binaries and the
matching dictionary for all six DECtalk languages on both supported targets
(**ESP32-S3** and **ESP32-C6**), uploads each language/target combination as a
workflow artifact, and attaches the packaged artifacts to published GitHub
releases.

## Serial Interfaces

The firmware's host transport depends on the target:

| Interface | Purpose | How to access |
|-----------|---------|---------------|
| **UART0** (`CONFIG_ESP_CONSOLE_UART_DEFAULT`) | ESP-IDF console, `ESP_LOG` output, boot messages | Connect via the board's UART USB bridge (`/dev/ttyUSB0` or similar); use `idf.py monitor` |
| **USB CDC-ACM** (TinyUSB, **ESP32-S3**) | ESPress protocol data — host ↔ device text, control chars, DLE sequences | Connect via the native USB port (`/dev/ttyACM0` or similar); use `host/dtesp_serial.py` or any serial terminal |
| **USB Serial/JTAG** (**ESP32-C6**) | ESPress protocol data — host ↔ device text, control chars, DLE sequences | Connect via the native USB Serial/JTAG port exposed by the board; use `host/dtesp_serial.py` or any serial terminal |

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

See `main/dtesp.h` for the full protocol constant definitions and
encoding/decoding helpers.

## Host Tools

The `host/` directory contains host software for controlling the device.  See
**[host/README.md](host/README.md)** for full details.

- **`web/index.html`** — browser-based Web GUI; no install required; runs in
  Chrome or Edge 89+ using the Web Serial API.  Also hosted on the project
  site — click **Web GUI** in the top navigation.
- **`dtesp_serial.py`** — `DECtalkESPressSerial` class implementing the
  ESPress protocol: connect, speak, flush, pause/resume, status query,
  device detection.
- **`dtesp_gui_qt.py`** — Qt GUI with voice/rate/pitch controls,
  pause/resume/flush buttons, device status panel, and a communications log.

```python
from dtesp_serial import DECtalkESPressSerial

dt = DECtalkESPressSerial()
dt.connect("/dev/ttyACM0")
dt.speak("Hello from DECtalk on ESP32.", voice="Betty", rate=180)
dt.disconnect()
```

## Custom `[:fw …]` Commands

Text passed through the ESPress protocol may contain custom
`[:fw <sub-command> <args…>]` directives.  These are intercepted
before DECtalk sees them and run as firmware-side actions.

Session / GPIO / Tone commands:

| Command                                  | Action                                              |
| ---------------------------------------- | --------------------------------------------------- |
| `[:fw gpio <pin> <on\|off\|0\|1>]`       | Set a GPIO pin level                                |
| `[:fw voice <name>]`                     | Change DECtalk voice (session-scoped)               |
| `[:fw rate <75..600>]`                   | Change DECtalk speaking rate (session-scoped)       |
| `[:fw tone <freq_hz> <duration_ms>]`     | Play a tone on the configured LEDC GPIO             |

Codec / output commands (TLV320DAC3100):

| Command                                  | Action                                              |
| ---------------------------------------- | --------------------------------------------------- |
| `[:fw volume <0..9>]`                    | Codec digital volume                                |
| `[:fw profile <speaker\|headphone>]`     | Output profile                                      |
| `[:fw autoswitch <on\|off>]`             | Headset-jack auto-switch                            |
| `[:fw mute <on\|off>]`                   | Soft-mute the codec                                 |
| `[:fw save]`                             | Persist current codec + DSP state to NVS            |

On-chip DSP tone controls — these exploit the TLV320DAC3100's
built-in biquad/IIR engine and have *no* CPU cost:

| Command                                  | Action                                              |
| ---------------------------------------- | --------------------------------------------------- |
| `[:fw bass <-12..+12>]`                  | Low-shelf at ≈200 Hz, gain in dB                    |
| `[:fw treble <-12..+12>]`                | High-shelf at ≈4.5 kHz, gain in dB                  |
| `[:fw eq <1..5> <-12..+12>]`             | Set a peaking-EQ band gain (160/500/1.5k/3k/5k Hz)  |
| `[:fw eq reset]`                         | Flatten every peaking band (bass/treble unchanged)  |
| `[:fw eq show]`                          | Log the current DSP state                           |
| `[:fw eq preset <flat\|speech\|crisp\|warm>]` | Load a named preset                            |
| `[:fw drc <on\|off>]`                    | Dynamic Range Compression on/off                    |
| `[:fw drc preset <soft\|speech\|loud>]`  | DRC tuning preset                                   |
| `[:fw spkgain <6\|12\|18\|24>]`          | Class-D speaker-amp analog gain (dB)                |

### Why EQ helps DECtalk TTS

DECtalk speech at 11 kHz has most of its energy in 200 Hz–4 kHz.
Perceived **muddiness** usually comes from excess 150–400 Hz output
plus weak 2–5 kHz; perceived **crispness** lives in the 2–4 kHz
"presence" band plus 4–5 kHz "sibilance".  Good default tunings
therefore cut the bass mud and boost presence — which is exactly
what the built-in presets do:

- `speech` — gentle bass cut, 500 Hz cut, 3 kHz boost, 4.5 kHz
  treble boost, mild DRC.  Recommended starting point for TTS.
- `crisp` — more aggressive 3 kHz + 5 kHz boost for extra
  intelligibility.
- `warm` — gentle bass lift, no treble boost.

All tone-control values are clamped to ±12 dB.  Each biquad
coefficient is stored as Q1.23 two's-complement and applied live
via a brief soft-mute so transitions are silent.  The complete
state (EQ + DRC + speaker gain) is persisted to NVS by `[:fw save]`.

Example:

```
[:fw eq preset speech] [:fw save] The quick brown fox.
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
| *Audio output* | Audio DAC selection (generic or TLV320DAC3100), I2S BCK/WS/DO GPIO pins, optional TLV320 MCLK, and TLV320DAC3100 I2C/reset/interrupt GPIOs; sample rate is hardcoded at 11.025 kHz |
| *Audio output → TLV320DAC3100 defaults* | Default profile, startup volume, DSP preset, DRC, speaker gain, and headset auto-switch behaviour |
| *Audio output → Analog volume knob (potentiometer)* | Optional ADC-driven hardware volume knob with smoothing, hysteresis, and soft-takeover tuning |
| *Audio output → Advanced audio tuning* | I2S DMA descriptor count, DMA frame count |
| *Runtime tuning* | Text buffer size, speech queue depth, RX timeout, idle flush timeout |
| *Runtime tuning → Advanced task tuning* | Speech task core affinity, main ESPress thread stack size |
| *USB CDC transport* / *JTAG serial transport* | Target-specific host transport buffer sizing |
| *Custom firmware commands* | Enable/disable `[:fw …]` command parsing; configure namespace, token length, native-command override, reconnect reset, and per-command enable flags (GPIO, tone, DSP/EQ/DRC) |
| *Onboard RGB LED* | Optionally drive the RGB LED data GPIO low at startup to keep the LED dark |
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
- [host/README.md](host/README.md) — host tools documentation (Web GUI, Qt GUI, Python API)
- [components/dectalk/README.md](components/dectalk/README.md) — DECtalk
  component: language, dictionary, source resolution
- [components/dectalk/BUILD.md](components/dectalk/BUILD.md) — DECtalk
  component: build process, porting notes
- [ESP-IDF Programming Guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/index.html)
- [ESP-IDF I2S Driver](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/peripherals/i2s.html)
- [TinyUSB CDC-ACM](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/usb_device.html)
- [ESP-IDF USB Serial/JTAG](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c6/api-reference/peripherals/usb_serial_jtag.html)
