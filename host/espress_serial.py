# SPDX-License-Identifier: MIT
# Copyright (c) 2025 Leland Lucius
"""
DECtalk Serial API - Python module for communicating with DECtalk on ESP32.

This module provides a serial interface for the DECtalk text-to-speech engine
running on an ESP32 microcontroller using the DECtalk ESPress serial protocol
(plain text + control characters + DLE sequences + XON/XOFF flow control).

The ESP32 firmware boots directly into ESPress protocol mode on its USB
CDC-ACM port.  From the host's perspective the USB CDC device appears as
a regular serial (COM / ttyACM) port.

Usage:
    from espress_serial import DECtalkESPressSerial

    espress = DECtalkESPressSerial()
    espress.connect("/dev/ttyACM0")
    espress.speak("Hello world")
    status = espress.request_status()
    espress.flush()
    espress.disconnect()
"""

import serial
import serial.tools.list_ports
import threading
import time
import json
import os


# ---- Canonical voice list (loaded from voices.json) -----------------
# The single source of truth is voices.json in the repository root.
# The firmware's custom_actions.c voice_map[] table is kept in sync
# with the same data.
_VOICES_JSON = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                            "voices.json")
try:
    with open(_VOICES_JSON, "r") as _f:
        _voice_data = json.load(_f)["voices"]
    VOICES = {v["name"]: v["code"] for v in _voice_data}
except (FileNotFoundError, KeyError, json.JSONDecodeError):
    # Fallback for standalone usage when voices.json is not found in the repository root
    VOICES = {
        "Paul": "np", "Betty": "nb", "Harry": "nh", "Frank": "nf",
        "Dennis": "nd", "Kit": "nk", "Ursula": "nu", "Rita": "nr",
        "Wendy": "nw",
    }

# Speaking rate limits (words per minute)
RATE_MIN = 75
RATE_MAX = 600
RATE_DEFAULT = 200

# Pitch limits (Hz, average pitch)
PITCH_MIN = 50
PITCH_MAX = 400
PITCH_DEFAULT = 0  # 0 means use voice default


def build_espress_prefix(voice=None, rate=None, pitch=None):
    """Build a DECtalk inline command prefix string.

    Args:
        voice: Voice name (e.g. 'Paul', 'Betty'). None for no change.
        rate: Speaking rate in WPM (75-600). None for no change.
        pitch: Average pitch in Hz (50-400). None/0 for voice default.

    Returns:
        str: DECtalk inline command string (e.g. '[:nb][:ra 200][:dv ap 120]').
    """
    parts = []
    if voice and voice in VOICES:
        parts.append("[:%s]" % VOICES[voice])
    if rate is not None:
        rate = max(RATE_MIN, min(RATE_MAX, int(rate)))
        parts.append("[:ra %d]" % rate)
    if pitch is not None and pitch > 0:
        pitch = max(PITCH_MIN, min(PITCH_MAX, int(pitch)))
        parts.append("[:dv ap %d]" % pitch)
    return "".join(parts)


# -- DECtalk ESPress Protocol Constants ------------------------------

# ASCII control characters used by the ESPress protocol
_DT_SOH  = 0x01   # Flush acknowledge
_DT_ETX  = 0x03   # Flush/cancel all speech
_DT_ENQ  = 0x05   # Request status
_DT_SO   = 0x0E   # Pause speech
_DT_SI   = 0x0F   # Resume speech
_DT_DLE  = 0x10   # Start 4-byte DLE sequence
_DT_XON  = 0x11   # Resume transmission
_DT_XOFF = 0x13   # Pause transmission

# DLE byte 1 prefixes
_DLE_PREFIX_STATUS = 0x40
_DLE_PREFIX_INDEX  = 0x50

# TSR FLUSH_TEXT sequence bracket character
_DT_FLUSH_BRACKET = ord(']')

# Status bits
ESPRESS_STAT_INT         = 0x0001
ESPRESS_STAT_TR_CHAR     = 0x0002
ESPRESS_STAT_RR_CHAR     = 0x0004
ESPRESS_STAT_CMD_READY   = 0x0008
ESPRESS_STAT_DMA_READY   = 0x0010
ESPRESS_STAT_DIGITIZED   = 0x0020
ESPRESS_STAT_NEW_INDEX   = 0x0040
ESPRESS_STAT_NEW_STATUS  = 0x0080
ESPRESS_STAT_INDEX_VALID = 0x0200
ESPRESS_STAT_FLUSHING    = 0x0400

