/* SPDX-License-Identifier: MIT */
#pragma once

#include "pipeline.hpp"
#include "presenter.hpp"

#include <vector>

struct SDL_Window;

namespace openfg::scaler {

class SdlPresenter final : public Presenter {
public:
    explicit SdlPresenter(PresentOptions options);
    ~SdlPresenter() override;

    bool wantsSurface() const override { return true; }
    std::vector<const char*> instanceExtensions() const override;
    bool createSurface(vk::Instance instance, vk::SurfaceKHR& surface) override;
    bool init(Runtime& runtime, std::string& error) override;

    bool acquire(PresentTarget& target, std::string& error) override;
    void record(
        Runtime& runtime,
        vk::CommandBuffer command,
        vk::ImageView source,
        vk::Extent2D sourceExtent
    ) override;
    bool submit(vk::CommandBuffer command, std::string& error) override;
    void pollEvents() override;
    bool shouldClose() const override { return close_; }
    bool saveFrame(const std::string& path, std::string& error) override;
    vk::Extent2D size() const override { return extent_; }

private:
    void destroySwapchain();
    bool createSwapchain(std::string& error);
    bool createFramebuffers(std::string& error);
    bool recreateSwapchain(std::string& error);

    PresentOptions options_;
    Runtime* runtime_{};
    bool close_{false};
    bool resizePending_{false};
    SDL_Window* window_{};
    vk::SurfaceKHR surface_{};
    vk::UniqueSwapchainKHR swapchain_;
    vk::Format format_{vk::Format::eUndefined};
    vk::Extent2D extent_{};
    std::vector<vk::Image> images_;
    std::vector<vk::UniqueImageView> views_;
    std::vector<vk::UniqueFramebuffer> framebuffers_;
    uint32_t imageIndex_{0};
    bool pipelineReady_{false};
    PresentPipeline pipeline_;
    vk::UniqueSemaphore available_;
    vk::UniqueSemaphore renderFinished_;
    vk::UniqueFence fence_;
};

} // namespace openfg::scaler
