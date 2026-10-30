#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""openfg control panel.

Edits ~/.config/openfg/conf.toml and can call openfg-cli to validate it. The
layer reads the file once per process, so changes apply to applications
started afterwards.
"""

import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from PySide6 import QtCore, QtGui, QtWidgets

APP_NAME = "openfg Control"
VERSION = "0.1.0"


def repo_root() -> Path:
    return Path(__file__).resolve().parent.parent


def config_path() -> Path:
    env = os.environ.get("OPENFG_CONFIG")
    if env:
        return Path(env)
    base = Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config"))
    return base / "openfg" / "conf.toml"


def cli_path():
    candidates = [
        os.environ.get("OPENFG_CLI"),
        str(repo_root() / "build" / "openfg-cli"),
        str(Path.home() / ".local" / "bin" / "openfg-cli"),
        shutil.which("openfg-cli"),
    ]
    for candidate in candidates:
        if candidate and Path(candidate).is_file():
            return str(candidate)
    return None


def vendor_name(vendor_id: str) -> str:
    names = {
        "1002": "AMD",
        "10de": "NVIDIA",
        "8086": "Intel",
        "13b5": "ARM",
        "5143": "Qualcomm",
        "106b": "Apple",
    }
    return names.get(vendor_id.lower(), f"0x{vendor_id}")


def connected_pci() -> set[str]:
    result = set()
    for card in Path("/sys/class/drm").glob("card[0-9]*"):
        device = card / "device"
        if not device.exists():
            continue
        try:
            pci = os.path.basename(os.readlink(device))
        except OSError:
            continue
        for status in card.glob("card*-*/status"):
            try:
                if status.read_text().strip() == "connected":
                    result.add(pci)
                    break
            except OSError:
                pass
    return result


def enumerate_gpus() -> list[dict]:
    """Parse Mesa's selectable-device list for name and PCI bus info."""
    try:
        proc = subprocess.run(
            ["vulkaninfo", "--summary"],
            capture_output=True,
            text=True,
            timeout=25,
            env={**os.environ, "MESA_VK_DEVICE_SELECT": "list"},
        )
        text = (proc.stdout or "") + "\n" + (proc.stderr or "")
    except Exception:
        return []

    displays = connected_pci()
    pattern = re.compile(
        r'GPU\s+(\d+):\s+([0-9a-fA-F]{4}:[0-9a-fA-F]{4})\s+"([^"]*)"\s+'
        r'(\S+)\s+GPU\s+([0-9a-fA-F:.]+)'
    )
    gpus = []
    for match in pattern.finditer(text):
        vendor_device = match.group(2)
        pci = match.group(5)
        gpus.append({
            "index": int(match.group(1)),
            "name": match.group(3),
            "vendor": vendor_name(vendor_device.split(":")[0]),
            "vendor_device": vendor_device,
            "pci": pci,
            "display": pci in displays,
        })
    return gpus

def gpu_label(gpu: dict) -> str:
    role = "display" if gpu["display"] else "no display"
    pci = gpu["pci"] or "no PCI info"
    return f'[{gpu["vendor"]}] {gpu["name"]}  ·  {pci}  ({role})'


def load_config() -> dict:
    config = {
        "enabled": False,
        "gpu": "auto",
        "cross_gpu": False,
        "multiplier": 2,
        "debug": False,
        "include": [],
        "exclude": [],
    }
    path = config_path()
    if not path.is_file():
        return config
    try:
        import tomllib
        with open(path, "rb") as handle:
            data = tomllib.load(handle)
    except Exception:
        return config
    section = data.get("openfg", {})
    if isinstance(section, dict):
        config["enabled"] = bool(section.get("enabled", config["enabled"]))
        config["gpu"] = str(section.get("gpu", config["gpu"]))
        config["cross_gpu"] = bool(section.get("cross_gpu", config["cross_gpu"]))
        try:
            config["multiplier"] = int(section.get("multiplier", config["multiplier"]))
        except (TypeError, ValueError):
            pass
        config["debug"] = bool(section.get("debug", config["debug"]))
    processes = data.get("processes", {})
    if isinstance(processes, dict):
        for key in ("include", "exclude"):
            values = processes.get(key, [])
            if isinstance(values, str):
                values = [values]
            if isinstance(values, list):
                config[key] = [str(v) for v in values if str(v).strip()]
    return config


def quote(value: str) -> str:
    return json.dumps(value, ensure_ascii=False)