ESPRESS_DEFAULT_BAUD = 115200
ESPRESS_DEFAULT_TIMEOUT = 1.0
ESPRESS_CONNECT_TIMEOUT = 5.0


def _dle_encode_byte(val6):
    """Encode a 6-bit value for DLE transmission."""
    val6 &= 0x3F
    return (val6 + 0x40) if val6 < 0x20 else val6


def _dle_encode_word(prefix, word):
    """Encode a 16-bit word into a 4-byte DLE sequence."""
    return bytes([
        _DT_DLE,
        prefix | ((word >> 12) & 0x0F),
        _dle_encode_byte((word >> 6) & 0x3F),
        _dle_encode_byte(word & 0x3F),
    ])


def _dle_decode_word(buf):
    """Decode a 16-bit word from a 4-byte DLE sequence."""
    return (((buf[1] & 0x0F) << 12) |
            ((buf[2] & 0x3F) << 6) |
             (buf[3] & 0x3F))


class DECtalkESPressSerial:
    """Host-side interface for the DECtalk ESPress serial protocol.

    Communicates with an ESP32 running the DECtalk firmware using the native
    ESPress protocol: plain ASCII text, control characters, DLE command
    sequences, and XON/XOFF software flow control.

    The ESP32 firmware boots directly into ESPress protocol mode on its
    USB CDC-ACM port.  The host sees it as a regular serial (COM / ttyACM)
    port.  Text is sent directly and the device speaks it immediately.
    """

    def __init__(self):
        self._serial = None
        self._lock = threading.Lock()
        self._connected = False
        self._device_xoff = False  # Device asked us to pause
        self._last_status = 0

    @property
    def connected(self):
        """True if a serial connection is active."""
        return self._connected

    @property
    def last_status(self):
        """The last received device status word."""
        return self._last_status

    @staticmethod
    def list_ports():
        """Return a list of available serial port names."""
        return [port.device for port in serial.tools.list_ports.comports()]

    def connect(self, port, baud=ESPRESS_DEFAULT_BAUD,
                timeout=ESPRESS_DEFAULT_TIMEOUT):
        """Open the serial port and synchronize with the ESP32.

        The ESP32 firmware boots directly into ESPress protocol mode.
        Opening the USB CDC port triggers a DTR assertion that the firmware
        detects as a (re)connection, causing it to reset protocol state
        and send XON.  This method waits for that XON or a DLE status
        response to confirm the device is ready.

        XON/XOFF flow control is handled at the application level (not
        by the pyserial driver) so that the protocol layer can track
        device flow-control state explicitly.

        Args:
            port: Serial port path (e.g. '/dev/ttyACM0' or 'COM3').
            baud: Baud rate (default 115200).  Ignored by USB CDC but
                  set for compatibility with pyserial.
            timeout: Read timeout in seconds.

        Raises:
            serial.SerialException: If the port cannot be opened.
            ConnectionError: If the device does not respond.
        """
        with self._lock:
            if self._serial and self._serial.is_open:
                self._serial.close()

            self._serial = serial.Serial(
                port,
                baud,
                bytesize=serial.EIGHTBITS,
                parity=serial.PARITY_NONE,
                stopbits=serial.STOPBITS_ONE,
                xonxoff=False,
                timeout=timeout,
            )
            self._connected = True
            self._device_xoff = False

            # Opening the port asserts DTR which triggers the firmware's
            # reconnection handler.  Give the device a moment to reset
            # protocol state and send its XON ready signal.
            time.sleep(0.5)
            self._serial.reset_input_buffer()

            # Wait for XON (device ready) or a DLE status sequence.
            # The firmware sends XON on startup and after each reconnect.
            # serial.read(1) blocks for up to `timeout` seconds when no
            # data is available, so this loop does not busy-wait.
            #
            # If the firmware's XON arrived before the buffer was cleared
            # above, no unsolicited data will follow.  After the first
            # empty read we send an ENQ (status request) to actively
            # probe the device.  The firmware always replies to ENQ with
            # a 4-byte DLE status sequence, confirming the connection.
            deadline = time.time() + ESPRESS_CONNECT_TIMEOUT
            entered = False
            probed = False
            while time.time() < deadline:
                data = self._serial.read(1)
                if not data:
                    if not probed:
                        # No passive XON received — actively probe.
                        self._serial.write(bytes([_DT_ENQ]))
                        self._serial.flush()
                        probed = True
                    continue
                b = data[0]
                if b == _DT_XON:
                    # Device is ready
                    entered = True
                    break
                if b == _DT_DLE:
                    # Got a DLE status -- device is already active
                    rest = self._serial.read(3)
                    if len(rest) == 3:
                        buf = bytes([_DT_DLE]) + rest
                        self._last_status = _dle_decode_word(buf)
                    entered = True
                    break
                # Skip any other bytes (stale data, boot messages, etc.)

            if not entered:
                self._serial.close()
                self._connected = False
                raise ConnectionError(
                    "No response from the ESP32 DECtalk device. "
                    "Check that the device is powered on and the correct "
                    "port is selected."
                )

            # Drain any remaining startup data
            self._serial.reset_input_buffer()

            # Send XON to tell the device we are ready to receive
            self._serial.write(bytes([_DT_XON]))
            self._serial.flush()
            time.sleep(0.1)

    def disconnect(self):
        """Close the serial connection."""
        with self._lock:
            if self._serial and self._serial.is_open:
                self._serial.close()
            self._connected = False

    def send_text(self, text):
        """Send plain text to the device for speech synthesis.

        The text is sent as-is over the serial port. The device parses it
        and produces speech. A carriage return is appended to signal
        end-of-clause.

        If the device has sent XOFF, this method waits briefly for XON
        before transmitting.

        Args:
            text: The text to speak (ASCII).

        Raises:
            ConnectionError: If not connected.
        """
        self._check_connected()
        with self._lock:
            self._wait_for_xon()
            data = text.encode("ascii", errors="replace") + b"\r"
            self._serial.write(data)
            self._serial.flush()

    def speak(self, text, voice=None, rate=None, pitch=None):
        """Send text with optional voice/rate/pitch settings.

        DECtalk inline commands are prepended for voice, rate, and pitch.

        Args:
            text: The text to speak.
            voice: Voice name (e.g. 'Paul', 'Betty'). None keeps current.
            rate: Speaking rate in WPM (75-600). None keeps current.
            pitch: Average pitch in Hz (50-400). None keeps current.

        Raises:
            ConnectionError: If not connected.
        """
        prefix = build_espress_prefix(voice=voice, rate=rate, pitch=pitch)
        self.send_text(prefix + text)

    def flush(self):
        """Cancel all pending speech (send ETX).

        Raises:
            ConnectionError: If not connected.
        """
        self._check_connected()
        with self._lock:
            self._serial.write(bytes([_DT_ETX]))
            self._serial.flush()

    def flush_with_ack(self):
        """Send the TSR FLUSH_TEXT sequence and wait for SOH acknowledge.

        Sends ']' + ETX + XON and waits for SOH (0x01) response.
        XON/XOFF bytes received during the wait are handled.

        Returns:
            True if SOH was received, False on timeout.

        Raises:
            ConnectionError: If not connected.
        """
        self._check_connected()
        with self._lock:
            self._serial.write(bytes([_DT_FLUSH_BRACKET, _DT_ETX, _DT_XON]))
            self._serial.flush()

            # Wait for SOH acknowledge
            deadline = time.time() + 2.0
            while time.time() < deadline:
                data = self._serial.read(1)
                if not data:
                    continue
                b = data[0]
                if b == _DT_SOH:
                    return True
                if b == _DT_XON:
                    self._device_xoff = False
                    continue
                if b == _DT_XOFF:
                    self._device_xoff = True
                    continue
                if b == _DT_DLE:
                    self._consume_dle_sequence()
            return False

    def pause(self):
        """Pause speech output (send SO).

        Raises:
            ConnectionError: If not connected.
        """
        self._check_connected()
        with self._lock:
            self._serial.write(bytes([_DT_SO]))
            self._serial.flush()

    def resume(self):
        """Resume speech output (send SI).

        Raises:
            ConnectionError: If not connected.
        """
        self._check_connected()
        with self._lock:
            self._serial.write(bytes([_DT_SI]))
            self._serial.flush()

    def request_status(self):
        """Request status from the device (send ENQ).

        Sends ENQ and reads the DLE status response.

        Returns:
            int: The 16-bit status word, or -1 on timeout.

        Raises:
            ConnectionError: If not connected.
        """
        self._check_connected()
        with self._lock:
            self._serial.write(bytes([_DT_ENQ]))
            self._serial.flush()
            return self._read_dle_status(timeout=2.0)

    def read_response(self, timeout=1.0):
        """Read and decode a DLE response from the device.

        Returns:
            tuple: (type, value) where type is 'status', 'index', or None,
                   and value is the decoded 16-bit word.
        """
        self._check_connected()
        with self._lock:
            return self._read_dle_response(timeout)

    def detect_device(self):
        """Detect a DECtalk ESPress device using the standard probe sequence.

        Sends XON, waits, sends ETX + ENQ, and checks for a status response.

        Returns:
            True if a device was detected, False otherwise.

        Raises:
            ConnectionError: If not connected.
        """
        self._check_connected()
        with self._lock:
            # Step 1: Send XON to synchronize
            self._serial.write(bytes([_DT_XON]))
            self._serial.flush()
            time.sleep(0.055)  # ~1 tick

            # Step 2: Flush receive buffer
            self._serial.reset_input_buffer()

            # Step 3: Send ETX + ENQ
            self._serial.write(bytes([_DT_ETX, _DT_ENQ]))
            self._serial.flush()
            time.sleep(0.110)  # ~2 ticks

            # Step 4: Check for status response
            status = self._read_dle_status(timeout=0.5)
            return status > 0

    def _check_connected(self):
        if not self._connected:
            raise ConnectionError("Not connected to DECtalk ESPress.")
        if not self._serial or not self._serial.is_open:
            self._connected = False
            raise ConnectionError("Serial port is not open.")

    def _read_dle_status(self, timeout=1.0):
        """Read a DLE status sequence and return the decoded status word."""
        result = self._read_dle_response(timeout)
        if result and result[0] == "status":
            return result[1]
        return -1

    def _read_dle_response(self, timeout=1.0):
        """Read and decode one DLE response (status or index).

        XON/XOFF bytes are consumed and update ``_device_xoff`` state
        since software flow control is handled at the application level.
        """
        old_timeout = self._serial.timeout
        self._serial.timeout = timeout
        try:
            # Scan for DLE byte
            deadline = time.time() + timeout
            while time.time() < deadline:
                data = self._serial.read(1)
                if not data:
                    return None
                b = data[0]
                if b == _DT_XON:
                    self._device_xoff = False
                    continue
                if b == _DT_XOFF:
                    self._device_xoff = True
                    continue
                if b == _DT_DLE:
                    # Read remaining 3 bytes
                    rest = self._serial.read(3)
                    if len(rest) < 3:
                        return None
                    buf = bytes([_DT_DLE]) + rest
                    word = _dle_decode_word(buf)

                    prefix = buf[1] & 0xF0
                    if prefix == _DLE_PREFIX_STATUS:
                        self._last_status = word
                        return ("status", word)
                    elif prefix == _DLE_PREFIX_INDEX:
                        return ("index", word)
                    else:
                        return ("command", word)
                # Skip other non-DLE bytes (SOH, etc.)
            return None
        finally:
            self._serial.timeout = old_timeout

    def _consume_dle_sequence(self):
        """Read and discard 3 more bytes of a DLE sequence."""
        self._serial.read(3)

    def _wait_for_xon(self, timeout=2.0):
        """If the device sent XOFF, wait until XON is received or timeout."""
        if not self._device_xoff:
            return
        deadline = time.time() + timeout
        old_timeout = self._serial.timeout
        self._serial.timeout = 0.1
        try:
            while self._device_xoff and time.time() < deadline:
                data = self._serial.read(1)
                if data:
                    b = data[0]
                    if b == _DT_XON:
                        self._device_xoff = False
                    elif b == _DT_XOFF:
                        self._device_xoff = True
        finally:
            self._serial.timeout = old_timeout
