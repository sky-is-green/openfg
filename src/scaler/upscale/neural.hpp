/* SPDX-License-Identifier: MIT */
#pragma once

#include "scaler/scaler.hpp"
#include "scaler/vulkan.hpp"

#include <memory>
#include <string>

namespace openfg::scaler {

/// Spatial 2x neural upscaler.
///
/// A .ofg file (produced by tools/model-ofg.py) runs on the zero-copy Vulkan
/// backend and records its inference in the caller's command buffer. A
/// .param/.bin pair selects the optional ncnn validation backend when ncnn
/// was available at build time.
///
/// The v0 package is a 20-channel, 3-block residual CNN with a PixelShuffle
/// head. It is a 2x model: the output extent must be exactly twice the input
/// extent.
class NeuralUpscaler final : public Upscaler {
public:
    NeuralUpscaler(Runtime& runtime, std::string modelPath);
    ~NeuralUpscaler() override;

    void resize(vk::Extent2D input, vk::Extent2D output) override;
    bool prepare(
        Runtime& runtime,
        const Frame& frame,
        std::string& error
    ) override;
    void record(
        vk::CommandBuffer command,
        vk::ImageView input,
        vk::Extent2D inputExtent
    ) override;
    vk::ImageView view() const override;
    vk::Extent2D outputExtent() const override;

    /// True when the backend records inference in the presenter command
    /// buffer, so its latency comes from GPU timestamps.
    bool gpuTimed() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace openfg::scaler
