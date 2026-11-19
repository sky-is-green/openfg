/* SPDX-License-Identifier: MIT */
#pragma once

#include "scaler/scaler.hpp"
#include "scaler/vulkan.hpp"

#include <memory>
#include <string>

namespace openfg::scaler {

/// Capture a monitor or window through the desktop ScreenCast portal and
/// PipeWire. The stream is imported as a DMA-BUF when the producer supports
/// it; a shared-memory fallback copies the frame through a staging buffer.
class PipeWireSource final : public CaptureSource {
public:
    PipeWireSource(
        Runtime& runtime,
        vk::Extent2D wanted,
        std::string capture
    );
    ~PipeWireSource() override;

    bool start(std::string& error) override;
    bool next(Frame& frame, std::string& error) override;
    void stop() override;
    std::string description() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace openfg::scaler
