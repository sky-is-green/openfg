/* SPDX-License-Identifier: MIT */

#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

// Device-level entry points collected by the layer bootstrap.
struct LayerDeviceFns {
    VkDevice device{};
    VkPhysicalDevice physdev{};
    VkInstance instance{};
    uint32_t queueFamily{};

    PFN_vkGetDeviceProcAddr gdpa{};
    PFN_vkGetPhysicalDeviceProperties2 getPhysDevProperties2{};
    PFN_vkEnumerateDeviceExtensionProperties enumerateDeviceExtensions{};
    PFN_vkGetPhysicalDeviceMemoryProperties getPhysDevMemoryProperties{};
    PFN_vkGetPhysicalDeviceQueueFamilyProperties getQueueFamilyProperties{};
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR getSurfaceSupport{};
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR getSurfaceCapabilities{};

    PFN_vkCreateSwapchainKHR createSwapchain{};
    PFN_vkDestroySwapchainKHR destroySwapchain{};
    PFN_vkQueuePresentKHR queuePresent{};
    PFN_vkGetDeviceQueue getDeviceQueue{};
    PFN_vkGetDeviceQueue2 getDeviceQueue2{};
    PFN_vkGetSwapchainImagesKHR getSwapchainImages{};
    PFN_vkAcquireNextImageKHR acquireNextImage{};

    PFN_vkQueueSubmit queueSubmit{};
    PFN_vkCreateCommandPool createCommandPool{};
    PFN_vkDestroyCommandPool destroyCommandPool{};
    PFN_vkAllocateCommandBuffers allocateCommandBuffers{};
    PFN_vkResetCommandBuffer resetCommandBuffer{};
    PFN_vkBeginCommandBuffer beginCommandBuffer{};
    PFN_vkEndCommandBuffer endCommandBuffer{};
    PFN_vkCmdPipelineBarrier cmdPipelineBarrier{};
    PFN_vkCmdBlitImage cmdBlitImage{};

    PFN_vkCreateSemaphore createSemaphore{};
    PFN_vkDestroySemaphore destroySemaphore{};
    PFN_vkImportSemaphoreFdKHR importSemaphoreFd{};
    PFN_vkCreateFence createFence{};
    PFN_vkDestroyFence destroyFence{};
    PFN_vkWaitForFences waitForFences{};
    PFN_vkResetFences resetFences{};
    PFN_vkDeviceWaitIdle deviceWaitIdle{};

    PFN_vkCreateImage createImage{};
    PFN_vkDestroyImage destroyImage{};
    PFN_vkGetImageMemoryRequirements getImageMemoryRequirements{};
    PFN_vkAllocateMemory allocateMemory{};
    PFN_vkFreeMemory freeMemory{};
    PFN_vkBindImageMemory bindImageMemory{};

    bool externalMemoryFd{};
    bool externalSemaphoreFd{};
    bool externalDmaBuf{};
};

namespace openfg_layer {

struct SwapchainInfo {
    VkSwapchainKHR swapchain{};
    VkFormat format{VK_FORMAT_UNDEFINED};
    VkExtent2D extent{};
    uint32_t queueFamily{UINT32_MAX};
    std::vector<VkImage> images;
};

bool enabled();
bool debug();
unsigned multiplier();
bool crossGpu();

void addSwapchain(const LayerDeviceFns& fns, const SwapchainInfo& info);
void removeSwapchain(VkDevice device, VkSwapchainKHR swapchain);
void destroyDevice(VkDevice device);

VkResult present(
    const LayerDeviceFns& fns,
    VkQueue queue,
    uint32_t queueFamily,
    const VkPresentInfoKHR* presentInfo,
    PFN_vkQueuePresentKHR nextPresent
);

}
