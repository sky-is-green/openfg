/* SPDX-License-Identifier: MIT */
#pragma once

#include <vk>

#include <cstdint>
#include <string>

namespace openfg::scaler {

/// One captured frame. The image is owned by the capture source and is
/// valid until the next call to CaptureSource::next().
struct Frame {
    vk::Image image{};
    vk::ImageView view{};
    vk::Extent2D extent{};
    vk::Format format{vk::Format::eUndefined};
    uint64_t sequence{};
    uint64_t timestampNs{};
};

/// Capture backends produce frames at the game/render resolution. The
/// upscaler and presenter do not know which backend is in use.
class CaptureSource {
public:
    virtual ~CaptureSource() = default;
    virtual bool start(std::string& error) = 0;

    /// Wait for the next frame. Returns false with an error on fatal failure
    /// or when the source has been stopped.
    virtual bool next(Frame& frame, std::string& error) = 0;
    virtual void stop() = 0;

    /// Human-readable description used in logs (for example
    /// "pipewire monitor 1280x720 BGRA").
    virtual std::string description() const = 0;
};

/// Spatial upscaler. It consumes a sampled image in
/// VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL and leaves its output in the same
/// layout so the presenter can sample it.
class Upscaler {
public:
    virtual ~Upscaler() = default;
    virtual void resize(vk::Extent2D input, vk::Extent2D output) = 0;
    virtual void record(
        vk::CommandBuffer command,
        vk::ImageView input,
        vk::Extent2D inputExtent
    ) = 0;
    virtual vk::ImageView view() const = 0;
    virtual vk::Extent2D outputExtent() const = 0;
};

} // namespace openfg::scaler
