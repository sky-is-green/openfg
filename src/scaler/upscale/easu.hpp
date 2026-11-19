/* SPDX-License-Identifier: MIT */
#pragma once

#include "scaler/scaler.hpp"
#include "scaler/vulkan.hpp"

#include <array>
#include <cstdint>

namespace openfg::scaler {

/// FidelityFX Super Resolution 1.0 EASU, with optional RCAS sharpening.
class EasuUpscaler final : public Upscaler {
public:
    EasuUpscaler(
        Runtime& runtime,
        vk::Extent2D input,
        vk::Extent2D output,
        bool sharpening,
        float sharpness
    );
    ~EasuUpscaler() override;

    void resize(vk::Extent2D input, vk::Extent2D output) override;
    void record(
        vk::CommandBuffer command,
        vk::ImageView input,
        vk::Extent2D inputExtent
    ) override;
    vk::ImageView view() const override { return *outputView_; }
    vk::Extent2D outputExtent() const override { return outputExtent_; }

private:
    struct PushConstants {
        std::array<uint32_t, 4> c0{};
        std::array<uint32_t, 4> c1{};
        std::array<uint32_t, 4> c2{};
        std::array<uint32_t, 4> c3{};
        std::array<uint32_t, 4> sample{};
    };

    void createImages();
    void destroyImages();
    void updateEasuSet(vk::ImageView input);

    Runtime& runtime_;
    vk::Extent2D inputExtent_{};
    vk::Extent2D outputExtent_{};
    bool sharpening_{false};
    float sharpness_{0.0F};
    bool initialized_{false};

    vk::UniqueImage intermediateImage_;
    vk::UniqueDeviceMemory intermediateMemory_;
    vk::UniqueImageView intermediateView_;
    vk::UniqueImage outputImage_;
    vk::UniqueDeviceMemory outputMemory_;
    vk::UniqueImageView outputView_;

    vk::UniqueSampler sampler_;
    vk::UniqueDescriptorSetLayout setLayout_;
    vk::UniquePipelineLayout pipelineLayout_;
    vk::UniqueDescriptorPool easuPool_;
    vk::DescriptorSet easuSet_;
    vk::UniqueDescriptorPool rcasPool_;
    vk::DescriptorSet rcasSet_;
    vk::UniquePipeline easuPipeline_;
    vk::UniquePipeline rcasPipeline_;
};

} // namespace openfg::scaler
