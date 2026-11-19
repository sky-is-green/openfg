/* SPDX-License-Identifier: MIT */
#pragma once

#include "scaler/vulkan.hpp"

#include <vk>

namespace openfg::scaler {

/// Fullscreen present pass shared by the windowed and headless presenters.
/// It samples the upscaler output, keeps the aspect ratio with black bars and
/// clears the target every frame.
class PresentPipeline {
public:
    void init(
        Runtime& runtime,
        vk::Format colorFormat,
        vk::ImageLayout finalLayout
    );
    void record(
        Runtime& runtime,
        vk::CommandBuffer command,
        vk::Framebuffer framebuffer,
        vk::Extent2D targetExtent,
        vk::ImageView source,
        vk::Extent2D sourceExtent
    );
    void destroy();
    vk::RenderPass renderPass() const { return *renderPass_; }

private:
    Runtime* runtime_{};
    vk::UniqueRenderPass renderPass_;
    vk::UniqueDescriptorSetLayout setLayout_;
    vk::UniquePipelineLayout pipelineLayout_;
    vk::UniquePipeline pipeline_;
    vk::UniqueSampler sampler_;
    vk::UniqueDescriptorPool pool_;
    vk::DescriptorSet set_;
};

} // namespace openfg::scaler