def save_config_to(path: Path, config: dict) -> None:
    def array(values):
        return "[" + ", ".join(quote(v) for v in values) + "]"

    path.parent.mkdir(parents=True, exist_ok=True)
    text = f"""# openfg configuration
#
# Written by openfg Control. The layer reads this file once per process.

[openfg]
enabled = {'true' if config['enabled'] else 'false'}
gpu = {quote(config['gpu'])}
cross_gpu = {'true' if config['cross_gpu'] else 'false'}
multiplier = {config['multiplier']}
debug = {'true' if config['debug'] else 'false'}

[processes]
include = {array(config['include'])}
exclude = {array(config['exclude'])}
"""
    path.write_text(text, encoding="utf-8")


def save_config(config: dict) -> None:
    save_config_to(config_path(), config)


def run_cli_validate(path: Path | None = None) -> tuple[bool, str]:
    cli = cli_path()
    if not cli:
        return True, "openfg-cli not found; saved without validation"
    proc = subprocess.run(
        [cli, "validate", str(path or config_path())],
        capture_output=True,
        text=True,
    )
    message = ((proc.stdout or "") + (proc.stderr or "")).strip()
    return proc.returncode == 0, message or f"exit code {proc.returncode}"


def hint(text: str) -> QtWidgets.QLabel:
    label = QtWidgets.QLabel(text)
    label.setWordWrap(True)
    label.setStyleSheet("color:#9aa0a6; font-size:11px;")
    return label


