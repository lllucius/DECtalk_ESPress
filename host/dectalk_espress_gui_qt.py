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
    python dectalk_espress_gui_qt.py

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
    )
    from PySide6.QtCore import Qt, Signal, QObject
    from PySide6.QtGui import QFont, QKeySequence, QTextCursor
    _QT_BINDING = "PySide6"
except ImportError:
    from PyQt6.QtWidgets import (
        QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout,
        QGroupBox, QLabel, QComboBox, QPushButton, QPlainTextEdit,
        QSlider, QStatusBar, QMenu, QMessageBox, QSplitter,
    )
    from PyQt6.QtCore import Qt, pyqtSignal as Signal, QObject
    from PyQt6.QtGui import QFont, QKeySequence, QTextCursor
    _QT_BINDING = "PyQt6"

# Allow running from the host/ directory or the repo root
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from dectalk_serial import (
    DECtalkESPressSerial,
    VOICES,
    RATE_MIN,
    RATE_MAX,
    RATE_DEFAULT,
    PITCH_MIN,
    PITCH_MAX,
    PITCH_DEFAULT,
    ESPRESS_DEFAULT_BAUD,
    ESPRESS_STAT_TR_CHAR,
    ESPRESS_STAT_RR_CHAR,
    ESPRESS_STAT_CMD_READY,
    ESPRESS_STAT_NEW_INDEX,
    ESPRESS_STAT_FLUSHING,
    build_dectalk_prefix,
)


class _SignalBridge(QObject):
    """Bridge for emitting Qt signals from background threads."""
    connected = Signal()
    connect_error = Signal(str)
    status_update = Signal(str)
    log_entry = Signal(str, str)
    device_status = Signal(int, bool)


class DECtalkESPressGUIQt(QMainWindow):
    """Main GUI application for DECtalk ESPress host control (Qt version)."""

    # Maximum number of characters from the speak text shown in the log.
    _LOG_SPEAK_MAX_CHARS = 120

    def __init__(self):
        super().__init__()
        self.setWindowTitle("DECtalk ESPress - Host GUI (Qt)")
        self.setMinimumSize(700, 650)
        self.resize(1097, 650)

        self.espress = DECtalkESPressSerial()
        self._paused = False
        self._polling = False

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
        self.baud_combo.setCurrentText(str(ESPRESS_DEFAULT_BAUD))
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
        if self.espress.connected:
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
                self.espress.connect(port, baud)
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
        self.espress.disconnect()
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

        if not self.espress.connected:
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
        prefix = build_dectalk_prefix(voice=voice, rate=rate, pitch=pitch)
        full_text = prefix + text
        log_text = (
            full_text
            if len(full_text) <= self._LOG_SPEAK_MAX_CHARS
            else full_text[: self._LOG_SPEAK_MAX_CHARS] + "\u2026"
        )
        self._log("TX", "SPEAK: %s" % log_text)

        def do_speak():
            try:
                self.espress.speak(text, voice=voice, rate=rate, pitch=pitch)
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
        if not self.espress.connected:
            return
        try:
            self.espress.pause()
            self._paused = True
            self._set_status("Paused")
            self._log("TX", "SO (Pause)")
        except Exception as exc:
            self._set_status("Error: %s" % str(exc))

    def _on_resume(self):
        """Resume speech output (send SI control character)."""
        if not self.espress.connected:
            return
        try:
            self.espress.resume()
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
        if not self.espress.connected:
            return

        self._set_status("Flushing...")
        self._log("TX", "] + ETX + XON (Flush with ack)")

        def do_flush():
            try:
                ack = self.espress.flush_with_ack()
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
        if not self.espress.connected:
            return

        self._log("TX", "ENQ (Query Status)")

        def do_query():
            try:
                status = self.espress.request_status()
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
                ESPRESS_STAT_RR_CHAR | ESPRESS_STAT_CMD_READY,
                "#006400",  # dark green
            ),
            "Transmitting": (ESPRESS_STAT_TR_CHAR, "#1E90FF"),  # dodger blue
            "Flushing": (ESPRESS_STAT_FLUSHING, "#FFA500"),  # orange
            "Index": (ESPRESS_STAT_NEW_INDEX, "#800080"),  # purple
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
        while self._polling and self.espress.connected:
            try:
                status = self.espress.request_status()
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
        if self.espress.connected:
            self.espress.disconnect()
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
