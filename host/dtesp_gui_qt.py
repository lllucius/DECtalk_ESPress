#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2025 Leland Lucius
"""
DECtalk ESPress Host GUI (Qt) - PySide6/PyQt6 application for controlling
DECtalk via the ESPress serial protocol.

Provides a graphical interface to:
  - Connect to an ESP32 running DECtalk in ESPress mode via serial port
  - Enter and edit multi-line text for speech synthesis
  - Select from 9 DECtalk voices
  - Adjust speaking rate (words per minute) and pitch (Hz)
  - Pause/Resume speech output
  - Flush (cancel) pending speech
  - Monitor device status via DLE status responses
  - Standard cut, copy, paste, and clear operations
  - Full keyboard and screen-reader accessibility

The ESPress protocol uses raw control characters (ETX, ENQ, SO, SI) and
DLE sequences instead of the line-based API protocol used by the standard
GUI.  This makes it compatible with the original DECtalk ESPress hardware.

Usage:
    python dtesp_gui_qt.py

Requirements:
    pip install PySide6 pyserial
"""

import sys
import os
import threading
import time

# Try PySide6 first, fall back to PyQt6
try:
    from PySide6.QtWidgets import (
        QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout,
        QGroupBox, QLabel, QComboBox, QPushButton, QPlainTextEdit,
        QSlider, QStatusBar, QMenu, QMessageBox, QSplitter,
        QDialog, QDialogButtonBox, QCheckBox, QGridLayout, QRadioButton,
        QButtonGroup,
    )
    from PySide6.QtCore import Qt, Signal, QObject
    from PySide6.QtGui import QFont, QKeySequence, QTextCursor, QAction
    _QT_BINDING = "PySide6"
except ImportError:
    from PyQt6.QtWidgets import (
        QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout,
        QGroupBox, QLabel, QComboBox, QPushButton, QPlainTextEdit,
        QSlider, QStatusBar, QMenu, QMessageBox, QSplitter,
        QDialog, QDialogButtonBox, QCheckBox, QGridLayout, QRadioButton,
        QButtonGroup,
    )
    from PyQt6.QtCore import Qt, pyqtSignal as Signal, QObject
    from PyQt6.QtGui import QFont, QKeySequence, QTextCursor, QAction
    _QT_BINDING = "PyQt6"

# Allow running from the host/ directory or the repo root
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from dtesp_serial import (
    DECtalkESPressSerial,
    VOICES,
    RATE_MIN,
    RATE_MAX,
    RATE_DEFAULT,
    PITCH_MIN,
    PITCH_MAX,
    PITCH_DEFAULT,
    DTESP_DEFAULT_BAUD,
    DTESP_STAT_TR_CHAR,
    DTESP_STAT_RR_CHAR,
    DTESP_STAT_CMD_READY,
    DTESP_STAT_NEW_INDEX,
    DTESP_STAT_FLUSHING,
    build_dtesp_prefix,
)


class _SignalBridge(QObject):
    """Bridge for emitting Qt signals from background threads."""
    connected = Signal()
    connect_error = Signal(str)
    status_update = Signal(str)
    log_entry = Signal(str, str)
    device_status = Signal(int, bool)


# ---- Codec / DSP control ranges (mirror firmware PR #63) ------------

# Bass / treble shelving tone controls
TONE_GAIN_MIN = -12
TONE_GAIN_MAX = 12

# 5-band peaking EQ centre frequencies (Hz), matched to tlv320_dsp.c
EQ_BAND_FREQS = (160, 500, 1500, 3000, 5000)
EQ_GAIN_MIN = -12
EQ_GAIN_MAX = 12

# Named EQ and DRC presets exposed by the firmware
EQ_PRESETS  = ("flat", "speech", "crisp", "warm")
DRC_PRESETS = ("soft", "speech", "loud")

# Class-D speaker-amp analog gain stages (dB)
SPK_GAIN_VALUES = (6, 12, 18, 24)

# Volume levels (TLV320_MAX_VOLUME = 9)
VOLUME_MIN = 0
VOLUME_MAX = 9
VOLUME_DEFAULT = 5


