/* SPDX-License-Identifier: MIT */
#pragma once

#include <string>

namespace openfg_layer {

/// Write the current layer state for the tray indicator.
///
/// Best effort: a missing runtime/state directory or a failed write is
/// ignored. The file is replaced atomically.
void writeStatus(const std::string& state, unsigned multiplier,
                 double appFps, double presentedFps, double overheadMs);

} // namespace openfg_layer
