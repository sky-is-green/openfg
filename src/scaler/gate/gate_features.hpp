/* SPDX-License-Identifier: MIT */
#pragma once

#include "scaler/vulkan.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace openfg::scaler {

struct GateTile {
    uint32_t x{};
    uint32_t y{};
    uint32_t width{};
    uint32_t height{};
};

/// Extract the 31 portable tile-gate features on the GPU.
///
/// Inputs are tightly packed RGBA8 images. The EASU image must be exactly
/// twice the low-resolution image. `features` is sized to
/// tiles.size() * 31 in feature order from tools/scaler-lab/GATE.md.
bool extractGateFeatures(
    Runtime& runtime,
    const uint8_t* lowRgba,
    uint32_t lowWidth,
    uint32_t lowHeight,
    const uint8_t* easuRgba,
    uint32_t highWidth,
    uint32_t highHeight,
    uint32_t tile,
    std::vector<float>& features,
    std::vector<GateTile>& tiles,
    std::string& error,
    double* gpuMilliseconds = nullptr
);

} // namespace openfg::scaler
