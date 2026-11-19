/* SPDX-License-Identifier: MIT */

#include "vulkan.hpp"

#include "vkutil/vkutil.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace openfg::scaler {
namespace {

    bool isPresentFamily(
        const vk::detail::DispatchLoaderDynamic& dld,
        const vk::PhysicalDevice& physical,
        uint32_t family,
        vk::SurfaceKHR surface
    ) {
        if (!surface)
            return true;
        return physical.getSurfaceSupportKHR(family, surface, dld);
    }

} // namespace

std::unique_ptr<Runtime> createRuntime(
    const std::string& appName,
    std::span<const char* const> instanceExtensions,
    std::span<const char* const> deviceExtensions,
    const std::string& gpu,
    const SurfaceFactory& surfaceFactory,
    bool presentTiming
) {
    auto runtime = std::make_unique<Runtime>();
    runtime->dld.init();
    runtime->instance = vkutil::createInstance(
        runtime->dld, appName, vk::ApiVersion13, instanceExtensions);
    runtime->dld.init(*runtime->instance);

    vk::SurfaceKHR surface;
    if (surfaceFactory && !surfaceFactory(*runtime->instance, surface))
        throw std::runtime_error("cannot create a Vulkan surface");

    const std::string selector = gpu == "auto" ? std::string{} : gpu;
    runtime->physical = vkutil::findPhysicalDevice(
        runtime->dld, *runtime->instance, selector);

    const auto families =
        runtime->physical.getQueueFamilyProperties(runtime->dld);
    bool found{false};
    for (uint32_t index = 0; index < families.size(); index++) {
        const auto flags = families[index].queueFlags;
        if (!(flags & vk::QueueFlagBits::eGraphics) ||
            !(flags & vk::QueueFlagBits::eCompute))
            continue;
        if (!isPresentFamily(runtime->dld, runtime->physical, index, surface))
            continue;
        runtime->queueFamily = index;
        found = true;
        break;
    }
    if (!found) {
        if (surface)
            throw std::runtime_error(
                "no graphics/compute/present queue family on this GPU");
        throw std::runtime_error(
            "no graphics/compute queue family on this GPU");
    }

    std::vector<const char*> enabledDeviceExtensions;
    for (const char* name : deviceExtensions) {
        if (!vkutil::supportsExtension(runtime->dld, runtime->physical, name))
            throw std::runtime_error(
                std::string("device does not support ") + name);
        enabledDeviceExtensions.push_back(name);
    }

    vk::PhysicalDevicePresentIdFeaturesKHR presentIdFeature{};
    vk::PhysicalDevicePresentWaitFeaturesKHR presentWaitFeature{};
    if (presentTiming) {
        vk::PhysicalDevicePresentIdFeaturesKHR queriedId{};
        vk::PhysicalDevicePresentWaitFeaturesKHR queriedWait{};
        vk::PhysicalDeviceFeatures2 queriedFeatures{
            .pNext = &queriedId
        };
        queriedId.pNext = &queriedWait;
        runtime->physical.getFeatures2(&queriedFeatures, runtime->dld);
        if (!queriedId.presentId || !queriedWait.presentWait)
            throw std::runtime_error(
                "the GPU does not support present timing");
        presentIdFeature.presentId = true;
        presentWaitFeature.presentWait = true;
        presentIdFeature.pNext = &presentWaitFeature;
    }

    const float priority{1.0F};
    const vk::DeviceQueueCreateInfo queueInfo{
        .queueFamilyIndex = runtime->queueFamily,
        .queueCount = 1,
        .pQueuePriorities = &priority
    };
    const vk::DeviceCreateInfo deviceInfo{
        .pNext = presentTiming ? &presentIdFeature : nullptr,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queueInfo,
        .enabledExtensionCount =
            static_cast<uint32_t>(enabledDeviceExtensions.size()),
        .ppEnabledExtensionNames = enabledDeviceExtensions.data()
    };
    runtime->device =
        runtime->physical.createDeviceUnique(deviceInfo, nullptr, runtime->dld);
    runtime->dld.init(*runtime->device);
    runtime->queue = runtime->device->getQueue(
        runtime->queueFamily, 0, runtime->dld);
    runtime->commandPool = vkutil::createCommandPool(
        runtime->dld, *runtime->device, runtime->queueFamily);

    const auto properties = runtime->physical.getProperties(runtime->dld);
    runtime->deviceName = properties.deviceName.data();
    runtime->timestampPeriod =
        static_cast<float>(properties.limits.timestampPeriod);

    return runtime;
}

