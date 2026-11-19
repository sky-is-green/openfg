/* SPDX-License-Identifier: MIT */

#include "neural.hpp"
#include "neural_ncnn.hpp"

#include "vkutil/vkutil.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace openfg::scaler {

struct NeuralUpscaler::Impl {
    Runtime& runtime;
    NcnnModel* model{};

    vk::Extent2D inputExtent{};
    vk::Extent2D outputExtent{};
    vk::UniqueImage outputImage;
    vk::UniqueDeviceMemory outputMemory;
    vk::UniqueImageView outputView;
    bool outputInitialized{false};

    vk::UniqueBuffer staging;
    vk::UniqueDeviceMemory stagingMemory;
    void* stagingMapped{};
    vk::DeviceSize stagingSize{};
    vk::UniqueBuffer readback;
    vk::UniqueDeviceMemory readbackMemory;
    void* readbackMapped{};
    vk::DeviceSize readbackSize{};
    vk::UniqueCommandPool readbackPool;
    vk::UniqueCommandBuffer readbackCommand;
    vk::UniqueFence readbackFence;

    vk::UniqueCommandPool commandPool;
    vk::UniqueCommandBuffer command;
    vk::UniqueFence fence;
    bool valid{true};
    std::string error;

    Impl(Runtime& runtimeValue, const std::string& paramPath)
        : runtime{runtimeValue}
    {
        model = loadNcnnModel(paramPath, error);
        if (!model)
            throw std::runtime_error(error);
    }

    ~Impl()
    {
        destroyNcnnModel(model);
        if (stagingMapped) {
            runtime.device->unmapMemory(*stagingMemory, runtime.dld);
            stagingMapped = nullptr;
        }
        if (readbackMapped) {
            runtime.device->unmapMemory(*readbackMemory, runtime.dld);
            readbackMapped = nullptr;
        }
        readbackFence = {};
        readbackCommand = {};
        readbackPool = {};
        readback = {};
        readbackMemory = {};
        staging = {};
        stagingMemory = {};
        fence = {};
        command = {};
        commandPool = {};
        outputView = {};
        outputMemory = {};
        outputImage = {};
    }

    void createOutput()
    {
        if (stagingMapped) {
            runtime.device->unmapMemory(*stagingMemory, runtime.dld);
            stagingMapped = nullptr;
        }
        if (readbackMapped) {
            runtime.device->unmapMemory(*readbackMemory, runtime.dld);
            readbackMapped = nullptr;
        }
        outputView = {};
        outputMemory = {};
        outputImage = {};
        outputInitialized = false;
        readback = {};
        readbackMemory = {};
        staging = {};
        stagingMemory = {};
        outputImage = vkutil::createImage(
            runtime.dld, *runtime.device, outputExtent,
            vk::Format::eR8G8B8A8Unorm, 1,
            vk::ImageUsageFlagBits::eSampled |
            vk::ImageUsageFlagBits::eTransferDst);
        const auto requirements =
            runtime.device->getImageMemoryRequirements(
                *outputImage, runtime.dld);
        outputMemory = vkutil::allocateMemory(
            runtime.dld, *runtime.device, runtime.physical,
            requirements.size, requirements.memoryTypeBits);
        runtime.device->bindImageMemory(
            *outputImage, *outputMemory, 0, runtime.dld);
        outputView = vkutil::createImageView(
            runtime.dld, *runtime.device, *outputImage,
            vk::Format::eR8G8B8A8Unorm, 1);

        stagingSize = static_cast<vk::DeviceSize>(
            outputExtent.width) * outputExtent.height * 4;
        auto [buffer, memory] = vkutil::createBuffer(
            runtime.dld, *runtime.device, runtime.physical,
            vk::BufferUsageFlagBits::eTransferSrc, nullptr,
            static_cast<size_t>(stagingSize));
        staging = std::move(buffer);
        stagingMemory = std::move(memory);
        if (runtime.device->mapMemory(
                *stagingMemory, 0, stagingSize, {}, &stagingMapped,
                runtime.dld) != vk::Result::eSuccess)
            throw std::runtime_error("cannot map neural staging buffer");

        readbackSize = static_cast<vk::DeviceSize>(
            inputExtent.width) * inputExtent.height * 4;
        const vk::BufferCreateInfo readbackInfo{
            .size = readbackSize,
            .usage = vk::BufferUsageFlagBits::eTransferDst,
            .sharingMode = vk::SharingMode::eExclusive
        };
        readback = runtime.device->createBufferUnique(
            readbackInfo, nullptr, runtime.dld);
        const auto readbackRequirements =
            runtime.device->getBufferMemoryRequirements(
                *readback, runtime.dld);
        const auto memoryProperties =
            runtime.physical.getMemoryProperties(runtime.dld);
        uint32_t readbackType = UINT32_MAX;
        uint32_t fallbackType = UINT32_MAX;
        for (uint32_t index = 0;
             index < memoryProperties.memoryTypeCount; index++) {
            if (!(readbackRequirements.memoryTypeBits & (1U << index)))
                continue;
            const auto flags = memoryProperties.memoryTypes[index]
                .propertyFlags;
            if (!(flags & vk::MemoryPropertyFlagBits::eHostVisible) ||
                !(flags & vk::MemoryPropertyFlagBits::eHostCoherent))
                continue;
            if (flags & vk::MemoryPropertyFlagBits::eHostCached) {
                readbackType = index;
                break;
            }
            if (fallbackType == UINT32_MAX)
                fallbackType = index;
        }
        if (readbackType == UINT32_MAX)
            readbackType = fallbackType;
        if (readbackType == UINT32_MAX)
            throw std::runtime_error(
                "no host-visible memory for neural readback");
        readbackMemory = runtime.device->allocateMemoryUnique({
            .allocationSize = readbackRequirements.size,
            .memoryTypeIndex = readbackType
        }, nullptr, runtime.dld);
        runtime.device->bindBufferMemory(
            *readback, *readbackMemory, 0, runtime.dld);
        if (runtime.device->mapMemory(
                *readbackMemory, 0, readbackSize, {}, &readbackMapped,
                runtime.dld) != vk::Result::eSuccess)
            throw std::runtime_error("cannot map neural readback buffer");
        readbackPool = vkutil::createCommandPool(
            runtime.dld, *runtime.device, runtime.queueFamily, true);
        readbackCommand = vkutil::createCommandBuffer(
            runtime.dld, *runtime.device, *readbackPool);
        readbackFence = vkutil::createFence(runtime.dld, *runtime.device);

        commandPool = vkutil::createCommandPool(
            runtime.dld, *runtime.device, runtime.queueFamily, true);
        command = vkutil::createCommandBuffer(
            runtime.dld, *runtime.device, *commandPool);
        fence = vkutil::createFence(runtime.dld, *runtime.device);
    }

