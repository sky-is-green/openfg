/* SPDX-License-Identifier: MIT */
#pragma once

#include <vk>

#include <functional>
#include <memory>
#include <span>
#include <string>

namespace openfg::scaler {

/// Vulkan objects shared by the capture source, upscaler and presenter.
/// The instance and surface are owned by the caller.
struct Runtime {
    vk::detail::DispatchLoaderDynamic dld;
    vk::UniqueInstance instance;
    vk::PhysicalDevice physical;
    vk::UniqueDevice device;
    vk::Queue queue;
    uint32_t queueFamily{};
    vk::UniqueCommandPool commandPool;
    std::string deviceName;
    float timestampPeriod{1.0F};
};

/// Create the instance and device. `surfaceFactory` is called after the
/// instance exists so the window backend can create its Vulkan surface.
/// It may be empty for headless runs.
using SurfaceFactory = std::function<bool(vk::Instance, vk::SurfaceKHR&)>;

std::unique_ptr<Runtime> createRuntime(
    const std::string& appName,
    std::span<const char* const> instanceExtensions,
    std::span<const char* const> deviceExtensions,
    const std::string& gpu,
    const SurfaceFactory& surfaceFactory = {}
);

/// Read an RGBA8 image back into a tightly packed buffer.
bool readbackRgba8(
    Runtime& runtime,
    vk::Image image,
    vk::Extent2D extent,
    vk::ImageLayout currentLayout,
    std::string& data,
    std::string& error
);

} // namespace openfg::scaler
