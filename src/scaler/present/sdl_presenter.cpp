/* SPDX-License-Identifier: MIT */

#include "sdl_presenter.hpp"

#include <vk>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace openfg::scaler {
namespace {

    struct SwapchainSupport {
        vk::SurfaceCapabilitiesKHR capabilities;
        std::vector<vk::SurfaceFormatKHR> formats;
        std::vector<vk::PresentModeKHR> modes;
    };

    vk::SurfaceFormatKHR chooseFormat(
        const std::vector<vk::SurfaceFormatKHR>& formats
    ) {
        for (const auto& format : formats) {
            if ((format.format == vk::Format::eB8G8R8A8Unorm ||
                 format.format == vk::Format::eR8G8B8A8Unorm) &&
                format.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear)
                return format;
        }
        return formats.front();
    }

    vk::PresentModeKHR chooseMode(
        const std::vector<vk::PresentModeKHR>& modes,
        bool vsync
    ) {
        if (vsync)
            return vk::PresentModeKHR::eFifo;
        for (const auto mode : modes)
            if (mode == vk::PresentModeKHR::eImmediate)
                return mode;
        for (const auto mode : modes)
            if (mode == vk::PresentModeKHR::eMailbox)
                return mode;
        return vk::PresentModeKHR::eFifo;
    }

    vk::CompositeAlphaFlagBitsKHR chooseAlpha(
        vk::CompositeAlphaFlagsKHR supported
    ) {
        if (supported & vk::CompositeAlphaFlagBitsKHR::eOpaque)
            return vk::CompositeAlphaFlagBitsKHR::eOpaque;
        if (supported & vk::CompositeAlphaFlagBitsKHR::ePreMultiplied)
            return vk::CompositeAlphaFlagBitsKHR::ePreMultiplied;
        if (supported & vk::CompositeAlphaFlagBitsKHR::ePostMultiplied)
            return vk::CompositeAlphaFlagBitsKHR::ePostMultiplied;
        return vk::CompositeAlphaFlagBitsKHR::eInherit;
    }

} // namespace

SdlPresenter::SdlPresenter(PresentOptions options)
    : options_{options}
{
    if (!SDL_Init(SDL_INIT_VIDEO))
        throw std::runtime_error(
            std::string("SDL init failed: ") + SDL_GetError());

    SDL_WindowFlags flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE;
    if (options_.fullscreen)
        flags |= SDL_WINDOW_FULLSCREEN;
    window_ = SDL_CreateWindow(
        options_.title.c_str(),
        static_cast<int>(options_.size.width),
        static_cast<int>(options_.size.height),
        flags);
    if (!window_)
        throw std::runtime_error(
            std::string("SDL window creation failed: ") + SDL_GetError());
}

SdlPresenter::~SdlPresenter()
{
    if (runtime_) {
        runtime_->device->waitIdle(runtime_->dld);
        destroySwapchain();
        pipeline_.destroy();
        available_ = {};
        renderFinished_ = {};
        fence_ = {};
        if (surface_) {
            runtime_->instance->destroySurfaceKHR(
                surface_, nullptr, runtime_->dld);
            surface_ = {};
        }
    }
    if (window_)
        SDL_DestroyWindow(window_);
    SDL_Quit();
}

std::vector<const char*> SdlPresenter::instanceExtensions() const
{
    Uint32 count{0};
    const char* const* names = SDL_Vulkan_GetInstanceExtensions(&count);
    if (!names)
        throw std::runtime_error(
            std::string("SDL Vulkan extensions failed: ") + SDL_GetError());
    return { names, names + count };
}

bool SdlPresenter::createSurface(
    vk::Instance instance,
    vk::SurfaceKHR& surface
) {
    VkSurfaceKHR raw{};
    if (!SDL_Vulkan_CreateSurface(
            window_, static_cast<VkInstance>(instance), nullptr, &raw))
        return false;
    surface_ = vk::SurfaceKHR(raw);
    surface = surface_;
    return true;
}

