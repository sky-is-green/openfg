/* SPDX-License-Identifier: MIT */

#include "headless_presenter.hpp"

#include "vkutil/vkutil.hpp"

#include <fstream>
#include <stdexcept>
#include <vector>

namespace openfg::scaler {

HeadlessPresenter::HeadlessPresenter(PresentOptions options)
    : options_{options}
{
}

HeadlessPresenter::~HeadlessPresenter() = default;

bool HeadlessPresenter::init(Runtime& runtime, std::string& error)
{
    runtime_ = &runtime;
    if (options_.size.width == 0 || options_.size.height == 0) {
        error = "headless output extent must be non-zero";
        return false;
    }

    image_ = vkutil::createImage(
        runtime.dld, *runtime.device, options_.size,
        vk::Format::eR8G8B8A8Unorm, 1,
        vk::ImageUsageFlagBits::eColorAttachment |
        vk::ImageUsageFlagBits::eTransferSrc);
    const auto requirements =
        runtime.device->getImageMemoryRequirements(*image_, runtime.dld);
    memory_ = vkutil::allocateMemory(
        runtime.dld, *runtime.device, runtime.physical,
        requirements.size, requirements.memoryTypeBits);
    runtime.device->bindImageMemory(
        *image_, *memory_, 0, runtime.dld);
    view_ = vkutil::createImageView(
        runtime.dld, *runtime.device, *image_,
        vk::Format::eR8G8B8A8Unorm, 1);

    pipeline_.init(runtime, vk::Format::eR8G8B8A8Unorm,
        vk::ImageLayout::eTransferSrcOptimal);
    framebuffer_ = runtime.device->createFramebufferUnique({
        .renderPass = pipeline_.renderPass(),
        .attachmentCount = 1,
        .pAttachments = &*view_,
        .width = options_.size.width,
        .height = options_.size.height,
        .layers = 1
    }, nullptr, runtime.dld);
    fence_ = vkutil::createFence(runtime.dld, *runtime.device);
    return true;
}

bool HeadlessPresenter::acquire(PresentTarget& target, std::string&)
{
    target.image = *image_;
    target.view = *view_;
    target.extent = options_.size;
    return true;
}

void HeadlessPresenter::record(
    Runtime& runtime,
    vk::CommandBuffer command,
    vk::ImageView source,
    vk::Extent2D sourceExtent
) {
    pipeline_.record(runtime, command, *framebuffer_, options_.size,
        source, sourceExtent);
}

bool HeadlessPresenter::submit(vk::CommandBuffer command, std::string& error)
{
    command.end(runtime_->dld);
    runtime_->queue.submit({{
        .commandBufferCount = 1,
        .pCommandBuffers = &command
    }}, *fence_, runtime_->dld);
    const auto result = runtime_->device->waitForFences(
        *fence_, true, UINT64_MAX, runtime_->dld);
    if (result != vk::Result::eSuccess) {
        error = "headless present fence wait failed";
        return false;
    }
    return true;
}

bool HeadlessPresenter::saveFrame(
    const std::string& path,
    std::string& error
) {
    if (!runtime_ || !image_) {
        error = "headless presenter is not initialized";
        return false;
    }

    const vk::DeviceSize size = static_cast<vk::DeviceSize>(
        options_.size.width) * options_.size.height * 4;
    auto [buffer, memory] = vkutil::createBuffer(
        runtime_->dld, *runtime_->device, runtime_->physical,
        vk::BufferUsageFlagBits::eTransferDst, nullptr,
        static_cast<size_t>(size));
    auto pool = vkutil::createCommandPool(
        runtime_->dld, *runtime_->device, runtime_->queueFamily);
    auto command = vkutil::createCommandBuffer(
        runtime_->dld, *runtime_->device, *pool);
    auto fence = vkutil::createFence(runtime_->dld, *runtime_->device);

    command->begin(
        { .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit },
        runtime_->dld);
    const vk::BufferImageCopy region{
        .imageSubresource = {
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .mipLevel = 0,
            .baseArrayLayer = 0,
            .layerCount = 1
        },
        .imageExtent = {
            options_.size.width,
            options_.size.height,
            1
        }
    };
    command->copyImageToBuffer(*image_,
        vk::ImageLayout::eTransferSrcOptimal, *buffer, { region },
        runtime_->dld);
    command->end(runtime_->dld);

    runtime_->queue.submit({{
        .commandBufferCount = 1,
        .pCommandBuffers = &*command
    }}, *fence, runtime_->dld);
    const auto result = runtime_->device->waitForFences(
        *fence, true, UINT64_MAX, runtime_->dld);
    if (result != vk::Result::eSuccess) {
        error = "readback fence wait failed";
        return false;
    }

    void* mapped{};
    if (runtime_->device->mapMemory(
            *memory, 0, size, {}, &mapped, runtime_->dld) !=
        vk::Result::eSuccess) {
        error = "cannot map readback buffer";
        return false;
    }
    std::ofstream file{path, std::ios::binary | std::ios::trunc};
    if (!file) {
        runtime_->device->unmapMemory(*memory, runtime_->dld);
        error = "cannot open " + path;
        return false;
    }
    file.write(static_cast<const char*>(mapped),
        static_cast<std::streamsize>(size));
    runtime_->device->unmapMemory(*memory, runtime_->dld);
    if (!file) {
        error = "cannot write " + path;
        return false;
    }
    return true;
}

} // namespace openfg::scaler
