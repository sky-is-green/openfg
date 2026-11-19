/* SPDX-License-Identifier: MIT */
#pragma once

#include "scaler/scaler.hpp"
#include "scaler/vulkan.hpp"

#include <memory>
#include <string>

namespace openfg::scaler {

/// Spatial 2x neural upscaler backed by an ncnn Vulkan model.
///
/// The v0 package is a 20-channel, 3-block residual CNN with a PixelShuffle
/// head. This class accepts the ncnn .param/.bin pair produced by
/// tools/model-ncnn.py. It is a 2x model: the output extent must be exactly
/// twice the input extent.
class NeuralUpscaler final : public Upscaler {
public:
    NeuralUpscaler(Runtime& runtime, std::string paramPath);
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

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace openfg::scaler
