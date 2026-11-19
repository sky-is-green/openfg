#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""OpenFG status tray.

Reads the status file written by the layer and shows whether generation is
active. It is a small companion to the control panel, not an in-game overlay.
"""

import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

from PySide6 import QtCore, QtGui, QtWidgets

APP_NAME = "OpenFG Status"
STALE_MS = 8000


def status_path() -> Path:
    runtime = os.environ.get("XDG_RUNTIME_DIR")
    if runtime:
        return Path(runtime) / "openfg" / "status.json"
    state = os.environ.get("XDG_STATE_HOME")
    if state:
        return Path(state) / "openfg" / "status.json"
    return Path.home() / ".local" / "state" / "openfg" / "status.json"


def autostart_path() -> Path:
    base = Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config"))
    return base / "autostart" / "openfg-tray.desktop"


def read_status(path=None):
    path = path or status_path()
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None


def is_stale(status, now_ms=None):
    if not status:
        return True
    now_ms = now_ms or int(time.time() * 1000)
    return now_ms - int(status.get("updated", 0)) > STALE_MS


def gui_command():
    found = shutil.which("openfg-gui")
    if found:
        return [found]
    return [sys.executable, str(Path(__file__).resolve().with_name("openfg-gui.py"))]


class Tray(QtCore.QObject):
    def __init__(self, app):
        super().__init__()
        self.app = app
        self.path = status_path()
        self.status = None
        self.tray = QtWidgets.QSystemTrayIcon(self.icon("#6b7280"), app)
        self.tray.setToolTip("OpenFG: no active game")
        self.menu = QtWidgets.QMenu()
        self.menu.addAction("Open Control Panel", self.open_control)
        self.menu.addAction("Open status folder", self.open_folder)
        self.autostart = self.menu.addAction("Start at login")
        self.autostart.setCheckable(True)
        self.autostart.setChecked(autostart_path().exists())
        self.autostart.toggled.connect(self.toggle_autostart)
        self.menu.addSeparator()
        self.menu.addAction("Quit", app.quit)
        self.tray.setContextMenu(self.menu)
        self.tray.activated.connect(self.activated)
        self.timer = QtCore.QTimer(self)
        self.timer.timeout.connect(self.refresh)
        self.timer.start(1000)
        self.refresh()
        self.tray.show()

    def icon(self, color):
        pixmap = QtGui.QPixmap(64, 64)
        pixmap.fill(QtCore.Qt.transparent)
        painter = QtGui.QPainter(pixmap)
        painter.setRenderHint(QtGui.QPainter.Antialiasing)
        painter.setBrush(QtGui.QColor(color))
        painter.setPen(QtCore.Qt.NoPen)
        painter.drawEllipse(4, 4, 56, 56)
        painter.setPen(QtGui.QColor("#ffffff"))
        font = painter.font()
        font.setBold(True)
        font.setPixelSize(26)
        painter.setFont(font)
        painter.drawText(pixmap.rect(), QtCore.Qt.AlignCenter, "FG")
        painter.end()
        return QtGui.QIcon(pixmap)

    def refresh(self):
        self.status = read_status(self.path)
        if is_stale(self.status):
            self.tray.setIcon(self.icon("#6b7280"))
            self.tray.setToolTip("OpenFG: no active game")
            return
        generating = self.status.get("state") == "generating"
        color = "#2ea043" if generating else "#3b6ea8"
        self.tray.setIcon(self.icon(color))
        process = self.status.get("process", "unknown")
        app_fps = self.status.get("app_fps", 0.0)
        presented = self.status.get("presented_fps", 0.0)
        overhead = self.status.get("overhead_ms", 0.0)
        state = "generating" if generating else "idle"
        self.tray.setToolTip(
            f"OpenFG: {state}\n{process}\n"
            f"{app_fps:.1f} -> {presented:.1f} fps  ·  {overhead:.2f} ms")

    def activated(self, reason):
        if reason == QtWidgets.QSystemTrayIcon.Trigger:
            self.open_control()

    def open_control(self):
        try:
            subprocess.Popen(gui_command(), start_new_session=True)
        except OSError as error:
            QtWidgets.QMessageBox.warning(None, APP_NAME, str(error))

    def open_folder(self):
        self.path.parent.mkdir(parents=True, exist_ok=True)
        QtGui.QDesktopServices.openUrl(
            QtCore.QUrl.fromLocalFile(str(self.path.parent)))

    def toggle_autostart(self, enabled):
        path = autostart_path()
        if enabled:
            path.parent.mkdir(parents=True, exist_ok=True)
            launcher = shutil.which("openfg-tray")
            command = launcher or f"{sys.executable} {Path(__file__).resolve()}"
            path.write_text(
                "[Desktop Entry]\n"
                "Type=Application\n"
                f"Name={APP_NAME}\n"
                f"Exec={command}\n"
                "Icon=openfg\n"
                "Terminal=false\n"
                "X-GNOME-Autostart-enabled=true\n",
                encoding="utf-8")
        else:
            try:
                path.unlink()
            except FileNotFoundError:
                pass


def main() -> int:
    app = QtWidgets.QApplication(sys.argv)
    app.setApplicationName("openfg-tray")
    app.setApplicationDisplayName(APP_NAME)
    app.setQuitOnLastWindowClosed(False)
    if not QtWidgets.QSystemTrayIcon.isSystemTrayAvailable():
        print("openfg-tray: no system tray available", file=sys.stderr)
        return 1
    icon_path = Path(__file__).resolve().with_name("openfg.svg")
    if icon_path.is_file():
        app.setWindowIcon(QtGui.QIcon(str(icon_path)))
    tray = Tray(app)
    app.aboutToQuit.connect(tray.tray.hide)
    return app.exec()


if __name__ == "__main__":
    sys.exit(main())
