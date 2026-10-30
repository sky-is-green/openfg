/* SPDX-License-Identifier: MIT */

#include "vkutil.hpp"

#include <algorithm>
#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <vk>

namespace vkutil {

vk::UniqueInstance createInstance(
    vk::detail::DispatchLoaderDynamic& dld,
    std::string_view name,
    uint32_t apiVersion
) {
    dld.init();

    const std::string appName{name};
    const vk::ApplicationInfo appInfo{
        .pApplicationName = appName.c_str(),
        .applicationVersion = vk::makeVersion(1, 0, 0),
        .pEngineName = appName.c_str(),
        .engineVersion = vk::makeVersion(1, 0, 0),
        .apiVersion = apiVersion
    };
    const vk::InstanceCreateInfo instanceInfo{
        .pApplicationInfo = &appInfo
    };
    auto instance{vk::createInstanceUnique(instanceInfo, nullptr, dld)};
    dld.init(*instance);
    return instance;
}

vk::PhysicalDevice findPhysicalDevice(
    const vk::detail::DispatchLoaderDynamic& dld,
    const vk::Instance& instance,
    const std::string& id
) {
    for (const auto& physdev : instance.enumeratePhysicalDevices(dld)) {
        const bool hasPci{supportsExtension(dld, physdev, vk::EXTPciBusInfoExtensionName)};

        vk::PhysicalDevicePCIBusInfoPropertiesEXT pciInfo{};
        vk::PhysicalDeviceProperties2 props{
            .pNext = hasPci ? &pciInfo : nullptr
        };
        physdev.getProperties2(&props, dld);

        if (id.empty())
            return physdev;

        std::string deviceName{props.properties.deviceName.data()};
        if (id == deviceName)
            return physdev;

        std::ostringstream vendorDevice;
        vendorDevice << std::hex << std::setfill('0')
            << std::setw(4) << props.properties.vendorID << ":"
            << std::setw(4) << props.properties.deviceID;
        if (id == vendorDevice.str())
            return physdev;

        if (!hasPci)
            continue;

        std::ostringstream pci;
        pci << std::hex << std::setfill('0')
            << std::setw(4) << pciInfo.pciDomain << ":"
            << std::setw(2) << pciInfo.pciBus << ":"
            << std::setw(2) << pciInfo.pciDevice << "."
            << std::setw(1) << pciInfo.pciFunction;
        if (id == pci.str())
            return physdev;
    }

    throw std::runtime_error("No Vulkan device matching '" + id + "' found");
}

uint32_t findQueueFamilyIndex(
    const vk::detail::DispatchLoaderDynamic& dld,
    const vk::PhysicalDevice& physdev,
    vk::QueueFlags flags,
    bool requireDedicated
) {
    uint32_t index{0};
    for (const auto& family : physdev.getQueueFamilyProperties(dld)) {
        const bool isGraphics{static_cast<bool>(family.queueFlags & vk::QueueFlagBits::eGraphics)};
        if (requireDedicated && isGraphics) {
            index++;
            continue;
        }
        if (family.queueFlags & flags)
            return index;
        index++;
    }
    throw std::runtime_error("No matching Vulkan queue family found");
}

bool supportsExtension(
    const vk::detail::DispatchLoaderDynamic& dld,
    const vk::PhysicalDevice& physdev,
    std::string_view name
) {
    for (const auto& ext : physdev.enumerateDeviceExtensionProperties(nullptr, dld)) {
        if (name == ext.extensionName)
            return true;
    }
    return false;
}

/* Resources */

vk::UniqueImage createImage(
    const vk::detail::DispatchLoaderDynamic& dld,
    const vk::Device& device,
    vk::Extent2D extent,
    vk::Format format,
    uint32_t layers,
    vk::ImageUsageFlags usage,
    ExternalMode external,
    vk::ImageLayout initialLayout
) {
    const bool isExternal{external != ExternalMode::None};
    const bool isDmaBuf{external == ExternalMode::DmaBuf};

    const vk::ExternalMemoryImageCreateInfo externalInfo{
        .handleTypes = isDmaBuf
            ? vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT
            : vk::ExternalMemoryHandleTypeFlagBits::eOpaqueFd
    };
    const vk::ImageCreateInfo imageInfo{
        .pNext = isExternal ? &externalInfo : nullptr,
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = { extent.width, extent.height, 1 },
        .mipLevels = 1,
        .arrayLayers = layers,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = isDmaBuf ? vk::ImageTiling::eLinear : vk::ImageTiling::eOptimal,
        .usage = usage,
        .sharingMode = vk::SharingMode::eExclusive,
        .initialLayout = initialLayout
    };
    return device.createImageUnique(imageInfo, nullptr, dld);
}

vk::UniqueImageView createImageView(
    const vk::detail::DispatchLoaderDynamic& dld,
    const vk::Device& device,
    const vk::Image& image,
    vk::Format format,
    uint32_t layers
) {
    const vk::ImageViewCreateInfo viewInfo{
        .image = image,
        .viewType = layers == 1 ? vk::ImageViewType::e2D : vk::ImageViewType::e2DArray,
        .format = format,
        .subresourceRange = {
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .levelCount = 1,
            .layerCount = layers
        }
    };
    return device.createImageViewUnique(viewInfo, nullptr, dld);
}

vk::UniqueSampler createSampler(
    const vk::detail::DispatchLoaderDynamic& dld,
    const vk::Device& device,
    vk::SamplerAddressMode mode,
    vk::CompareOp compare,
    bool whiteBorder
) {
    const vk::SamplerCreateInfo samplerInfo{
        .magFilter = vk::Filter::eLinear,
        .minFilter = vk::Filter::eLinear,
        .mipmapMode = vk::SamplerMipmapMode::eLinear,
        .addressModeU = mode,
        .addressModeV = mode,
        .addressModeW = mode,
        .compareOp = compare,
        .maxLod = vk::LodClampNone,
        .borderColor = whiteBorder
            ? vk::BorderColor::eFloatOpaqueWhite
            : vk::BorderColor::eFloatTransparentBlack
    };
    return device.createSamplerUnique(samplerInfo, nullptr, dld);
}

vk::UniqueShaderModule createShaderModule(
    const vk::detail::DispatchLoaderDynamic& dld,
    const vk::Device& device,
    std::span<const uint32_t> code
) {
    const vk::ShaderModuleCreateInfo shaderInfo{
        .codeSize = code.size() * sizeof(uint32_t),
        .pCode = code.data()
    };
    return device.createShaderModuleUnique(shaderInfo, nullptr, dld);
}

std::pair<vk::UniqueBuffer, vk::UniqueDeviceMemory> createBuffer(
    const vk::detail::DispatchLoaderDynamic& dld,
    const vk::Device& device,
    const vk::PhysicalDevice& physdev,
    vk::BufferUsageFlags usage,
    const void* data,
    size_t size
) {
    const vk::BufferCreateInfo bufferInfo{
        .size = size,
        .usage = usage,
        .sharingMode = vk::SharingMode::eExclusive
    };
    auto buffer{device.createBufferUnique(bufferInfo, nullptr, dld)};
    const auto requirements{device.getBufferMemoryRequirements(*buffer, dld)};

    auto memory{allocateMemory(
        dld, device, physdev,
        requirements.size,
        requirements.memoryTypeBits,
        true
    )};
    device.bindBufferMemory(*buffer, *memory, 0, dld);

    if (data) {
        void* mapped{device.mapMemory(*memory, 0, size, {}, dld)};
        std::memcpy(mapped, data, size);
        device.unmapMemory(*memory, dld);
    }

    return { std::move(buffer), std::move(memory) };
}

vk::UniqueDeviceMemory allocateMemory(
    const vk::detail::DispatchLoaderDynamic& dld,
    const vk::Device& device,
    const vk::PhysicalDevice& physdev,
    vk::DeviceSize size,
    std::bitset<32> types,
    bool hostVisible,
    const void* next
) {
    const auto memProps{physdev.getMemoryProperties(dld)};

    std::optional<uint32_t> selected;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if (!types.test(i))
            continue;

        const auto& type{memProps.memoryTypes.at(i)};
        const bool isHostVisible{
            (type.propertyFlags & vk::MemoryPropertyFlagBits::eHostVisible) &&
            (type.propertyFlags & vk::MemoryPropertyFlagBits::eHostCoherent)
        };
        if (hostVisible && !isHostVisible)
            continue;

        selected = i;
        if (type.propertyFlags & vk::MemoryPropertyFlagBits::eDeviceLocal)
            break;
    }

