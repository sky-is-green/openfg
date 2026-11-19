/* SPDX-License-Identifier: MIT */
#pragma once

#include "scaler/scaler.hpp"

namespace openfg::scaler {

/// Present the captured frame without spatial upscaling. This is a reference
/// mode for native/A-B comparisons and latency measurements, not a product
/// upscaler. The presenter still does the final letterbox/scale to its target.
class PassthroughUpscaler final : public Upscaler {
public:
    void resize(vk::Extent2D input, vk::Extent2D) override
    {
        input_ = input;
    }

    void record(
        vk::CommandBuffer,
        vk::ImageView input,
        vk::Extent2D
    ) override
    {
        view_ = input;
    }

    vk::ImageView view() const override { return view_; }
    vk::Extent2D outputExtent() const override { return input_; }

private:
    vk::Extent2D input_{};
    vk::ImageView view_{};
};

} // namespace openfg::scaler