class AudioSettingsDialog(QDialog):
    """Modal dialog exposing the firmware's `[:fw ...]` audio / DSP commands.

    All controls operate in "fire-and-forget" mode — each change is sent
    immediately to the device as an inline ``[:fw ...]`` command via
    :meth:`DECtalkESPressGUIQt._send_fw_cmd`.  The device is authoritative;
    the dialog simply remembers the last values the user selected so they
    persist across opens within the same session.

    Accessibility:
      - Every control has an accessible name and description.
      - Labels use ``&`` mnemonics and ``setBuddy`` to focus their control.
      - A logical tab order is installed at the end of construction.
      - The dialog can be closed with Escape / Alt+C (Close).
    """

    def __init__(self, parent, state):
        super().__init__(parent)
        self._parent = parent
        self._state = state          # Shared dict of last-known settings
        self._loading = True         # Suppress signals during initial load

        self.setWindowTitle("Audio Settings")
        self.setAccessibleName("Audio settings dialog")
        self.setAccessibleDescription(
            "Dialog for codec output routing, tone controls, equalizer, "
            "dynamic range compression, and speaker amplifier gain."
        )
        self.setModal(True)
        # Give screen-reader users a sensible default focus.
        self.setMinimumWidth(560)

        self._build_ui()
        self._load_from_state()
        self._loading = False

    # -- Construction -----------------------------------------------

    def _build_ui(self):
        root = QVBoxLayout(self)
        root.setSpacing(8)

        self._build_output_group(root)
        self._build_tone_group(root)
        self._build_eq_group(root)
        self._build_drc_group(root)

        # Persist + Close buttons
        btns = QDialogButtonBox(self)
        btns.setAccessibleName("Dialog buttons")
        self.save_btn = btns.addButton(
            "&Save to Device", QDialogButtonBox.ButtonRole.ActionRole
        )
        self.save_btn.setAccessibleName("Save to device")
        self.save_btn.setAccessibleDescription(
            "Persist current codec and DSP settings to non-volatile storage "
            "on the device. Sends the [:fw save] command."
        )
        self.save_btn.clicked.connect(self._on_save)

        self.close_btn = btns.addButton(
            "&Close", QDialogButtonBox.ButtonRole.RejectRole
        )
        self.close_btn.setAccessibleName("Close audio settings")
        self.close_btn.setAccessibleDescription(
            "Close the Audio Settings dialog. "
            "Changes already made remain in effect on the device."
        )
        btns.rejected.connect(self.reject)
        root.addWidget(btns)

        self._setup_tab_order()

    # ---- Output routing / levels ---------------------------------

    def _build_output_group(self, parent_layout):
        group = QGroupBox("&Output")
        group.setAccessibleName("Output routing and levels")
        grid = QGridLayout(group)
        grid.setHorizontalSpacing(8)
        grid.setVerticalSpacing(4)

        # Profile (speaker / headphone) as radio buttons
        profile_label = QLabel("Profile:")
        grid.addWidget(profile_label, 0, 0)

        self.profile_speaker = QRadioButton("Spea&ker")
        self.profile_speaker.setAccessibleName("Speaker profile")
        self.profile_speaker.setAccessibleDescription(
            "Route audio to the class-D speaker output"
        )
        self.profile_headphone = QRadioButton("&Headphone")
        self.profile_headphone.setAccessibleName("Headphone profile")
        self.profile_headphone.setAccessibleDescription(
            "Route audio to the headphone jack"
        )
        self._profile_group = QButtonGroup(self)
        self._profile_group.addButton(self.profile_speaker, 0)
        self._profile_group.addButton(self.profile_headphone, 1)
        self._profile_group.buttonClicked.connect(self._on_profile_changed)
        prow = QHBoxLayout()
        prow.addWidget(self.profile_speaker)
        prow.addWidget(self.profile_headphone)
        prow.addStretch()
        grid.addLayout(prow, 0, 1, 1, 3)

        # Autoswitch
        self.autoswitch_chk = QCheckBox("Headset &auto-switch")
        self.autoswitch_chk.setAccessibleName("Headset auto-switch")
        self.autoswitch_chk.setAccessibleDescription(
            "When enabled, inserting a headphone plug switches to the "
            "headphone profile and removal switches back to the speaker"
        )
        self.autoswitch_chk.toggled.connect(self._on_autoswitch_toggled)
        grid.addWidget(self.autoswitch_chk, 1, 1, 1, 3)

        # Volume slider
        vol_label = QLabel("&Volume:")
        self.volume_slider = QSlider(Qt.Orientation.Horizontal)
        self.volume_slider.setMinimum(VOLUME_MIN)
        self.volume_slider.setMaximum(VOLUME_MAX)
        self.volume_slider.setPageStep(1)
        self.volume_slider.setSingleStep(1)
        self.volume_slider.setTickPosition(QSlider.TickPosition.TicksBelow)
        self.volume_slider.setTickInterval(1)
        self.volume_slider.setAccessibleName("Codec digital volume")
        self.volume_slider.setAccessibleDescription(
            "Codec digital volume level, from %d (near-mute) to %d (0 dB)"
            % (VOLUME_MIN, VOLUME_MAX)
        )
        vol_label.setBuddy(self.volume_slider)
        self.volume_slider.valueChanged.connect(self._on_volume_changed)
        self.volume_slider.sliderReleased.connect(
            lambda: self._on_volume_commit(self.volume_slider.value())
        )
        self.volume_value_label = QLabel("--")
        self.volume_value_label.setFixedWidth(30)
        self.volume_value_label.setAccessibleName("Current volume value")
        grid.addWidget(vol_label, 2, 0)
        grid.addWidget(self.volume_slider, 2, 1, 1, 2)
        grid.addWidget(self.volume_value_label, 2, 3)

        # Speaker gain combobox
        spk_label = QLabel("Speaker &gain (dB):")
        self.spkgain_combo = QComboBox()
        for v in SPK_GAIN_VALUES:
            self.spkgain_combo.addItem(str(v), userData=v)
        self.spkgain_combo.setAccessibleName("Speaker amplifier gain")
        self.spkgain_combo.setAccessibleDescription(
            "Class-D speaker amplifier analog gain in decibels. "
            "Only affects the speaker output path."
        )
        spk_label.setBuddy(self.spkgain_combo)
        self.spkgain_combo.currentIndexChanged.connect(
            self._on_spkgain_changed
        )
        grid.addWidget(spk_label, 3, 0)
        grid.addWidget(self.spkgain_combo, 3, 1)

        # Mute
        self.mute_chk = QCheckBox("&Mute")
        self.mute_chk.setAccessibleName("Mute")
        self.mute_chk.setAccessibleDescription("Soft-mute the codec output")
        self.mute_chk.toggled.connect(self._on_mute_toggled)
        grid.addWidget(self.mute_chk, 3, 2, 1, 2)

        parent_layout.addWidget(group)

    # ---- Tone controls -------------------------------------------

    def _build_tone_group(self, parent_layout):
        group = QGroupBox("&Tone Controls")
        group.setAccessibleName("Tone controls")
        group.setAccessibleDescription(
            "Low-shelf bass and high-shelf treble tone controls"
        )
        grid = QGridLayout(group)
        grid.setHorizontalSpacing(8)

        self.bass_slider, self.bass_value_label = self._make_gain_slider(
            grid, 0, "&Bass (dB):", "Bass tone control",
            "Low-shelf bass gain around 200 Hz, from %d to %d dB"
            % (TONE_GAIN_MIN, TONE_GAIN_MAX),
            TONE_GAIN_MIN, TONE_GAIN_MAX,
            commit=lambda v: self._on_tone_commit("bass", v),
        )

        self.treble_slider, self.treble_value_label = self._make_gain_slider(
            grid, 1, "T&reble (dB):", "Treble tone control",
            "High-shelf treble gain around 4.5 kHz, from %d to %d dB"
            % (TONE_GAIN_MIN, TONE_GAIN_MAX),
            TONE_GAIN_MIN, TONE_GAIN_MAX,
            commit=lambda v: self._on_tone_commit("treble", v),
        )

        parent_layout.addWidget(group)

    # ---- Equalizer -----------------------------------------------

    def _build_eq_group(self, parent_layout):
        group = QGroupBox("&Equalizer (5-band peaking)")
        group.setAccessibleName("Equalizer")
        group.setAccessibleDescription(
            "Five-band peaking equalizer at 160, 500, 1500, 3000, and "
            "5000 Hz, with named presets"
        )
        vlay = QVBoxLayout(group)

        grid = QGridLayout()
        grid.setHorizontalSpacing(8)
        self.eq_sliders = []
        self.eq_value_labels = []
        # Mnemonic digit keys: Alt+1 .. Alt+5 jump to each band
        for i, f in enumerate(EQ_BAND_FREQS):
            label_text = "&%d: %s Hz (dB):" % (i + 1, self._freq_text(f))
            s, v = self._make_gain_slider(
                grid, i, label_text,
                "Equalizer band %d" % (i + 1),
                "Peaking EQ gain at %d Hz, from %d to %d dB"
                % (f, EQ_GAIN_MIN, EQ_GAIN_MAX),
                EQ_GAIN_MIN, EQ_GAIN_MAX,
                commit=lambda val, idx=i: self._on_eq_commit(idx, val),
            )
            self.eq_sliders.append(s)
            self.eq_value_labels.append(v)
        vlay.addLayout(grid)

        # Preset + reset row
        prow = QHBoxLayout()
        preset_label = QLabel("EQ &preset:")
        self.eq_preset_combo = QComboBox()
        self.eq_preset_combo.addItem("(custom)", userData=None)
        for name in EQ_PRESETS:
            self.eq_preset_combo.addItem(name, userData=name)
        self.eq_preset_combo.setAccessibleName("Equalizer preset")
        self.eq_preset_combo.setAccessibleDescription(
            "Load a named equalizer preset: flat, speech, crisp, or warm"
        )
        preset_label.setBuddy(self.eq_preset_combo)
        self.eq_preset_combo.activated.connect(self._on_eq_preset_activated)
        prow.addWidget(preset_label)
        prow.addWidget(self.eq_preset_combo)

        self.eq_reset_btn = QPushButton("Reset &Bands")
        self.eq_reset_btn.setAccessibleName("Reset equalizer bands")
        self.eq_reset_btn.setAccessibleDescription(
            "Flatten every peaking band to 0 dB. Bass and treble are "
            "preserved. Sends the [:fw eq reset] command."
        )
        self.eq_reset_btn.clicked.connect(self._on_eq_reset)
        prow.addWidget(self.eq_reset_btn)
        prow.addStretch()
        vlay.addLayout(prow)

        parent_layout.addWidget(group)

    # ---- DRC -----------------------------------------------------

    def _build_drc_group(self, parent_layout):
        group = QGroupBox("D&ynamic Range Compression")
        group.setAccessibleName("Dynamic range compression")
        group.setAccessibleDescription(
            "Enable dynamic range compression and select a tuning preset"
        )
        hlay = QHBoxLayout(group)

        self.drc_chk = QCheckBox("DRC &enabled")
        self.drc_chk.setAccessibleName("DRC enabled")
        self.drc_chk.setAccessibleDescription(
            "Turn dynamic range compression on or off"
        )
        self.drc_chk.toggled.connect(self._on_drc_toggled)
        hlay.addWidget(self.drc_chk)

        drcp_label = QLabel("Prese&t:")
        self.drc_preset_combo = QComboBox()
        for name in DRC_PRESETS:
            self.drc_preset_combo.addItem(name, userData=name)
        self.drc_preset_combo.setAccessibleName("DRC preset")
        self.drc_preset_combo.setAccessibleDescription(
            "Select a DRC tuning preset: soft, speech, or loud"
        )
        drcp_label.setBuddy(self.drc_preset_combo)
        self.drc_preset_combo.currentIndexChanged.connect(
            self._on_drc_preset_changed
        )
        hlay.addWidget(drcp_label)
        hlay.addWidget(self.drc_preset_combo)
        hlay.addStretch()

        parent_layout.addWidget(group)

    # ---- Helpers -------------------------------------------------

    @staticmethod
    def _freq_text(hz):
        if hz >= 1000:
            khz = hz / 1000.0
            # "1.5 kHz", but trim to "3 kHz" when the fractional part is 0
            if abs(khz - round(khz)) < 1e-6:
                return "%d kHz" % round(khz)
            return "%.1f kHz" % khz
        return "%d Hz" % hz

    def _make_gain_slider(self, grid, row, label_text,
                          access_name, access_desc,
                          lo, hi, commit):
        """Create one labelled gain slider and place it on ``grid``.

        Commit semantics:
          - ``valueChanged`` always refreshes the numeric read-out.
          - Keyboard / mouse-wheel / click-on-track changes commit
            immediately (``isSliderDown()`` is False in those cases).
          - Mouse-drag changes coalesce: no command is sent until the
            user releases the slider, via ``sliderReleased``.
        """
        lbl = QLabel(label_text)
        s = QSlider(Qt.Orientation.Horizontal)
        s.setMinimum(lo)
        s.setMaximum(hi)
        s.setSingleStep(1)
        s.setPageStep(3)
        s.setTickPosition(QSlider.TickPosition.TicksBelow)
        s.setTickInterval(3)
        s.setValue(0)
        s.setMinimumWidth(220)
        s.setAccessibleName(access_name)
        s.setAccessibleDescription(access_desc)
        lbl.setBuddy(s)

        vlbl = QLabel("0")
        vlbl.setFixedWidth(40)
        vlbl.setAccessibleName(access_name + " value")

        def on_change(val):
            vlbl.setText("%+d" % val if val else "0")
            # Only commit if the user isn't in the middle of a drag;
            # keyboard / wheel / click-on-track all leave isSliderDown
            # as False, so they commit immediately.
            if not s.isSliderDown():
                commit(val)
        s.valueChanged.connect(on_change)
        # End-of-drag commit for mouse users.
        s.sliderReleased.connect(lambda sl=s: commit(sl.value()))

        grid.addWidget(lbl, row, 0)
        grid.addWidget(s, row, 1)
        grid.addWidget(vlbl, row, 2)
        return s, vlbl

    def _setup_tab_order(self):
        chain = [
            self.profile_speaker, self.profile_headphone,
            self.autoswitch_chk,
            self.volume_slider, self.spkgain_combo, self.mute_chk,
            self.bass_slider, self.treble_slider,
        ]
        chain += list(self.eq_sliders)
        chain += [
            self.eq_preset_combo, self.eq_reset_btn,
            self.drc_chk, self.drc_preset_combo,
            self.save_btn, self.close_btn,
        ]
        for a, b in zip(chain, chain[1:]):
            self.setTabOrder(a, b)

    # -- Load / sync ------------------------------------------------

    def _load_from_state(self):
        """Populate controls from the parent's persisted settings dict."""
        s = self._state
        if s["profile"] == 1:
            self.profile_headphone.setChecked(True)
        else:
            self.profile_speaker.setChecked(True)
        self.autoswitch_chk.setChecked(bool(s["autoswitch"]))
        self.volume_slider.setValue(int(s["volume"]))
        self.volume_value_label.setText(str(int(s["volume"])))
        try:
            self.spkgain_combo.setCurrentIndex(
                SPK_GAIN_VALUES.index(int(s["spkgain"]))
            )
        except ValueError:
            self.spkgain_combo.setCurrentIndex(0)
        self.mute_chk.setChecked(bool(s["mute"]))

        self.bass_slider.setValue(int(s["bass"]))
        self.treble_slider.setValue(int(s["treble"]))
        for i, sl in enumerate(self.eq_sliders):
            sl.setValue(int(s["eq"][i]))

        # EQ preset combo: "(custom)" unless user explicitly loaded one.
        if s.get("eq_preset") in EQ_PRESETS:
            idx = EQ_PRESETS.index(s["eq_preset"]) + 1  # +1 for "(custom)"
            self.eq_preset_combo.setCurrentIndex(idx)
        else:
            self.eq_preset_combo.setCurrentIndex(0)

        self.drc_chk.setChecked(bool(s["drc"]))
        try:
            self.drc_preset_combo.setCurrentIndex(
                DRC_PRESETS.index(s["drc_preset"])
            )
        except ValueError:
            self.drc_preset_combo.setCurrentIndex(0)

    # -- Command emitters (each routed through parent) --------------

    def _send(self, subcmd):
        if self._loading:
            return
        self._parent._send_fw_cmd(subcmd)

    # Output
    def _on_profile_changed(self, _btn):
        idx = self._profile_group.checkedId()
        name = "headphone" if idx == 1 else "speaker"
        self._state["profile"] = idx
        self._send("profile %s" % name)

    def _on_autoswitch_toggled(self, checked):
        self._state["autoswitch"] = bool(checked)
        self._send("autoswitch %s" % ("on" if checked else "off"))

    def _on_volume_changed(self, val):
        """Refresh the volume read-out; commit unless a drag is in progress."""
        self.volume_value_label.setText(str(val))
        if not self.volume_slider.isSliderDown():
            self._on_volume_commit(val)

    def _on_volume_commit(self, val):
        self._state["volume"] = int(val)
        self._send("volume %d" % int(val))

    def _on_spkgain_changed(self, idx):
        val = self.spkgain_combo.itemData(idx)
        if val is None:
            return
        self._state["spkgain"] = int(val)
        self._send("spkgain %d" % int(val))

    def _on_mute_toggled(self, checked):
        self._state["mute"] = bool(checked)
        self._send("mute %s" % ("on" if checked else "off"))

    # Tone
    def _on_tone_commit(self, which, val):
        self._state[which] = int(val)
        self._send("%s %d" % (which, int(val)))

    # EQ
    def _on_eq_commit(self, band_index, val):
        self._state["eq"][band_index] = int(val)
        # User-driven band change → invalidate preset selection
        self._state["eq_preset"] = None
        self._loading = True
        try:
            self.eq_preset_combo.setCurrentIndex(0)
        finally:
            self._loading = False
        # Firmware command uses 1-based band index.
        self._send("eq %d %d" % (band_index + 1, int(val)))

    def _on_eq_preset_activated(self, idx):
        name = self.eq_preset_combo.itemData(idx)
        if not name:
            return
        self._state["eq_preset"] = name
        # Local mirror: firmware presets only touch peaking bands, not
        # bass/treble (except "flat" which also zeroes those).  Rather
        # than duplicate that table here we simply rely on [:fw eq show]
        # /user interaction; the UI leaves slider positions alone and
        # re-flags as "(custom)" on the next manual tweak.
        self._send("eq preset %s" % name)

    def _on_eq_reset(self):
        for i, sl in enumerate(self.eq_sliders):
            self._loading = True
            try:
                sl.setValue(0)
                self._state["eq"][i] = 0
            finally:
                self._loading = False
            self.eq_value_labels[i].setText("0")
        self._state["eq_preset"] = None
        self._loading = True
        try:
            self.eq_preset_combo.setCurrentIndex(0)
        finally:
            self._loading = False
        self._send("eq reset")

    # DRC
    def _on_drc_toggled(self, checked):
        self._state["drc"] = bool(checked)
        self._send("drc %s" % ("on" if checked else "off"))

    def _on_drc_preset_changed(self, idx):
        name = self.drc_preset_combo.itemData(idx)
        if not name:
            return
        self._state["drc_preset"] = name
        self._send("drc preset %s" % name)

    def _on_save(self):
        self._send("save")


