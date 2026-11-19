/* SPDX-License-Identifier: MIT */
#pragma once

#include "scaler/vulkan.hpp"

#include <vk>

#include <memory>
#include <string>
#include <vector>

namespace openfg::scaler {

struct PresentOptions {
    vk::Extent2D size{1280, 720};
    bool fullscreen{false};
    bool vsync{true};
    bool presentTiming{false};
    bool headless{false};
    std::string title{"OpenFG Scaler"};
};

struct PresentTarget {
    vk::Image image{};
    vk::ImageView view{};
    vk::Extent2D extent{};
};

class Presenter {
public:
    virtual ~Presenter() = default;

    virtual bool wantsSurface() const = 0;
    virtual std::vector<const char*> instanceExtensions() const = 0;
    virtual bool createSurface(vk::Instance instance, vk::SurfaceKHR& surface) = 0;
    virtual bool init(Runtime& runtime, std::string& error) = 0;

    virtual bool acquire(PresentTarget& target, std::string& error) = 0;
    virtual void record(
        Runtime& runtime,
        vk::CommandBuffer command,
        vk::ImageView source,
        vk::Extent2D sourceExtent
    ) = 0;
    virtual bool submit(vk::CommandBuffer command, std::string& error) = 0;

    virtual void pollEvents() = 0;
    virtual bool shouldClose() const = 0;
    virtual bool saveFrame(const std::string& path, std::string& error) = 0;
    virtual vk::Extent2D size() const = 0;
};

/// Create a windowed presenter when SDL3 is available, otherwise a headless
/// one. `options.headless` always selects the headless presenter.
std::unique_ptr<Presenter> createPresenter(const PresentOptions& options);

} // namespace openfg::scaler
