/* SPDX-License-Identifier: MIT */

// Same-GPU frame generation for the clean-room layer.
//
// The first milestone is intentionally narrow: one backend context per device,
// two real layers, multiplier 2, OpaqueFd sharing. Anything unexpected falls
// back to a normal present rather than breaking the application.

#include "layer.hpp"
#include "openfg_backend.hpp"

#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

    using openfg_layer::SwapchainInfo;

    constexpr const char* TAG = "[openfg]";

    bool envFlag(const char* name) {
        const char* value = std::getenv(name);
        if (!value || !*value)
            return false;
        return std::strcmp(value, "0") != 0 &&
            std::strcmp(value, "false") != 0 &&
            std::strcmp(value, "off") != 0;
    }

    int envInt(const char* name, int fallback) {
        const char* value = std::getenv(name);
        if (!value || !*value)
            return fallback;
        return std::atoi(value);
    }

    bool hasExtension(
        const LayerDeviceFns& fns,
        const char* name
    ) {
        uint32_t count{0};
        if (fns.enumerateDeviceExtensions(fns.physdev, nullptr, &count, nullptr) != VK_SUCCESS)
            return false;
        std::vector<VkExtensionProperties> extensions(count);
        if (fns.enumerateDeviceExtensions(fns.physdev, nullptr, &count, extensions.data()) != VK_SUCCESS)
            return false;
        for (const auto& ext : extensions) {
            if (std::strcmp(ext.extensionName, name) == 0)
                return true;
        }
        return false;
    }

    uint32_t findMemoryType(
        const LayerDeviceFns& fns,
        uint32_t typeBits,
        bool hostVisible
    ) {
        VkPhysicalDeviceMemoryProperties props{};
        fns.getPhysDevMemoryProperties(fns.physdev, &props);

        int fallback{-1};
        for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
            if ((typeBits & (1u << i)) == 0)
                continue;
            const auto flags = props.memoryTypes[i].propertyFlags;
            if (hostVisible) {
                if ((flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0 ||
                    (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0)
                    continue;
            }
            if (!hostVisible && (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
                return i;
            if (fallback < 0)
                fallback = static_cast<int>(i);
        }
        return fallback < 0 ? 0u : static_cast<uint32_t>(fallback);
    }

    std::string identifyPhysicalDevice(const LayerDeviceFns& fns) {
        if (const char* env = std::getenv("OPENFG_GPU"); env && *env)
            return env;

        const bool hasPci = hasExtension(fns, VK_EXT_PCI_BUS_INFO_EXTENSION_NAME);
        VkPhysicalDevicePCIBusInfoPropertiesEXT pci{};
        pci.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT;

        VkPhysicalDeviceProperties2 props{};
        props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        props.pNext = hasPci ? &pci : nullptr;
        if (fns.getPhysDevProperties2)
            fns.getPhysDevProperties2(fns.physdev, &props);

        if (hasPci) {
            char buffer[64]{};
            std::snprintf(buffer, sizeof(buffer), "%04x:%02x:%02x.%x",
                pci.pciDomain, pci.pciBus, pci.pciDevice, pci.pciFunction);
            return buffer;
        }

        return props.properties.deviceName;
    }

    struct FgState {
        const LayerDeviceFns* fns{};
        VkSwapchainKHR swapchain{};
        VkExtent2D extent{};
        std::unique_ptr<openfg::Instance> instance;
        std::unique_ptr<openfg::Context> context;

        VkImage source{};
        VkDeviceMemory sourceMemory{};
        VkImage destination{};
        VkDeviceMemory destinationMemory{};
        VkSemaphore sync{};

        VkCommandPool pool{};
        VkCommandBuffer sourceCmd{};
        VkCommandBuffer destinationCmd{};
        VkSemaphore sourceDone{};
        VkSemaphore acquire{};
        VkSemaphore copyDone{};
        VkFence fence{};
        bool fencePending{};

        uint64_t syncValue{};
        uint32_t iteration{};
        uint32_t multiplier{2};
        bool crossGpu{};
        bool ready{};
        bool failed{};
        std::string error;
    };

    std::mutex g_mutex;
    std::map<VkDevice, std::unique_ptr<FgState>> g_states;
    std::map<VkSwapchainKHR, SwapchainInfo> g_swapchains;

    void setError(FgState& state, std::string message) {
        state.error = std::move(message);
        state.failed = true;
        std::fprintf(stderr, "%s %s\n", TAG, state.error.c_str());
    }

    void destroyState(FgState& state) {
        if (!state.fns)
            return;
        const auto& fns = *state.fns;
        if (state.fencePending && fns.waitForFences && state.fence) {
            fns.waitForFences(fns.device, 1, &state.fence, VK_TRUE, UINT64_MAX);
            state.fencePending = false;
        }

        if (fns.destroySemaphore) {
            for (VkSemaphore sem : { state.sourceDone, state.acquire, state.copyDone }) {
                if (sem)
                    fns.destroySemaphore(fns.device, sem, nullptr);
            }
            if (state.sync)
                fns.destroySemaphore(fns.device, state.sync, nullptr);
        }
        if (fns.destroyFence && state.fence)
            fns.destroyFence(fns.device, state.fence, nullptr);
        if (fns.destroyCommandPool && state.pool)
            fns.destroyCommandPool(fns.device, state.pool, nullptr);
        if (fns.destroyImage) {
            if (state.source)
                fns.destroyImage(fns.device, state.source, nullptr);
            if (state.destination)
                fns.destroyImage(fns.device, state.destination, nullptr);
        }
        if (fns.freeMemory) {
            if (state.sourceMemory)
                fns.freeMemory(fns.device, state.sourceMemory, nullptr);
            if (state.destinationMemory)
                fns.freeMemory(fns.device, state.destinationMemory, nullptr);
        }
    }

    bool importImage(
        FgState& state,
        int fd,
        uint32_t layers,
        bool destination
    ) {
        const auto& fns = *state.fns;
        const VkExternalMemoryHandleTypeFlagBits handleType = state.crossGpu
            ? VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT
            : VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

        VkExternalMemoryImageCreateInfo external{};
        external.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
        external.handleTypes = handleType;

        VkImageCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.pNext = &external;
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = VK_FORMAT_R8G8B8A8_UNORM;
        info.extent = { state.extent.width, state.extent.height, 1 };
        info.mipLevels = 1;
        info.arrayLayers = layers;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = state.crossGpu ? VK_IMAGE_TILING_LINEAR : VK_IMAGE_TILING_OPTIMAL;
        info.usage = destination
            ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT
            : VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkImage image{};
        if (fns.createImage(fns.device, &info, nullptr, &image) != VK_SUCCESS) {
            setError(state, "failed to create imported image");
            return false;
        }

        VkMemoryRequirements requirements{};
        fns.getImageMemoryRequirements(fns.device, image, &requirements);

        VkMemoryDedicatedAllocateInfo dedicated{};
        dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
        dedicated.image = image;

        VkImportMemoryFdInfoKHR import{};
        import.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
        import.pNext = &dedicated;
        import.handleType = handleType;
        import.fd = fd;

        VkMemoryAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.pNext = &import;
        alloc.allocationSize = requirements.size;
        alloc.memoryTypeIndex = findMemoryType(fns, requirements.memoryTypeBits, state.crossGpu);

        VkDeviceMemory memory{};
        if (fns.allocateMemory(fns.device, &alloc, nullptr, &memory) != VK_SUCCESS) {
            fns.destroyImage(fns.device, image, nullptr);
            setError(state, "failed to import image memory");
            return false;
        }
        if (fns.bindImageMemory(fns.device, image, memory, 0) != VK_SUCCESS) {
            fns.freeMemory(fns.device, memory, nullptr);
            fns.destroyImage(fns.device, image, nullptr);
            setError(state, "failed to bind imported image memory");
            return false;
        }

        if (destination) {
            state.destination = image;
            state.destinationMemory = memory;
        } else {
            state.source = image;
            state.sourceMemory = memory;
        }
        return true;
    }

    bool importSyncSemaphore(FgState& state, int fd) {
        const auto& fns = *state.fns;

        VkSemaphoreTypeCreateInfo type{};
        type.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
        type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        type.initialValue = 0;

        VkSemaphoreCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        info.pNext = &type;

        if (fns.createSemaphore(fns.device, &info, nullptr, &state.sync) != VK_SUCCESS) {
            setError(state, "failed to create timeline semaphore");
            return false;
        }

        VkImportSemaphoreFdInfoKHR import{};
        import.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR;
        import.semaphore = state.sync;
        import.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
        import.fd = fd;
        if (fns.importSemaphoreFd(fns.device, &import) != VK_SUCCESS) {
            setError(state, "failed to import timeline semaphore");
            return false;
        }
        return true;
    }

    bool createBinarySemaphore(const LayerDeviceFns& fns, VkSemaphore& semaphore) {
        VkSemaphoreCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        return fns.createSemaphore(fns.device, &info, nullptr, &semaphore) == VK_SUCCESS;
    }

    bool initializeState(FgState& state, const LayerDeviceFns& fns, const SwapchainInfo& swapchain) {
        state.fns = &fns;
        state.swapchain = swapchain.swapchain;
        state.extent = swapchain.extent;
        // One generated frame per real frame in this milestone.
        state.multiplier = 2;
        state.crossGpu = envFlag("OPENFG_CROSS_GPU");

        if (state.crossGpu && !fns.externalDmaBuf) {
            setError(state, "cross-GPU requested but VK_EXT_external_memory_dma_buf is unavailable");
            return false;
        }
        if (!fns.externalMemoryFd || !fns.externalSemaphoreFd) {
            setError(state, "external memory/semaphore FD is unavailable");
            return false;
        }

        const std::string deviceId = identifyPhysicalDevice(fns);
        std::fprintf(stderr, "%s backend on %s, %s, %ux\n", TAG,
            deviceId.c_str(), state.crossGpu ? "cross-GPU" : "same-GPU", state.multiplier);

        state.instance = std::make_unique<openfg::Instance>(
            deviceId,
            std::filesystem::path{},
            state.crossGpu,
            true,
            [](const std::string& message) {
                std::fprintf(stderr, "%s %s\n", TAG, message.c_str());
            }
        );
        state.context = std::make_unique<openfg::Context>(
            *state.instance,
            swapchain.extent.width,
            swapchain.extent.height,
            1.0F,
            false
        );

        const auto fds = state.context->exportFds();
        if (!importImage(state, fds.sourceFd, 2, false) ||
            !importImage(state, fds.destinationFd, 1, true) ||
            !importSyncSemaphore(state, fds.syncFd)) {
            return false;
        }

        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = fns.queueFamily;
        if (fns.createCommandPool(fns.device, &poolInfo, nullptr, &state.pool) != VK_SUCCESS) {
            setError(state, "failed to create command pool");
            return false;
        }

        VkCommandBufferAllocateInfo cmdInfo{};
        cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cmdInfo.commandPool = state.pool;
        cmdInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmdInfo.commandBufferCount = 2;
        VkCommandBuffer cmds[2]{};
        if (fns.allocateCommandBuffers(fns.device, &cmdInfo, cmds) != VK_SUCCESS) {
            setError(state, "failed to allocate command buffers");
            return false;
        }
        state.sourceCmd = cmds[0];
        state.destinationCmd = cmds[1];

        if (!createBinarySemaphore(fns, state.sourceDone) ||
            !createBinarySemaphore(fns, state.acquire) ||
            !createBinarySemaphore(fns, state.copyDone)) {
            setError(state, "failed to create semaphores");
            return false;
        }

        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (fns.createFence(fns.device, &fenceInfo, nullptr, &state.fence) != VK_SUCCESS) {
            setError(state, "failed to create fence");
            return false;
        }

        state.ready = true;
        return true;
    }

    void imageBarrier(
        const LayerDeviceFns& fns,
        VkCommandBuffer cmd,
        VkImage image,
        VkImageLayout oldLayout,
        VkImageLayout newLayout,
        VkAccessFlags srcAccess,
        VkAccessFlags dstAccess,
        uint32_t baseLayer,
        uint32_t layerCount
    ) {
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcAccessMask = srcAccess;
        barrier.dstAccessMask = dstAccess;
        barrier.oldLayout = oldLayout;
        barrier.newLayout = newLayout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange = {
            VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, baseLayer, layerCount
        };
        fns.cmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

    void recordCopy(
        const LayerDeviceFns& fns,
        VkCommandBuffer cmd,
        VkImage src,
        uint32_t srcLayer,
        VkImage dst,
        uint32_t dstLayer,
        VkExtent2D extent,
        bool sourceFromPresent,
        bool destinationToPresent
    ) {
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        fns.beginCommandBuffer(cmd, &begin);

        if (sourceFromPresent) {
            imageBarrier(fns, cmd, src,
                VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                0, VK_ACCESS_TRANSFER_READ_BIT, srcLayer, 1);
        } else {
            imageBarrier(fns, cmd, src,
                VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                0, VK_ACCESS_TRANSFER_READ_BIT, srcLayer, 1);
        }

        if (destinationToPresent) {
            imageBarrier(fns, cmd, dst,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                0, VK_ACCESS_TRANSFER_WRITE_BIT, dstLayer, 1);
        } else {
            imageBarrier(fns, cmd, dst,
                VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                0, VK_ACCESS_TRANSFER_WRITE_BIT, dstLayer, 1);
        }

        VkImageBlit blit{};
        blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, srcLayer, 1 };
        blit.srcOffsets[0] = { 0, 0, 0 };
        blit.srcOffsets[1] = {
            static_cast<int32_t>(extent.width),
            static_cast<int32_t>(extent.height), 1
        };
        blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, dstLayer, 1 };
        blit.dstOffsets[0] = { 0, 0, 0 };
        blit.dstOffsets[1] = {
            static_cast<int32_t>(extent.width),
            static_cast<int32_t>(extent.height), 1
        };
        fns.cmdBlitImage(cmd,
            src, sourceFromPresent ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL,
            dst, destinationToPresent ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL,
            1, &blit, VK_FILTER_NEAREST);

        if (sourceFromPresent) {
            imageBarrier(fns, cmd, src,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_ACCESS_TRANSFER_READ_BIT, 0, srcLayer, 1);
        }
        if (destinationToPresent) {
            imageBarrier(fns, cmd, dst,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_ACCESS_TRANSFER_WRITE_BIT, 0, dstLayer, 1);
        }

        fns.endCommandBuffer(cmd);
    }

}

namespace openfg_layer {

bool enabled() {
    return envFlag("OPENFG_ENABLE");
}

void addSwapchain(const LayerDeviceFns&, const SwapchainInfo& info) {
    std::lock_guard lock(g_mutex);
    g_swapchains[info.swapchain] = info;
}

void removeSwapchain(VkDevice device, VkSwapchainKHR swapchain) {
    std::lock_guard lock(g_mutex);
    auto it = g_states.find(device);
    if (it != g_states.end() && it->second->swapchain == swapchain) {
        destroyState(*it->second);
        g_states.erase(it);
    }
    g_swapchains.erase(swapchain);
}

void destroyDevice(VkDevice device) {
    std::lock_guard lock(g_mutex);
    auto it = g_states.find(device);
    if (it != g_states.end()) {
        destroyState(*it->second);
        g_states.erase(it);
    }
}

VkResult present(
    const LayerDeviceFns& fns,
    VkQueue queue,
    const VkPresentInfoKHR* presentInfo,
    PFN_vkQueuePresentKHR nextPresent
) {
    if (!enabled() || !presentInfo || presentInfo->swapchainCount != 1 ||
        !presentInfo->pSwapchains || !presentInfo->pImageIndices)
        return nextPresent(queue, presentInfo);

    std::lock_guard lock(g_mutex);

    const VkSwapchainKHR swapchain = presentInfo->pSwapchains[0];
    const auto swapIt = g_swapchains.find(swapchain);
    if (swapIt == g_swapchains.end())
        return nextPresent(queue, presentInfo);
    const SwapchainInfo& swapInfo = swapIt->second;

    FgState* state{};
    auto stateIt = g_states.find(fns.device);
    if (stateIt == g_states.end()) {
        auto newState = std::make_unique<FgState>();
        state = newState.get();
        g_states[fns.device] = std::move(newState);
    } else {
        state = stateIt->second.get();
    }

    state->fns = &fns;

    if (state->failed)
        return nextPresent(queue, presentInfo);

    if (!state->ready) {
        if (state->swapchain != swapchain) {
            destroyState(*state);
            *state = FgState{};
        }
        if (!initializeState(*state, fns, swapInfo))
            return nextPresent(queue, presentInfo);
    }

    if (state->fencePending) {
        if (fns.waitForFences(fns.device, 1, &state->fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS)
            return nextPresent(queue, presentInfo);
        fns.resetFences(fns.device, 1, &state->fence);
        state->fencePending = false;
    }

    // Acquire the extra swapchain image before consuming the application's
    // present semaphores, so a failure can still fall back cleanly.
    uint32_t generatedIndex{};
    VkResult acquireResult = fns.acquireNextImage(
        fns.device, swapchain, UINT64_MAX, state->acquire, VK_NULL_HANDLE, &generatedIndex);
    if (acquireResult != VK_SUCCESS && acquireResult != VK_SUBOPTIMAL_KHR)
        return nextPresent(queue, presentInfo);
    if (generatedIndex >= swapInfo.images.size())
        return nextPresent(queue, presentInfo);

    const uint32_t layer = state->iteration % 2;
    if (envFlag("OPENFG_DEBUG"))
        std::fprintf(stderr, "%s frame %u: real=%u generated=%u layer=%u\n",
            TAG, state->iteration, presentInfo->pImageIndices[0], generatedIndex, layer);
    recordCopy(fns, state->sourceCmd,
        swapInfo.images[presentInfo->pImageIndices[0]], 0,
        state->source, layer,
        swapInfo.extent, true, false);

    const uint64_t sourceSync = state->syncValue + 1;
    const uint64_t generatedSync = sourceSync + 1;

    std::vector<uint64_t> waitValues(presentInfo->waitSemaphoreCount, 0);
    std::vector<VkPipelineStageFlags> waitStages(
        presentInfo->waitSemaphoreCount, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);

    VkSemaphore sourceSignals[2]{ state->sourceDone, state->sync };
    uint64_t sourceSignalValues[2]{ 0, sourceSync };

    VkTimelineSemaphoreSubmitInfo sourceTimeline{};
    sourceTimeline.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
    sourceTimeline.waitSemaphoreValueCount = presentInfo->waitSemaphoreCount;
    sourceTimeline.pWaitSemaphoreValues = waitValues.data();
    sourceTimeline.signalSemaphoreValueCount = 2;
    sourceTimeline.pSignalSemaphoreValues = sourceSignalValues;

    VkSubmitInfo sourceSubmit{};
    sourceSubmit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    sourceSubmit.pNext = &sourceTimeline;
    sourceSubmit.waitSemaphoreCount = presentInfo->waitSemaphoreCount;
    sourceSubmit.pWaitSemaphores = presentInfo->pWaitSemaphores;
    sourceSubmit.pWaitDstStageMask = waitStages.data();
    sourceSubmit.commandBufferCount = 1;
    sourceSubmit.pCommandBuffers = &state->sourceCmd;
    sourceSubmit.signalSemaphoreCount = 2;
    sourceSubmit.pSignalSemaphores = sourceSignals;
    if (fns.queueSubmit(queue, 1, &sourceSubmit, VK_NULL_HANDLE) != VK_SUCCESS) {
        setError(*state, "failed to submit source copy");
        return nextPresent(queue, presentInfo);
    }

    state->context->dispatch(1);
    state->context->acquire();

    recordCopy(fns, state->destinationCmd,
        state->destination, 0,
        swapInfo.images[generatedIndex], 0,
        swapInfo.extent, false, true);

    VkSemaphore copyWaits[2]{ state->acquire, state->sync };
    uint64_t copyWaitValues[2]{ 0, generatedSync };

    VkTimelineSemaphoreSubmitInfo copyTimeline{};
    copyTimeline.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
    copyTimeline.waitSemaphoreValueCount = 2;
    copyTimeline.pWaitSemaphoreValues = copyWaitValues;
    copyTimeline.signalSemaphoreValueCount = 1;
    uint64_t zeroSignal{0};
    copyTimeline.pSignalSemaphoreValues = &zeroSignal;

    std::vector<VkPipelineStageFlags> copyStages(
        2, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);

    VkSubmitInfo copySubmit{};
    copySubmit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    copySubmit.pNext = &copyTimeline;
    copySubmit.waitSemaphoreCount = 2;
    copySubmit.pWaitSemaphores = copyWaits;
    copySubmit.pWaitDstStageMask = copyStages.data();
    copySubmit.commandBufferCount = 1;
    copySubmit.pCommandBuffers = &state->destinationCmd;
    copySubmit.signalSemaphoreCount = 1;
    copySubmit.pSignalSemaphores = &state->copyDone;
    if (fns.queueSubmit(queue, 1, &copySubmit, state->fence) != VK_SUCCESS) {
        setError(*state, "failed to submit generated frame copy");
        return nextPresent(queue, presentInfo);
    }
    state->fencePending = true;

    VkPresentInfoKHR generatedPresent = *presentInfo;
    generatedPresent.pNext = presentInfo->pNext;
    generatedPresent.waitSemaphoreCount = 1;
    generatedPresent.pWaitSemaphores = &state->copyDone;
    generatedPresent.swapchainCount = 1;
    generatedPresent.pSwapchains = &swapchain;
    generatedPresent.pImageIndices = &generatedIndex;
    generatedPresent.pResults = nullptr;
    const VkResult generatedResult = nextPresent(queue, &generatedPresent);

    VkPresentInfoKHR originalPresent = *presentInfo;
    originalPresent.waitSemaphoreCount = 1;
    originalPresent.pWaitSemaphores = &state->sourceDone;
    const VkResult originalResult = nextPresent(queue, &originalPresent);

    state->syncValue = generatedSync;
    state->iteration++;

    if (envFlag("OPENFG_DEBUG"))
        std::fprintf(stderr, "%s frame %u: present gen=%d original=%d\n",
            TAG, state->iteration, generatedResult, originalResult);

    if (generatedResult != VK_SUCCESS && generatedResult != VK_SUBOPTIMAL_KHR)
        return generatedResult;
    return originalResult;
}

}