    if (!selected)
        throw std::runtime_error("No suitable Vulkan memory type");

    const vk::MemoryAllocateInfo allocInfo{
        .pNext = next,
        .allocationSize = size,
        .memoryTypeIndex = *selected
    };
    return device.allocateMemoryUnique(allocInfo, nullptr, dld);
}

/* Descriptors & pipelines */

std::pair<vk::UniqueDescriptorSetLayout, vk::UniquePipelineLayout> createLayout(
    const vk::detail::DispatchLoaderDynamic& dld,
    const vk::Device& device,
    const std::vector<vk::DescriptorSetLayoutBinding>& bindings,
    size_t pushConstantSize
) {
    const vk::DescriptorSetLayoutCreateInfo layoutInfo{
        .bindingCount = static_cast<uint32_t>(bindings.size()),
        .pBindings = bindings.data()
    };
    auto setLayout{device.createDescriptorSetLayoutUnique(layoutInfo, nullptr, dld)};

    const vk::PushConstantRange range{
        .stageFlags = vk::ShaderStageFlagBits::eCompute,
        .size = static_cast<uint32_t>(pushConstantSize)
    };
    const vk::PipelineLayoutCreateInfo pipelineInfo{
        .setLayoutCount = 1,
        .pSetLayouts = &*setLayout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &range
    };
    auto pipelineLayout{device.createPipelineLayoutUnique(pipelineInfo, nullptr, dld)};
    return { std::move(setLayout), std::move(pipelineLayout) };
}