    void barrierWith(
        vk::CommandBuffer target,
        vk::Image image,
        vk::ImageLayout oldLayout,
        vk::ImageLayout newLayout,
        vk::AccessFlags srcAccess,
        vk::AccessFlags dstAccess,
        vk::PipelineStageFlags srcStage,
        vk::PipelineStageFlags dstStage
    ) {
        const vk::ImageMemoryBarrier info{
            .srcAccessMask = srcAccess,
            .dstAccessMask = dstAccess,
            .oldLayout = oldLayout,
            .newLayout = newLayout,
            .srcQueueFamilyIndex = vk::QueueFamilyIgnored,
            .dstQueueFamilyIndex = vk::QueueFamilyIgnored,
            .image = image,
            .subresourceRange = {
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .levelCount = 1,
                .layerCount = 1
            }
        };
        target.pipelineBarrier(srcStage, dstStage, {}, {}, {}, { info },
            runtime.dld);
    }

    void barrier(
        vk::Image image,
        vk::ImageLayout oldLayout,
        vk::ImageLayout newLayout,
        vk::AccessFlags srcAccess,
        vk::AccessFlags dstAccess,
        vk::PipelineStageFlags srcStage,
        vk::PipelineStageFlags dstStage
    ) {
        barrierWith(*command, image, oldLayout, newLayout, srcAccess,
            dstAccess, srcStage, dstStage);
    }

