/* SPDX-License-Identifier: MIT */
#pragma once

#include <string>
#include <vector>

#include <cstdint>

namespace openfg::scaler {

struct NcnnModel;

NcnnModel* loadNcnnModel(
    const std::string& paramPath,
    std::string& error
);

void destroyNcnnModel(NcnnModel* model);

/// Run the 2x model on tightly packed 8-bit pixels and write tightly packed
/// RGBA8 output. The caller owns the frame buffers.
bool runNcnnModel(
    NcnnModel& model,
    int width,
    int height,
    const uint8_t* pixels,
    bool bgr,
    int& outputWidth,
    int& outputHeight,
    std::vector<uint8_t>& output,
    std::string& error
);

} // namespace openfg::scaler
