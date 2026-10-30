/* SPDX-License-Identifier: MIT */

// Small capability probe for the openfg layer/backend.
//
// It does not render or create a device. It answers the questions that decide
// whether the layer can run: external FD memory/semaphores, timeline
// semaphores, the storage formats the interpolator uses, and DMA-BUF support
// for cross-GPU operation.

#include "openfg_backend.hpp"
#include "vkutil/vkutil.hpp"

#include <vk>

#include <cstdio>
#include <unistd.h>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

    const char* boolText(bool value) {
        return value ? "yes" : "no";
    }

    bool hasExtension(
        const std::vector<vk::ExtensionProperties>& extensions,
        std::string_view name
    ) {
        for (const auto& extension : extensions) {
            if (name == extension.extensionName)
                return true;
        }
        return false;
    }

    bool formatSupports(
        const vk::PhysicalDevice& physdev,
        const vk::detail::DispatchLoaderDynamic& dld,
        vk::Format format,
        bool storage
    ) {
        const auto properties = physdev.getFormatProperties(format, dld);
        // The backend's internal images are optimal-tiled.
        const auto features = properties.optimalTilingFeatures;
        if (!(features & vk::FormatFeatureFlagBits::eSampledImage))
            return false;
        if (storage && !(features & vk::FormatFeatureFlagBits::eStorageImage))
            return false;
        return true;
    }

    bool externalImageSupports(
        const vk::PhysicalDevice& physdev,
        const vk::detail::DispatchLoaderDynamic& dld,
        vk::ExternalMemoryHandleTypeFlagBits handleType,
        vk::ImageTiling tiling,
        vk::ImageUsageFlags usage
    ) {
        if (!dld.vkGetPhysicalDeviceImageFormatProperties2)
            return false;

        VkPhysicalDeviceExternalImageFormatInfo external{};
        external.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO;
        external.pNext = nullptr;
        external.handleType = static_cast<VkExternalMemoryHandleTypeFlagBits>(handleType);
        const VkPhysicalDeviceImageFormatInfo2 info{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
            .pNext = &external,
            .format = VK_FORMAT_R8G8B8A8_UNORM,
            .type = VK_IMAGE_TYPE_2D,
            .tiling = static_cast<VkImageTiling>(tiling),
            .usage = static_cast<VkImageUsageFlags>(usage),
            .flags = 0
        };
        VkExternalImageFormatProperties externalProperties{};
        externalProperties.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES;
        VkImageFormatProperties2 properties{};
        properties.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
        properties.pNext = &externalProperties;
        const VkResult result = dld.vkGetPhysicalDeviceImageFormatProperties2(
            physdev, &info, &properties);
        if (result != VK_SUCCESS)
            return false;
        return (externalProperties.externalMemoryProperties.compatibleHandleTypes &
            static_cast<VkExternalMemoryHandleTypeFlags>(handleType)) != 0;
    }

    bool externalSemaphoreSupports(
        const vk::PhysicalDevice& physdev,
        const vk::detail::DispatchLoaderDynamic& dld,
        vk::ExternalSemaphoreHandleTypeFlagBits handleType
    ) {
        if (!dld.vkGetPhysicalDeviceExternalSemaphoreProperties)
            return false;

        VkPhysicalDeviceExternalSemaphoreInfo info{};
        info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO;
        info.handleType = static_cast<VkExternalSemaphoreHandleTypeFlagBits>(handleType);
        VkExternalSemaphoreProperties properties{};
        properties.sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES;
        dld.vkGetPhysicalDeviceExternalSemaphoreProperties(physdev, &info, &properties);
        const auto features = properties.externalSemaphoreFeatures;
        return (features & VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT) != 0 &&
            (features & VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT) != 0;
    }

    struct Report {
        bool sameGpu{};
        bool crossGpu{};
        bool hdr{};
        bool hdrCross{};
        std::string sameError;
        std::string crossError;
        std::string hdrError;
        std::string hdrCrossError;
    };

    bool testBackend(
        const std::string& deviceId,
        bool crossGpu,
        bool hdr,
        std::string& error
    ) {
        try {
            openfg::Instance instance(
                deviceId,
                std::filesystem::path{},
                crossGpu,
                false,
                nullptr
            );
            openfg::Context context(
                instance, 64, 64, 1.0F, false, false,
                hdr ? vk::Format::eR16G16B16A16Sfloat
                    : vk::Format::eR8G8B8A8Unorm);
            const auto fds = context.exportFds();
            if (fds.sourceFd >= 0)
                ::close(fds.sourceFd);
            if (fds.destinationFd >= 0)
                ::close(fds.destinationFd);
            if (fds.syncFd >= 0)
                ::close(fds.syncFd);
            return true;
        } catch (const std::exception& exception) {
            error = exception.what();
            return false;
        }
    }

    Report probe(
        const vk::PhysicalDevice& physdev,
        const vk::detail::DispatchLoaderDynamic& dld,
        uint32_t index
    ) {
        Report report;
        std::printf("GPU %u\n", index);

        vk::PhysicalDevicePCIBusInfoPropertiesEXT pci{};
        const bool hasPci = vkutil::supportsExtension(
            dld, physdev, vk::EXTPciBusInfoExtensionName);
        vk::PhysicalDeviceProperties2 properties{};
        properties.pNext = hasPci ? &pci : nullptr;
        physdev.getProperties2(&properties, dld);

        const auto& device = properties.properties;
        std::printf("  name          : %s\n", device.deviceName.data());
        std::printf("  api           : %u.%u.%u\n",
            VK_API_VERSION_MAJOR(device.apiVersion),
            VK_API_VERSION_MINOR(device.apiVersion),
            VK_API_VERSION_PATCH(device.apiVersion));
        std::printf("  driver        : 0x%08x\n", device.driverVersion);
        std::printf("  vendor:device : %04x:%04x\n", device.vendorID, device.deviceID);
        if (hasPci) {
            std::printf("  pci           : %04x:%02x:%02x.%x\n",
                pci.pciDomain, pci.pciBus, pci.pciDevice, pci.pciFunction);
        } else {
            std::printf("  pci           : unavailable\n");
        }

        const auto extensions = physdev.enumerateDeviceExtensionProperties(nullptr, dld);
        const bool externalMemory = hasExtension(
            extensions, vk::KHRExternalMemoryFdExtensionName);
        const bool externalSemaphore = hasExtension(
            extensions, vk::KHRExternalSemaphoreFdExtensionName);
        const bool dmaBuf = hasExtension(
            extensions, vk::EXTExternalMemoryDmaBufExtensionName);
        const bool sync2 = hasExtension(
            extensions, vk::KHRSynchronization2ExtensionName) ||
            device.apiVersion >= VK_API_VERSION_1_3;

        const bool timeline = device.apiVersion >= VK_API_VERSION_1_2 ||
            hasExtension(extensions, vk::KHRTimelineSemaphoreExtensionName);

        VkPhysicalDeviceSynchronization2Features sync2Features{};
        sync2Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES;
        VkPhysicalDeviceTimelineSemaphoreFeatures timelineFeatures{};
        timelineFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
        timelineFeatures.pNext = &sync2Features;
        VkPhysicalDeviceFeatures2 features{};
        features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        features.pNext = &timelineFeatures;
        dld.vkGetPhysicalDeviceFeatures2(physdev, &features);

        std::printf("  extensions    : mem-fd=%s sem-fd=%s dma-buf=%s sync2=%s\n",
            boolText(externalMemory), boolText(externalSemaphore),
            boolText(dmaBuf), boolText(sync2));
        std::printf("  features      : timeline=%s sync2=%s\n",
            boolText(timeline && timelineFeatures.timelineSemaphore),
            boolText(sync2 && sync2Features.synchronization2));

        bool computeQueue{false};
        for (const auto& family : physdev.getQueueFamilyProperties(dld)) {
            if (family.queueFlags & vk::QueueFlagBits::eCompute)
                computeQueue = true;
        }
        std::printf("  compute queue : %s\n", boolText(computeQueue));

        const bool storageR8 = formatSupports(
            physdev, dld, vk::Format::eR8Unorm, true);
        const bool storageRg16f = formatSupports(
            physdev, dld, vk::Format::eR16G16Sfloat, true);
        const bool colorRgba8 = formatSupports(
            physdev, dld, vk::Format::eR8G8B8A8Unorm, true);
        std::printf("  formats       : r8-storage=%s rg16f-storage=%s rgba8=%s\n",
            boolText(storageR8), boolText(storageRg16f), boolText(colorRgba8));

        const bool opaqueImage = externalImageSupports(
            physdev, dld,
            vk::ExternalMemoryHandleTypeFlagBits::eOpaqueFd,
            vk::ImageTiling::eOptimal,
            vk::ImageUsageFlagBits::eSampled |
                vk::ImageUsageFlagBits::eStorage |
                vk::ImageUsageFlagBits::eTransferSrc |
                vk::ImageUsageFlagBits::eTransferDst);
        const bool opaqueSemaphore = externalSemaphoreSupports(
            physdev, dld, vk::ExternalSemaphoreHandleTypeFlagBits::eOpaqueFd);
        std::printf("  opaque-fd     : image=%s semaphore=%s\n",
            boolText(opaqueImage), boolText(opaqueSemaphore));

        std::printf("  dma-buf       : extension=%s\n", boolText(dmaBuf));


        std::string deviceId;
        if (hasPci) {
            char buffer[64]{};
            std::snprintf(buffer, sizeof(buffer), "%04x:%02x:%02x.%x",
                pci.pciDomain, pci.pciBus, pci.pciDevice, pci.pciFunction);
            deviceId = buffer;
        } else {
            deviceId = device.deviceName.data();
        }

        // The format queries above are advisory: some RADV versions do not
        // advertise linear DMA-BUF images even though the exact create/export
        // path used by the backend works. Run the real backend instead.
        report.sameGpu = testBackend(deviceId, false, false, report.sameError);
        report.crossGpu = testBackend(deviceId, true, false, report.crossError);
        report.hdr = testBackend(deviceId, false, true, report.hdrError);
        report.hdrCross = testBackend(deviceId, true, true, report.hdrCrossError);

        std::printf("  runtime same  : %s", boolText(report.sameGpu));
        if (!report.sameError.empty())
            std::printf(" (%s)", report.sameError.c_str());
        std::printf("\n");
        std::printf("  runtime cross : %s", boolText(report.crossGpu));
        if (!report.crossError.empty())
            std::printf(" (%s)", report.crossError.c_str());
        std::printf("\n");
        std::printf("  runtime hdr   : %s", boolText(report.hdr));
        if (!report.hdrError.empty())
            std::printf(" (%s)", report.hdrError.c_str());
        std::printf("\n");
        std::printf("  runtime hdr x : %s", boolText(report.hdrCross));
        if (!report.hdrCrossError.empty())
            std::printf(" (%s)", report.hdrCrossError.c_str());
        std::printf("\n");
        std::printf("\n");
        return report;
    }

}

