#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Build and install openfg into a per-user prefix (~/.local by default).
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
build="${OPENFG_BUILD_DIR:-$repo/build}"
prefix="${OPENFG_PREFIX:-$HOME/.local}"

cmake -S "$repo" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$build" -j"$(nproc)"
cmake --install "$build" --prefix "$prefix"

config="${OPENFG_CONFIG:-${XDG_CONFIG_HOME:-$HOME/.config}/openfg/conf.toml}"
if [ ! -f "$config" ]; then
    "$prefix/bin/openfg-cli" init
    echo
    echo "Frame generation is disabled in the new config. Edit $config or run"
    echo "  $prefix/bin/openfg-cli show"
    echo "then set enabled = true."
fi

echo
echo "Installed openfg:"
echo "  layer    : $prefix/lib/libopenfg-layer.so"
echo "  manifest : $prefix/share/vulkan/implicit_layer.d/VkLayer_OPENFG_frame_generation.json"
echo "  cli      : $prefix/bin/openfg-cli"
echo "  probe    : $prefix/bin/openfg-gpu-probe"
echo "  config   : $config"
echo
echo "The layer is loaded by every Vulkan application; the config decides"
echo "whether it is active. Use DISABLE_OPENFG=1 to bypass it for one process."
