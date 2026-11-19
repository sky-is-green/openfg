/* SPDX-License-Identifier: MIT */
#pragma once

#include "scaler/scaler.hpp"
#include "scaler/vulkan.hpp"

#include <cstdint>
#include <string>

namespace openfg::scaler {

/// Deterministic moving test pattern. It exercises hard edges, gradients and
/// fine detail so the upscaler and presenter can be tested without a game or
/// a screen-capture permission.
class TestPatternSource final : public CaptureSource {
public:
    TestPatternSource(
        Runtime& runtime,
        vk::Extent2D extent,
        bool animate
    );
    ~TestPatternSource() override;

    bool start(std::string& error) override;
    bool next(Frame& frame, std::string& error) override;
    void stop() override;
    std::string description() const override;

private:
    void fill(uint64_t frame);
    bool upload(std::string& error);
    void createResources();

    Runtime& runtime_;
    vk::Extent2D extent_;
    bool animate_{true};
    bool initialized_{false};
    uint64_t sequence_{0};

    vk::UniqueImage image_;
    vk::UniqueDeviceMemory memory_;
    vk::UniqueImageView view_;
    vk::UniqueBuffer staging_;
    vk::UniqueDeviceMemory stagingMemory_;
    void* mapped_{};
    vk::DeviceSize stagingSize_{};
    vk::UniqueCommandPool commandPool_;
    vk::UniqueCommandBuffer command_;
    vk::UniqueFence fence_;
};

} // namespace openfg::scaler
