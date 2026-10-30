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
        "performance_mode": False,
        "generator": "flow",
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
        for key in ("multiplier", "target_fps"):
            try:
                config[key] = int(section.get(key, config[key]))
            except (TypeError, ValueError):
                pass
        config["performance_mode"] = bool(
            section.get("performance_mode", config["performance_mode"]))
        config["generator"] = str(section.get("generator", config["generator"]))
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
performance_mode = {'true' if config['performance_mode'] else 'false'}
generator = {quote(config['generator'])}
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
        self.resize(820, 960)
        self.gpus = enumerate_gpus()
        self.config = load_config()

        central = QtWidgets.QWidget()
        root = QtWidgets.QVBoxLayout(central)
        root.setSpacing(10)
        scroll = QtWidgets.QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setFrameShape(QtWidgets.QFrame.NoFrame)
        scroll.setWidget(central)
        self.setCentralWidget(scroll)

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

        # Presets
        presets = QtWidgets.QGroupBox("Presets")
        preset_layout = QtWidgets.QHBoxLayout(presets)
        preset_layout.setSpacing(6)

        def preset(text, tooltip, callback):
            button = QtWidgets.QPushButton(text)
            button.setToolTip(tooltip)
            button.clicked.connect(callback)
            return button

        preset_layout.addWidget(preset(
            "Latency-friendly",
            "2x, same GPU, generate only below half the display refresh, "
            "quality flow. This keeps native input latency when the game is "
            "already fast enough.",
            lambda: self._apply_preset("latency")))
        preset_layout.addWidget(preset(
            "Always generate · quality",
            "2x, generate on every real frame with the full-resolution flow.",
            lambda: self._apply_preset("quality")))
        preset_layout.addWidget(preset(
            "Always generate · performance",
            "2x, generate on every real frame with the half-resolution flow. "
            "About 15% faster interpolation, slightly softer silhouettes.",
            lambda: self._apply_preset("performance")))
        preset_layout.addWidget(preset(
            "Video · 4x",
            "4x, always generate, quality flow. Use this with a Vulkan video "
            "player (mpv/gpu-next); the preset adds mpv to the video profile.",
            lambda: self._apply_preset("video")))
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
            "The GPU that runs the interpolator. 'Automatic' uses the GPU the "
            "application renders on. For cross-GPU, pick the second card's PCI "
            "address (usually the one with no display attached)."
        )
        self._select_gpu(self.config["gpu"])
        gpu_row = QtWidgets.QHBoxLayout()
        gpu_row.addWidget(self.gpu, 1)
        probe_button = QtWidgets.QPushButton("Probe…")
        probe_button.setToolTip(
            "Run openfg-gpu-probe and show the Vulkan capabilities and "
            "same-GPU/cross-GPU backend result for this machine.")
        probe_button.clicked.connect(self.run_gpu_probe)
        gpu_row.addWidget(probe_button)
        form.addRow("Frame-generation GPU:", gpu_row)

        self.cross_gpu = QtWidgets.QCheckBox("Generate on a second GPU (DMA-BUF)")
        self.cross_gpu.setChecked(self.config["cross_gpu"])
        self.cross_gpu.setToolTip(
            "The backend runs on the selected GPU and shares frames with the "
            "display GPU through DMA-BUF. Adds two copies and cross-device "
            "semaphore round-trips; the Helldivers 2 test was slower than "
            "same-GPU on this machine."
        )
        self.cross_gpu.toggled.connect(self._cross_gpu_toggled)
        form.addRow("", self.cross_gpu)

        self.multiplier = QtWidgets.QComboBox()
        for value in (2, 3, 4):
            self.multiplier.addItem(
                f"{value}x  ({value - 1} generated frame"
                f"{'s' if value > 2 else ''} per real frame)", value)
        index = self.multiplier.findData(self.config["multiplier"])
        self.multiplier.setCurrentIndex(index if index >= 0 else 0)
        self.multiplier.setToolTip(
            "2x inserts one frame and has the lowest added delay. "
            "3x/4x insert more frames before the real one."
        )
        form.addRow("Multiplier:", self.multiplier)

        self.generator = QtWidgets.QComboBox()
        for value, label in (
            ("flow", "Optical flow (quality)"),
            ("blend", "Linear blend (no flow; fast baseline)"),
            ("hold", "Hold current frame (no interpolation)"),
            ("off", "Off / timing only"),
        ):
            self.generator.addItem(label, value)
        index = self.generator.findData(self.config["generator"])
        self.generator.setCurrentIndex(index if index >= 0 else 0)
        self.generator.setToolTip(
            "Image-based generator. blend and hold are comparison baselines; "
            "off keeps the layer loaded for present/latency timing only."
        )
        form.addRow("Generator:", self.generator)

        self.debug = QtWidgets.QCheckBox("Log generated frames to stderr")
        self.debug.setChecked(self.config["debug"])
        self.present_timing = QtWidgets.QCheckBox(
            "Present timing instrumentation (benchmarking)")
        self.present_timing.setChecked(self.config["present_timing"])
        self.present_timing.setToolTip(
            "Adds present IDs and waits for present completion so the log can "
            "report present latency percentiles. It can make the compositor "
            "pace presents; leave it off for normal play."
        )

        settings_layout.addLayout(form)
        settings_layout.addWidget(hint(
            "2x has the lowest latency. Generated frames are displayed before "
            "the current real frame, so every multiplier adds display delay."
        ))
        settings_layout.addWidget(self.debug)
        settings_layout.addWidget(self.present_timing)

        root.addWidget(settings)

        # Latency settings
        latency = QtWidgets.QGroupBox("Latency")
        latency_layout = QtWidgets.QVBoxLayout(latency)
        latency_form = QtWidgets.QFormLayout()
        latency_form.setFieldGrowthPolicy(QtWidgets.QFormLayout.ExpandingFieldsGrow)

        self.target_fps = QtWidgets.QSpinBox()
        self.target_fps.setRange(0, 1000)
        self.target_fps.setSingleStep(5)
        self.target_fps.setSpecialValueText("Always generate")
        self.target_fps.setSuffix(" fps")
        self.target_fps.setValue(self.config["target_fps"])
        self.target_fps.setToolTip(
            "Generate only while the estimated native rate is below this value. "
            "0 = always generate. Set near refresh / multiplier: about 82 for "
            "2x or 41 for 4x on a 165 Hz display. Generation starts off and "
            "enables only after a sustained drop."
        )
        self.target_note = hint("")
        latency_form.addRow("Target FPS:", self.target_fps)

        self.performance = QtWidgets.QCheckBox(
            "Performance mode (half-resolution flow, softer silhouettes)")
        self.performance.setChecked(self.config["performance_mode"])
        self.performance.setToolTip(
            "Skips the full-resolution flow refinement. About 15% faster at "
            "1440p with a 0.04 dB pan / 0.02 dB ghost PSNR change. Only matters "
            "while frames are being generated."
        )
        latency_form.addRow("", self.performance)

        self.summary = QtWidgets.QLabel("")
        self.summary.setWordWrap(True)
        self.summary.setStyleSheet(
            "font-size:12px; font-weight:600; color:#dfe3e7; "
            "padding:6px; background:#20242a; border-radius:4px;")

        latency_layout.addLayout(latency_form)
        latency_layout.addWidget(self.target_note)
        latency_layout.addWidget(hint(
            "Performance mode skips the full-resolution flow refinement: about "
            "15% faster at 1440p with a 0.04 dB pan / 0.02 dB ghost PSNR change. "
            "The target comparison subtracts the layer's copy/generate/present "
            "time, so it approximates the game's native rate."
        ))
        latency_layout.addWidget(hint(
            "Measured on the dual RX 7900 XT test system (Helldivers 2, 1440p): "
            "2x active ≈134 fps / 7.5 ms; adaptive off at target 120 ≈174 fps / "
            "5.7 ms (that was the old target; use refresh / multiplier for "
            "latency). Cross-GPU was slower than same-GPU on that system."
        ))
        latency_layout.addWidget(self.summary)

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
            edit.setMinimumHeight(90)
        process_layout.addWidget(self.include, 1, 0)
        process_layout.addWidget(self.exclude, 1, 1)
        process_layout.addWidget(hint(
            "Process names, case-insensitive. '*' and '?' wildcards are "
            "supported. Wine/Proton games are matched by their .exe name. "
            "An empty include list means every process; exclude always wins."
        ), 2, 0, 1, 2)
        root.addWidget(processes)

        # Video profile
        video = QtWidgets.QGroupBox("Video profile")
        video_layout = QtWidgets.QGridLayout(video)
        video_layout.addWidget(QtWidgets.QLabel("Players (one per line):"), 0, 0, 1, 2)
        self.video_processes = QtWidgets.QPlainTextEdit(
            "\n".join(self.config["video_processes"]))
        self.video_processes.setPlaceholderText("empty = video profile disabled")
        self.video_processes.setMinimumHeight(60)
        video_layout.addWidget(self.video_processes, 1, 0, 1, 2)

        self.video_multiplier = QtWidgets.QComboBox()
        for value in (2, 3, 4):
            self.video_multiplier.addItem(f"{value}x", value)
        index = self.video_multiplier.findData(self.config["video_multiplier"])
        self.video_multiplier.setCurrentIndex(index if index >= 0 else 2)
        video_layout.addWidget(QtWidgets.QLabel("Multiplier:"), 2, 0)
        video_layout.addWidget(self.video_multiplier, 2, 1)

        self.video_target_fps = QtWidgets.QSpinBox()
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
            "Adds one small reduction pass per frame pair. A global cut score "
            "detects unrelated frames and skips interpolation for that pair."
        )
        video_layout.addWidget(self.video_safe, 5, 0, 1, 2)
        video_layout.addWidget(hint(
            "Overrides multiplier, target FPS and performance mode for these "
            "players. Use 4x for 24 fps video; the cut guard is on by default. "
            "The player must present through Vulkan (mpv --vo=gpu-next)."
        ), 6, 0, 1, 2)
        root.addWidget(video)

        # Status and buttons
        self.status = QtWidgets.QLabel("")
        self.status.setWordWrap(True)
        root.addWidget(self.status)

        buttons = QtWidgets.QHBoxLayout()
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

        # Live state
        for signal in (
            self.enabled.toggled,
            self.cross_gpu.toggled,
            self.performance.toggled,
            self.target_fps.valueChanged,
            self.multiplier.currentIndexChanged,
            self.gpu.currentIndexChanged,
            self.gpu.editTextChanged,
            self.video_multiplier.currentIndexChanged,
            self.video_safe.toggled,
        ):
            signal.connect(self._update_summary)
        self.video_processes.textChanged.connect(self._update_summary)
        self.target_fps.valueChanged.connect(self._update_target_note)
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
        if name == "video":
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
            screen = QtWidgets.QApplication.primaryScreen()
            refresh = screen.refreshRate() if screen else 0.0
            self.target_fps.setValue(
                max(20, round(refresh / 2.0)) if refresh else 120)
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
            target_text = "always generating" if target == 0 else f"below {target} fps"
            flow = "performance flow" if self.performance.isChecked() else "quality flow"
            text = f"{multiplier}x · {cross} · {gpu_text} · {target_text} · {flow}"
            video_players = [line.strip() for line in
                             self.video_processes.toPlainText().splitlines() if line.strip()]
            if video_players:
                text += f"  ·  video: {self.video_multiplier.currentData()}x"
                if self.video_safe.isChecked():
                    text += " + cut guard"
            color = "#7ec699"
        self.summary.setText(text)
        self.summary.setStyleSheet(
            "font-size:12px; font-weight:600; color:%s; "
            "padding:6px; background:#20242a; border-radius:4px;" % color)

    def _update_target_note(self) -> None:
        target = int(self.target_fps.value())
        if target == 0:
            self.target_note.setText(
                "Generate on every real frame. This adds one or more displayed "
                "frames of delay, depending on the multiplier.")
        else:
            stop = round(target * 1.05)
            self.target_note.setText(
                f"Generation starts off. It enables after the estimated native "
                f"rate stays below {target} fps for one second, and stops after "
                f"it stays above {stop} fps (5% hysteresis). A game already "
                "faster than this never initializes the backend. Use "
                "refresh / multiplier: ~82 for 2x or ~41 for 4x on 165 Hz.")

    def _collect(self) -> dict:
        return {
            "enabled": self.enabled.isChecked(),
            "gpu": self._selected_gpu(),
            "cross_gpu": self.cross_gpu.isChecked(),
            "multiplier": int(self.multiplier.currentData()),
            "target_fps": int(self.target_fps.value()),
            "performance_mode": self.performance.isChecked(),
            "generator": self.generator.currentData(),
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
        self.status.setStyleSheet(f"color:{color};")
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
            self._set_status(f"Saved {config_path()}. {message}", False)
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
        index = self.multiplier.findData(self.config["multiplier"])
        self.multiplier.setCurrentIndex(index if index >= 0 else 0)
        self.target_fps.setValue(self.config["target_fps"])
        self.performance.setChecked(self.config["performance_mode"])
        self.debug.setChecked(self.config["debug"])
        self.include.setPlainText("\n".join(self.config["include"]))
        self.exclude.setPlainText("\n".join(self.config["exclude"]))
        self._update_target_note()
        self._update_summary()
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
