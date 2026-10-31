/* SPDX-License-Identifier: MIT */

#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace openfg_layer {

/// Current mode of one active display.
struct DisplayInfo {
    double refreshHz{};
    uint32_t width{};
    uint32_t height{};
    /// Connector name from sysfs, e.g. "DP-2".
    std::string connector;
};

/// Best-effort display refresh detection for the frame-generation cap.
///
/// Reads the active connector/CRTC mode from DRM using sysfs and three ioctls,
/// so no libdrm dependency is needed. When `pciAddress` identifies a card,
/// only that card is scanned; otherwise every card is considered. A connector
/// whose mode matches `width`x`height` wins, otherwise the highest refresh
/// wins. `OPENFG_REFRESH` overrides the result (useful in sandboxes where the
/// DRM card nodes are not readable).
std::optional<DisplayInfo> queryDisplayRefresh(
    const std::string& pciAddress,
    uint32_t width,
    uint32_t height);

}
