#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2025 Leland Lucius
"""
DECtalk ESPress Host GUI - Tkinter application for controlling DECtalk via
 the ESPress serial protocol.

Provides a graphical interface to:
  - Connect to an ESP32 running DECtalk in ESPress mode via serial port
  - Enter and edit multi-line text for speech synthesis
  - Select from 9 DECtalk voices
  - Adjust speaking rate (words per minute) and pitch (Hz)
  - Pause/Resume speech output
  - Flush (cancel) pending speech
  - Monitor device status via DLE status responses
  - Standard cut, copy, paste, and clear operations

The ESPress protocol uses raw control characters (ETX, ENQ, SO, SI) and
DLE sequences instead of the line-based API protocol used by the standard
GUI.  This makes it compatible with the original DECtalk ESPress hardware.

Usage:
    python dectalk_ESPress_gui.py
"""

import sys
import os
import platform
import subprocess
import tkinter as tk
from tkinter import ttk
import threading
import time

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


# -- Theme Detection & Color Palettes ----------------------------

# Colors for non-themed widgets (tk.Text, tk.Menu) and explicit
# foregrounds on ttk.Labels.  The ttk theme itself handles the
# remaining widgets (buttons, comboboxes, frames, sliders, etc.).

_LIGHT_COLORS = {
    "text_bg":          "white",
    "text_fg":          "black",
    "text_insert":      "black",
    "text_select_bg":   "#b0c4de",
    "log_bg":           "#f0f0f0",
    "log_fg":           "black",
    "menu_bg":          "#f0f0f0",
    "menu_fg":          "black",
    "status_fg":        "gray30",
    "inactive_fg":      "gray70",
    "ready_fg":         "green4",
    "transmitting_fg":  "DodgerBlue",
    "flushing_fg":      "orange",
    "index_fg":         "purple",
}

_DARK_COLORS = {
    "text_bg":          "#1e1e1e",
    "text_fg":          "#d4d4d4",
    "text_insert":      "#d4d4d4",
    "text_select_bg":   "#264f78",
    "log_bg":           "#252526",
    "log_fg":           "#cccccc",
    "menu_bg":          "#2d2d30",
    "menu_fg":          "#d4d4d4",
    "status_fg":        "#b0b0b0",
    "inactive_fg":      "#555555",
    "ready_fg":         "#4ec94e",
    "transmitting_fg":  "#69b4f0",
    "flushing_fg":      "#e0a030",
    "index_fg":         "#c080e0",
}


def _is_dark_mode():
    """Detect whether the OS is using a dark color scheme.

    Checks platform-specific settings on macOS, Windows, and Linux
    (freedesktop / GNOME / KDE).  Returns ``False`` when detection fails.
    """
    system = platform.system()

    if system == "Darwin":
        try:
            result = subprocess.run(
                ["defaults", "read", "-g", "AppleInterfaceStyle"],
                capture_output=True, text=True, timeout=2,
            )
            return result.stdout.strip().lower() == "dark"
        except Exception:
            return False

    if system == "Windows":
        try:
            import winreg
            key = winreg.OpenKey(
                winreg.HKEY_CURRENT_USER,
                r"Software\Microsoft\Windows\CurrentVersion\Themes\Personalize",
            )
            value, _ = winreg.QueryValueEx(key, "AppsUseLightTheme")
            winreg.CloseKey(key)
            return value == 0
        except Exception:
            return False

    # Linux / BSD – try freedesktop color-scheme first, then GTK theme name
    for cmd in (
        ["gsettings", "get", "org.gnome.desktop.interface", "color-scheme"],
        ["gsettings", "get", "org.gnome.desktop.interface", "gtk-theme"],
    ):
        try:
            result = subprocess.run(cmd, capture_output=True, text=True,
                                    timeout=2)
            if "dark" in result.stdout.lower():
                return True
        except Exception:
            pass

    return False


