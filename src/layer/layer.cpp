/* SPDX-License-Identifier: MIT */

// Vulkan layer bootstrap for openfg.
//
// Loader interface and instance/device/swapchain/present hooks. Frame
// generation itself lives in fg.cpp; this file only records what the hooks
// need and forwards the rest to the next layer/driver.

#include "layer.hpp"

#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

    struct InstanceData {
        VkInstance instance{};
        PFN_vkGetInstanceProcAddr gipa{};
        PFN_vkGetDeviceProcAddr gdpa{};
        PFN_vkCreateDevice createDevice{};
        PFN_vkDestroyInstance destroyInstance{};
    };

    struct DeviceData {
        LayerDeviceFns fns{};
    };

    std::mutex g_mutex;
    std::unordered_map<VkInstance, InstanceData> g_instances;
    std::unordered_map<VkDevice, DeviceData> g_devices;
    std::unordered_map<VkQueue, VkDevice> g_queues;
    std::unordered_map<VkQueue, uint32_t> g_queueFamilies;
    VkInstance g_lastInstance{};

    template<typename T>
    T* findLayerStruct(void* next, VkStructureType type, VkLayerFunction function) {
        auto* current = static_cast<VkBaseOutStructure*>(next);
        while (current) {
            if (current->sType == type) {
                auto* info = reinterpret_cast<T*>(current);
                if (info->function == function)
                    return info;
            }
            current = current->pNext;
        }
        return nullptr;
    }

    InstanceData* getInstanceData(VkInstance instance) {
        std::lock_guard lock(g_mutex);
        auto it = g_instances.find(instance);
        return it == g_instances.end() ? nullptr : &it->second;
    }

    DeviceData* getDeviceData(VkDevice device) {
        std::lock_guard lock(g_mutex);
        auto it = g_devices.find(device);
        return it == g_devices.end() ? nullptr : &it->second;
    }

    std::unordered_set<std::string> deviceExtensions(
        PFN_vkGetInstanceProcAddr gipa,
        VkInstance instance,
        VkPhysicalDevice physdev
    ) {
        std::unordered_set<std::string> result;
        const auto enumerate = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
            gipa(instance, "vkEnumerateDeviceExtensionProperties"));
        if (!enumerate)
            return result;

        uint32_t count{0};
        if (enumerate(physdev, nullptr, &count, nullptr) != VK_SUCCESS)
            return result;
        std::vector<VkExtensionProperties> props(count);
        if (enumerate(physdev, nullptr, &count, props.data()) != VK_SUCCESS)
            return result;
        for (const auto& prop : props)
            result.emplace(prop.extensionName);
        return result;
    }

    /* Hooks */

    VKAPI_ATTR VkResult VKAPI_CALL hookCreateInstance(
        const VkInstanceCreateInfo* createInfo,
        const VkAllocationCallbacks* allocator,
        VkInstance* instance
    ) {
        auto* chain = findLayerStruct<VkLayerInstanceCreateInfo>(
            const_cast<void*>(createInfo->pNext),
            VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO,
            VK_LAYER_LINK_INFO
        );
        if (!chain || !chain->u.pLayerInfo)
            return VK_ERROR_INITIALIZATION_FAILED;

        auto* link = chain->u.pLayerInfo;
        const auto nextGipa = link->pfnNextGetInstanceProcAddr;
        chain->u.pLayerInfo = link->pNext;

        const auto create = reinterpret_cast<PFN_vkCreateInstance>(
            nextGipa(VK_NULL_HANDLE, "vkCreateInstance"));
        if (!create)
            return VK_ERROR_INITIALIZATION_FAILED;

        const VkResult result = create(createInfo, allocator, instance);
        if (result != VK_SUCCESS)
            return result;

        InstanceData data{};
        data.instance = *instance;
        data.gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
            nextGipa(*instance, "vkGetInstanceProcAddr"));
        data.gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
            nextGipa(*instance, "vkGetDeviceProcAddr"));
        data.createDevice = reinterpret_cast<PFN_vkCreateDevice>(
            nextGipa(*instance, "vkCreateDevice"));
        data.destroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(
            nextGipa(*instance, "vkDestroyInstance"));

        std::lock_guard lock(g_mutex);
        g_instances[*instance] = data;
        g_lastInstance = *instance;
        return VK_SUCCESS;
    }

    VKAPI_ATTR VkResult VKAPI_CALL hookCreateDevice(
        VkPhysicalDevice physdev,
        const VkDeviceCreateInfo* createInfo,
        const VkAllocationCallbacks* allocator,
        VkDevice* device
    ) {
        auto* chain = findLayerStruct<VkLayerDeviceCreateInfo>(
            const_cast<void*>(createInfo->pNext),
            VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO,
            VK_LAYER_LINK_INFO
        );
        if (!chain || !chain->u.pLayerInfo)
            return VK_ERROR_INITIALIZATION_FAILED;

        auto* link = chain->u.pLayerInfo;
        const auto nextGipa = link->pfnNextGetInstanceProcAddr;
        const auto nextGdpa = link->pfnNextGetDeviceProcAddr;
        chain->u.pLayerInfo = link->pNext;

        VkInstance instance{};
        {
            std::lock_guard lock(g_mutex);
            instance = g_lastInstance;
        }

        // Add the external-memory, semaphore and timeline support the layer
        // needs. Anything already requested by the application is preserved.
        const auto supported = deviceExtensions(nextGipa, instance, physdev);

        std::vector<const char*> extensions;
        if (createInfo->ppEnabledExtensionNames) {
            for (uint32_t i = 0; i < createInfo->enabledExtensionCount; i++)
                extensions.push_back(createInfo->ppEnabledExtensionNames[i]);
        }
        auto addExtension = [&](const char* name) {
            if (supported.find(name) == supported.end())
                return;
            for (const char* existing : extensions) {
                if (std::strcmp(existing, name) == 0)
                    return;
            }
            extensions.push_back(name);
        };
        const bool fgEnabled = openfg_layer::enabled();
        if (fgEnabled) {
            addExtension(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
            addExtension(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);
            addExtension(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
            if (openfg_layer::crossGpu())
                addExtension(VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
        }

        VkDeviceCreateInfo modified = *createInfo;
        modified.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        modified.ppEnabledExtensionNames = extensions.data();

        VkPhysicalDeviceVulkan12Features features{};
        features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        if (fgEnabled) {
            bool haveFeatures{false};
            auto* pnext = const_cast<VkBaseOutStructure*>(
                static_cast<const VkBaseOutStructure*>(modified.pNext));
            while (pnext) {
                if (pnext->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) {
                    reinterpret_cast<VkPhysicalDeviceVulkan12Features*>(pnext)->timelineSemaphore = VK_TRUE;
                    haveFeatures = true;
                    break;
                }
                pnext = pnext->pNext;
            }
            if (!haveFeatures) {
                features.timelineSemaphore = VK_TRUE;
                features.pNext = const_cast<void*>(modified.pNext);
                modified.pNext = &features;
            }
        }

        const auto create = reinterpret_cast<PFN_vkCreateDevice>(
            nextGipa(instance, "vkCreateDevice"));
        if (!create)
            return VK_ERROR_INITIALIZATION_FAILED;

        const VkResult result = create(physdev, &modified, allocator, device);
        if (result != VK_SUCCESS)
            return result;

        LayerDeviceFns fns{};
        fns.device = *device;
        fns.physdev = physdev;
        fns.instance = instance;
        fns.gdpa = nextGdpa;

        fns.getPhysDevProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
            nextGipa(instance, "vkGetPhysicalDeviceProperties2"));
        if (!fns.getPhysDevProperties2) {
            fns.getPhysDevProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
                nextGipa(instance, "vkGetPhysicalDeviceProperties2KHR"));
        }
        fns.enumerateDeviceExtensions = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
            nextGipa(instance, "vkEnumerateDeviceExtensionProperties"));
        fns.getPhysDevMemoryProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
            nextGipa(instance, "vkGetPhysicalDeviceMemoryProperties"));
        fns.getQueueFamilyProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
            nextGipa(instance, "vkGetPhysicalDeviceQueueFamilyProperties"));
        fns.getSurfaceSupport = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceSupportKHR>(
            nextGipa(instance, "vkGetPhysicalDeviceSurfaceSupportKHR"));
        fns.getSurfaceCapabilities = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(
            nextGipa(instance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"));

        const auto getDeviceProc = [&](const char* name) {
            return nextGdpa(*device, name);
        };
        fns.createSwapchain = reinterpret_cast<PFN_vkCreateSwapchainKHR>(
            getDeviceProc("vkCreateSwapchainKHR"));
        fns.destroySwapchain = reinterpret_cast<PFN_vkDestroySwapchainKHR>(
            getDeviceProc("vkDestroySwapchainKHR"));
        fns.queuePresent = reinterpret_cast<PFN_vkQueuePresentKHR>(
            getDeviceProc("vkQueuePresentKHR"));
        fns.getSwapchainImages = reinterpret_cast<PFN_vkGetSwapchainImagesKHR>(
            getDeviceProc("vkGetSwapchainImagesKHR"));
        fns.acquireNextImage = reinterpret_cast<PFN_vkAcquireNextImageKHR>(
            getDeviceProc("vkAcquireNextImageKHR"));
        fns.getDeviceQueue = reinterpret_cast<PFN_vkGetDeviceQueue>(
            getDeviceProc("vkGetDeviceQueue"));
        fns.getDeviceQueue2 = reinterpret_cast<PFN_vkGetDeviceQueue2>(
            getDeviceProc("vkGetDeviceQueue2"));
        fns.queueSubmit = reinterpret_cast<PFN_vkQueueSubmit>(
            getDeviceProc("vkQueueSubmit"));
        fns.createCommandPool = reinterpret_cast<PFN_vkCreateCommandPool>(
            getDeviceProc("vkCreateCommandPool"));
        fns.destroyCommandPool = reinterpret_cast<PFN_vkDestroyCommandPool>(
            getDeviceProc("vkDestroyCommandPool"));
        fns.allocateCommandBuffers = reinterpret_cast<PFN_vkAllocateCommandBuffers>(
            getDeviceProc("vkAllocateCommandBuffers"));
        fns.resetCommandBuffer = reinterpret_cast<PFN_vkResetCommandBuffer>(
            getDeviceProc("vkResetCommandBuffer"));
        fns.beginCommandBuffer = reinterpret_cast<PFN_vkBeginCommandBuffer>(
            getDeviceProc("vkBeginCommandBuffer"));
        fns.endCommandBuffer = reinterpret_cast<PFN_vkEndCommandBuffer>(
            getDeviceProc("vkEndCommandBuffer"));
        fns.cmdPipelineBarrier = reinterpret_cast<PFN_vkCmdPipelineBarrier>(
            getDeviceProc("vkCmdPipelineBarrier"));
        fns.cmdBlitImage = reinterpret_cast<PFN_vkCmdBlitImage>(
            getDeviceProc("vkCmdBlitImage"));
        fns.createSemaphore = reinterpret_cast<PFN_vkCreateSemaphore>(
            getDeviceProc("vkCreateSemaphore"));
        fns.destroySemaphore = reinterpret_cast<PFN_vkDestroySemaphore>(
            getDeviceProc("vkDestroySemaphore"));
        fns.importSemaphoreFd = reinterpret_cast<PFN_vkImportSemaphoreFdKHR>(
            getDeviceProc("vkImportSemaphoreFdKHR"));
        fns.createFence = reinterpret_cast<PFN_vkCreateFence>(
            getDeviceProc("vkCreateFence"));
        fns.destroyFence = reinterpret_cast<PFN_vkDestroyFence>(
            getDeviceProc("vkDestroyFence"));
        fns.waitForFences = reinterpret_cast<PFN_vkWaitForFences>(
            getDeviceProc("vkWaitForFences"));
        fns.resetFences = reinterpret_cast<PFN_vkResetFences>(
            getDeviceProc("vkResetFences"));
        fns.deviceWaitIdle = reinterpret_cast<PFN_vkDeviceWaitIdle>(
            getDeviceProc("vkDeviceWaitIdle"));
        fns.createImage = reinterpret_cast<PFN_vkCreateImage>(
            getDeviceProc("vkCreateImage"));
        fns.destroyImage = reinterpret_cast<PFN_vkDestroyImage>(
            getDeviceProc("vkDestroyImage"));
        fns.getImageMemoryRequirements = reinterpret_cast<PFN_vkGetImageMemoryRequirements>(
            getDeviceProc("vkGetImageMemoryRequirements"));
        fns.allocateMemory = reinterpret_cast<PFN_vkAllocateMemory>(
            getDeviceProc("vkAllocateMemory"));
        fns.freeMemory = reinterpret_cast<PFN_vkFreeMemory>(
            getDeviceProc("vkFreeMemory"));
        fns.bindImageMemory = reinterpret_cast<PFN_vkBindImageMemory>(
            getDeviceProc("vkBindImageMemory"));
        fns.externalMemoryFd = supported.count(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME) != 0;
        fns.externalSemaphoreFd = supported.count(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME) != 0;
        fns.externalDmaBuf = supported.count(VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME) != 0;

        // Preferred family for the layer's command buffers. The swapchain
        // hook refines this to the family that actually supports present.
        if (fns.getQueueFamilyProperties) {
            uint32_t count{0};
            fns.getQueueFamilyProperties(physdev, &count, nullptr);
            std::vector<VkQueueFamilyProperties> families(count);
            fns.getQueueFamilyProperties(physdev, &count, families.data());
            for (uint32_t i = 0; i < count; i++) {
                if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                    fns.queueFamily = i;
                    break;
                }
            }
        }

        std::lock_guard lock(g_mutex);
        g_devices[*device] = DeviceData{ .fns = fns };
        return VK_SUCCESS;
    }

    VKAPI_ATTR VkResult VKAPI_CALL hookCreateSwapchain(
        VkDevice device,
        const VkSwapchainCreateInfoKHR* createInfo,
        const VkAllocationCallbacks* allocator,
        VkSwapchainKHR* swapchain
    ) {
        auto* data = getDeviceData(device);
        if (!data || !data->fns.createSwapchain)
            return VK_ERROR_INITIALIZATION_FAILED;

        const bool fgEnabled = openfg_layer::enabled();
        const uint32_t multiplier = fgEnabled ? openfg_layer::multiplier() : 1;

        VkSwapchainCreateInfoKHR modified = *createInfo;
        if (fgEnabled) {
            modified.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                VK_IMAGE_USAGE_TRANSFER_DST_BIT;

            uint32_t images = modified.minImageCount + multiplier;
            bool haveCaps{false};
            if (data->fns.getSurfaceCapabilities) {
                VkSurfaceCapabilitiesKHR caps{};
                if (data->fns.getSurfaceCapabilities(
                        data->fns.physdev, modified.surface, &caps) == VK_SUCCESS) {
                    haveCaps = true;
                    if (caps.maxImageCount > 0)
                        images = std::min(images, caps.maxImageCount);
                }
            }
            if (!haveCaps)
                images = std::min(images, 8u);
            modified.minImageCount = std::max(modified.minImageCount, images);
        }

        // Pick the present family that can also run the layer's blits. If the
        // application presents from another family, the layer falls back.
        uint32_t presentFamily{UINT32_MAX};
        if (fgEnabled && data->fns.getSurfaceSupport &&
            data->fns.getQueueFamilyProperties) {
            uint32_t count{0};
            data->fns.getQueueFamilyProperties(data->fns.physdev, &count, nullptr);
            std::vector<VkQueueFamilyProperties> families(count);
            data->fns.getQueueFamilyProperties(data->fns.physdev, &count, families.data());

            auto supports = [&](uint32_t family) {
                if ((families[family].queueFlags & VK_QUEUE_TRANSFER_BIT) == 0)
                    return false;
                VkBool32 supported{VK_FALSE};
                data->fns.getSurfaceSupport(
                    data->fns.physdev, family, modified.surface, &supported);
                return supported == VK_TRUE;
            };
            if (data->fns.queueFamily < count && supports(data->fns.queueFamily)) {
                presentFamily = data->fns.queueFamily;
            } else {
                for (uint32_t i = 0; i < count; i++) {
                    if (supports(i)) {
                        presentFamily = i;
                        break;
                    }
                }
            }
        }

        const VkResult result = data->fns.createSwapchain(device, &modified, allocator, swapchain);
        if (result != VK_SUCCESS)
            return result;

        uint32_t count{0};
        if (data->fns.getSwapchainImages(device, *swapchain, &count, nullptr) != VK_SUCCESS)
            return VK_SUCCESS;
        std::vector<VkImage> images(count);
        if (data->fns.getSwapchainImages(device, *swapchain, &count, images.data()) != VK_SUCCESS)
            return VK_SUCCESS;

        if (openfg_layer::debug())
            std::fprintf(stderr,
                "[openfg] swapchain: %ux%u, minImageCount=%u, family=%u\n",
                modified.imageExtent.width, modified.imageExtent.height,
                modified.minImageCount, presentFamily);

        openfg_layer::SwapchainInfo info{};
        info.device = device;
        info.swapchain = *swapchain;
        info.format = modified.imageFormat;
        info.extent = modified.imageExtent;
        info.queueFamily = presentFamily;
        info.images = std::move(images);
        openfg_layer::addSwapchain(data->fns, info);
        return VK_SUCCESS;
    }

    VKAPI_ATTR void VKAPI_CALL hookDestroySwapchain(
        VkDevice device,
        VkSwapchainKHR swapchain,
        const VkAllocationCallbacks* allocator
    ) {
        openfg_layer::removeSwapchain(device, swapchain);
        auto* data = getDeviceData(device);
        if (data && data->fns.destroySwapchain)
            data->fns.destroySwapchain(device, swapchain, allocator);
    }

    VKAPI_ATTR void VKAPI_CALL hookGetDeviceQueue(
        VkDevice device,
        uint32_t queueFamilyIndex,
        uint32_t queueIndex,
        VkQueue* queue
    ) {
        auto* data = getDeviceData(device);
        if (!data || !data->fns.getDeviceQueue)
            return;
        data->fns.getDeviceQueue(device, queueFamilyIndex, queueIndex, queue);
        if (*queue) {
            std::lock_guard lock(g_mutex);
            g_queues[*queue] = device;
            g_queueFamilies[*queue] = queueFamilyIndex;
        }
    }

    VKAPI_ATTR void VKAPI_CALL hookGetDeviceQueue2(
        VkDevice device,
        const VkDeviceQueueInfo2* info,
        VkQueue* queue
    ) {
        auto* data = getDeviceData(device);
        if (!data || !data->fns.getDeviceQueue2)
            return;
        data->fns.getDeviceQueue2(device, info, queue);
        if (*queue) {
            std::lock_guard lock(g_mutex);
            g_queues[*queue] = device;
            g_queueFamilies[*queue] = info->queueFamilyIndex;
        }
    }

    VKAPI_ATTR VkResult VKAPI_CALL hookQueuePresent(
        VkQueue queue,
        const VkPresentInfoKHR* presentInfo
    ) {
        VkDevice device{};
        uint32_t queueFamily{UINT32_MAX};
        {
            std::lock_guard lock(g_mutex);
            auto it = g_queues.find(queue);
            if (it != g_queues.end())
                device = it->second;
            auto fit = g_queueFamilies.find(queue);
            if (fit != g_queueFamilies.end())
                queueFamily = fit->second;
        }
        auto* data = getDeviceData(device);
        if (!data || !data->fns.queuePresent)
            return VK_ERROR_INITIALIZATION_FAILED;

        if (openfg_layer::enabled())
            return openfg_layer::present(
                data->fns, queue, queueFamily, presentInfo, data->fns.queuePresent);
        return data->fns.queuePresent(queue, presentInfo);
    }

    VKAPI_ATTR void VKAPI_CALL hookDestroyDevice(
        VkDevice device,
        const VkAllocationCallbacks* allocator
    ) {
        openfg_layer::destroyDevice(device);
        PFN_vkDestroyDevice destroy{};
        {
            std::lock_guard lock(g_mutex);
            auto it = g_devices.find(device);
            if (it != g_devices.end()) {
                destroy = reinterpret_cast<PFN_vkDestroyDevice>(
                    it->second.fns.gdpa(device, "vkDestroyDevice"));
                for (auto qit = g_queues.begin(); qit != g_queues.end();) {
                    if (qit->second == device) {
                        g_queueFamilies.erase(qit->first);
                        qit = g_queues.erase(qit);
                    } else {
                        ++qit;
                    }
                }
                g_devices.erase(it);
            }
        }
        if (destroy)
            destroy(device, allocator);
    }

    VKAPI_ATTR void VKAPI_CALL hookDestroyInstance(
        VkInstance instance,
        const VkAllocationCallbacks* allocator
    ) {
        PFN_vkDestroyInstance destroy{};
        {
            std::lock_guard lock(g_mutex);
            auto it = g_instances.find(instance);
            if (it != g_instances.end()) {
                destroy = it->second.destroyInstance;
                g_instances.erase(it);
            }
        }
        if (destroy)
            destroy(instance, allocator);
    }

    /* Dispatch */

    VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL layerGetDeviceProcAddr(
        VkDevice device,
        const char* name
    );

    VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL layerGetInstanceProcAddr(
        VkInstance instance,
        const char* name
    ) {
        if (!name)
            return nullptr;

        if (std::strcmp(name, "vkGetInstanceProcAddr") == 0)
            return reinterpret_cast<PFN_vkVoidFunction>(layerGetInstanceProcAddr);
        if (std::strcmp(name, "vkGetDeviceProcAddr") == 0)
            return reinterpret_cast<PFN_vkVoidFunction>(layerGetDeviceProcAddr);
        if (std::strcmp(name, "vkCreateInstance") == 0)
            return reinterpret_cast<PFN_vkVoidFunction>(hookCreateInstance);
        if (std::strcmp(name, "vkCreateDevice") == 0)
            return reinterpret_cast<PFN_vkVoidFunction>(hookCreateDevice);
        if (std::strcmp(name, "vkDestroyInstance") == 0)
            return reinterpret_cast<PFN_vkVoidFunction>(hookDestroyInstance);

        if (instance == VK_NULL_HANDLE)
            return nullptr;

        auto* data = getInstanceData(instance);
        if (!data || !data->gipa)
            return nullptr;
        return data->gipa(instance, name);
    }

    VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL layerGetDeviceProcAddr(
        VkDevice device,
        const char* name
    ) {
        if (!name)
            return nullptr;

        if (std::strcmp(name, "vkGetDeviceProcAddr") == 0)
            return reinterpret_cast<PFN_vkVoidFunction>(layerGetDeviceProcAddr);
        if (std::strcmp(name, "vkCreateSwapchainKHR") == 0)
            return reinterpret_cast<PFN_vkVoidFunction>(hookCreateSwapchain);
        if (std::strcmp(name, "vkDestroySwapchainKHR") == 0)
            return reinterpret_cast<PFN_vkVoidFunction>(hookDestroySwapchain);
        if (std::strcmp(name, "vkQueuePresentKHR") == 0)
            return reinterpret_cast<PFN_vkVoidFunction>(hookQueuePresent);
        if (std::strcmp(name, "vkGetDeviceQueue") == 0)
            return reinterpret_cast<PFN_vkVoidFunction>(hookGetDeviceQueue);
        if (std::strcmp(name, "vkGetDeviceQueue2") == 0)
            return reinterpret_cast<PFN_vkVoidFunction>(hookGetDeviceQueue2);
        if (std::strcmp(name, "vkDestroyDevice") == 0)
            return reinterpret_cast<PFN_vkVoidFunction>(hookDestroyDevice);

        auto* data = getDeviceData(device);
        if (!data || !data->fns.gdpa)
            return nullptr;
        return data->fns.gdpa(device, name);
    }

}

extern "C" __attribute__((visibility("default")))
VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(
    VkNegotiateLayerInterface* version
) {
    if (!version || version->loaderLayerInterfaceVersion < 2)
        return VK_ERROR_INITIALIZATION_FAILED;

    version->loaderLayerInterfaceVersion = 2;
    version->pfnGetInstanceProcAddr = layerGetInstanceProcAddr;
    version->pfnGetDeviceProcAddr = layerGetDeviceProcAddr;
    version->pfnGetPhysicalDeviceProcAddr = nullptr;
    return VK_SUCCESS;
}

extern "C" __attribute__((visibility("default")))
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(
    VkInstance instance,
    const char* name
) {
    return layerGetInstanceProcAddr(instance, name);
}

extern "C" __attribute__((visibility("default")))
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(
    VkDevice device,
    const char* name
) {
    return layerGetDeviceProcAddr(device, name);
}
