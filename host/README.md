<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2025 Leland Lucius -->

# DECtalk - Host GUI & Serial API

This directory contains a Python Tkinter GUI application and serial API module
for controlling the DECtalk text-to-speech engine running on an ESP32
microcontroller.

## Overview

The host communicates with the ESP32 over USB using the DECtalk ESPress serial
protocol.  The ESP32 firmware boots directly into ESPress protocol mode on its
USB CDC-ACM port, which appears as a standard serial (COM / ttyACM) port on the
host computer.

## Requirements

- Python 3.7 or later
- [pyserial](https://pypi.org/project/pyserial/) (`pip install pyserial`)
- Tkinter (included with most Python installations) — for the Tkinter GUI
- [PySide6](https://pypi.org/project/PySide6/) (`pip install PySide6`) — for the Qt GUI
- ESP32-S3 running the DECtalk firmware (see `../README.md`)

## Quick Start

1. Flash the DECtalk firmware to your ESP32 (see `../README.md`)

2. Install the Python dependencies:
   ```bash
   pip install pyserial
   ```
   For the Qt GUI, also install PySide6:
   ```bash
   pip install PySide6
   ```

3. Run the ESPress protocol GUI (full-featured, Tkinter):
   ```bash
   python dectalk_espress_gui_tk.py
   ```
   Or the Qt version (with full accessibility support):
   ```bash
   python dectalk_espress_gui_qt.py
   ```
   Or run the simple GUI:
   ```bash
   python dectalk_gui_tk.py
   ```

4. Select the serial port for your ESP32 and click **Connect**

5. Type text in the text box and click **Speak**

## GUI Features

### Voice Selection

Choose from 9 built-in DECtalk voices:

| Voice   | Description           |
|---------|-----------------------|
| Paul    | Standard male (default) |
| Betty   | Standard female       |
| Harry   | Deep male             |
| Frank   | Older male            |
| Dennis  | Breathy male          |
| Kit     | Child (~10 years)     |
| Ursula  | Light female          |
| Rita    | Deep female           |
| Wendy   | Whispery female       |

### Rate Control

Adjust the speaking rate from 75 to 600 words per minute (WPM) using the
slider. The default is 200 WPM.

### Pitch Control

Adjust the average pitch from 50 to 400 Hz using the slider. The leftmost
position uses the voice's default pitch.

### Text Editing

The multi-line text box supports:
- **Vertical scrolling** for long text
- **Horizontal scrolling** for wide lines (word wrap is disabled)
- **Cut**: Ctrl+X
- **Copy**: Ctrl+C
- **Paste**: Ctrl+V
- **Select All**: Ctrl+A
- **Undo**: Ctrl+Z
- **Redo**: Ctrl+Y (or Ctrl+Shift+Z)
- **Clear**: via the Clear Text button or right-click context menu
- **Right-click context menu** with Cut, Copy, Paste, Select All, Clear

### DECtalk Inline Commands

You can also include DECtalk inline commands directly in the text box. These
are processed by the DECtalk engine on the ESP32. For example:

```
[:phoneme on][hxeh<500,20>low<500,22>]
[:np] Hello, I am Paul. [:nb] And I am Betty.
[:dv ap 160 pr 50] This voice has modified pitch.
[:tone 500,500] A tone before speech.
```

See `../ref_man.txt` for the full DECtalk command reference.

## ESPress GUI (`dectalk_espress_gui_tk.py`)

The full-featured GUI application that uses the DECtalk ESPress serial protocol.
Run with:

```bash
python dectalk_espress_gui_tk.py
```

The ESPress GUI includes all the features of the simple GUI plus:

- **Pause / Resume** buttons -- send SO (0x0E) / SI (0x0F) control characters
- **Flush** button -- cancel all pending speech using the TSR flush sequence
  (`]` + ETX + XON) with SOH acknowledgment
- **Query Status** button -- send ENQ (0x05) and decode the DLE status response
- **Device Status panel** -- displays the raw status word and colored indicators
  for Ready, Transmitting, Flushing, and Index states
- **Automatic status polling** -- periodically queries the device for live status
- **Communications Log** -- timestamped TX/RX event display

## ESPress GUI - Qt Version (`dectalk_espress_gui_qt.py`)

A Qt-based (PySide6/PyQt6) version of the ESPress GUI with identical
functionality and full accessibility support. Run with:

```bash
python dectalk_espress_gui_qt.py
```

Requires `pip install PySide6` (or `pip install PyQt6`).

All features from the Tkinter version are included, plus:

- **Full screen-reader accessibility** -- every control has an accessible name
  and description, enabling use with NVDA, Orca, VoiceOver, and other
  assistive technologies
- **Keyboard mnemonics** -- Alt+key shortcuts on all buttons and labels
  (e.g., Alt+S for Speak, Alt+P for Port)
- **Logical tab order** -- Tab and Shift+Tab navigate controls in the expected
  sequence
- **High-contrast status indicators** -- colored indicator labels with bold
  styling when active, plus accessible state descriptions

## DECtalk ESPress Protocol

The ESP32 firmware boots directly into ESPress protocol mode on its USB CDC-ACM
port.  No handshake or mode-switch command is needed.  Opening the USB serial
port triggers a DTR assertion that the firmware detects, causing it to reset
protocol state and send XON to indicate readiness.

### Key Protocol Elements

| Feature | Description |
|---------|-------------|
| Transport | USB CDC-ACM (appears as COM / ttyACM port) |
| Text format | Plain ASCII text + CR |
| Flush/stop | ETX (0x03) |
| Status query | ENQ (0x05) → 4-byte DLE status response |
| Pause/Resume | SO (0x0E) / SI (0x0F) |
| Flow control | XON/XOFF (application-level, in-band) |
| Device ready | XON sent on startup / reconnect |

## Python API Module

The `dectalk_serial.py` module can be used independently from the GUI:

```python
from dectalk_serial import DECtalkESPressSerial

espress = DECtalkESPressSerial()

# List available serial ports
print(DECtalkESPressSerial.list_ports())

# Connect to the ESP32
espress.connect("/dev/ttyACM0")

# Detect device (same probe as the original comchk utility)
if espress.detect_device():
    print("DECtalk device detected!")

# Send text (device speaks it immediately)
espress.send_text("Hello world.")

# Speak with voice and rate (uses inline commands)
espress.speak("Now I am Betty.", voice="Betty", rate=250)

# Request device status
status = espress.request_status()
if status >= 0:
    print("Device status: 0x%04X" % status)

# Pause and resume speech
espress.pause()
espress.resume()

# Flush (cancel) all speech
espress.flush()

# Flush with acknowledgment (TSR FLUSH_TEXT sequence)
if espress.flush_with_ack():
    print("Flush acknowledged")

# Disconnect
espress.disconnect()
```

### Available Voices

```python
from dectalk_serial import VOICES
print(list(VOICES.keys()))
# ['Paul', 'Betty', 'Harry', 'Frank', 'Dennis', 'Kit', 'Ursula', 'Rita', 'Wendy']
```

### Building DECtalk Command Strings

```python
from dectalk_serial import build_dectalk_prefix

# Build inline command prefix
prefix = build_dectalk_prefix(voice="Harry", rate=150, pitch=90)
print(prefix)  # "[:nh][:ra 150][:dv ap 90]"
```