std::pair<vk::UniqueDescriptorPool, vk::DescriptorSet> createDescriptorSet(
    const vk::detail::DispatchLoaderDynamic& dld,
    const vk::Device& device,
    const vk::DescriptorSetLayout& layout,
    uint32_t samplers,
    uint32_t buffers,
    uint32_t sampledImages,
    uint32_t storageImages
) {
    const std::array<vk::DescriptorPoolSize, 4> sizes{{
        { .type = vk::DescriptorType::eSampler, .descriptorCount = samplers },
        { .type = vk::DescriptorType::eSampledImage, .descriptorCount = sampledImages },
        { .type = vk::DescriptorType::eStorageImage, .descriptorCount = storageImages },
        { .type = vk::DescriptorType::eUniformBuffer, .descriptorCount = buffers }
    }};
    auto pool{device.createDescriptorPoolUnique({
        .maxSets = 1,
        .poolSizeCount = static_cast<uint32_t>(sizes.size()),
        .pPoolSizes = sizes.data()
    }, nullptr, dld)};
    auto set{device.allocateDescriptorSets({
        .descriptorPool = *pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &layout
    }, dld).front()};
    return { std::move(pool), set };
}

/* Commands & synchronization */

vk::UniqueCommandPool createCommandPool(
    const vk::detail::DispatchLoaderDynamic& dld,
    const vk::Device& device,
    uint32_t queueFamilyIndex,
    bool resettable
) {
    const vk::CommandPoolCreateInfo poolInfo{
        .flags = resettable
            ? vk::CommandPoolCreateFlagBits::eResetCommandBuffer
            : vk::CommandPoolCreateFlags{},
        .queueFamilyIndex = queueFamilyIndex
    };
    return device.createCommandPoolUnique(poolInfo, nullptr, dld);
}

vk::UniqueCommandBuffer createCommandBuffer(
    const vk::detail::DispatchLoaderDynamic& dld,
    const vk::Device& device,
    const vk::CommandPool& pool
) {
    const vk::CommandBufferAllocateInfo info{
        .commandPool = pool,
        .level = vk::CommandBufferLevel::ePrimary,
        .commandBufferCount = 1
    };
    return { std::move(device.allocateCommandBuffersUnique(info, dld).front()) };
}

vk::UniqueSemaphore createSemaphore(
    const vk::detail::DispatchLoaderDynamic& dld,
    const vk::Device& device,
    bool timeline,
    bool external
) {
    const vk::ExportSemaphoreCreateInfo exportInfo{
        .handleTypes = vk::ExternalSemaphoreHandleTypeFlagBits::eOpaqueFd
    };
    vk::SemaphoreTypeCreateInfo typeInfo{
        .semaphoreType = timeline
            ? vk::SemaphoreType::eTimeline
            : vk::SemaphoreType::eBinary,
        .initialValue = 0
    };
    if (external)
        typeInfo.pNext = &exportInfo;

    const vk::SemaphoreCreateInfo semaphoreInfo{
        .pNext = timeline ? static_cast<const void*>(&typeInfo)
                          : (external ? static_cast<const void*>(&exportInfo) : nullptr)
    };
    return device.createSemaphoreUnique(semaphoreInfo, nullptr, dld);
}

vk::UniqueFence createFence(
    const vk::detail::DispatchLoaderDynamic& dld,
    const vk::Device& device
) {
    return device.createFenceUnique({
        .flags = vk::FenceCreateFlags{}
    }, nullptr, dld);
}

/* External memory */

int exportMemoryFd(
    const vk::detail::DispatchLoaderDynamic& dld,
    const vk::Device& device,
    const vk::DeviceMemory& memory,
    ExternalMode mode
) {
    const vk::MemoryGetFdInfoKHR info{
        .memory = memory,
        .handleType = mode == ExternalMode::DmaBuf
            ? vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT
            : vk::ExternalMemoryHandleTypeFlagBits::eOpaqueFd
    };
    return device.getMemoryFdKHR(info, dld);
}

int exportSemaphoreFd(
    const vk::detail::DispatchLoaderDynamic& dld,
    const vk::Device& device,
    const vk::Semaphore& semaphore
) {
    const vk::SemaphoreGetFdInfoKHR info{
        .semaphore = semaphore,
        .handleType = vk::ExternalSemaphoreHandleTypeFlagBits::eOpaqueFd
    };
    return device.getSemaphoreFdKHR(info, dld);
}

void importSemaphoreFd(
    const vk::detail::DispatchLoaderDynamic& dld,
    const vk::Device& device,
    const vk::Semaphore& semaphore,
    int fd
) {
    const vk::ImportSemaphoreFdInfoKHR info{
        .semaphore = semaphore,
        .handleType = vk::ExternalSemaphoreHandleTypeFlagBits::eOpaqueFd,
        .fd = fd
    };
    device.importSemaphoreFdKHR(info, dld);
}

}