bool SdlPresenter::init(Runtime& runtime, std::string& error)
{
    runtime_ = &runtime;
    presentTiming_ = options_.presentTiming;
    if (!createSwapchain(error))
        return false;
    pipeline_.init(runtime, format_, vk::ImageLayout::ePresentSrcKHR);
    pipelineReady_ = true;
    if (!createFramebuffers(error))
        return false;

    available_ = runtime.device->createSemaphoreUnique({}, nullptr, runtime.dld);
    renderFinished_ = runtime.device->createSemaphoreUnique({}, nullptr, runtime.dld);
    fence_ = runtime.device->createFenceUnique(
        { .flags = vk::FenceCreateFlagBits::eSignaled },
        nullptr, runtime.dld);
    return true;
}

bool SdlPresenter::createSwapchain(std::string& error)
{
    const auto capabilities =
        runtime_->physical.getSurfaceCapabilitiesKHR(surface_, runtime_->dld);
    const auto formats =
        runtime_->physical.getSurfaceFormatsKHR(surface_, runtime_->dld);
    const auto modes =
        runtime_->physical.getSurfacePresentModesKHR(surface_, runtime_->dld);
    if (formats.empty() || modes.empty()) {
        error = "the display does not expose a usable Vulkan surface";
        return false;
    }

    const auto format = chooseFormat(formats);
    format_ = format.format;

    vk::Extent2D extent = capabilities.currentExtent;
    if (extent.width == UINT32_MAX) {
        int width{0};
        int height{0};
        SDL_GetWindowSizeInPixels(window_, &width, &height);
        extent.width = static_cast<uint32_t>(std::max(1, width));
        extent.height = static_cast<uint32_t>(std::max(1, height));
        extent.width = std::clamp(extent.width,
            capabilities.minImageExtent.width,
            capabilities.maxImageExtent.width);
        extent.height = std::clamp(extent.height,
            capabilities.minImageExtent.height,
            capabilities.maxImageExtent.height);
    }
    extent_ = extent;

    uint32_t imageCount = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount > 0)
        imageCount = std::min(imageCount, capabilities.maxImageCount);

    const vk::SwapchainCreateInfoKHR info{
        .surface = surface_,
        .minImageCount = imageCount,
        .imageFormat = format_,
        .imageColorSpace = format.colorSpace,
        .imageExtent = extent_,
        .imageArrayLayers = 1,
        .imageUsage = vk::ImageUsageFlagBits::eColorAttachment,
        .imageSharingMode = vk::SharingMode::eExclusive,
        .preTransform = capabilities.currentTransform,
        .compositeAlpha = chooseAlpha(capabilities.supportedCompositeAlpha),
        .presentMode = chooseMode(modes, options_.vsync),
        .clipped = true
    };
    swapchain_ = runtime_->device->createSwapchainKHRUnique(
        info, nullptr, runtime_->dld);
    images_ = runtime_->device->getSwapchainImagesKHR(
        *swapchain_, runtime_->dld);
    views_.clear();
    views_.reserve(images_.size());
    for (const auto image : images_) {
        views_.push_back(runtime_->device->createImageViewUnique({
            .image = image,
            .viewType = vk::ImageViewType::e2D,
            .format = format_,
            .subresourceRange = {
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .levelCount = 1,
                .layerCount = 1
            }
        }, nullptr, runtime_->dld));
    }
    return true;
}

bool SdlPresenter::createFramebuffers(std::string& error)
{
    framebuffers_.clear();
    framebuffers_.reserve(views_.size());
    for (const auto& view : views_) {
        framebuffers_.push_back(runtime_->device->createFramebufferUnique({
            .renderPass = pipeline_.renderPass(),
            .attachmentCount = 1,
            .pAttachments = &*view,
            .width = extent_.width,
            .height = extent_.height,
            .layers = 1
        }, nullptr, runtime_->dld));
    }
    if (framebuffers_.empty()) {
        error = "swapchain has no images";
        return false;
    }
    return true;
}

void SdlPresenter::destroySwapchain()
{
    framebuffers_.clear();
    views_.clear();
    swapchain_ = {};
    images_.clear();
}