int main(int argc, char** argv) {
    std::optional<std::string> requested;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--gpu") == 0 && i + 1 < argc) {
            requested = argv[++i];
        } else {
            std::fprintf(stderr, "usage: openfg-gpu-probe [--gpu <name|vendor:device|pci>]\n");
            return 2;
        }
    }

    try {
        vk::detail::DispatchLoaderDynamic dld;
        auto instance = vkutil::createInstance(dld, "openfg-gpu-probe", vk::ApiVersion13);
        const auto devices = instance->enumeratePhysicalDevices(dld);
        if (devices.empty()) {
            std::fprintf(stderr, "no Vulkan devices\n");
            return 1;
        }

        bool found{false};
        for (uint32_t i = 0; i < devices.size(); i++) {
            const auto& physdev = devices[i];
            if (requested) {
                // Reuse vkutil's matching so names, vendor:device and PCI IDs
                // behave exactly like the backend and fg-lab.
                try {
                    const auto selected = vkutil::findPhysicalDevice(
                        dld, *instance, *requested);
                    if (selected != physdev)
                        continue;
                } catch (...) {
                    continue;
                }
            }
            found = true;
            probe(physdev, dld, i);
        }

        if (requested && !found) {
            std::fprintf(stderr, "no Vulkan device matching '%s'\n", requested->c_str());
            return 1;
        }
        return 0;
    } catch (const std::exception& exception) {
        std::fprintf(stderr, "gpu probe failed: %s\n", exception.what());
        return 1;
    }
}