    bool prepare(const Frame& frame, std::string& error)
    {
        if (!valid) {
            error = this->error;
            return false;
        }
        if (frame.extent != inputExtent) {
            error = "neural input extent changed";
            return false;
        }

        readbackCommand->reset({}, runtime.dld);
        readbackCommand->begin(
            { .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit },
            runtime.dld);
        barrierWith(*readbackCommand, frame.image,
            vk::ImageLayout::eShaderReadOnlyOptimal,
            vk::ImageLayout::eTransferSrcOptimal,
            vk::AccessFlagBits::eShaderRead,
            vk::AccessFlagBits::eTransferRead,
            vk::PipelineStageFlagBits::eFragmentShader,
            vk::PipelineStageFlagBits::eTransfer);
        const vk::BufferImageCopy region{
            .imageSubresource = {
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = 1
            },
            .imageExtent = {
                frame.extent.width,
                frame.extent.height,
                1
            }
        };
        readbackCommand->copyImageToBuffer(frame.image,
            vk::ImageLayout::eTransferSrcOptimal, *readback,
            { region }, runtime.dld);
        barrierWith(*readbackCommand, frame.image,
            vk::ImageLayout::eTransferSrcOptimal,
            vk::ImageLayout::eShaderReadOnlyOptimal,
            vk::AccessFlagBits::eTransferRead,
            vk::AccessFlagBits::eShaderRead,
            vk::PipelineStageFlagBits::eTransfer,
            vk::PipelineStageFlagBits::eFragmentShader);
        readbackCommand->end(runtime.dld);
        runtime.device->resetFences(*readbackFence, runtime.dld);
        runtime.queue.submit({{
            .commandBufferCount = 1,
            .pCommandBuffers = &*readbackCommand
        }}, *readbackFence, runtime.dld);
        if (runtime.device->waitForFences(
                *readbackFence, true, UINT64_MAX, runtime.dld) !=
            vk::Result::eSuccess) {
            error = "neural readback fence wait failed";
            return false;
        }

        const auto* pixels = static_cast<const uint8_t*>(readbackMapped);
        const bool bgr = frame.format == vk::Format::eB8G8R8A8Unorm;
        int outputWidth{};
        int outputHeight{};
        std::vector<uint8_t> rgba;
        if (!runNcnnModel(*model, frame.extent.width, frame.extent.height,
                pixels, bgr, outputWidth, outputHeight, rgba, error))
            return false;
        if (outputWidth != static_cast<int>(outputExtent.width) ||
            outputHeight != static_cast<int>(outputExtent.height)) {
            error = "neural output shape does not match the requested 2x";
            return false;
        }
        if (rgba.size() != static_cast<size_t>(outputExtent.width) *
                outputExtent.height * 4) {
            error = "neural output size mismatch";
            return false;
        }
        std::copy(rgba.begin(), rgba.end(),
            static_cast<uint8_t*>(stagingMapped));

        command->reset({}, runtime.dld);
        command->begin(
            { .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit },
            runtime.dld);
        barrier(*outputImage,
            outputInitialized
                ? vk::ImageLayout::eShaderReadOnlyOptimal
                : vk::ImageLayout::eUndefined,
            vk::ImageLayout::eTransferDstOptimal,
            outputInitialized
                ? vk::AccessFlags{vk::AccessFlagBits::eShaderRead}
                : vk::AccessFlags{},
            vk::AccessFlagBits::eTransferWrite,
            outputInitialized
                ? vk::PipelineStageFlagBits::eFragmentShader
                : vk::PipelineStageFlagBits::eTopOfPipe,
            vk::PipelineStageFlagBits::eTransfer);
        const vk::BufferImageCopy copy{
            .imageSubresource = {
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = 1
            },
            .imageExtent = {
                outputExtent.width,
                outputExtent.height,
                1
            }
        };
        command->copyBufferToImage(*staging, *outputImage,
            vk::ImageLayout::eTransferDstOptimal, { copy }, runtime.dld);
        barrier(*outputImage,
            vk::ImageLayout::eTransferDstOptimal,
            vk::ImageLayout::eShaderReadOnlyOptimal,
            vk::AccessFlagBits::eTransferWrite,
            vk::AccessFlagBits::eShaderRead,
            vk::PipelineStageFlagBits::eTransfer,
            vk::PipelineStageFlagBits::eFragmentShader);
        outputInitialized = true;
        command->end(runtime.dld);

        runtime.device->resetFences(*fence, runtime.dld);
        runtime.queue.submit({{
            .commandBufferCount = 1,
            .pCommandBuffers = &*command
        }}, *fence, runtime.dld);
        if (runtime.device->waitForFences(
                *fence, true, UINT64_MAX, runtime.dld) !=
            vk::Result::eSuccess) {
            error = "neural upload fence wait failed";
            return false;
        }
        return true;
    }
};

NeuralUpscaler::NeuralUpscaler(Runtime& runtime, std::string paramPath)
    : impl_{std::make_unique<Impl>(runtime, paramPath)}
{
}

NeuralUpscaler::~NeuralUpscaler() = default;

void NeuralUpscaler::resize(vk::Extent2D input, vk::Extent2D output)
{
    if (input == impl_->inputExtent && output == impl_->outputExtent)
        return;
    impl_->inputExtent = input;
    impl_->outputExtent = output;
    impl_->valid = output.width == input.width * 2 &&
        output.height == input.height * 2;
    if (!impl_->valid) {
        impl_->error =
            "the v0 neural model is 2x; output must be input * 2";
        return;
    }
    impl_->createOutput();
}

bool NeuralUpscaler::prepare(
    Runtime&,
    const Frame& frame,
    std::string& error
)
{
    return impl_->prepare(frame, error);
}

void NeuralUpscaler::record(
    vk::CommandBuffer,
    vk::ImageView,
    vk::Extent2D
) {
}

vk::ImageView NeuralUpscaler::view() const
{
    return *impl_->outputView;
}

vk::Extent2D NeuralUpscaler::outputExtent() const
{
    return impl_->outputExtent;
}

} // namespace openfg::scaler