bool SdlPresenter::recreateSwapchain(std::string& error)
{
    runtime_->device->waitIdle(runtime_->dld);
    destroySwapchain();
    if (!createSwapchain(error))
        return false;
    if (pipelineReady_) {
        pipeline_.destroy();
        pipeline_.init(*runtime_, format_, vk::ImageLayout::ePresentSrcKHR);
    }
    return createFramebuffers(error);
}

bool SdlPresenter::acquire(PresentTarget& target, std::string& error)
{
    if (resizePending_) {
        resizePending_ = false;
        if (!recreateSwapchain(error))
            return false;
    }

    uint32_t index{0};
    auto result = runtime_->device->acquireNextImageKHR(
        *swapchain_, UINT64_MAX, *available_, nullptr, &index, runtime_->dld);
    if (result == vk::Result::eErrorOutOfDateKHR) {
        if (!recreateSwapchain(error))
            return false;
        result = runtime_->device->acquireNextImageKHR(
            *swapchain_, UINT64_MAX, *available_, nullptr, &index,
            runtime_->dld);
    }
    if (result == vk::Result::eSuboptimalKHR) {
        // The image and semaphore are valid; recreate after this frame.
        resizePending_ = true;
    } else if (result != vk::Result::eSuccess) {
        error = "swapchain image acquisition failed";
        return false;
    }

    imageIndex_ = index;
    target.image = images_[index];
    target.view = *views_[index];
    target.extent = extent_;
    return true;
}

void SdlPresenter::record(
    Runtime& runtime,
    vk::CommandBuffer command,
    vk::ImageView source,
    vk::Extent2D sourceExtent
) {
    pipeline_.record(runtime, command, *framebuffers_[imageIndex_],
        extent_, source, sourceExtent);
}

bool SdlPresenter::submit(vk::CommandBuffer command, std::string& error)
{
    command.end(runtime_->dld);
    runtime_->device->resetFences(*fence_, runtime_->dld);

    const vk::PipelineStageFlags waitStage =
        vk::PipelineStageFlagBits::eColorAttachmentOutput;
    const vk::SubmitInfo submitInfo{
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &*available_,
        .pWaitDstStageMask = &waitStage,
        .commandBufferCount = 1,
        .pCommandBuffers = &command,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &*renderFinished_
    };
    runtime_->queue.submit(submitInfo, *fence_, runtime_->dld);

    uint64_t presentId = ++presentId_;
    const vk::PresentIdKHR presentIdInfo{
        .swapchainCount = 1,
        .pPresentIds = &presentId
    };
    const vk::PresentInfoKHR presentInfo{
        .pNext = presentTiming_ ? &presentIdInfo : nullptr,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &*renderFinished_,
        .swapchainCount = 1,
        .pSwapchains = &*swapchain_,
        .pImageIndices = &imageIndex_
    };
    auto result = runtime_->queue.presentKHR(
        presentInfo, runtime_->dld);
    if (result == vk::Result::eErrorOutOfDateKHR ||
        result == vk::Result::eSuboptimalKHR) {
        resizePending_ = true;
    } else if (result != vk::Result::eSuccess) {
        error = "present failed";
        return false;
    }

    if (presentTiming_) {
        result = runtime_->device->waitForPresentKHR(
            *swapchain_, presentId, UINT64_MAX, runtime_->dld);
        if (result == vk::Result::eErrorOutOfDateKHR ||
            result == vk::Result::eSuboptimalKHR) {
            resizePending_ = true;
        } else if (result != vk::Result::eSuccess) {
            error = "present wait failed";
            return false;
        }
    }

    const auto fenceResult = runtime_->device->waitForFences(
        *fence_, true, UINT64_MAX, runtime_->dld);
    if (fenceResult != vk::Result::eSuccess) {
        error = "present fence wait failed";
        return false;
    }
    return true;
}

void SdlPresenter::pollEvents()
{
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                close_ = true;
                break;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                resizePending_ = true;
                break;
            case SDL_EVENT_KEY_DOWN:
                if (event.key.key == SDLK_ESCAPE)
                    close_ = true;
                break;
            default:
                break;
        }
    }
}

bool SdlPresenter::saveFrame(const std::string&, std::string& error)
{
    error = "frame dump is only available with --headless";
    return false;
}

} // namespace openfg::scaler