class MainWindow(QtWidgets.QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle(f"{APP_NAME} {VERSION}")
        self.resize(760, 780)
        self.gpus = enumerate_gpus()
        self.config = load_config()

        central = QtWidgets.QWidget()
        root = QtWidgets.QVBoxLayout(central)
        root.setSpacing(10)
        self.setCentralWidget(central)

        title = QtWidgets.QLabel("openfg frame generation")
        title.setStyleSheet("font-size:20px; font-weight:600;")
        root.addWidget(title)
        root.addWidget(hint(
            "Optical-flow frame generation for Vulkan. Changes are written to "
            f"{config_path()} and apply to applications started afterwards."
        ))

        # Master switch
        self.enabled = QtWidgets.QCheckBox("Enable frame generation")
        self.enabled.setChecked(self.config["enabled"])
        self.enabled.setStyleSheet("font-size:14px;")
        root.addWidget(self.enabled)

        # Frame generation settings
        settings = QtWidgets.QGroupBox("Frame generation")
        form = QtWidgets.QFormLayout(settings)
        form.setFieldGrowthPolicy(QtWidgets.QFormLayout.ExpandingFieldsGrow)

        self.gpu = QtWidgets.QComboBox()
        self.gpu.setEditable(True)
        self.gpu.addItem("Automatic (application's GPU / display card)", "auto")
        for gpu in self.gpus:
            self.gpu.addItem(gpu_label(gpu), gpu["pci"] or gpu["name"])
        self.gpu.setToolTip(
            "The GPU that runs the interpolator. 'Automatic' uses the GPU the "
            "application renders on. For cross-GPU, pick the second card's PCI "
            "address (usually the one with no display attached)."
        )
        self._select_gpu(self.config["gpu"])
        form.addRow("Frame-generation GPU:", self.gpu)

        self.cross_gpu = QtWidgets.QCheckBox("Generate on a second GPU (DMA-BUF)")
        self.cross_gpu.setChecked(self.config["cross_gpu"])
        self.cross_gpu.setToolTip(
            "The backend runs on the selected GPU and shares frames with the "
            "display GPU through DMA-BUF. Needs an explicit GPU selection."
        )
        self.cross_gpu.toggled.connect(self._cross_gpu_toggled)
        form.addRow("", self.cross_gpu)

        self.multiplier = QtWidgets.QComboBox()
        for value in (2, 3, 4):
            self.multiplier.addItem(f"{value}x  ({value - 1} generated frame"
                                    f"{'s' if value > 2 else ''} per real frame)", value)
        index = self.multiplier.findData(self.config["multiplier"])
        self.multiplier.setCurrentIndex(index if index >= 0 else 0)
        form.addRow("Multiplier:", self.multiplier)

        self.debug = QtWidgets.QCheckBox("Log generated frames to stderr")
        self.debug.setChecked(self.config["debug"])
        form.addRow("", self.debug)

        root.addWidget(settings)

        # Process filters
        processes = QtWidgets.QGroupBox("Processes")
        process_layout = QtWidgets.QGridLayout(processes)
        process_layout.addWidget(QtWidgets.QLabel("Include (one per line):"), 0, 0)
        process_layout.addWidget(QtWidgets.QLabel("Exclude (one per line):"), 0, 1)
        self.include = QtWidgets.QPlainTextEdit("\n".join(self.config["include"]))
        self.exclude = QtWidgets.QPlainTextEdit("\n".join(self.config["exclude"]))
        for edit in (self.include, self.exclude):
            edit.setPlaceholderText("empty = all processes")
            edit.setMinimumHeight(110)
        process_layout.addWidget(self.include, 1, 0)
        process_layout.addWidget(self.exclude, 1, 1)
        process_layout.addWidget(hint(
            "Process names, case-insensitive. '*' and '?' wildcards are "
            "supported. Wine/Proton games are matched by their .exe name. "
            "An empty include list means every process; exclude always wins."
        ), 2, 0, 1, 2)
        root.addWidget(processes)

        # Buttons
        buttons = QtWidgets.QHBoxLayout()
        self.status = QtWidgets.QLabel("")
        self.status.setWordWrap(True)
        buttons.addWidget(self.status, 1)

        reload_button = QtWidgets.QPushButton("Reload")
        reload_button.clicked.connect(self.reload)
        validate_button = QtWidgets.QPushButton("Validate")
        validate_button.clicked.connect(self.validate)
        save_button = QtWidgets.QPushButton("Save")
        save_button.setDefault(True)
        save_button.clicked.connect(self.save)
        buttons.addWidget(reload_button)
        buttons.addWidget(validate_button)
        buttons.addWidget(save_button)
        root.addLayout(buttons)

        root.addStretch(1)

    def _select_gpu(self, gpu: str) -> None:
        index = self.gpu.findData(gpu)
        if index >= 0:
            self.gpu.setCurrentIndex(index)
        else:
            self.gpu.setEditText(gpu)

    def _cross_gpu_toggled(self, checked: bool) -> None:
        if not checked:
            return
        if self.gpu.currentData() not in (None, "auto"):
            return
        # Prefer a card with no display attached.
        for gpu in self.gpus:
            if gpu["pci"] and not gpu["display"]:
                self._select_gpu(gpu["pci"])
                return
        for gpu in self.gpus:
            if gpu["pci"]:
                self._select_gpu(gpu["pci"])
                return

    def _collect(self) -> dict:
        data = self.gpu.currentData()
        gpu = str(data) if data not in (None, "") else self.gpu.currentText().strip()
        if gpu.lower().startswith("auto"):
            gpu = "auto"
        return {
            "enabled": self.enabled.isChecked(),
            "gpu": gpu or "auto",
            "cross_gpu": self.cross_gpu.isChecked(),
            "multiplier": int(self.multiplier.currentData()),
            "debug": self.debug.isChecked(),
            "include": [line.strip() for line in self.include.toPlainText().splitlines()
                        if line.strip()],
            "exclude": [line.strip() for line in self.exclude.toPlainText().splitlines()
                        if line.strip()],
        }

    def _set_status(self, text: str, error: bool = False) -> None:
        color = "#e06c75" if error else "#7ec699"
        self.status.setStyleSheet(f"color:{color};")
        self.status.setText(text)

    def save(self) -> None:
        config = self._collect()
        if config["cross_gpu"] and config["gpu"] == "auto":
            self._set_status("Cross-GPU needs an explicit frame-generation GPU.", True)
            return
        try:
            save_config(config)
        except OSError as error:
            self._set_status(f"Cannot write {config_path()}: {error}", True)
            return
        self.config = config
        ok, message = run_cli_validate()
        if ok:
            self._set_status(f"Saved {config_path()}. {message}", False)
        else:
            self._set_status(message, True)

    def validate(self) -> None:
        config = self._collect()
        if config["cross_gpu"] and config["gpu"] == "auto":
            self._set_status("Cross-GPU needs an explicit frame-generation GPU.", True)
            return
        # Validate the current widgets against a temporary copy, so unsaved
        # edits are checked without touching the real config.
        try:
            with tempfile.TemporaryDirectory(prefix="openfg-gui-") as directory:
                path = Path(directory) / "conf.toml"
                save_config_to(path, config)
                ok, message = run_cli_validate(path)
        except OSError as error:
            self._set_status(f"Cannot validate: {error}", True)
            return
        self._set_status(message, not ok)

    def reload(self) -> None:
        self.config = load_config()
        self.enabled.setChecked(self.config["enabled"])
        self.cross_gpu.setChecked(self.config["cross_gpu"])
        self._select_gpu(self.config["gpu"])
        index = self.multiplier.findData(self.config["multiplier"])
        self.multiplier.setCurrentIndex(index if index >= 0 else 0)
        self.debug.setChecked(self.config["debug"])
        self.include.setPlainText("\n".join(self.config["include"]))
        self.exclude.setPlainText("\n".join(self.config["exclude"]))
        self._set_status(f"Reloaded {config_path()}.")


def main() -> int:
    app = QtWidgets.QApplication(sys.argv)
    app.setApplicationName("openfg-gui")
    app.setApplicationDisplayName(APP_NAME)
    window = MainWindow()
    window.show()
    return app.exec()


if __name__ == "__main__":
    sys.exit(main())
