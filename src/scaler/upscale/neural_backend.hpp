/* SPDX-License-Identifier: MIT */
#pragma once

#include "scaler/scaler.hpp"
#include "scaler/vulkan.hpp"

#include <memory>
#include <string>

namespace openfg::scaler {

/// Backend interface for the optional neural upscaler.
///
/// The Vulkan backend records all work into the caller's command buffer, so
/// inference is zero-copy and runs on the same queue as presentation. The
/// ncnn backend reads the frame back to the CPU and uploads its result.
class NeuralBackend {
public:
    virtual ~NeuralBackend() = default;

    virtual void resize(vk::Extent2D input, vk::Extent2D output) = 0;
    virtual bool prepare(
        Runtime& runtime,
        const Frame& frame,
        std::string& error
    ) = 0;
    virtual void record(
        vk::CommandBuffer command,
        vk::ImageView input,
        vk::Extent2D inputExtent
    ) = 0;
    virtual vk::ImageView view() const = 0;
    virtual vk::Extent2D outputExtent() const = 0;

    /// True when record() performs the inference in the caller's command
    /// buffer, so the main loop should use GPU timestamps for its latency.
    virtual bool gpuTimed() const { return false; }
};

/// Zero-copy Vulkan runtime for the v0 .ofg weight format.
std::unique_ptr<NeuralBackend> createVulkanNeuralBackend(
    Runtime& runtime,
    const std::string& modelPath);

/// Optional ncnn validation backend for .param/.bin pairs.
/// Returns nullptr and fills error when ncnn support was not compiled in.
std::unique_ptr<NeuralBackend> createNcnnNeuralBackend(
    Runtime& runtime,
    const std::string& modelPath,
    std::string& error);

} // namespace openfg::scaler
