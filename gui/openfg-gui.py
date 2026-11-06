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
import textwrap
from pathlib import Path

from PySide6 import QtCore, QtGui, QtWidgets

APP_NAME = "OpenFG Control"
VERSION = "0.1.0-preview1"

APP_STYLE = """
QWidget {
    background-color: #12161b;
    color: #e6e8eb;
    font-size: 13px;
}
QMainWindow { background-color: #12161b; }
QScrollArea { border: none; background: transparent; }
QScrollArea > QWidget > QWidget { background: #12161b; }
QGroupBox {
    background-color: #1a1f26;
    border: 1px solid #2b333d;
    border-radius: 8px;
    margin-top: 16px;
    padding: 14px 12px 12px 12px;
    font-weight: 600;
}
QGroupBox::title {
    subcontrol-origin: margin;
    subcontrol-position: top left;
    left: 12px;
    padding: 0 6px;
    color: #aab3bf;
}
QLabel { background: transparent; }
QCheckBox { background: transparent; spacing: 8px; }
QCheckBox::indicator {
    width: 16px; height: 16px;
    border: 1px solid #3a4350; border-radius: 4px;
    background: #0e1217;
}
QCheckBox::indicator:checked {
    background: #3b6ea8; border-color: #4c82bf;
}
QLineEdit, QComboBox, QSpinBox, QPlainTextEdit {
    background-color: #0e1217;
    border: 1px solid #2b333d;
    border-radius: 5px;
    padding: 5px 7px;
    selection-background-color: #3b5b8a;
}
QLineEdit:focus, QComboBox:focus, QSpinBox:focus, QPlainTextEdit:focus {
    border-color: #4c82bf;
}
QLineEdit:disabled, QComboBox:disabled, QSpinBox:disabled,
QPlainTextEdit:disabled {
    color: #6b7280;
    background-color: #151a20;
}
QComboBox::drop-down { border: none; width: 22px; }
QComboBox QAbstractItemView {
    background: #161b22;
    border: 1px solid #2b333d;
    selection-background-color: #2d3b4f;
}
QPushButton {
    background-color: #232a33;
    border: 1px solid #323b47;
    border-radius: 5px;
    padding: 7px 12px;
}
QPushButton:hover { background-color: #2b3440; }
QPushButton:pressed { background-color: #1d242c; }
QPushButton:default { background-color: #3b6ea8; border-color: #4c82bf; }
QPushButton:default:hover { background-color: #457cb8; }
QToolTip {
    background-color: #1a1f26;
    color: #e6e8eb;
    border: 1px solid #3a4350;
    padding: 6px 8px;
}
QScrollBar:vertical { background: transparent; width: 10px; margin: 2px; }
QScrollBar::handle:vertical {
    background: #2b333d; border-radius: 5px; min-height: 30px;
}
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }
"""


def short_path(path: Path) -> str:
    try:
        return "~/" + str(path.relative_to(Path.home()))
    except ValueError:
        return str(path)


def wrap_tooltip(text: str, width: int = 48) -> str:
    return textwrap.fill(" ".join(text.split()), width)


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


