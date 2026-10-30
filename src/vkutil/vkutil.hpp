/* SPDX-License-Identifier: MIT */

#pragma once

#include <bitset>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <vk>

// Small, original Vulkan helper library for the openfg project.
// It deliberately exposes only the operations the backend and layer need.

namespace vkutil {

enum class ExternalMode : uint8_t {
    None,
    OpaqueFd,
    DmaBuf
};

/* Instance & device */

vk::UniqueInstance createInstance(
    vk::detail::DispatchLoaderDynamic& dld,
    std::string_view name,
    uint32_t apiVersion
);

vk::PhysicalDevice findPhysicalDevice(
    const vk::detail::DispatchLoaderDynamic& dld,
    const vk::Instance& instance,
    const std::string& id
);

uint32_t findQueueFamilyIndex(
    const vk::detail::DispatchLoaderDynamic& dld,
    const vk::PhysicalDevice& physdev,
    vk::QueueFlags flags,
    bool requireDedicated = false
);

bool supportsExtension(
    const vk::detail::DispatchLoaderDynamic& dld,
    const vk::PhysicalDevice& physdev,
    std::string_view name
);

/* Resources */

vk::UniqueImage createImage(
    const vk::detail::DispatchLoaderDynamic& dld,
    const vk::Device& device,
    vk::Extent2D extent,
    vk::Format format,
    uint32_t layers,
    vk::ImageUsageFlags usage,
    ExternalMode external = ExternalMode::None,
    vk::ImageLayout initialLayout = vk::ImageLayout::eUndefined
);

vk::UniqueImageView createImageView(
    const vk::detail::DispatchLoaderDynamic& dld,
    const vk::Device& device,
    const vk::Image& image,
    vk::Format format,
    uint32_t layers
);

vk::UniqueSampler createSampler(
    const vk::detail::DispatchLoaderDynamic& dld,
    const vk::Device& device,
    vk::SamplerAddressMode mode,
    vk::CompareOp compare,
    bool whiteBorder
);

vk::UniqueShaderModule createShaderModule(
    const vk::detail::DispatchLoaderDynamic& dld,
    const vk::Device& device,
    std::span<const uint32_t> code
);

std::pair<vk::UniqueBuffer, vk::UniqueDeviceMemory> createBuffer(
    const vk::detail::DispatchLoaderDynamic& dld,
    const vk::Device& device,
    const vk::PhysicalDevice& physdev,
    vk::BufferUsageFlags usage,
    const void* data,
    size_t size
);

vk::UniqueDeviceMemory allocateMemory(
    const vk::detail::DispatchLoaderDynamic& dld,
    const vk::Device& device,
    const vk::PhysicalDevice& physdev,
    vk::DeviceSize size,
    std::bitset<32> types,
    bool hostVisible = false,
    const void* next = nullptr
);

/* Descriptors & pipelines */

std::pair<vk::UniqueDescriptorSetLayout, vk::UniquePipelineLayout> createLayout(
    const vk::detail::DispatchLoaderDynamic& dld,
    const vk::Device& device,
    const std::vector<vk::DescriptorSetLayoutBinding>& bindings,
    size_t pushConstantSize
);

std::pair<vk::UniqueDescriptorPool, vk::DescriptorSet> createDescriptorSet(
    const vk::detail::DispatchLoaderDynamic& dld,
    const vk::Device& device,
    const vk::DescriptorSetLayout& layout,
    uint32_t samplers,
    uint32_t buffers,
    uint32_t sampledImages,
    uint32_t storageImages
);

/* Commands & synchronization */

vk::UniqueCommandPool createCommandPool(
    const vk::detail::DispatchLoaderDynamic& dld,
    const vk::Device& device,
    uint32_t queueFamilyIndex,
    bool resettable = false
);

vk::UniqueCommandBuffer createCommandBuffer(
    const vk::detail::DispatchLoaderDynamic& dld,
    const vk::Device& device,
    const vk::CommandPool& pool
);

vk::UniqueSemaphore createSemaphore(
    const vk::detail::DispatchLoaderDynamic& dld,
    const vk::Device& device,
    bool timeline = false,
    bool external = false
);

vk::UniqueFence createFence(
    const vk::detail::DispatchLoaderDynamic& dld,
    const vk::Device& device
);

/* External memory */

int exportMemoryFd(
    const vk::detail::DispatchLoaderDynamic& dld,
    const vk::Device& device,
    const vk::DeviceMemory& memory,
    ExternalMode mode
);

int exportSemaphoreFd(
    const vk::detail::DispatchLoaderDynamic& dld,
    const vk::Device& device,
    const vk::Semaphore& semaphore
);

void importSemaphoreFd(
    const vk::detail::DispatchLoaderDynamic& dld,
    const vk::Device& device,
    const vk::Semaphore& semaphore,
    int fd
);

}