class DECtalkESPressGUIQt(QMainWindow):
    """Main GUI application for DECtalk ESPress host control (Qt version)."""

    # Maximum number of characters from the speak text shown in the log.
    _LOG_SPEAK_MAX_CHARS = 120

    def __init__(self):
        super().__init__()
        self.setWindowTitle("DECtalk ESPress - Host GUI (Qt)")
        self.setMinimumSize(700, 650)
        self.resize(1097, 650)

        self.dtesp = DECtalkESPressSerial()
        self._paused = False
        self._polling = False

        # Last-known audio/DSP settings, mirrored from the dialog.
        # These are the values that would be restored on the device
        # by firmware NVS defaults (see main/fw_settings.c) or by the
        # [:fw save] command.  We keep a local copy so reopening the
        # Audio Settings dialog shows the user their most recent choices.
        self._audio_state = {
            "profile":    0,                  # 0 = speaker, 1 = headphone
            "autoswitch": True,
            "volume":     VOLUME_DEFAULT,
            "spkgain":    SPK_GAIN_VALUES[0], # 6 dB
            "mute":       False,
            "bass":       0,
            "treble":     0,
            "eq":         [0] * len(EQ_BAND_FREQS),
            "eq_preset":  None,
            "drc":        False,
            "drc_preset": DRC_PRESETS[0],     # "soft"
        }
        self._audio_dialog = None

        # Signal bridge for thread-safe UI updates
        self._signals = _SignalBridge()
        self._signals.connected.connect(self._on_connected)
        self._signals.connect_error.connect(self._on_connect_error)
        self._signals.status_update.connect(self._set_status)
        self._signals.log_entry.connect(self._log)
        self._signals.device_status.connect(self._display_status)

        self._build_ui()
        self._refresh_ports()

    # -- UI Construction ------------------------------------------

    def _build_ui(self):
        """Build the complete GUI layout."""
        self._build_menubar()
        central = QWidget()
        self.setCentralWidget(central)
        layout = QVBoxLayout(central)
        layout.setContentsMargins(8, 8, 8, 8)
        layout.setSpacing(4)

        self._build_connection_frame(layout)
        self._build_voice_frame(layout)

        # Splitter for text entry and bottom section (buttons + log)
        splitter = QSplitter(Qt.Orientation.Vertical)
        splitter.setAccessibleName("Main content splitter")

        self._build_text_frame(splitter)

        # Bottom section: buttons, device status, and comm log
        bottom_widget = QWidget()
        bottom_layout = QVBoxLayout(bottom_widget)
        bottom_layout.setContentsMargins(0, 0, 0, 0)
        bottom_layout.setSpacing(4)

        button_widget = QWidget()
        button_layout = QHBoxLayout(button_widget)
        button_layout.setContentsMargins(0, 4, 0, 4)
        self._build_button_frame(button_layout)
        bottom_layout.addWidget(button_widget, stretch=0)

        self._build_device_status_frame(bottom_layout)
        self._build_comm_log_frame(bottom_layout)

        splitter.addWidget(bottom_widget)
        splitter.setStretchFactor(0, 1)  # Text to Speak
        splitter.setStretchFactor(1, 1)  # Bottom (buttons + log)

        layout.addWidget(splitter, stretch=2)

        self._build_status_bar()

        # Set tab order for keyboard navigation
        self._setup_tab_order()

    def _build_connection_frame(self, parent_layout):
        """Serial port connection controls."""
        group = QGroupBox("Connection (ESPress Protocol)")
        group.setAccessibleName("Connection settings")
        group.setAccessibleDescription(
            "Serial port connection controls for the ESPress protocol"
        )
        hlayout = QHBoxLayout(group)

        port_label = QLabel("&Port:")
        self.port_combo = QComboBox()
        self.port_combo.setMinimumWidth(160)
        self.port_combo.setAccessibleName("Serial port")
        self.port_combo.setAccessibleDescription(
            "Select the serial port for your ESP32 device"
        )
        port_label.setBuddy(self.port_combo)
        hlayout.addWidget(port_label)
        hlayout.addWidget(self.port_combo)

        self.refresh_btn = QPushButton("&Refresh")
        self.refresh_btn.setAccessibleName("Refresh ports")
        self.refresh_btn.setAccessibleDescription(
            "Refresh the list of available serial ports"
        )
        self.refresh_btn.clicked.connect(self._refresh_ports)
        hlayout.addWidget(self.refresh_btn)

        hlayout.addSpacing(8)

        baud_label = QLabel("&Baud:")
        self.baud_combo = QComboBox()
        self.baud_combo.addItems(["115200", "9600", "19200", "38400", "57600"])
        self.baud_combo.setCurrentText(str(DTESP_DEFAULT_BAUD))
        self.baud_combo.setAccessibleName("Baud rate")
        self.baud_combo.setAccessibleDescription(
            "Select the baud rate for serial communication"
        )
        baud_label.setBuddy(self.baud_combo)
        hlayout.addWidget(baud_label)
        hlayout.addWidget(self.baud_combo)

        hlayout.addSpacing(8)

        self.connect_btn = QPushButton("Co&nnect")
        self.connect_btn.setAccessibleName("Connect")
        self.connect_btn.setAccessibleDescription(
            "Connect to or disconnect from the DECtalk device"
        )
        self.connect_btn.clicked.connect(self._toggle_connection)
        hlayout.addWidget(self.connect_btn)

        hlayout.addStretch()
        parent_layout.addWidget(group)

    def _build_voice_frame(self, parent_layout):
        """Voice, rate, and pitch controls."""
        group = QGroupBox("Speech Settings")
        group.setAccessibleName("Speech settings")
        group.setAccessibleDescription(
            "Controls for voice selection, speaking rate, and pitch"
        )
        hlayout = QHBoxLayout(group)

        voice_label = QLabel("&Voice:")
        self.voice_combo = QComboBox()
        self.voice_combo.addItems(list(VOICES.keys()))
        self.voice_combo.setCurrentText("Paul")
        self.voice_combo.setAccessibleName("Voice")
        self.voice_combo.setAccessibleDescription(
            "Select the DECtalk voice for speech synthesis"
        )
        voice_label.setBuddy(self.voice_combo)
        hlayout.addWidget(voice_label)
        hlayout.addWidget(self.voice_combo)

        hlayout.addSpacing(16)

        # Rate slider
        rate_label = QLabel("R&ate (WPM):")
        self.rate_slider = QSlider(Qt.Orientation.Horizontal)
        self.rate_slider.setMinimum(RATE_MIN)
        self.rate_slider.setMaximum(RATE_MAX)
        self.rate_slider.setValue(RATE_DEFAULT)
        self.rate_slider.setMinimumWidth(180)
        self.rate_slider.setAccessibleName("Speaking rate")
        self.rate_slider.setAccessibleDescription(
            "Speaking rate in words per minute, from %d to %d" % (RATE_MIN, RATE_MAX)
        )
        self.rate_slider.valueChanged.connect(self._on_rate_change)
        rate_label.setBuddy(self.rate_slider)
        hlayout.addWidget(rate_label)
        hlayout.addWidget(self.rate_slider)
        self.rate_value_label = QLabel(str(RATE_DEFAULT))
        self.rate_value_label.setFixedWidth(35)
        self.rate_value_label.setAccessibleName("Current rate value")
        hlayout.addWidget(self.rate_value_label)

        hlayout.addSpacing(16)

        # Pitch slider
        pitch_label = QLabel("P&itch (Hz):")
        self.pitch_slider = QSlider(Qt.Orientation.Horizontal)
        self.pitch_slider.setMinimum(PITCH_MIN)
        self.pitch_slider.setMaximum(PITCH_MAX)
        self.pitch_slider.setValue(max(PITCH_DEFAULT, PITCH_MIN))
        self.pitch_slider.setMinimumWidth(180)
        self.pitch_slider.setAccessibleName("Pitch")
        self.pitch_slider.setAccessibleDescription(
            "Average pitch in Hz, from %d to %d. Leftmost uses voice default"
            % (PITCH_MIN, PITCH_MAX)
        )
        self.pitch_slider.valueChanged.connect(self._on_pitch_change)
        pitch_label.setBuddy(self.pitch_slider)
        hlayout.addWidget(pitch_label)
        hlayout.addWidget(self.pitch_slider)
        self.pitch_value_label = QLabel("Default")
        self.pitch_value_label.setFixedWidth(50)
        self.pitch_value_label.setAccessibleName("Current pitch value")
        hlayout.addWidget(self.pitch_value_label)

        hlayout.addStretch()
        parent_layout.addWidget(group)

    def _build_text_frame(self, splitter):
        """Multi-line text entry with context menu."""
        group = QGroupBox("Text to Speak")
        group.setAccessibleName("Text to speak")
        group.setAccessibleDescription(
            "Enter the text you want the DECtalk device to speak"
        )
        vlayout = QVBoxLayout(group)

        self.text_edit = QPlainTextEdit()
        self.text_edit.setFont(QFont("monospace", 10))
        self.text_edit.setAccessibleName("Speech text input")
        self.text_edit.setAccessibleDescription(
            "Multi-line text editor. Enter text here then press the Speak "
            "button to send it to the DECtalk device. Supports DECtalk "
            "inline commands."
        )
        self.text_edit.setUndoRedoEnabled(True)
        self.text_edit.setTabChangesFocus(True)

        # Custom context menu
        self.text_edit.setContextMenuPolicy(
            Qt.ContextMenuPolicy.CustomContextMenu
        )
        self.text_edit.customContextMenuRequested.connect(
            self._show_text_context_menu
        )

        vlayout.addWidget(self.text_edit)
        splitter.addWidget(group)

    def _build_button_frame(self, parent_layout):
        """Speak, Pause, Resume, Flush, Query Status, and Clear buttons."""
        self.speak_btn = QPushButton("&Speak")
        self.speak_btn.setEnabled(False)
        self.speak_btn.setAccessibleName("Speak")
        self.speak_btn.setAccessibleDescription(
            "Send the entered text to the DECtalk device for speech synthesis"
        )
        self.speak_btn.clicked.connect(self._on_speak)
        parent_layout.addWidget(self.speak_btn)

        self.pause_btn = QPushButton("Pa&use")
        self.pause_btn.setEnabled(False)
        self.pause_btn.setAccessibleName("Pause speech")
        self.pause_btn.setAccessibleDescription(
            "Pause the current speech output on the device"
        )
        self.pause_btn.clicked.connect(self._on_pause)
        parent_layout.addWidget(self.pause_btn)

        self.resume_btn = QPushButton("Resu&me")
        self.resume_btn.setEnabled(False)
        self.resume_btn.setAccessibleName("Resume speech")
        self.resume_btn.setAccessibleDescription(
            "Resume paused speech output on the device"
        )
        self.resume_btn.clicked.connect(self._on_resume)
        parent_layout.addWidget(self.resume_btn)

        self.flush_btn = QPushButton("F&lush")
        self.flush_btn.setEnabled(False)
        self.flush_btn.setAccessibleName("Flush speech")
        self.flush_btn.setAccessibleDescription(
            "Cancel all pending speech on the device"
        )
        self.flush_btn.clicked.connect(self._on_flush)
        parent_layout.addWidget(self.flush_btn)

        self.status_btn = QPushButton("Query S&tatus")
        self.status_btn.setEnabled(False)
        self.status_btn.setAccessibleName("Query device status")
        self.status_btn.setAccessibleDescription(
            "Send an ENQ command to query the device status"
        )
        self.status_btn.clicked.connect(self._on_query_status)
        parent_layout.addWidget(self.status_btn)

        parent_layout.addStretch()

        self.clear_btn = QPushButton("Cl&ear Text")
        self.clear_btn.setAccessibleName("Clear text")
        self.clear_btn.setAccessibleDescription(
            "Clear all text from the speech text input"
        )
        self.clear_btn.clicked.connect(self._clear)
        parent_layout.addWidget(self.clear_btn)

    def _build_device_status_frame(self, parent_layout):
        """Device status display showing decoded DLE status bits."""
        group = QGroupBox("Device Status")
        group.setAccessibleName("Device status")
        group.setAccessibleDescription(
            "Displays the current device status word and indicator states"
        )
        hlayout = QHBoxLayout(group)

        status_label = QLabel("Status:")
        hlayout.addWidget(status_label)
        self.device_status_label = QLabel("--")
        self.device_status_label.setFont(QFont("monospace", 9))
        self.device_status_label.setAccessibleName("Device status value")
        self.device_status_label.setAccessibleDescription(
            "Raw hexadecimal status word from the device"
        )
        hlayout.addWidget(self.device_status_label)

        hlayout.addSpacing(16)

        # Status indicator labels
        self._status_indicators = {}
        indicator_defs = [
            ("Ready", "Indicates the device is ready to receive commands"),
            ("Transmitting", "Indicates the device is transmitting speech"),
            ("Flushing", "Indicates the device is flushing pending speech"),
            ("Index", "Indicates a new index marker has been reached"),
        ]
        for name, description in indicator_defs:
            lbl = QLabel("(\u2022) " + name)
            lbl.setStyleSheet("color: #b0b0b0;")
            lbl.setAccessibleName("%s indicator" % name)
            lbl.setAccessibleDescription(description + ". Currently inactive")
            hlayout.addWidget(lbl)
            hlayout.addSpacing(12)
            self._status_indicators[name] = lbl

        hlayout.addStretch()
        parent_layout.addWidget(group)

    def _build_comm_log_frame(self, parent_layout):
        """Communications log panel showing timestamped TX/RX events."""
        group = QGroupBox("Communications Log")
        group.setAccessibleName("Communications log")
        group.setAccessibleDescription(
            "Timestamped log of transmitted and received protocol events"
        )
        vlayout = QVBoxLayout(group)

        # Toolbar row
        toolbar = QHBoxLayout()
        clear_log_btn = QPushButton("Clear &Log")
        clear_log_btn.setAccessibleName("Clear communications log")
        clear_log_btn.setAccessibleDescription(
            "Remove all entries from the communications log"
        )
        clear_log_btn.clicked.connect(self._clear_log)
        toolbar.addWidget(clear_log_btn)
        toolbar.addStretch()
        vlayout.addLayout(toolbar)

        # Log text widget (read-only)
        self.log_text = QPlainTextEdit()
        self.log_text.setReadOnly(True)
        self.log_text.setFont(QFont("monospace", 9))
        self.log_text.setAccessibleName("Communications log output")
        self.log_text.setAccessibleDescription(
            "Read-only log of all protocol communications with timestamps"
        )

        # Custom context menu for log
        self.log_text.setContextMenuPolicy(
            Qt.ContextMenuPolicy.CustomContextMenu
        )
        self.log_text.customContextMenuRequested.connect(
            self._show_log_context_menu
        )

        vlayout.addWidget(self.log_text)
        parent_layout.addWidget(group, stretch=1)

    # -- Firmware [:fw ...] command dispatch ----------------------

    def _send_fw_cmd(self, subcmd):
        """Send a ``[:fw <subcmd>]`` inline command to the device.

        The firmware's inline-command tokenizer recognises ``[:fw ...]``
        and dispatches it through ``custom_actions.c`` — the same path
        that parses commands embedded in the speech text.  Sending it
        as text over the existing serial connection therefore reuses
        the transport and flow-control logic for free.

        Called from the Audio Settings dialog.  Logs both the outgoing
        command and any transport error.
        """
        if not self.dtesp.connected:
            self._set_status("Not connected — audio command ignored")
            self._log("--", "FW cmd ignored (not connected): [:fw %s]" % subcmd)
            return

        cmd = "[:fw %s]" % subcmd
        self._log("TX", "FW: %s" % cmd)

        def do_send():
            try:
                self.dtesp.send_text(cmd)
            except Exception as exc:
                err = str(exc)
                self._set_status_from_thread("Error: %s" % err)
                self._log_from_thread("RX", "FW cmd error: %s" % err)

        threading.Thread(target=do_send, daemon=True).start()

    def _open_audio_settings(self):
        """Show the modal Audio Settings dialog."""
        if self._audio_dialog is None:
            self._audio_dialog = AudioSettingsDialog(self, self._audio_state)
        else:
            # Refresh widgets from current state in case of later edits.
            self._audio_dialog._loading = True
            try:
                self._audio_dialog._load_from_state()
            finally:
                self._audio_dialog._loading = False
        self._audio_dialog.exec()

    def _build_status_bar(self):
        """Status bar at the bottom of the window."""
        self.status_bar = QStatusBar()
        self.setStatusBar(self.status_bar)
        self._status_label = QLabel("Disconnected")
        self._status_label.setAccessibleName("Connection status")
        self._status_label.setAccessibleDescription(
            "Shows the current connection status and recent activity"
        )
        self.status_bar.addWidget(self._status_label)

    def _build_menubar(self):
        """Menu bar with keyboard-accessible entry points for features
        that cannot fit inside the main window without growing it.
        """
        menubar = self.menuBar()
        menubar.setAccessibleName("Application menu bar")

        # File menu
        file_menu = menubar.addMenu("&File")
        file_menu.setAccessibleName("File menu")
        quit_act = QAction("&Quit", self)
        quit_act.setShortcut(QKeySequence.StandardKey.Quit)
        quit_act.setStatusTip("Close the DECtalk ESPress GUI")
        quit_act.triggered.connect(self.close)
        file_menu.addAction(quit_act)

        # Device menu
        dev_menu = menubar.addMenu("&Device")
        dev_menu.setAccessibleName("Device menu")

        self.audio_settings_act = QAction("&Audio Settings...", self)
        self.audio_settings_act.setShortcut("Ctrl+Shift+A")
        self.audio_settings_act.setStatusTip(
            "Open codec output, tone, equalizer, compression, and "
            "amplifier-gain controls"
        )
        self.audio_settings_act.setEnabled(False)
        self.audio_settings_act.triggered.connect(self._open_audio_settings)
        dev_menu.addAction(self.audio_settings_act)

        dev_menu.addSeparator()

        self.save_settings_act = QAction(
            "&Save Settings to Device", self
        )
        self.save_settings_act.setStatusTip(
            "Persist current codec and DSP settings to the device's "
            "non-volatile storage"
        )
        self.save_settings_act.setEnabled(False)
        self.save_settings_act.triggered.connect(
            lambda: self._send_fw_cmd("save")
        )
        dev_menu.addAction(self.save_settings_act)

    def _setup_tab_order(self):
        """Set a logical tab order for keyboard navigation."""
        self.setTabOrder(self.port_combo, self.refresh_btn)
        self.setTabOrder(self.refresh_btn, self.baud_combo)
        self.setTabOrder(self.baud_combo, self.connect_btn)
        self.setTabOrder(self.connect_btn, self.voice_combo)
        self.setTabOrder(self.voice_combo, self.rate_slider)
        self.setTabOrder(self.rate_slider, self.pitch_slider)
        self.setTabOrder(self.pitch_slider, self.text_edit)
        self.setTabOrder(self.text_edit, self.speak_btn)
        self.setTabOrder(self.speak_btn, self.pause_btn)
        self.setTabOrder(self.pause_btn, self.resume_btn)
        self.setTabOrder(self.resume_btn, self.flush_btn)
        self.setTabOrder(self.flush_btn, self.status_btn)
        self.setTabOrder(self.status_btn, self.clear_btn)
        self.setTabOrder(self.clear_btn, self.log_text)

    # -- Context Menus --------------------------------------------

    def _show_text_context_menu(self, pos):
        """Show the right-click context menu for the text editor."""
        menu = QMenu(self.text_edit)
        menu.setAccessibleName("Text editing context menu")

        cut_action = menu.addAction("Cut")
        cut_action.setShortcut(QKeySequence.StandardKey.Cut)
        cut_action.triggered.connect(self.text_edit.cut)

        copy_action = menu.addAction("Copy")
        copy_action.setShortcut(QKeySequence.StandardKey.Copy)
        copy_action.triggered.connect(self.text_edit.copy)

        paste_action = menu.addAction("Paste")
        paste_action.setShortcut(QKeySequence.StandardKey.Paste)
        paste_action.triggered.connect(self.text_edit.paste)

        menu.addSeparator()

        select_all_action = menu.addAction("Select All")
        select_all_action.setShortcut(QKeySequence.StandardKey.SelectAll)
        select_all_action.triggered.connect(self.text_edit.selectAll)

        clear_action = menu.addAction("Clear")
        clear_action.triggered.connect(self._clear)

        menu.exec(self.text_edit.mapToGlobal(pos))

    def _show_log_context_menu(self, pos):
        """Show the right-click context menu for the log widget."""
        menu = QMenu(self.log_text)
        menu.setAccessibleName("Log context menu")

        copy_action = menu.addAction("Copy")
        copy_action.triggered.connect(self.log_text.copy)

        select_all_action = menu.addAction("Select All")
        select_all_action.triggered.connect(self.log_text.selectAll)

        menu.addSeparator()

        clear_action = menu.addAction("Clear Log")
        clear_action.triggered.connect(self._clear_log)

        menu.exec(self.log_text.mapToGlobal(pos))

    # -- Communications Log ---------------------------------------

    def _log(self, direction, message):
        """Append a timestamped entry to the communications log.

        ``direction`` should be one of ``"TX"``, ``"RX"``, or ``"--"``.
        ``message`` is a human-readable description of the event.

        This method is safe to call from any thread via the signal bridge.
        """
        now = time.time()
        millis = int((now % 1) * 1000)
        timestamp = time.strftime("%H:%M:%S", time.localtime(now))
        entry = "[%s.%03d] %-2s  %s" % (timestamp, millis, direction, message)

        self.log_text.appendPlainText(entry)

        # Cap at 1000 lines — delete oldest lines when exceeded.
        doc = self.log_text.document()
        while doc.blockCount() > 1000:
            cursor = QTextCursor(doc.begin())
            cursor.select(QTextCursor.SelectionType.BlockUnderCursor)
            cursor.movePosition(
                QTextCursor.MoveOperation.NextBlock,
                QTextCursor.MoveMode.KeepAnchor,
            )
            cursor.removeSelectedText()

        # Scroll to bottom
        scrollbar = self.log_text.verticalScrollBar()
        scrollbar.setValue(scrollbar.maximum())

    def _log_from_thread(self, direction, message):
        """Thread-safe log entry via signal bridge."""
        self._signals.log_entry.emit(direction, message)

    def _clear_log(self):
        """Clear all entries from the communications log."""
        self.log_text.clear()

    # -- Status Bar -----------------------------------------------

    def _set_status(self, text):
        """Update the status bar text."""
        self._status_label.setText(text)

    def _set_status_from_thread(self, text):
        """Thread-safe status bar update via signal bridge."""
        self._signals.status_update.emit(text)

    # -- Connection Handling --------------------------------------

    def _refresh_ports(self):
        """Refresh the list of available serial ports."""
        current = self.port_combo.currentText()
        self.port_combo.clear()
        ports = DECtalkESPressSerial.list_ports()
        self.port_combo.addItems(ports)
        if current in ports:
            self.port_combo.setCurrentText(current)
        elif ports:
            self.port_combo.setCurrentIndex(0)

    def _toggle_connection(self):
        """Connect or disconnect based on current state."""
        if self.dtesp.connected:
            self._disconnect()
        else:
            self._connect()

    def _connect(self):
        """Establish a serial connection using ESPress protocol."""
        port = self.port_combo.currentText()
        if not port:
            QMessageBox.warning(
                self, "No Port", "Please select a serial port."
            )
            return

        baud = int(self.baud_combo.currentText())
        self._set_status("Connecting to %s..." % port)
        self.connect_btn.setEnabled(False)
        self._log("--", "Connecting to %s at %d baud..." % (port, baud))

        def do_connect():
            try:
                self.dtesp.connect(port, baud)
                self._signals.connected.emit()
            except Exception as exc:
                self._signals.connect_error.emit(str(exc))

        threading.Thread(target=do_connect, daemon=True).start()

    def _on_connected(self):
        """Called on the main thread after a successful connection."""
        port = self.port_combo.currentText()
        baud = self.baud_combo.currentText()
        self._set_status(
            "Connected to %s at %s baud (ESPress)" % (port, baud)
        )
        self._log("--", "Connected")
        self.connect_btn.setText("Disc&onnect")
        self.connect_btn.setEnabled(True)
        self.connect_btn.setAccessibleDescription(
            "Disconnect from the DECtalk device"
        )
        self.speak_btn.setEnabled(True)
        self.pause_btn.setEnabled(True)
        self.resume_btn.setEnabled(True)
        self.flush_btn.setEnabled(True)
        self.status_btn.setEnabled(True)
        self.audio_settings_act.setEnabled(True)
        self.save_settings_act.setEnabled(True)
        self._paused = False

        # Start polling device status
        self._polling = True
        threading.Thread(
            target=self._poll_status_loop, daemon=True
        ).start()

    def _on_connect_error(self, message):
        """Called on the main thread when connection fails."""
        self._set_status("Connection failed")
        self._log("--", "Connection failed: %s" % message)
        self.connect_btn.setEnabled(True)
        QMessageBox.critical(self, "Connection Error", message)

    def _disconnect(self):
        """Close the serial connection."""
        self._polling = False
        self.dtesp.disconnect()
        self._log("--", "Disconnected")
        self._set_status("Disconnected")
        self.device_status_label.setText("--")
        self._update_status_indicators(0)
        self.connect_btn.setText("Co&nnect")
        self.connect_btn.setEnabled(True)
        self.connect_btn.setAccessibleDescription(
            "Connect to the DECtalk device"
        )
        self.speak_btn.setEnabled(False)
        self.pause_btn.setEnabled(False)
        self.resume_btn.setEnabled(False)
        self.flush_btn.setEnabled(False)
        self.status_btn.setEnabled(False)
        self.audio_settings_act.setEnabled(False)
        self.save_settings_act.setEnabled(False)
        self._paused = False

    # -- Speech Controls ------------------------------------------

    def _on_speak(self):
        """Send the text box contents to the device via ESPress protocol."""
        text = self.text_edit.toPlainText().strip()
        if not text:
            QMessageBox.information(
                self, "No Text", "Please enter some text to speak."
            )
            return

        if not self.dtesp.connected:
            QMessageBox.warning(
                self, "Not Connected",
                "Please connect to the device first.",
            )
            return

        voice = self.voice_combo.currentText()
        rate = self.rate_slider.value()
        pitch = self.pitch_slider.value()

        self._set_status("Sending text...")

        # Build the prefixed text for logging
        prefix = build_dtesp_prefix(voice=voice, rate=rate, pitch=pitch)
        full_text = prefix + text
        log_text = (
            full_text
            if len(full_text) <= self._LOG_SPEAK_MAX_CHARS
            else full_text[: self._LOG_SPEAK_MAX_CHARS] + "\u2026"
        )
        self._log("TX", "SPEAK: %s" % log_text)

        def do_speak():
            try:
                self.dtesp.speak(text, voice=voice, rate=rate, pitch=pitch)
                port = self.port_combo.currentText()
                self._set_status_from_thread(
                    "Text sent -- Connected to %s (ESPress)" % port
                )
                self._log_from_thread("RX", "Text sent")
            except Exception as exc:
                err = str(exc)
                self._set_status_from_thread("Error: %s" % err)
                self._log_from_thread("RX", "Error: %s" % err)

        threading.Thread(target=do_speak, daemon=True).start()

    def _on_pause(self):
        """Pause speech output (send SO control character)."""
        if not self.dtesp.connected:
            return
        try:
            self.dtesp.pause()
            self._paused = True
            self._set_status("Paused")
            self._log("TX", "SO (Pause)")
        except Exception as exc:
            self._set_status("Error: %s" % str(exc))

    def _on_resume(self):
        """Resume speech output (send SI control character)."""
        if not self.dtesp.connected:
            return
        try:
            self.dtesp.resume()
            self._paused = False
            self._set_status(
                "Resumed -- Connected to %s (ESPress)"
                % self.port_combo.currentText()
            )
            self._log("TX", "SI (Resume)")
        except Exception as exc:
            self._set_status("Error: %s" % str(exc))

    def _on_flush(self):
        """Flush (cancel) all pending speech."""
        if not self.dtesp.connected:
            return

        self._set_status("Flushing...")
        self._log("TX", "] + ETX + XON (Flush with ack)")

        def do_flush():
            try:
                ack = self.dtesp.flush_with_ack()
                if ack:
                    msg = "Flush acknowledged"
                    log_msg = "SOH (Flush acknowledged)"
                else:
                    msg = "Flush sent (no ack)"
                    log_msg = "Flush sent (no ack)"
                port = self.port_combo.currentText()
                self._set_status_from_thread(
                    "%s -- Connected to %s (ESPress)" % (msg, port)
                )
                self._log_from_thread("RX", log_msg)
            except Exception as exc:
                err = str(exc)
                self._set_status_from_thread("Error: %s" % err)

        threading.Thread(target=do_flush, daemon=True).start()

    def _on_query_status(self):
        """Query device status via ENQ and display the result."""
        if not self.dtesp.connected:
            return

        self._log("TX", "ENQ (Query Status)")

        def do_query():
            try:
                status = self.dtesp.request_status()
                self._signals.device_status.emit(status, False)
            except Exception as exc:
                self._set_status_from_thread("Error: %s" % str(exc))

        threading.Thread(target=do_query, daemon=True).start()

    # -- Status Display -------------------------------------------

    def _display_status(self, status, poll=False):
        """Update the device status display with a decoded status word."""
        if status < 0:
            self.device_status_label.setText("No response")
            self._update_status_indicators(0)
            if poll:
                self._log("RX", "Poll status: No response")
            else:
                self._log("RX", "Status: No response")
            return

        self.device_status_label.setText("0x%04X" % status)
        self._update_status_indicators(status)
        if poll:
            self._log("RX", "Poll status: 0x%04X" % status)
        else:
            self._log("RX", "Status: 0x%04X" % status)

    def _update_status_indicators(self, status):
        """Update the colored status indicator labels."""
        indicators = {
            "Ready": (
                DTESP_STAT_RR_CHAR | DTESP_STAT_CMD_READY,
                "#006400",  # dark green
            ),
            "Transmitting": (DTESP_STAT_TR_CHAR, "#1E90FF"),  # dodger blue
            "Flushing": (DTESP_STAT_FLUSHING, "#FFA500"),  # orange
            "Index": (DTESP_STAT_NEW_INDEX, "#800080"),  # purple
        }
        for name, (mask, color) in indicators.items():
            lbl = self._status_indicators[name]
            active = bool(status & mask)
            if active:
                lbl.setStyleSheet("color: %s; font-weight: bold;" % color)
                lbl.setAccessibleDescription(
                    "%s indicator. Currently active" % name
                )
            else:
                lbl.setStyleSheet("color: #b0b0b0;")
                lbl.setAccessibleDescription(
                    "%s indicator. Currently inactive" % name
                )

    def _poll_status_loop(self):
        """Background thread that periodically polls device status."""
        while self._polling and self.dtesp.connected:
            try:
                status = self.dtesp.request_status()
                if status >= 0:
                    self._signals.device_status.emit(status, True)
            except Exception:
                pass
            time.sleep(2.0)

    # -- Slider Callbacks -----------------------------------------

    def _on_rate_change(self, value):
        """Update the rate display label."""
        self.rate_value_label.setText(str(value))

    def _on_pitch_change(self, value):
        """Update the pitch display label."""
        if value <= PITCH_MIN:
            self.pitch_value_label.setText("Default")
        else:
            self.pitch_value_label.setText(str(value))

    # -- Text Editing ---------------------------------------------

    def _clear(self):
        """Clear all text from the text box."""
        self.text_edit.clear()

    # -- Window Management ----------------------------------------

    def closeEvent(self, event):
        """Clean up on window close."""
        self._polling = False
        if self.dtesp.connected:
            self.dtesp.disconnect()
        event.accept()


def main():
    app = QApplication(sys.argv)
    app.setApplicationName("DECtalk ESPress GUI")
    app.setOrganizationName("DECtalk")

    window = DECtalkESPressGUIQt()
    window.show()
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
