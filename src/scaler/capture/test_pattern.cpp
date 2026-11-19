/* SPDX-License-Identifier: MIT */

#include "test_pattern.hpp"

#include "vkutil/vkutil.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace openfg::scaler {
namespace {

    uint8_t channel(uint32_t value) {
        return static_cast<uint8_t>(std::clamp(value, 0U, 255U));
    }

} // namespace

TestPatternSource::TestPatternSource(
    Runtime& runtime,
    vk::Extent2D extent,
    bool animate
)
    : runtime_{runtime}, extent_{extent}, animate_{animate}
{
    if (extent.width == 0 || extent.height == 0)
        throw std::runtime_error("test pattern extent must be non-zero");
    createResources();
}

TestPatternSource::~TestPatternSource()
{
    stop();
}

void TestPatternSource::createResources()
{
    image_ = vkutil::createImage(
        runtime_.dld, *runtime_.device, extent_,
        vk::Format::eR8G8B8A8Unorm, 1,
        vk::ImageUsageFlagBits::eSampled |
        vk::ImageUsageFlagBits::eTransferDst);
    const auto requirements =
        runtime_.device->getImageMemoryRequirements(*image_, runtime_.dld);
    memory_ = vkutil::allocateMemory(
        runtime_.dld, *runtime_.device, runtime_.physical,
        requirements.size, requirements.memoryTypeBits);
    runtime_.device->bindImageMemory(
        *image_, *memory_, 0, runtime_.dld);
    view_ = vkutil::createImageView(
        runtime_.dld, *runtime_.device, *image_,
        vk::Format::eR8G8B8A8Unorm, 1);

    stagingSize_ = static_cast<vk::DeviceSize>(extent_.width) *
        extent_.height * 4;
    auto [buffer, memory] = vkutil::createBuffer(
        runtime_.dld, *runtime_.device, runtime_.physical,
        vk::BufferUsageFlagBits::eTransferSrc, nullptr,
        static_cast<size_t>(stagingSize_));
    staging_ = std::move(buffer);
    stagingMemory_ = std::move(memory);
    if (runtime_.device->mapMemory(
            *stagingMemory_, 0, stagingSize_, {}, &mapped_, runtime_.dld) !=
        vk::Result::eSuccess)
        throw std::runtime_error("cannot map test-pattern staging buffer");

    commandPool_ = vkutil::createCommandPool(
        runtime_.dld, *runtime_.device, runtime_.queueFamily);
    command_ = vkutil::createCommandBuffer(
        runtime_.dld, *runtime_.device, *commandPool_);
    fence_ = vkutil::createFence(runtime_.dld, *runtime_.device);
}

void TestPatternSource::fill(uint64_t frame)
{
    auto* pixels = static_cast<uint8_t*>(mapped_);
    const uint32_t width = extent_.width;
    const uint32_t height = extent_.height;
    const uint32_t phase = animate_
        ? static_cast<uint32_t>(frame % 240)
        : 0;
    const uint32_t circleX = (phase * 3) % width;
    const uint32_t circleY = height / 2 +
        static_cast<uint32_t>(
            (height / 3) * std::sin(static_cast<double>(phase) / 32.0));
    const uint32_t heightScale = std::max(1U, height);

    for (uint32_t y = 0; y < height; y++) {
        const int dy = static_cast<int>(y) - static_cast<int>(circleY);
        const uint32_t rowBase = 32 + 96 * y / heightScale;
        for (uint32_t x = 0; x < width; x++) {
            const uint32_t checker =
                ((x / 32) + (y / 32) + (phase / 8)) & 1;
            const uint32_t bars =
                ((x + phase * 2) % 64 < 3) ? 1 : 0;
            const uint32_t diagonal =
                ((x * 3 + y * 2 + phase * 4) / 7) % 96 < 3 ? 1 : 0;
            const int dx = static_cast<int>(x) - static_cast<int>(circleX);
            const bool circle = dx * dx + dy * dy < 80 * 80;

            const uint32_t base = rowBase;
            const uint32_t r = base + (checker ? 110 : 0) + (circle ? 90 : 0);
            const uint32_t g = base + 40 + (bars ? 120 : 0) + (circle ? 40 : 0);
            const uint32_t b = base + 80 + (diagonal ? 100 : 0);
            uint8_t* pixel = pixels +
                (static_cast<size_t>(y) * width + x) * 4;
            pixel[0] = channel(r);
            pixel[1] = channel(g);
            pixel[2] = channel(b);
            pixel[3] = 255;
        }
    }
}