class DECtalkESPressGUI:
    """Main GUI application for DECtalk ESPress host control."""

    # Maximum number of characters from the speak text shown in the log.
    _LOG_SPEAK_MAX_CHARS = 120

    def __init__(self, root):
        self.root = root
        self.root.title("DECtalk ESPress - Host GUI")
        self.root.minsize(700, 650)
        self.root.geometry("1097x650")

        # Detect OS theme and pick colours
        self._dark = _is_dark_mode()
        self._colors = _DARK_COLORS if self._dark else _LIGHT_COLORS

        self.ESPress = DECtalkESPressSerial()
        self._paused = False
        self._polling = False

        # Tkinter variables
        self.port_var = tk.StringVar()
        self.baud_var = tk.StringVar(value=str(ESPRESS_DEFAULT_BAUD))
        self.voice_var = tk.StringVar(value="Paul")
        self.rate_var = tk.IntVar(value=RATE_DEFAULT)
        self.pitch_var = tk.IntVar(value=PITCH_DEFAULT)
        self.status_var = tk.StringVar(value="Disconnected")
        self.device_status_var = tk.StringVar(value="--")

        self._apply_theme()
        self._build_ui()
        self._refresh_ports()

        # Handle window close
        self.root.protocol("WM_DELETE_WINDOW", self._on_close)

    # -- Theme Setup ------------------------------------------------

    def _apply_theme(self):
        """Select a ttk theme appropriate for the platform and mode.

        On macOS the built-in 'aqua' theme already follows the system
        appearance, so nothing extra is needed.  On other platforms we
        use 'clam' as a solid cross-platform theme and, when in dark
        mode, override its palette so that themed widgets (frames,
        buttons, labels, etc.) render with dark colours.
        """
        style = ttk.Style(self.root)

        if platform.system() == "Darwin":
            # Aqua theme tracks macOS dark/light automatically.
            try:
                style.theme_use("aqua")
            except tk.TclError:
                pass
            return

        # Windows / Linux — use clam and override for dark mode.
        try:
            style.theme_use("clam")
        except tk.TclError:
            pass

        if self._dark:
            bg = "#2d2d30"
            fg = "#d4d4d4"
            field_bg = "#1e1e1e"
            select_bg = "#264f78"
            border = "#555555"
            style.configure(".", background=bg, foreground=fg,
                            fieldbackground=field_bg,
                            troughcolor="#3e3e42",
                            selectbackground=select_bg,
                            selectforeground=fg,
                            bordercolor=border,
                            darkcolor=bg, lightcolor=bg)
            style.configure("TLabelframe", background=bg)
            style.configure("TLabelframe.Label", background=bg,
                            foreground=fg)
            style.configure("TButton", background="#3e3e42",
                            foreground=fg, bordercolor=border)
            style.map("TButton",
                       background=[("active", "#505058"),
                                   ("pressed", "#606068")])
            style.configure("TCombobox", foreground=fg,
                            fieldbackground=field_bg,
                            selectbackground=select_bg,
                            selectforeground=fg)
            style.map("TCombobox",
                       fieldbackground=[("readonly", field_bg)],
                       foreground=[("readonly", fg)],
                       selectbackground=[("readonly", select_bg)],
                       selectforeground=[("readonly", fg)])
            # Style the dropdown Listbox that appears under each Combobox
            self.root.option_add("*TCombobox*Listbox.background", field_bg)
            self.root.option_add("*TCombobox*Listbox.foreground", fg)
            self.root.option_add("*TCombobox*Listbox.selectBackground",
                                 select_bg)
            self.root.option_add("*TCombobox*Listbox.selectForeground", fg)
            style.configure("TScale", background=bg, troughcolor="#3e3e42")
            style.map("TScale",
                       background=[("active", "#505058")])
            # Root window background
            self.root.configure(bg=bg)

    # -- Themed Message Dialogs -----------------------------------

    def _show_message(self, title, message, icon="info"):
        """Show a themed pop-up message dialog.

        Unlike ``tkinter.messagebox``, this respects the dark-mode palette
        so the dialog doesn't flash a light window on a dark desktop.

        *icon* may be ``"info"``, ``"warning"``, or ``"error"``.
        """
        c = self._colors
        dlg = tk.Toplevel(self.root)
        dlg.title(title)
        dlg.resizable(False, False)
        dlg.transient(self.root)
        dlg.grab_set()
        dlg.configure(bg=c["menu_bg"])

        # Icon character
        icon_chars = {"info": "\u2139", "warning": "\u26A0", "error": "\u274C"}
        icon_char = icon_chars.get(icon, "\u2139")

        body = tk.Frame(dlg, bg=c["menu_bg"], padx=16, pady=12)
        body.pack(fill=tk.BOTH, expand=True)

        tk.Label(
            body, text=icon_char, font=("TkDefaultFont", 28),
            bg=c["menu_bg"], fg=c["menu_fg"],
        ).pack(side=tk.LEFT, padx=(0, 12))

        tk.Label(
            body, text=message, bg=c["menu_bg"], fg=c["menu_fg"],
            wraplength=350, justify=tk.LEFT,
        ).pack(side=tk.LEFT, fill=tk.BOTH, expand=True)

        btn_frame = tk.Frame(dlg, bg=c["menu_bg"], pady=8)
        btn_frame.pack(fill=tk.X)

        ok_btn = ttk.Button(btn_frame, text="OK", command=dlg.destroy)
        ok_btn.pack()
        ok_btn.focus_set()
        dlg.bind("<Return>", lambda _e: dlg.destroy())
        dlg.bind("<Escape>", lambda _e: dlg.destroy())

        # Center on parent window
        dlg.update_idletasks()
        pw = self.root.winfo_width()
        ph = self.root.winfo_height()
        px = self.root.winfo_x()
        py = self.root.winfo_y()
        dw = dlg.winfo_width()
        dh = dlg.winfo_height()
        dlg.geometry("+%d+%d" % (px + (pw - dw) // 2, py + (ph - dh) // 2))

        dlg.wait_window()

    # -- UI Construction ------------------------------------------

    def _build_ui(self):
        """Build the complete GUI layout.

        Uses grid geometry on the main frame.  The 'Text to Speak' row (row 2)
        has weight=2 and the 'Communications Log' row (row 5) has weight=1 so
        the text entry area gets roughly twice the extra vertical space as the
        log when the window is resized.  All other rows have weight 0 and stay
        fixed-height, keeping the connection/button/status controls always
        visible.
        """
        self._build_status_bar()

        main = ttk.Frame(self.root, padding=8)
        main.pack(fill=tk.BOTH, expand=True)

        # Expandable rows: text entry gets 2x space, comm log gets 1x.
        main.rowconfigure(0, weight=0)  # Connection
        main.rowconfigure(1, weight=0)  # Speech Settings
        main.rowconfigure(2, weight=1)  # Text to Speak  <-- expands (2x)
        main.rowconfigure(3, weight=0)  # Buttons
        main.rowconfigure(4, weight=0)  # Device Status
        main.rowconfigure(5, weight=1)  # Communications Log  <-- expands (1x)
        main.columnconfigure(0, weight=1)

        self._build_connection_frame(main)
        self._build_voice_frame(main)
        self._build_text_frame(main)
        self._build_button_frame(main)
        self._build_device_status_frame(main)
        self._build_comm_log_frame(main)

    def _build_connection_frame(self, parent):
        """Serial port connection controls."""
        frame = ttk.LabelFrame(parent, text="Connection (ESPress Protocol)", padding=4)
        frame.grid(row=0, column=0, sticky=tk.EW, pady=(0, 4))

        ttk.Label(frame, text="Port:").pack(side=tk.LEFT, padx=(0, 4))
        self.port_combo = ttk.Combobox(
            frame, textvariable=self.port_var, width=20, state="readonly"
        )
        self.port_combo.pack(side=tk.LEFT, padx=(0, 4))

        ttk.Button(
            frame, text="Refresh", command=self._refresh_ports
        ).pack(side=tk.LEFT, padx=(0, 8))

        ttk.Label(frame, text="Baud:").pack(side=tk.LEFT, padx=(0, 4))
        baud_combo = ttk.Combobox(
            frame,
            textvariable=self.baud_var,
            values=["115200", "9600", "19200", "38400", "57600"],
            width=8,
            state="readonly",
        )
        baud_combo.pack(side=tk.LEFT, padx=(0, 8))

        self.connect_btn = ttk.Button(
            frame, text="Connect", command=self._toggle_connection
        )
        self.connect_btn.pack(side=tk.LEFT, padx=(0, 4))

    def _build_voice_frame(self, parent):
        """Voice, rate, and pitch controls."""
        frame = ttk.LabelFrame(parent, text="Speech Settings", padding=4)
        frame.grid(row=1, column=0, sticky=tk.EW, pady=(0, 4))

        voice_frame = ttk.Frame(frame)
        voice_frame.pack(fill=tk.X, pady=(0, 4))

        ttk.Label(voice_frame, text="Voice:").pack(side=tk.LEFT, padx=(0, 4))
        voice_combo = ttk.Combobox(
            voice_frame,
            textvariable=self.voice_var,
            values=list(VOICES.keys()),
            width=12,
            state="readonly",
        )
        voice_combo.pack(side=tk.LEFT, padx=(0, 16))

        # Rate slider
        ttk.Label(voice_frame, text="Rate (WPM):").pack(
            side=tk.LEFT, padx=(0, 4)
        )
        self.rate_scale = ttk.Scale(
            voice_frame,
            from_=RATE_MIN,
            to=RATE_MAX,
            variable=self.rate_var,
            orient=tk.HORIZONTAL,
            length=180,
            command=self._on_rate_change,
        )
        self.rate_scale.pack(side=tk.LEFT, padx=(0, 4))
        self.rate_label = ttk.Label(
            voice_frame, text=str(RATE_DEFAULT), width=4
        )
        self.rate_label.pack(side=tk.LEFT, padx=(0, 16))

        # Pitch slider
        ttk.Label(voice_frame, text="Pitch (Hz):").pack(
            side=tk.LEFT, padx=(0, 4)
        )
        self.pitch_scale = ttk.Scale(
            voice_frame,
            from_=PITCH_MIN,
            to=PITCH_MAX,
            variable=self.pitch_var,
            orient=tk.HORIZONTAL,
            length=180,
            command=self._on_pitch_change,
        )
        self.pitch_scale.pack(side=tk.LEFT, padx=(0, 4))
        self.pitch_label = ttk.Label(voice_frame, text="Default", width=6)
        self.pitch_label.pack(side=tk.LEFT)

    def _build_text_frame(self, parent):
        """Multi-line text entry with scrollbars.

        Placed in the grid row that has weight=1 so it stretches vertically
        when the window is resized.  The inner Text widget and its container
        still use pack with expand=True to fill the LabelFrame.
        """
        frame = ttk.LabelFrame(parent, text="Text to Speak", padding=4)
        frame.grid(row=2, column=0, sticky=tk.NSEW, pady=(0, 4))

        # Allow the LabelFrame itself to distribute space to its child.
        frame.rowconfigure(0, weight=1)
        frame.columnconfigure(0, weight=1)

        text_container = ttk.Frame(frame)
        text_container.grid(row=0, column=0, sticky=tk.NSEW)

        text_container.rowconfigure(0, weight=1)
        text_container.columnconfigure(0, weight=1)

        v_scroll = ttk.Scrollbar(text_container, orient=tk.VERTICAL)
        v_scroll.grid(row=0, column=1, sticky=tk.NS)

        h_scroll = ttk.Scrollbar(text_container, orient=tk.HORIZONTAL)
        h_scroll.grid(row=1, column=0, sticky=tk.EW)

        self.text_box = tk.Text(
            text_container,
            wrap=tk.NONE,
            undo=True,
            font=("TkFixedFont", 10),
            bg=self._colors["text_bg"],
            fg=self._colors["text_fg"],
            insertbackground=self._colors["text_insert"],
            selectbackground=self._colors["text_select_bg"],
            yscrollcommand=v_scroll.set,
            xscrollcommand=h_scroll.set,
        )
        self.text_box.grid(row=0, column=0, sticky=tk.NSEW)
        v_scroll.config(command=self.text_box.yview)
        h_scroll.config(command=self.text_box.xview)

        # Right-click context menu
        self.context_menu = tk.Menu(self.text_box, tearoff=0,
                                    bg=self._colors["menu_bg"],
                                    fg=self._colors["menu_fg"])
        self.context_menu.add_command(
            label="Cut", accelerator="Ctrl+X", command=self._cut
        )
        self.context_menu.add_command(
            label="Copy", accelerator="Ctrl+C", command=self._copy
        )
        self.context_menu.add_command(
            label="Paste", accelerator="Ctrl+V", command=self._paste
        )
        self.context_menu.add_separator()
        self.context_menu.add_command(
            label="Select All", accelerator="Ctrl+A", command=self._select_all
        )
        self.context_menu.add_command(
            label="Clear", command=self._clear
        )

        self.text_box.bind("<Button-3>", self._show_context_menu)
        self.text_box.bind("<Control-a>", lambda e: self._select_all())

    def _build_button_frame(self, parent):
        """Speak, Pause, Resume, Flush, and Clear buttons."""
        frame = ttk.Frame(parent)
        frame.grid(row=3, column=0, sticky=tk.EW, pady=(0, 4))

        self.speak_btn = ttk.Button(
            frame, text="Speak", command=self._on_speak, state=tk.DISABLED
        )
        self.speak_btn.pack(side=tk.LEFT, padx=(0, 8))

        self.pause_btn = ttk.Button(
            frame, text="Pause", command=self._on_pause, state=tk.DISABLED
        )
        self.pause_btn.pack(side=tk.LEFT, padx=(0, 8))

        self.resume_btn = ttk.Button(
            frame, text="Resume", command=self._on_resume, state=tk.DISABLED
        )
        self.resume_btn.pack(side=tk.LEFT, padx=(0, 8))

        self.flush_btn = ttk.Button(
            frame, text="Flush", command=self._on_flush, state=tk.DISABLED
        )
        self.flush_btn.pack(side=tk.LEFT, padx=(0, 8))

        self.status_btn = ttk.Button(
            frame, text="Query Status", command=self._on_query_status,
            state=tk.DISABLED
        )
        self.status_btn.pack(side=tk.LEFT, padx=(0, 8))

        self.clear_btn = ttk.Button(
            frame, text="Clear Text", command=self._clear
        )
        self.clear_btn.pack(side=tk.RIGHT)

    def _build_device_status_frame(self, parent):
        """Device status display showing decoded DLE status bits."""
        frame = ttk.LabelFrame(parent, text="Device Status", padding=4)
        frame.grid(row=4, column=0, sticky=tk.EW, pady=(0, 4))

        status_row = ttk.Frame(frame)
        status_row.pack(fill=tk.X)

        ttk.Label(status_row, text="Status:").pack(side=tk.LEFT, padx=(0, 4))
        self.device_status_label = ttk.Label(
            status_row, textvariable=self.device_status_var,
            font=("TkFixedFont", 9), foreground=self._colors["status_fg"]
        )
        self.device_status_label.pack(side=tk.LEFT, padx=(0, 16))

        # Status indicator LEDs (text labels that change color)
        self._status_indicators = {}
        for name in ["Ready", "Transmitting", "Flushing", "Index"]:
            lbl = ttk.Label(status_row, text="(*) " + name,
                            foreground=self._colors["inactive_fg"])
            lbl.pack(side=tk.LEFT, padx=(0, 12))
            self._status_indicators[name] = lbl

    def _build_comm_log_frame(self, parent):
        """Communications log panel showing timestamped TX/RX events."""
        frame = ttk.LabelFrame(parent, text="Communications Log", padding=4)
        frame.grid(row=5, column=0, sticky=tk.NSEW, pady=(0, 4))

        frame.rowconfigure(1, weight=1)
        frame.columnconfigure(0, weight=1)

        # Toolbar row with Clear Log button
        toolbar = ttk.Frame(frame)
        toolbar.grid(row=0, column=0, sticky=tk.EW, pady=(0, 2))
        ttk.Button(toolbar, text="Clear Log", command=self._clear_log).pack(
            side=tk.LEFT
        )

        # Log text widget with vertical scrollbar
        log_container = ttk.Frame(frame)
        log_container.grid(row=1, column=0, sticky=tk.NSEW)
        log_container.rowconfigure(0, weight=1)
        log_container.columnconfigure(0, weight=1)

        log_scroll = ttk.Scrollbar(log_container, orient=tk.VERTICAL)
        log_scroll.grid(row=0, column=1, sticky=tk.NS)

        self.log_text = tk.Text(
            log_container,
            font=("TkFixedFont", 9),
            state=tk.DISABLED,
            wrap=tk.WORD,
            bg=self._colors["log_bg"],
            fg=self._colors["log_fg"],
            insertbackground=self._colors["text_insert"],
            selectbackground=self._colors["text_select_bg"],
            yscrollcommand=log_scroll.set,
        )
        self.log_text.grid(row=0, column=0, sticky=tk.NSEW)
        log_scroll.config(command=self.log_text.yview)

        # Right-click context menu for the log widget
        self._log_context_menu = tk.Menu(self.log_text, tearoff=0,
                                         bg=self._colors["menu_bg"],
                                         fg=self._colors["menu_fg"])
        self._log_context_menu.add_command(
            label="Copy", command=self._log_copy
        )
        self._log_context_menu.add_command(
            label="Select All", command=self._log_select_all
        )
        self._log_context_menu.add_separator()
        self._log_context_menu.add_command(
            label="Clear Log", command=self._clear_log
        )
        self.log_text.bind("<Button-3>", self._show_log_context_menu)

    def _build_status_bar(self):
        """Status bar at the bottom of the window."""
        status_frame = ttk.Frame(self.root, relief=tk.SUNKEN, padding=(4, 2))
        status_frame.pack(fill=tk.X, side=tk.BOTTOM)
        ttk.Label(status_frame, textvariable=self.status_var).pack(
            side=tk.LEFT
        )

    # -- Communications Log ---------------------------------------

    def _log(self, direction, message):
        """Append a timestamped entry to the communications log.

        ``direction`` should be one of ``"TX"``, ``"RX"``, or ``"--"``.
        ``message`` is a human-readable description of the event.

        This method is safe to call from any thread.  When called from a
        background thread it schedules itself on the main thread via
        ``root.after``.
        """
        if threading.current_thread() != threading.main_thread():
            self.root.after(0, lambda: self._log(direction, message))
            return

        now = time.time()
        millis = int((now % 1) * 1000)
        timestamp = time.strftime("%H:%M:%S", time.localtime(now))
        entry = "[%s.%03d] %-2s  %s\n" % (timestamp, millis, direction, message)

        self.log_text.config(state=tk.NORMAL)
        self.log_text.insert(tk.END, entry)

        # Cap at 1000 lines — delete oldest lines when exceeded.
        line_count = int(self.log_text.index(tk.END).split(".")[0]) - 1
        if line_count > 1000:
            self.log_text.delete("1.0", "%d.0" % (line_count - 999))

        self.log_text.config(state=tk.DISABLED)
        self.log_text.see(tk.END)

    def _clear_log(self):
        """Clear all entries from the communications log."""
        self.log_text.config(state=tk.NORMAL)
        self.log_text.delete("1.0", tk.END)
        self.log_text.config(state=tk.DISABLED)

    def _log_copy(self):
        """Copy selected log text to clipboard."""
        try:
            self.log_text.event_generate("<<Copy>>")
        except tk.TclError:
            pass

    def _log_select_all(self):
        """Select all text in the log widget."""
        self.log_text.tag_add(tk.SEL, "1.0", tk.END)
        self.log_text.mark_set(tk.INSERT, tk.END)
        self.log_text.see(tk.INSERT)

    def _show_log_context_menu(self, event):
        """Show the right-click context menu on the log widget."""
        try:
            self._log_context_menu.tk_popup(event.x_root, event.y_root)
        finally:
            self._log_context_menu.grab_release()

    # -- Connection Handling --------------------------------------

    def _refresh_ports(self):
        """Refresh the list of available serial ports."""
        ports = DECtalkESPressSerial.list_ports()
        self.port_combo["values"] = ports
        if ports and not self.port_var.get():
            self.port_var.set(ports[0])

    def _toggle_connection(self):
        """Connect or disconnect based on current state."""
        if self.ESPress.connected:
            self._disconnect()
        else:
            self._connect()

    def _connect(self):
        """Establish a serial connection using ESPress protocol."""
        port = self.port_var.get()
        if not port:
            self._show_message("No Port", "Please select a serial port.",
                               icon="warning")
            return

        baud = int(self.baud_var.get())
        self.status_var.set("Connecting to %s..." % port)
        self.connect_btn.config(state=tk.DISABLED)
        self._log("--", "Connecting to %s at %d baud..." % (port, baud))
        self.root.update()

        def do_connect():
            try:
                self.ESPress.connect(port, baud)
                self.root.after(0, self._on_connected)
            except Exception as exc:
                err = str(exc)
                self.root.after(0, lambda: self._on_connect_error(err))

        threading.Thread(target=do_connect, daemon=True).start()

    def _on_connected(self):
        """Called on the main thread after a successful connection."""
        self.status_var.set(
            "Connected to %s at %s baud (ESPress)"
            % (self.port_var.get(), self.baud_var.get())
        )
        self._log("--", "Connected")
        self.connect_btn.config(text="Disconnect", state=tk.NORMAL)
        self.speak_btn.config(state=tk.NORMAL)
        self.pause_btn.config(state=tk.NORMAL)
        self.resume_btn.config(state=tk.NORMAL)
        self.flush_btn.config(state=tk.NORMAL)
        self.status_btn.config(state=tk.NORMAL)
        self._paused = False

        # Start polling device status
        self._polling = True
        threading.Thread(target=self._poll_status_loop, daemon=True).start()

    def _on_connect_error(self, message):
        """Called on the main thread when connection fails."""
        self.status_var.set("Connection failed")
        self._log("--", "Connection failed: %s" % message)
        self.connect_btn.config(state=tk.NORMAL)
        self._show_message("Connection Error", message, icon="error")

    def _disconnect(self):
        """Close the serial connection."""
        self._polling = False
        self.ESPress.disconnect()
        self._log("--", "Disconnected")
        self.status_var.set("Disconnected")
        self.device_status_var.set("--")
        self._update_status_indicators(0)
        self.connect_btn.config(text="Connect", state=tk.NORMAL)
        self.speak_btn.config(state=tk.DISABLED)
        self.pause_btn.config(state=tk.DISABLED)
        self.resume_btn.config(state=tk.DISABLED)
        self.flush_btn.config(state=tk.DISABLED)
        self.status_btn.config(state=tk.DISABLED)
        self._paused = False

    # -- Speech Controls ------------------------------------------

    def _on_speak(self):
        """Send the text box contents to the device via ESPress protocol."""
        text = self.text_box.get("1.0", tk.END).strip()
        if not text:
            self._show_message("No Text", "Please enter some text to speak.")
            return

        if not self.ESPress.connected:
            self._show_message(
                "Not Connected",
                "Please connect to the device first.",
                icon="warning",
            )
            return

        voice = self.voice_var.get()
        rate = self.rate_var.get()
        pitch = self.pitch_var.get()

        self.status_var.set("Sending text...")

        # Build the prefixed text for logging (same as what the serial layer sends)
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
                self.ESPress.speak(text, voice=voice, rate=rate, pitch=pitch)
                self.root.after(0, lambda: self.status_var.set(
                    "Text sent -- Connected to %s (ESPress)"
                    % self.port_var.get()
                ))
                self.root.after(0, lambda: self._log("RX", "Text sent"))
            except Exception as exc:
                err = str(exc)
                self.root.after(
                    0,
                    lambda: self.status_var.set("Error: %s" % err),
                )
                self.root.after(0, lambda: self._log("RX", "Error: %s" % err))

        threading.Thread(target=do_speak, daemon=True).start()

    def _on_pause(self):
        """Pause speech output (send SO control character)."""
        if not self.ESPress.connected:
            return
        try:
            self.ESPress.pause()
            self._paused = True
            self.status_var.set("Paused")
            self._log("TX", "SO (Pause)")
        except Exception as exc:
            self.status_var.set("Error: %s" % str(exc))

    def _on_resume(self):
        """Resume speech output (send SI control character)."""
        if not self.ESPress.connected:
            return
        try:
            self.ESPress.resume()
            self._paused = False
            self.status_var.set(
                "Resumed -- Connected to %s (ESPress)"
                % self.port_var.get()
            )
            self._log("TX", "SI (Resume)")
        except Exception as exc:
            self.status_var.set("Error: %s" % str(exc))

    def _on_flush(self):
        """Flush (cancel) all pending speech."""
        if not self.ESPress.connected:
            return

        self.status_var.set("Flushing...")
        self._log("TX", "] + ETX + XON (Flush with ack)")

        def do_flush():
            try:
                ack = self.ESPress.flush_with_ack()
                if ack:
                    msg = "Flush acknowledged"
                    log_msg = "SOH (Flush acknowledged)"
                    log_dir = "RX"
                else:
                    msg = "Flush sent (no ack)"
                    log_msg = "Flush sent (no ack)"
                    log_dir = "RX"
                self.root.after(0, lambda: self.status_var.set(
                    "%s -- Connected to %s (ESPress)"
                    % (msg, self.port_var.get())
                ))
                self.root.after(0, lambda: self._log(log_dir, log_msg))
            except Exception as exc:
                err = str(exc)
                self.root.after(
                    0,
                    lambda: self.status_var.set("Error: %s" % err),
                )

        threading.Thread(target=do_flush, daemon=True).start()

    def _on_query_status(self):
        """Query device status via ENQ and display the result."""
        if not self.ESPress.connected:
            return

        self._log("TX", "ENQ (Query Status)")

        def do_query():
            try:
                status = self.ESPress.request_status()
                self.root.after(0, lambda: self._display_status(status, poll=False))
            except Exception as exc:
                self.root.after(
                    0,
                    lambda: self.status_var.set("Error: %s" % str(exc)),
                )

        threading.Thread(target=do_query, daemon=True).start()

    # -- Status Display -------------------------------------------

    def _display_status(self, status, poll=False):
        """Update the device status display with a decoded status word."""
        if status < 0:
            self.device_status_var.set("No response")
            self._update_status_indicators(0)
            if poll:
                self._log("RX", "Poll status: No response")
            else:
                self._log("RX", "Status: No response")
            return

        # Hex display
        self.device_status_var.set("0x%04X" % status)
        self._update_status_indicators(status)
        if poll:
            self._log("RX", "Poll status: 0x%04X" % status)
        else:
            self._log("RX", "Status: 0x%04X" % status)

    def _update_status_indicators(self, status):
        """Update the colored status indicator labels."""
        c = self._colors
        indicators = {
            "Ready":        (ESPRESS_STAT_RR_CHAR | ESPRESS_STAT_CMD_READY, c["ready_fg"]),
            "Transmitting": (ESPRESS_STAT_TR_CHAR, c["transmitting_fg"]),
            "Flushing":     (ESPRESS_STAT_FLUSHING, c["flushing_fg"]),
            "Index":        (ESPRESS_STAT_NEW_INDEX, c["index_fg"]),
        }
        for name, (mask, color) in indicators.items():
            lbl = self._status_indicators[name]
            if status & mask:
                lbl.config(foreground=color)
            else:
                lbl.config(foreground=c["inactive_fg"])

    def _poll_status_loop(self):
        """Background thread that periodically polls device status."""
        while self._polling and self.ESPress.connected:
            try:
                status = self.ESPress.request_status()
                if status >= 0:
                    self.root.after(
                        0, lambda s=status: self._display_status(s, poll=True)
                    )
            except Exception:
                pass
            time.sleep(2.0)

    # -- Slider Callbacks -----------------------------------------

    def _on_rate_change(self, value):
        """Update the rate display label."""
        self.rate_label.config(text=str(int(float(value))))

    def _on_pitch_change(self, value):
        """Update the pitch display label."""
        val = int(float(value))
        if val <= PITCH_MIN:
            self.pitch_label.config(text="Default")
        else:
            self.pitch_label.config(text=str(val))

    # -- Text Editing ---------------------------------------------

    def _cut(self):
        """Cut selected text to clipboard."""
        try:
            self.text_box.event_generate("<<Cut>>")
        except tk.TclError:
            pass

    def _copy(self):
        """Copy selected text to clipboard."""
        try:
            self.text_box.event_generate("<<Copy>>")
        except tk.TclError:
            pass

    def _paste(self):
        """Paste text from clipboard."""
        try:
            self.text_box.event_generate("<<Paste>>")
        except tk.TclError:
            pass

    def _select_all(self):
        """Select all text in the text box."""
        self.text_box.tag_add(tk.SEL, "1.0", tk.END)
        self.text_box.mark_set(tk.INSERT, tk.END)
        self.text_box.see(tk.INSERT)
        return "break"

    def _clear(self):
        """Clear all text from the text box."""
        self.text_box.delete("1.0", tk.END)

    def _show_context_menu(self, event):
        """Show the right-click context menu."""
        try:
            self.context_menu.tk_popup(event.x_root, event.y_root)
        finally:
            self.context_menu.grab_release()

    # -- Window Management ----------------------------------------

    def _on_close(self):
        """Clean up on window close."""
        self._polling = False
        if self.ESPress.connected:
            self.ESPress.disconnect()
        self.root.destroy()


def main():
    root = tk.Tk()
    DECtalkESPressGUI(root)
    root.mainloop()


if __name__ == "__main__":
    main()
