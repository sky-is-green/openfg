/* SPDX-License-Identifier: MIT */
#pragma once

#include "pipeline.hpp"
#include "presenter.hpp"

namespace openfg::scaler {

class HeadlessPresenter final : public Presenter {
public:
    explicit HeadlessPresenter(PresentOptions options);
    ~HeadlessPresenter() override;

    bool wantsSurface() const override { return false; }
    std::vector<const char*> instanceExtensions() const override { return {}; }
    bool createSurface(vk::Instance, vk::SurfaceKHR&) override { return false; }
    bool init(Runtime& runtime, std::string& error) override;

    bool acquire(PresentTarget& target, std::string& error) override;
    void record(
        Runtime& runtime,
        vk::CommandBuffer command,
        vk::ImageView source,
        vk::Extent2D sourceExtent
    ) override;
    bool submit(vk::CommandBuffer command, std::string& error) override;
    void pollEvents() override {}
    bool shouldClose() const override { return false; }
    bool saveFrame(const std::string& path, std::string& error) override;
    vk::Extent2D size() const override { return options_.size; }

private:
    PresentOptions options_;
    Runtime* runtime_{};
    PresentPipeline pipeline_;
    vk::UniqueImage image_;
    vk::UniqueDeviceMemory memory_;
    vk::UniqueImageView view_;
    vk::UniqueFramebuffer framebuffer_;
    vk::UniqueFence fence_;
};

} // namespace openfg::scaler