bool TestPatternSource::upload(std::string& error)
{
    command_->reset({}, runtime_.dld);
    command_->begin(
        { .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit },
        runtime_.dld);

    const vk::ImageMemoryBarrier toTransfer{
        .srcAccessMask = initialized_
            ? vk::AccessFlags{vk::AccessFlagBits::eShaderRead}
            : vk::AccessFlags{},
        .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
        .oldLayout = initialized_
            ? vk::ImageLayout::eShaderReadOnlyOptimal
            : vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eTransferDstOptimal,
        .srcQueueFamilyIndex = vk::QueueFamilyIgnored,
        .dstQueueFamilyIndex = vk::QueueFamilyIgnored,
        .image = *image_,
        .subresourceRange = {
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .levelCount = 1,
            .layerCount = 1
        }
    };
    command_->pipelineBarrier(
        initialized_ ? vk::PipelineStageFlagBits::eFragmentShader
                     : vk::PipelineStageFlagBits::eTopOfPipe,
        vk::PipelineStageFlagBits::eTransfer, {}, {}, {},
        { toTransfer }, runtime_.dld);

    const vk::BufferImageCopy region{
        .imageSubresource = {
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .mipLevel = 0,
            .baseArrayLayer = 0,
            .layerCount = 1
        },
        .imageExtent = { extent_.width, extent_.height, 1 }
    };
    command_->copyBufferToImage(*staging_,
        *image_, vk::ImageLayout::eTransferDstOptimal, { region },
        runtime_.dld);

    const vk::ImageMemoryBarrier toSample{
        .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
        .dstAccessMask = vk::AccessFlagBits::eShaderRead,
        .oldLayout = vk::ImageLayout::eTransferDstOptimal,
        .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        .srcQueueFamilyIndex = vk::QueueFamilyIgnored,
        .dstQueueFamilyIndex = vk::QueueFamilyIgnored,
        .image = *image_,
        .subresourceRange = {
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .levelCount = 1,
            .layerCount = 1
        }
    };
    command_->pipelineBarrier(
        vk::PipelineStageFlagBits::eTransfer,
        vk::PipelineStageFlagBits::eFragmentShader, {}, {}, {},
        { toSample }, runtime_.dld);
    command_->end(runtime_.dld);

    runtime_.queue.submit({{
        .commandBufferCount = 1,
        .pCommandBuffers = &*command_
    }}, *fence_, runtime_.dld);
    const auto result =
        runtime_.device->waitForFences(*fence_, true, UINT64_MAX, runtime_.dld);
    if (result != vk::Result::eSuccess) {
        error = "test-pattern upload fence wait failed";
        return false;
    }
    initialized_ = true;
    return true;
}

bool TestPatternSource::start(std::string&)
{
    return true;
}

bool TestPatternSource::next(Frame& frame, std::string& error)
{
    fill(sequence_);
    if (!upload(error))
        return false;

    frame.image = *image_;
    frame.view = *view_;
    frame.extent = extent_;
    frame.format = vk::Format::eR8G8B8A8Unorm;
    frame.sequence = sequence_++;
    frame.timestampNs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    return true;
}

void TestPatternSource::stop()
{
    if (mapped_) {
        runtime_.device->unmapMemory(*stagingMemory_, runtime_.dld);
        mapped_ = nullptr;
    }
    staging_ = {};
    stagingMemory_ = {};
    fence_ = {};
    command_ = {};
    commandPool_ = {};
    view_ = {};
    image_ = {};
    memory_ = {};
    initialized_ = false;
    sequence_ = 0;
}

std::string TestPatternSource::description() const
{
    return "test pattern " + std::to_string(extent_.width) + "x" +
        std::to_string(extent_.height);
}

} // namespace openfg::scaler