def probe_path():
    candidates = [
        os.environ.get("OPENFG_PROBE"),
        str(repo_root() / "build" / "openfg-gpu-probe"),
        str(Path.home() / ".local" / "bin" / "openfg-gpu-probe"),
        shutil.which("openfg-gpu-probe"),
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
    if gpus:
        return gpus
    return enumerate_gpus_from_probe()


def enumerate_gpus_from_probe() -> list[dict]:
    """Fallback for systems without vulkaninfo."""
    probe = probe_path()
    if not probe:
        return []
    try:
        proc = subprocess.run(
            [probe], capture_output=True, text=True, timeout=60)
    except Exception:
        return []
    text = (proc.stdout or "") + "\n" + (proc.stderr or "")
    displays = connected_pci()
    gpus = []
    for block in re.split(r'^GPU\s+\d+\s*$', text, flags=re.MULTILINE):
        name = re.search(r'^\s*name\s+:\s*(.+?)\s*$', block, re.MULTILINE)
        pci = re.search(r'^\s*pci\s+:\s*([0-9a-fA-F:.]+)\s*$', block, re.MULTILINE)
        vendor_device = re.search(
            r'^\s*vendor:device\s+:\s*([0-9a-fA-F]{4}:[0-9a-fA-F]{4})',
            block, re.MULTILINE)
        if not (name and vendor_device):
            continue
        pci_text = pci.group(1) if pci else ""
        gpus.append({
            "index": len(gpus),
            "name": name.group(1),
            "vendor": vendor_name(vendor_device.group(1).split(":")[0]),
            "vendor_device": vendor_device.group(1),
            "pci": pci_text,
            "display": pci_text in displays,
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
        "target_fps": 0,
        "max_presented_fps": 0,
        "performance_mode": False,
        "generator": "ffx",
        "pacing": "adaptive",
        "present_timing": False,
        "debug": False,
        "include": [],
        "exclude": [],
        "video_processes": [],
        "video_multiplier": 4,
        "video_target_fps": 0,
        "video_performance_mode": False,
        "video_safe": True,
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
        for key in ("multiplier", "target_fps", "max_presented_fps"):
            try:
                config[key] = int(section.get(key, config[key]))
            except (TypeError, ValueError):
                pass
        config["performance_mode"] = bool(
            section.get("performance_mode", config["performance_mode"]))
        config["generator"] = str(section.get("generator", config["generator"]))
        config["pacing"] = str(section.get("pacing", config["pacing"]))
        config["present_timing"] = bool(
            section.get("present_timing", config["present_timing"]))
        config["debug"] = bool(section.get("debug", config["debug"]))
    processes = data.get("processes", {})
    if isinstance(processes, dict):
        for key in ("include", "exclude"):
            values = processes.get(key, [])
            if isinstance(values, str):
                values = [values]
            if isinstance(values, list):
                config[key] = [str(v) for v in values if str(v).strip()]
    video = data.get("video", {})
    if isinstance(video, dict):
        values = video.get("processes", [])
        if isinstance(values, str):
            values = [values]
        if isinstance(values, list):
            config["video_processes"] = [str(v) for v in values if str(v).strip()]
        for key in ("video_multiplier", "video_target_fps"):
            try:
                config[key] = int(video.get(key.removeprefix("video_"), config[key]))
            except (TypeError, ValueError):
                pass
        config["video_performance_mode"] = bool(
            video.get("performance_mode", config["video_performance_mode"]))
        config["video_safe"] = bool(video.get("safe", config["video_safe"]))
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
target_fps = {config['target_fps']}
max_presented_fps = {config['max_presented_fps']}
performance_mode = {'true' if config['performance_mode'] else 'false'}
generator = {quote(config['generator'])}
pacing = {quote(config['pacing'])}
present_timing = {'true' if config['present_timing'] else 'false'}
debug = {'true' if config['debug'] else 'false'}

[processes]
include = {array(config['include'])}
exclude = {array(config['exclude'])}

[video]
processes = {array(config['video_processes'])}
multiplier = {config['video_multiplier']}
target_fps = {config['video_target_fps']}
performance_mode = {'true' if config['video_performance_mode'] else 'false'}
safe = {'true' if config['video_safe'] else 'false'}
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
        self.resize(960, 920)
        self.gpus = enumerate_gpus()
        self.config = load_config()

        central = QtWidgets.QWidget()
        root = QtWidgets.QVBoxLayout(central)
        root.setContentsMargins(18, 16, 18, 16)
        root.setSpacing(14)
        scroll = QtWidgets.QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setFrameShape(QtWidgets.QFrame.NoFrame)
        scroll.setHorizontalScrollBarPolicy(QtCore.Qt.ScrollBarAlwaysOff)
        scroll.setWidget(central)
        self.setCentralWidget(scroll)

        header = QtWidgets.QHBoxLayout()
        titles = QtWidgets.QVBoxLayout()
        titles.setSpacing(0)
        title = QtWidgets.QLabel("OpenFG")
        title.setStyleSheet(
            "font-size:24px; font-weight:700; color:#f2f4f7;")
        subtitle = QtWidgets.QLabel("Frame generation for Vulkan")
        subtitle.setStyleSheet("color:#9aa0a6; font-size:13px;")
        titles.addWidget(title)
        titles.addWidget(subtitle)
        header.addLayout(titles)
        header.addStretch(1)
        version = QtWidgets.QLabel(VERSION)
        version.setStyleSheet(
            "color:#9ecbff; background:#1d2630; border:1px solid #2e4359; "
            "border-radius:9px; padding:3px 9px; font-size:11px; "
            "font-weight:600;")
        header.addWidget(version, 0, QtCore.Qt.AlignTop)
        root.addLayout(header)

        path_label = QtWidgets.QLabel(
            f"Config: {short_path(config_path())}  ·  "
            "changes apply to newly started applications")
        path_label.setStyleSheet("color:#7f8790; font-size:11px;")
        root.addWidget(path_label)

        # Master switch
        self.enabled = QtWidgets.QCheckBox("Enable frame generation")
        self.enabled.setChecked(self.config["enabled"])
        self.enabled.setStyleSheet(
            "font-size:14px; font-weight:600; padding:2px 0;")
        root.addWidget(self.enabled)

        self.summary = QtWidgets.QLabel("")
        self.summary.setWordWrap(True)
        root.addWidget(self.summary)

        # Presets
        presets = QtWidgets.QGroupBox("Presets")
        preset_layout = QtWidgets.QGridLayout(presets)
        preset_layout.setHorizontalSpacing(8)
        preset_layout.setVerticalSpacing(8)

        def preset(text, tooltip, callback):
            button = QtWidgets.QPushButton(text)
            button.setMinimumHeight(30)
            button.setToolTip(tooltip)
            button.clicked.connect(callback)
            return button

        preset_layout.addWidget(preset(
            "Latency-friendly",
            "2x, capped pipeline. Cap the game at half the display refresh "
            "(~82 on 165 Hz) so generation starts immediately.",
            lambda: self._apply_preset("latency")), 0, 0)
        preset_layout.addWidget(preset(
            "Quality",
            "2x, always generate, full-resolution optical flow (FFX by "
            "default).",
            lambda: self._apply_preset("quality")), 0, 1)
        preset_layout.addWidget(preset(
            "Performance",
            "2x, always generate, half-resolution flow. About 50% faster at "
            "1440p with slightly softer edges.",
            lambda: self._apply_preset("performance")), 1, 0)
        preset_layout.addWidget(preset(
            "Video · 4x",
            "4x for a Vulkan video player (mpv/gpu-next); adds mpv to the "
            "video profile and enables the cut guard.",
            lambda: self._apply_preset("video")), 1, 1)
        preset_layout.addWidget(hint(
            "Presets apply 2x, same GPU and FFX. Nothing is written until "
            "Save."), 2, 0, 1, 2)
        root.addWidget(presets)

        # Frame generation settings
        settings = QtWidgets.QGroupBox("Frame generation")
        settings_layout = QtWidgets.QVBoxLayout(settings)
        form = QtWidgets.QFormLayout()
        form.setFieldGrowthPolicy(QtWidgets.QFormLayout.ExpandingFieldsGrow)

        self.gpu = QtWidgets.QComboBox()
        self.gpu.setEditable(True)
        self.gpu.addItem("Automatic (application's GPU / display card)", "auto")
        for gpu in self.gpus:
            self.gpu.addItem(gpu_label(gpu), gpu["pci"] or gpu["name"])
        self.gpu.setToolTip(
            "GPU that runs interpolation. Automatic uses the application's "
            "display GPU. Cross-GPU: pick the second card's PCI address."
        )
        self.gpu.setSizePolicy(
            QtWidgets.QSizePolicy.Expanding, QtWidgets.QSizePolicy.Fixed)
        self._select_gpu(self.config["gpu"])
        gpu_row = QtWidgets.QHBoxLayout()
        gpu_row.addWidget(self.gpu, 1)
        probe_button = QtWidgets.QPushButton("Probe…")
        probe_button.setToolTip(
            "Run openfg-gpu-probe and show this machine's backend "
            "capabilities.")
        probe_button.clicked.connect(self.run_gpu_probe)
        gpu_row.addWidget(probe_button)
        form.addRow("Frame-generation GPU:", gpu_row)

        self.cross_gpu = QtWidgets.QCheckBox("Generate on a second GPU (DMA-BUF)")
        self.cross_gpu.setChecked(self.config["cross_gpu"])
        self.cross_gpu.setToolTip(
            "Run interpolation on the selected GPU and share frames over "
            "DMA-BUF. Adds two copies and is usually slower than same-GPU."
        )
        self.cross_gpu.toggled.connect(self._cross_gpu_toggled)
        form.addRow("", self.cross_gpu)

        self.multiplier = QtWidgets.QComboBox()
        for value in (2, 3, 4):
            self.multiplier.addItem(
                f"{value}x  ({value - 1} generated frame"
                f"{'s' if value > 2 else ''} per real frame)", value)
        self.multiplier.setSizePolicy(
            QtWidgets.QSizePolicy.Expanding, QtWidgets.QSizePolicy.Fixed)
        index = self.multiplier.findData(self.config["multiplier"])
        self.multiplier.setCurrentIndex(index if index >= 0 else 0)
        self.multiplier.setToolTip(
            "2x inserts one frame and has the lowest delay. 3x/4x insert more "
            "frames before the real one and add display delay."
        )
        form.addRow("Multiplier:", self.multiplier)

        self.generator = QtWidgets.QComboBox()
        for value, label in (
            ("ffx", "FidelityFX optical flow (default, fast)"),
            ("flow", "openfg optical flow (quality; FFX fallback)"),
            ("blend", "Linear blend (no flow; fast baseline)"),
            ("hold", "Hold current frame (no interpolation)"),
            ("off", "Off / timing only"),
        ):
            self.generator.addItem(label, value)
        self.generator.setSizePolicy(
            QtWidgets.QSizePolicy.Expanding, QtWidgets.QSizePolicy.Fixed)
        index = self.generator.findData(self.config["generator"])
        self.generator.setCurrentIndex(index if index >= 0 else 0)
        self.generator.setToolTip(
            "FFX is the default and includes the static-UI mask for HUD text. "
            "Unsupported GPUs fall back to flow. Blend and hold are baselines; "
            "off keeps the layer loaded for timing."
        )
        form.addRow("Generator:", self.generator)

        self.debug = QtWidgets.QCheckBox("Log generated frames to stderr")
        self.debug.setChecked(self.config["debug"])
        self.present_timing = QtWidgets.QCheckBox(
            "Present timing instrumentation (benchmarking)")
        self.present_timing.setChecked(self.config["present_timing"])
        self.present_timing.setToolTip(
            "Record present-latency percentiles in the log. It can change "
            "compositor pacing; use for benchmarks only."
        )

        settings_layout.addLayout(form)
        settings_layout.addWidget(hint(
            "2x has the lowest latency; each extra frame adds display delay. "
            "FFX falls back to flow when the GPU lacks its requirements."
        ))
        settings_layout.addWidget(self.debug)
        settings_layout.addWidget(self.present_timing)

        root.addWidget(settings)

        # Latency settings
        latency = QtWidgets.QGroupBox("Latency")
        latency_layout = QtWidgets.QVBoxLayout(latency)
        latency_form = QtWidgets.QFormLayout()
        latency_form.setFieldGrowthPolicy(QtWidgets.QFormLayout.ExpandingFieldsGrow)

        self.pacing = QtWidgets.QComboBox()
        self.pacing.addItem("Adaptive (1 s threshold dwell)", "adaptive")
        self.pacing.addItem("Capped source (start immediately)", "capped")
        self.pacing.setSizePolicy(
            QtWidgets.QSizePolicy.Expanding, QtWidgets.QSizePolicy.Fixed)
        index = self.pacing.findData(self.config["pacing"])
        self.pacing.setCurrentIndex(index if index >= 0 else 0)
        self.pacing.setToolTip(
            "Adaptive waits for a sustained drop below the target. Capped "
            "starts immediately; cap the game yourself at refresh ÷ multiplier."
        )
        latency_form.addRow("Pacing:", self.pacing)

        self.target_fps = QtWidgets.QSpinBox()
        self.target_fps.setRange(0, 1000)
        self.target_fps.setSingleStep(5)
        self.target_fps.setSpecialValueText("Always generate")
        self.target_fps.setSuffix(" fps")
        self.target_fps.setSizePolicy(
            QtWidgets.QSizePolicy.Expanding, QtWidgets.QSizePolicy.Fixed)
        self.target_fps.setValue(self.config["target_fps"])
        self.target_fps.setToolTip(
            "Generate only below this native rate. 0 = always. Use refresh ÷ "
            "multiplier: about 82 fps for 2x at 165 Hz."
        )
        self.target_note = hint("")
        latency_form.addRow("Target FPS:", self.target_fps)

        self.max_presented = QtWidgets.QComboBox()
        self.max_presented.addItem("Off", 0)
        self.max_presented.addItem("Auto (detected display refresh)", -1)
        self.max_presented.addItem("Custom", 1)
        self.max_presented.setToolTip(
            "Stop when presented fps would exceed this value. Auto reads the "
            "display refresh; set OPENFG_REFRESH if detection fails."
        )
        self.max_presented_spin = QtWidgets.QSpinBox()
        self.max_presented_spin.setRange(20, 1000)
        self.max_presented_spin.setSingleStep(5)
        self.max_presented_spin.setSuffix(" presented fps")
        screen = QtWidgets.QApplication.primaryScreen()
        refresh = screen.refreshRate() if screen else 0.0
        self.max_presented_spin.setValue(
            round(refresh) if 20 <= refresh <= 1000 else 165)
        configured_cap = int(self.config["max_presented_fps"])
        if configured_cap == -1:
            self.max_presented.setCurrentIndex(1)
        elif configured_cap >= 20:
            self.max_presented.setCurrentIndex(2)
            self.max_presented_spin.setValue(configured_cap)
        else:
            self.max_presented.setCurrentIndex(0)
        cap_row = QtWidgets.QHBoxLayout()
        cap_row.addWidget(self.max_presented, 1)
        cap_row.addWidget(self.max_presented_spin)
        latency_form.addRow("Max presented FPS:", cap_row)
        self.max_presented_spin.setEnabled(self.max_presented.currentData() == 1)

        self.performance = QtWidgets.QCheckBox(
            "Performance mode (half-resolution flow)")
        self.performance.setChecked(self.config["performance_mode"])
        self.performance.setToolTip(
            "Half-resolution flow: about 1.03 → 0.52 ms with FFX at 1440p. "
            "Quality stays within run-to-run noise."
        )
        latency_form.addRow("", self.performance)

        latency_layout.addLayout(latency_form)
        latency_layout.addWidget(self.target_note)
        latency_layout.addWidget(hint(
            "Measured on the dual RX 7900 XT test system (Helldivers 2, "
            "1440p). See docs/LATENCY.md for the methodology."))

        root.addWidget(latency)

        # Process filters
        processes = QtWidgets.QGroupBox("Processes")
        process_layout = QtWidgets.QGridLayout(processes)
        process_layout.addWidget(QtWidgets.QLabel("Include (one per line):"), 0, 0)
        process_layout.addWidget(QtWidgets.QLabel("Exclude (one per line):"), 0, 1)
        self.include = QtWidgets.QPlainTextEdit("\n".join(self.config["include"]))
        self.exclude = QtWidgets.QPlainTextEdit("\n".join(self.config["exclude"]))
        for edit in (self.include, self.exclude):
            edit.setPlaceholderText("empty = all processes")
            edit.setMinimumHeight(80)
            edit.setMaximumHeight(110)
        process_layout.addWidget(self.include, 1, 0)
        process_layout.addWidget(self.exclude, 1, 1)
        process_layout.addWidget(hint(
            "Case-insensitive; * and ? wildcards. Wine/Proton games use their "
            ".exe name. Empty include = all; exclude always wins."
        ), 2, 0, 1, 2)
        root.addWidget(processes)

        # Video profile
        video = QtWidgets.QGroupBox("Video profile")
        video_layout = QtWidgets.QGridLayout(video)
        video_layout.addWidget(QtWidgets.QLabel("Players (one per line):"), 0, 0, 1, 2)
        self.video_processes = QtWidgets.QPlainTextEdit(
            "\n".join(self.config["video_processes"]))
        self.video_processes.setPlaceholderText("empty = video profile disabled")
        self.video_processes.setMinimumHeight(56)
        self.video_processes.setMaximumHeight(70)
        video_layout.addWidget(self.video_processes, 1, 0, 1, 2)

        self.video_multiplier = QtWidgets.QComboBox()
        self.video_multiplier.setSizePolicy(
            QtWidgets.QSizePolicy.Expanding, QtWidgets.QSizePolicy.Fixed)
        for value in (2, 3, 4):
            self.video_multiplier.addItem(f"{value}x", value)
        index = self.video_multiplier.findData(self.config["video_multiplier"])
        self.video_multiplier.setCurrentIndex(index if index >= 0 else 2)
        video_layout.addWidget(QtWidgets.QLabel("Multiplier:"), 2, 0)
        video_layout.addWidget(self.video_multiplier, 2, 1)

        self.video_target_fps = QtWidgets.QSpinBox()
        self.video_target_fps.setSizePolicy(
            QtWidgets.QSizePolicy.Expanding, QtWidgets.QSizePolicy.Fixed)
        self.video_target_fps.setRange(0, 1000)
        self.video_target_fps.setSingleStep(5)
        self.video_target_fps.setSpecialValueText("Always generate")
        self.video_target_fps.setSuffix(" fps")
        self.video_target_fps.setValue(self.config["video_target_fps"])
        video_layout.addWidget(QtWidgets.QLabel("Target FPS:"), 3, 0)
        video_layout.addWidget(self.video_target_fps, 3, 1)

        self.video_performance = QtWidgets.QCheckBox(
            "Performance mode (half-resolution flow)")
        self.video_performance.setChecked(self.config["video_performance_mode"])
        video_layout.addWidget(self.video_performance, 4, 0, 1, 2)

        self.video_safe = QtWidgets.QCheckBox(
            "Scene-cut guard (show the current frame on a hard cut)")
        self.video_safe.setChecked(self.config["video_safe"])
        self.video_safe.setToolTip(
            "Detect unrelated frames and show the current one instead of "
            "blending across a hard cut. Small per-frame cost."
        )
        video_layout.addWidget(self.video_safe, 5, 0, 1, 2)
        video_layout.addWidget(hint(
            "Overrides multiplier, target and performance mode for these "
            "players. Requires a Vulkan-presenting player (mpv/gpu-next)."
        ), 6, 0, 1, 2)
        root.addWidget(video)

        # Status and buttons
        self.status = QtWidgets.QLabel("")
        self.status.setWordWrap(True)
        root.addWidget(self.status)

        buttons = QtWidgets.QHBoxLayout()
        buttons.setSpacing(8)
        reload_button = QtWidgets.QPushButton("Reload")
        reload_button.clicked.connect(self.reload)
        validate_button = QtWidgets.QPushButton("Validate")
        validate_button.clicked.connect(self.validate)
        show_button = QtWidgets.QPushButton("Show config")
        show_button.clicked.connect(self.show_config)
        open_button = QtWidgets.QPushButton("Open folder")
        open_button.clicked.connect(self.open_config_folder)
        save_button = QtWidgets.QPushButton("Save")
        save_button.setDefault(True)
        save_button.clicked.connect(self.save)
        buttons.addWidget(reload_button)
        buttons.addWidget(validate_button)
        buttons.addWidget(show_button)
        buttons.addWidget(open_button)
        buttons.addStretch(1)
        buttons.addWidget(save_button)
        root.addLayout(buttons)

        root.addStretch(1)
        self._wrap_tooltips()

        # Live state
        for signal in (
            self.enabled.toggled,
            self.cross_gpu.toggled,
            self.performance.toggled,
            self.generator.currentIndexChanged,
            self.pacing.currentIndexChanged,
            self.target_fps.valueChanged,
            self.max_presented_spin.valueChanged,
            self.multiplier.currentIndexChanged,
            self.gpu.currentIndexChanged,
            self.gpu.editTextChanged,
            self.video_multiplier.currentIndexChanged,
            self.video_safe.toggled,
        ):
            signal.connect(self._update_summary)
        self.video_processes.textChanged.connect(self._update_summary)
        self.target_fps.valueChanged.connect(self._update_target_note)
        self.max_presented.currentIndexChanged.connect(self._update_cap_state)
        self.pacing.currentIndexChanged.connect(self._update_target_note)
        self._update_target_note()
        self._update_summary()

    def _wrap_tooltips(self) -> None:
        for widget in self.findChildren(QtWidgets.QWidget):
            text = widget.toolTip()
            if text:
                widget.setToolTip(wrap_tooltip(text))

    def _update_cap_state(self) -> None:
        self.max_presented_spin.setEnabled(self.max_presented.currentData() == 1)
        self._update_target_note()
        self._update_summary()

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

    def _apply_preset(self, name: str) -> None:
        self.enabled.setChecked(True)
        self.cross_gpu.setChecked(False)
        self.multiplier.setCurrentIndex(self.multiplier.findData(2))
        self.generator.setCurrentIndex(self.generator.findData("ffx"))
        self.max_presented.setCurrentIndex(self.max_presented.findData(0))
        self.pacing.setCurrentIndex(self.pacing.findData("adaptive"))
        if name == "video":
            self.target_fps.setValue(0)
            self.performance.setChecked(False)
            if not self.video_processes.toPlainText().strip():
                self.video_processes.setPlainText("mpv")
            self.video_multiplier.setCurrentIndex(
                self.video_multiplier.findData(4))
            self.video_target_fps.setValue(0)
            self.video_performance.setChecked(False)
            self.video_safe.setChecked(True)
            self._set_status(
                "Video profile set to 4x with the scene-cut guard. "
                "Add the player's process name if it is not mpv.")
            self._update_summary()
            return
        if name == "latency":
            self.target_fps.setValue(0)
            self.max_presented.setCurrentIndex(
                self.max_presented.findData(-1))
            self.pacing.setCurrentIndex(self.pacing.findData("capped"))
            self.performance.setChecked(False)
        elif name == "quality":
            self.target_fps.setValue(0)
            self.performance.setChecked(False)
        else:
            self.target_fps.setValue(0)
            self.performance.setChecked(True)
        self._set_status("Preset applied. Press Save to write it.")
        self._update_summary()

    def _selected_gpu(self) -> str:
        data = self.gpu.currentData()
        gpu = str(data) if data not in (None, "") else self.gpu.currentText().strip()
        if gpu.lower().startswith("auto"):
            return "auto"
        return gpu or "auto"

    def _update_summary(self) -> None:
        if not self.enabled.isChecked():
            text = "Frame generation is disabled for matching processes."
            color = "#9aa0a6"
        else:
            multiplier = int(self.multiplier.currentData())
            gpu = self._selected_gpu()
            gpu_text = "application GPU" if gpu == "auto" else gpu
            cross = "second GPU" if self.cross_gpu.isChecked() else "same GPU"
            target = int(self.target_fps.value())
            cap = int(self.max_presented.currentData())
            pacing = self.pacing.currentData()
            pacing_text = "capped source" if pacing == "capped" else "adaptive"
            if target == 0:
                target_text = "auto source cap" if cap else "always generating"
            else:
                target_text = f"source < {target} fps"
            if cap == -1:
                cap_text = " · auto presented cap"
            elif cap == 1:
                cap_text = f" · ≤{self.max_presented_spin.value()} presented fps"
            else:
                cap_text = ""
            gen = str(self.generator.currentData())
            gen_text = {"ffx": "FFX", "flow": "flow", "blend": "blend",
                        "hold": "hold", "off": "off"}.get(gen, gen)
            if gen in ("ffx", "flow"):
                quality = ("performance" if self.performance.isChecked()
                           else "quality")
                gen_text += f" {quality}"
            text = (f"{multiplier}x · {cross} · {gpu_text} · {pacing_text} · "
                    f"{target_text}{cap_text} · {gen_text}")
            video_players = [line.strip() for line in
                             self.video_processes.toPlainText().splitlines() if line.strip()]
            if video_players:
                text += f"  ·  video: {self.video_multiplier.currentData()}x"
                if self.video_safe.isChecked():
                    text += " + cut guard"
            color = "#7ec699"
        self.summary.setText(text)
        self.summary.setStyleSheet(
            f"font-size:12px; font-weight:600; color:{color}; "
            "padding:8px 10px; background:#1d2630; border:1px solid #2e4359; "
            "border-radius:6px;")

    def _update_target_note(self) -> None:
        if self.pacing.currentData() == "capped":
            self.target_note.setText(
                "Starts immediately. Cap the game at refresh ÷ multiplier "
                "(~82 fps for 2x, ~41 for 4x on 165 Hz).")
            return
        cap = int(self.max_presented.currentData())
        if cap == -1:
            self.target_note.setText(
                "Stops when source fps × multiplier exceeds the detected "
                "display refresh. Lowest latency on a fixed-refresh display.")
            return
        if cap == 1:
            self.target_note.setText(
                f"Stops above {self.max_presented_spin.value()} presented fps; "
                "Target FPS is a second limit.")
            return
        target = int(self.target_fps.value())
        if target == 0:
            self.target_note.setText(
                "Generates on every real frame; each multiplier adds display "
                "delay.")
        else:
            stop = round(target * 1.05)
            self.target_note.setText(
                f"Starts after a sustained drop below {target} fps and stops "
                f"above {stop} fps. Use refresh ÷ multiplier.")

    def _collect(self) -> dict:
        return {
            "enabled": self.enabled.isChecked(),
            "gpu": self._selected_gpu(),
            "cross_gpu": self.cross_gpu.isChecked(),
            "multiplier": int(self.multiplier.currentData()),
            "target_fps": int(self.target_fps.value()),
            "max_presented_fps": (
                int(self.max_presented_spin.value())
                if self.max_presented.currentData() == 1
                else int(self.max_presented.currentData())),
            "performance_mode": self.performance.isChecked(),
            "generator": self.generator.currentData(),
            "pacing": self.pacing.currentData(),
            "present_timing": self.present_timing.isChecked(),
            "debug": self.debug.isChecked(),
            "include": [line.strip() for line in self.include.toPlainText().splitlines()
                        if line.strip()],
            "exclude": [line.strip() for line in self.exclude.toPlainText().splitlines()
                        if line.strip()],
            "video_processes": [line.strip() for line in
                                self.video_processes.toPlainText().splitlines()
                                if line.strip()],
            "video_multiplier": int(self.video_multiplier.currentData()),
            "video_target_fps": int(self.video_target_fps.value()),
            "video_performance_mode": self.video_performance.isChecked(),
            "video_safe": self.video_safe.isChecked(),
        }

    def _validate_widgets(self, config: dict) -> bool:
        if config["cross_gpu"] and config["gpu"] == "auto":
            self._set_status("Cross-GPU needs an explicit frame-generation GPU.", True)
            return False
        if config["target_fps"] != 0 and config["target_fps"] < 20:
            self._set_status("Target FPS must be 0 (always) or at least 20.", True)
            return False
        if config["video_target_fps"] != 0 and config["video_target_fps"] < 20:
            self._set_status("Video target FPS must be 0 (always) or at least 20.", True)
            return False
        return True

    def _set_status(self, text: str, error: bool = False) -> None:
        color = "#e06c75" if error else "#7ec699"
        self.status.setStyleSheet(
            f"color:{color}; padding:7px 9px; background:#1a1f26; "
            "border:1px solid #2b333d; border-radius:6px;")
        self.status.setText(text)

    def save(self) -> None:
        config = self._collect()
        if not self._validate_widgets(config):
            return
        try:
            save_config(config)
        except OSError as error:
            self._set_status(f"Cannot write {config_path()}: {error}", True)
            return
        self.config = config
        ok, message = run_cli_validate()
        if ok:
            self._set_status(f"Saved {short_path(config_path())}. {message}", False)
        else:
            self._set_status(message, True)

    def run_gpu_probe(self) -> None:
        probe = probe_path()
        if not probe:
            self._set_status(
                "openfg-gpu-probe not found; build or install it first.", True)
            return
        try:
            proc = subprocess.run(
                [probe], capture_output=True, text=True, timeout=60)
        except Exception as error:
            self._set_status(f"Cannot run {probe}: {error}", True)
            return
        text = (proc.stdout or "") + ("\n" + proc.stderr if proc.stderr else "")
        dialog = QtWidgets.QDialog(self)
        dialog.setWindowTitle("openfg GPU probe")
        dialog.resize(820, 520)
        layout = QtWidgets.QVBoxLayout(dialog)
        edit = QtWidgets.QPlainTextEdit(text)
        edit.setReadOnly(True)
        edit.setFont(QtGui.QFontDatabase.systemFont(
            QtGui.QFontDatabase.FixedFont))
        layout.addWidget(edit)
        buttons = QtWidgets.QDialogButtonBox(QtWidgets.QDialogButtonBox.Close)
        copy = QtWidgets.QPushButton("Copy")
        copy.clicked.connect(
            lambda: QtWidgets.QApplication.clipboard().setText(text))
        buttons.addButton(copy, QtWidgets.QDialogButtonBox.ActionRole)
        buttons.rejected.connect(dialog.reject)
        layout.addWidget(buttons)
        dialog.exec()

    def validate(self) -> None:
        config = self._collect()
        if not self._validate_widgets(config):
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

    def show_config(self) -> None:
        cli = cli_path()
        if not cli:
            self._set_status("openfg-cli not found; cannot show the effective config.", True)
            return
        proc = subprocess.run(
            [cli, "show"],
            capture_output=True,
            text=True,
        )
        text = ((proc.stdout or "") + (proc.stderr or "")).strip()
        dialog = QtWidgets.QDialog(self)
        dialog.setWindowTitle("Effective openfg configuration")
        dialog.resize(720, 520)
        layout = QtWidgets.QVBoxLayout(dialog)
        viewer = QtWidgets.QPlainTextEdit(text)
        viewer.setReadOnly(True)
        viewer.setLineWrapMode(QtWidgets.QPlainTextEdit.NoWrap)
        viewer.setFont(QtGui.QFontDatabase.systemFont(
            QtGui.QFontDatabase.FixedFont))
        layout.addWidget(viewer)
        close = QtWidgets.QPushButton("Close")
        close.clicked.connect(dialog.accept)
        row = QtWidgets.QHBoxLayout()
        row.addStretch(1)
        row.addWidget(close)
        layout.addLayout(row)
        dialog.exec()

    def open_config_folder(self) -> None:
        path = config_path()
        try:
            path.parent.mkdir(parents=True, exist_ok=True)
        except OSError as error:
            self._set_status(f"Cannot create {path.parent}: {error}", True)
            return
        QtGui.QDesktopServices.openUrl(
            QtCore.QUrl.fromLocalFile(str(path.parent)))

    def reload(self) -> None:
        self.config = load_config()
        self.enabled.setChecked(self.config["enabled"])
        self.cross_gpu.setChecked(self.config["cross_gpu"])
        self._select_gpu(self.config["gpu"])
        self.multiplier.setCurrentIndex(
            max(0, self.multiplier.findData(self.config["multiplier"])))
        self.generator.setCurrentIndex(
            max(0, self.generator.findData(self.config["generator"])))
        self.pacing.setCurrentIndex(
            max(0, self.pacing.findData(self.config["pacing"])))
        self.target_fps.setSizePolicy(
            QtWidgets.QSizePolicy.Expanding, QtWidgets.QSizePolicy.Fixed)
        self.target_fps.setValue(self.config["target_fps"])
        self.performance.setChecked(self.config["performance_mode"])
        self.present_timing.setChecked(self.config["present_timing"])
        self.debug.setChecked(self.config["debug"])
        self.max_presented_spin.setValue(
            self.config["max_presented_fps"]
            if self.config["max_presented_fps"] >= 20 else 165)
        if self.config["max_presented_fps"] == -1:
            self.max_presented.setCurrentIndex(1)
        elif self.config["max_presented_fps"] >= 20:
            self.max_presented.setCurrentIndex(2)
        else:
            self.max_presented.setCurrentIndex(0)
        self.include.setPlainText("\n".join(self.config["include"]))
        self.exclude.setPlainText("\n".join(self.config["exclude"]))
        self.video_processes.setPlainText(
            "\n".join(self.config["video_processes"]))
        self.video_multiplier.setCurrentIndex(
            max(0, self.video_multiplier.findData(
                self.config["video_multiplier"])))
        self.video_target_fps.setValue(self.config["video_target_fps"])
        self.video_performance.setChecked(
            self.config["video_performance_mode"])
        self.video_safe.setChecked(self.config["video_safe"])
        self._update_cap_state()
        self._set_status(f"Reloaded {short_path(config_path())}.")

def main() -> int:
    app = QtWidgets.QApplication(sys.argv)
    app.setStyle("Fusion")
    app.setStyleSheet(APP_STYLE)
    app.setApplicationName("openfg-gui")
    app.setApplicationDisplayName(APP_NAME)
    icon = QtGui.QIcon.fromTheme("openfg")
    if icon.isNull():
        icon_path = repo_root() / "gui" / "openfg.svg"
        if icon_path.is_file():
            icon = QtGui.QIcon(str(icon_path))
    app.setWindowIcon(icon)
    window = MainWindow()
    window.show()
    return app.exec()


if __name__ == "__main__":
    sys.exit(main())