bool readbackRgba8(
    Runtime& runtime,
    vk::Image image,
    vk::Extent2D extent,
    vk::ImageLayout currentLayout,
    std::string& data,
    std::string& error
) {
    const vk::DeviceSize size =
        static_cast<vk::DeviceSize>(extent.width) * extent.height * 4;
    auto [buffer, memory] = vkutil::createBuffer(
        runtime.dld, *runtime.device, runtime.physical,
        vk::BufferUsageFlagBits::eTransferDst, nullptr, size);
    auto pool = vkutil::createCommandPool(
        runtime.dld, *runtime.device, runtime.queueFamily);
    auto command = vkutil::createCommandBuffer(
        runtime.dld, *runtime.device, *pool);
    auto fence = vkutil::createFence(runtime.dld, *runtime.device);

    const vk::ImageMemoryBarrier toTransfer{
        .srcAccessMask = vk::AccessFlagBits::eShaderRead,
        .dstAccessMask = vk::AccessFlagBits::eTransferRead,
        .oldLayout = currentLayout,
        .newLayout = vk::ImageLayout::eTransferSrcOptimal,
        .srcQueueFamilyIndex = vk::QueueFamilyIgnored,
        .dstQueueFamilyIndex = vk::QueueFamilyIgnored,
        .image = image,
        .subresourceRange = {
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .levelCount = 1,
            .layerCount = 1
        }
    };
    const vk::BufferImageCopy region{
        .imageSubresource = {
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .mipLevel = 0,
            .baseArrayLayer = 0,
            .layerCount = 1
        },
        .imageExtent = { extent.width, extent.height, 1 }
    };

    command->begin({ .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit },
        runtime.dld);
    command->pipelineBarrier(
        vk::PipelineStageFlagBits::eFragmentShader,
        vk::PipelineStageFlagBits::eTransfer, {}, {}, {},
        { toTransfer }, runtime.dld);
    command->copyImageToBuffer(image,
        vk::ImageLayout::eTransferSrcOptimal, *buffer, { region },
        runtime.dld);
    const vk::ImageMemoryBarrier toSample{
        .srcAccessMask = vk::AccessFlagBits::eTransferRead,
        .dstAccessMask = vk::AccessFlagBits::eShaderRead,
        .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
        .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        .srcQueueFamilyIndex = vk::QueueFamilyIgnored,
        .dstQueueFamilyIndex = vk::QueueFamilyIgnored,
        .image = image,
        .subresourceRange = {
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .levelCount = 1,
            .layerCount = 1
        }
    };
    command->pipelineBarrier(
        vk::PipelineStageFlagBits::eTransfer,
        vk::PipelineStageFlagBits::eFragmentShader, {}, {}, {},
        { toSample }, runtime.dld);
    command->end(runtime.dld);

    runtime.queue.submit({{
        .commandBufferCount = 1,
        .pCommandBuffers = &*command
    }}, *fence, runtime.dld);
    const auto result =
        runtime.device->waitForFences(*fence, true, UINT64_MAX, runtime.dld);
    if (result != vk::Result::eSuccess) {
        error = "readback fence wait failed";
        return false;
    }

    void* mapped{};
    if (runtime.device->mapMemory(*memory, 0, size, {}, &mapped, runtime.dld) !=
        vk::Result::eSuccess) {
        error = "cannot map readback buffer";
        return false;
    }
    data.assign(static_cast<const char*>(mapped), static_cast<size_t>(size));
    runtime.device->unmapMemory(*memory, runtime.dld);
    return true;
}

} // namespace openfg::scaler
