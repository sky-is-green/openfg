/* SPDX-License-Identifier: MIT */

// Frame generation for the clean-room layer.
//
// One backend instance per device (kept across swapchain recreation) and one
// context per swapchain. The application presents normally; the layer copies
// the real frame into the backend source image, interpolates
// `multiplier - 1` frames (2x/3x/4x), and inserts them before the real
// present. Anything unexpected falls back to a normal present rather than
// breaking the application.

#include "layer.hpp"
#include "config/config.hpp"
#include "openfg_backend.hpp"

#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdarg>
#include <atomic>
#include <deque>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

    using openfg_layer::SwapchainInfo;

    constexpr const char* TAG = "[openfg]";

    const openfg::config::LoadResult& layerConfig() {
        return openfg::config::processConfig();
    }

    const openfg::config::Config& activeConfig() {
        static const openfg::config::Config config = [] {
            const auto& loaded = layerConfig();
            return openfg::config::withVideoProfile(
                loaded.config, openfg::config::processNames());
        }();
        return config;
    }

    std::FILE* g_log{};

    void logLine(const char* format, ...) {
        va_list args;
        va_start(args, format);
        std::vfprintf(stderr, format, args);
        va_end(args);
        if (g_log) {
            va_start(args, format);
            std::vfprintf(g_log, format, args);
            va_end(args);
            std::fflush(g_log);
        }
    }

    void openLog() {
        static bool tried{false};
        if (tried)
            return;
        tried = true;
        if (!activeConfig().debug)
            return;

        std::filesystem::path path;
        if (const char* overridePath = std::getenv("OPENFG_LOG");
            overridePath && *overridePath) {
            path = overridePath;
        } else if (const char* state = std::getenv("XDG_STATE_HOME");
                   state && *state) {
            path = std::filesystem::path(state) / "openfg" / "layer.log";
        } else if (const char* home = std::getenv("HOME"); home && *home) {
            path = std::filesystem::path(home) / ".local" / "state" /
                "openfg" / "layer.log";
        } else {
            return;
        }

        std::error_code ec;
        if (path.has_parent_path())
            std::filesystem::create_directories(path.parent_path(), ec);
        g_log = std::fopen(path.string().c_str(), "a");
        if (g_log)
            std::fprintf(stderr, "%s debug log: %s\n", TAG, path.string().c_str());
    }

    bool enabled() {
        static const bool value = [] {
            openLog();
            const auto& result = layerConfig();
            if (!result.error.empty()) {
                logLine("%s config error: %s\n", TAG, result.error.c_str());
                return false;
            }
            for (const auto& warning : result.warnings)
                std::fprintf(stderr, "%s warning: %s\n", TAG, warning.c_str());
            if (!result.config.enabled)
                return false;
            if (!openfg::config::processMatches(result.config)) {
                if (result.config.debug)
                    std::fprintf(stderr, "%s process does not match the config\n", TAG);
                return false;
            }
            return true;
        }();
        return value;
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

    std::string identifyPhysicalDevice(const LayerDeviceFns& fns, const std::string& requested) {
        if (!requested.empty() && requested != "auto")
            return requested;

        const bool hasPci = hasExtension(fns, VK_EXT_PCI_BUS_INFO_EXTENSION_NAME);
        VkPhysicalDevicePCIBusInfoPropertiesEXT pci{};
        pci.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT;

        VkPhysicalDeviceProperties2 props{};
        props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        props.pNext = hasPci ? &pci : nullptr;
        if (fns.getPhysDevProperties2)
            fns.getPhysDevProperties2(fns.physdev, &props);

        if (hasPci &&
            (pci.pciDomain || pci.pciBus || pci.pciDevice || pci.pciFunction)) {
            char buffer[64]{};
            std::snprintf(buffer, sizeof(buffer), "%04x:%02x:%02x.%x",
                pci.pciDomain, pci.pciBus, pci.pciDevice, pci.pciFunction);
            return buffer;
        }

        if (hasPci)
            logLine("%s PCI bus info is empty; using the device name\n", TAG);
        return props.properties.deviceName;
    }

    using SemaphoreArray = std::array<VkSemaphore, openfg::kMaxInterpolatedFrames>;
    using CommandArray = std::array<VkCommandBuffer, openfg::kMaxInterpolatedFrames>;

    // Present semaphores are tied to the swapchain image they were used with.
    // Re-signalling a binary semaphore before the presentation engine has
    // consumed its previous wait is invalid; indexing by the application's
    // image gives each semaphore a full swapchain rotation to be consumed.
    struct ImageSync {
        VkSemaphore sourceDone{};
        SemaphoreArray acquire{};
        SemaphoreArray copyDone{};
    };

    struct AdaptiveState {
        std::chrono::steady_clock::time_point lastPresent{};
        std::chrono::steady_clock::time_point aboveSince{};
        std::chrono::steady_clock::time_point belowSince{};
        double intervalMs{};
        double overheadMs{};
        std::chrono::steady_clock::time_point reportTime{};
        uint64_t reportPresents{};
        uint64_t reportGenerated{};
        std::deque<std::pair<uint64_t, std::chrono::steady_clock::time_point>>
            pendingPresents;
        std::vector<double> presentLatencies;
        bool generating{false};
        bool resetPending{false};
    };

    struct FgState {
        const LayerDeviceFns* fns{};
        VkDevice device{};
        VkSwapchainKHR swapchain{};
        VkExtent2D extent{};
        uint32_t queueFamily{};
        std::shared_ptr<openfg::Instance> instance;
        std::unique_ptr<openfg::Context> context;

        VkImage source{};
        VkDeviceMemory sourceMemory{};
        VkImage destination{};
        VkDeviceMemory destinationMemory{};
        VkSemaphore sync{};

        VkCommandPool pool{};
        VkCommandBuffer sourceCmd{};
        CommandArray destinationCmds{};
        std::vector<ImageSync> imageSync;
        VkFence fence{};
        bool fencePending{};

        uint64_t syncValue{};
        uint32_t iteration{};
        uint32_t multiplier{2};
        VkFormat colorFormat{VK_FORMAT_R8G8B8A8_UNORM};
        bool crossGpu{};
        bool ready{};
        bool failed{};
        std::string error;
        AdaptiveState adaptive;
    };

    std::mutex g_mutex;
    // A backend instance is per application device; frame-generation state is
    // per swapchain so applications with more than one swapchain do not
    // destroy and recreate the context on every present.
    std::map<VkDevice, std::shared_ptr<openfg::Instance>> g_instances;
    std::map<VkSwapchainKHR, std::unique_ptr<FgState>> g_states;
    std::map<VkSwapchainKHR, SwapchainInfo> g_swapchains;

    void closeFd(int fd) {
        if (fd >= 0)
            ::close(fd);
    }

    void setError(FgState& state, std::string message) {
        state.error = std::move(message);
        state.failed = true;
        logLine("%s %s\n", TAG, state.error.c_str());
    }

    void destroyState(FgState& state) {
        if (!state.fns)
            return;
        const auto& fns = *state.fns;
        if (fns.deviceWaitIdle)
            fns.deviceWaitIdle(fns.device);
        if (state.fencePending && fns.waitForFences && state.fence) {
            fns.waitForFences(fns.device, 1, &state.fence, VK_TRUE, UINT64_MAX);
            state.fencePending = false;
        }

        if (fns.destroySemaphore) {
            for (const ImageSync& sync : state.imageSync) {
                if (sync.sourceDone)
                    fns.destroySemaphore(fns.device, sync.sourceDone, nullptr);
                for (VkSemaphore sem : sync.acquire) {
                    if (sem)
                        fns.destroySemaphore(fns.device, sem, nullptr);
                }
                for (VkSemaphore sem : sync.copyDone) {
                    if (sem)
                        fns.destroySemaphore(fns.device, sem, nullptr);
                }
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

        // Destroy the context (and its exported images/semaphores) but keep
        // the backend instance. Destroying the backend VkDevice while the
        // application still has imports from it can wedge RADV, and swapchain
        // recreation happens while the application device is very much alive.
        state.context.reset();
    }

    /// Tears down per-swapchain resources. The backend instance is owned by
    /// g_instances and survives for the lifetime of the application device.
    void resetState(FgState& state) {
        auto adaptive = state.adaptive;
        destroyState(state);
        state = FgState{};
        state.adaptive = adaptive;
    }

    bool shouldGenerate(FgState& state, const openfg::config::Config& config) {
        using namespace std::chrono;
        const auto now = steady_clock::now();
        auto& adaptive = state.adaptive;

        if (config.generator == "off") {
            adaptive.generating = false;
            return false;
        }

        if (config.targetFps == 0) {
            adaptive.generating = true;
            adaptive.lastPresent = now;
            return true;
        }

        if (adaptive.lastPresent != steady_clock::time_point{}) {
            const double dt = duration<double, std::milli>(now - adaptive.lastPresent).count();
            if (dt > 500.0) {
                // Loading screen or a long stall: start over instead of
                // concluding that the game suddenly runs at 2 fps.
                adaptive.intervalMs = 0.0;
                adaptive.aboveSince = {};
                adaptive.belowSince = {};
                adaptive.lastPresent = now;
                return adaptive.generating;
            }
            adaptive.intervalMs = adaptive.intervalMs <= 0.0
                ? dt
                : adaptive.intervalMs * 0.9 + dt * 0.1;
        }
        adaptive.lastPresent = now;

        if (adaptive.intervalMs <= 0.0)
            return adaptive.generating;

        // When generation is active, the measured interval includes the
        // layer's own copy/generate/present work. Subtract the previous
        // frame's overhead to estimate the application's native frame rate.
        double sampleMs = adaptive.intervalMs;
        if (adaptive.generating && adaptive.overheadMs > 0.0 &&
            adaptive.overheadMs < sampleMs * 0.9)
            sampleMs -= adaptive.overheadMs;
        if (sampleMs < 1.0)
            sampleMs = 1.0;

        const double fps = 1000.0 / sampleMs;
        const double target = static_cast<double>(config.targetFps);

        if (adaptive.generating) {
            if (fps > target * 1.05) {
                if (adaptive.aboveSince == steady_clock::time_point{})
                    adaptive.aboveSince = now;
                if (now - adaptive.aboveSince >= seconds(1)) {
                    adaptive.generating = false;
                    adaptive.aboveSince = {};
                    adaptive.belowSince = {};
                    if (config.debug)
                        std::fprintf(stderr,
                            "%s adaptive: stop generating (%.1f fps, target %u)\n",
                            TAG, fps, config.targetFps);
                }
            } else {
                adaptive.aboveSince = {};
            }
        } else {
            if (fps < target * 0.95) {
                if (adaptive.belowSince == steady_clock::time_point{})
                    adaptive.belowSince = now;
                if (now - adaptive.belowSince >= seconds(1)) {
                    adaptive.generating = true;
                    adaptive.resetPending = true;
                    adaptive.aboveSince = {};
                    adaptive.belowSince = {};
                    if (config.debug)
                        std::fprintf(stderr,
                            "%s adaptive: start generating (%.1f fps, target %u)\n",
                            TAG, fps, config.targetFps);
                }
            } else {
                adaptive.belowSince = {};
            }
        }
        return adaptive.generating;
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
        info.format = state.colorFormat;
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
            // A successful import transfers ownership to Vulkan. Until then
            // the descriptor is ours and would leak.
            closeFd(fd);
            setError(state, "failed to create imported image");
            return false;
        }

        VkMemoryRequirements requirements{};
        fns.getImageMemoryRequirements(fns.device, image, &requirements);

        // A DMA-BUF import must use a memory type compatible with the handle.
        if (state.crossGpu && fns.getMemoryFdProperties) {
            VkMemoryFdPropertiesKHR properties{};
            properties.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR;
            if (fns.getMemoryFdProperties(
                    fns.device, handleType, fd, &properties) == VK_SUCCESS) {
                requirements.memoryTypeBits &= properties.memoryTypeBits;
                if (requirements.memoryTypeBits == 0) {
                    closeFd(fd);
                    fns.destroyImage(fns.device, image, nullptr);
                    setError(state, "no compatible memory type for the imported DMA-BUF");
                    return false;
                }
            }
        }

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
        // The image is only accessed by the GPU. Prefer device-local memory;
        // for DMA-BUF the fd properties above already removed invalid types.
        alloc.memoryTypeIndex = findMemoryType(fns, requirements.memoryTypeBits, false);

        VkDeviceMemory memory{};
        if (fns.allocateMemory(fns.device, &alloc, nullptr, &memory) != VK_SUCCESS) {
            closeFd(fd);
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
            closeFd(fd);
            setError(state, "failed to create timeline semaphore");
            return false;
        }

        VkImportSemaphoreFdInfoKHR import{};
        import.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR;
        import.semaphore = state.sync;
        import.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
        import.fd = fd;
        if (fns.importSemaphoreFd(fns.device, &import) != VK_SUCCESS) {
            closeFd(fd);
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
        state.queueFamily = swapchain.queueFamily;
        state.multiplier = std::clamp(activeConfig().multiplier, 2u, 4u);
        state.crossGpu = activeConfig().crossGpu;

        if (activeConfig().debug)
            logLine("%s swapchain format: %d, colour space: %d\n",
                TAG, static_cast<int>(swapchain.format),
                static_cast<int>(swapchain.colorSpace));

        const bool hdr =
            swapchain.colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT ||
            swapchain.colorSpace == VK_COLOR_SPACE_HDR10_HLG_EXT ||
            swapchain.colorSpace == VK_COLOR_SPACE_DOLBYVISION_EXT ||
            swapchain.colorSpace == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT;
        const bool wide =
            swapchain.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 ||
            swapchain.format == VK_FORMAT_A2B10G10R10_SNORM_PACK32 ||
            swapchain.format == VK_FORMAT_A2R10G10B10_UNORM_PACK32 ||
            swapchain.format == VK_FORMAT_A2R10G10B10_SNORM_PACK32 ||
            swapchain.format == VK_FORMAT_R16G16B16A16_SFLOAT ||
            swapchain.format == VK_FORMAT_R16G16B16A16_UNORM;
        state.colorFormat = (wide || hdr)
            ? VK_FORMAT_R16G16B16A16_SFLOAT
            : VK_FORMAT_R8G8B8A8_UNORM;
        if (wide || hdr)
            std::fprintf(stderr,
                "%s %s swapchain: half-float interpolation path\n", TAG,
                hdr ? "HDR" : "10-bit");

        if (state.crossGpu && !fns.externalDmaBuf) {
            setError(state, "cross-GPU requested but VK_EXT_external_memory_dma_buf is unavailable");
            return false;
        }
        if (!fns.externalMemoryFd || !fns.externalSemaphoreFd) {
            setError(state, "external memory/semaphore FD is unavailable");
            return false;
        }

        const std::string deviceId = identifyPhysicalDevice(fns, activeConfig().gpu);
        logLine("%s backend on %s, %s, %ux, %s%s\n", TAG,
            deviceId.c_str(), state.crossGpu ? "cross-GPU" : "same-GPU",
            state.multiplier, activeConfig().generator.c_str(),
            activeConfig().videoSafe ? ", video-safe" : "");

        auto& instance = g_instances[fns.device];
        if (!instance) {
            instance = std::make_shared<openfg::Instance>(
                deviceId,
                std::filesystem::path{},
                state.crossGpu,
                // Destroying the backend VkDevice while the application's
                // device still has imports from it can hang RADV. Leak the
                // backend device/instance at process exit; the layer still
                // tears down all images, semaphores and its context.
                true,
                [](const std::string& message) {
                    logLine("%s %s\n", TAG, message.c_str());
                }
            );
        }
        state.instance = instance;
        openfg::Generator generator{openfg::Generator::Flow};
        if (activeConfig().generator == "blend")
            generator = openfg::Generator::Blend;
        else if (activeConfig().generator == "hold")
            generator = openfg::Generator::Hold;

        state.context = std::make_unique<openfg::Context>(
            *instance,
            swapchain.extent.width,
            swapchain.extent.height,
            1.0F,
            activeConfig().performanceMode,
            activeConfig().videoSafe,
            static_cast<vk::Format>(state.colorFormat),
            generator
        );

        const auto fds = state.context->exportFds();
        if (!importImage(state, fds.sourceFd, 2, false)) {
            closeFd(fds.destinationFd);
            closeFd(fds.syncFd);
            return false;
        }
        if (!importImage(state, fds.destinationFd, openfg::kMaxInterpolatedFrames, true)) {
            closeFd(fds.syncFd);
            return false;
        }
        if (!importSyncSemaphore(state, fds.syncFd))
            return false;

        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = swapchain.queueFamily;
        if (fns.createCommandPool(fns.device, &poolInfo, nullptr, &state.pool) != VK_SUCCESS) {
            setError(state, "failed to create command pool");
            return false;
        }

        VkCommandBufferAllocateInfo cmdInfo{};
        cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cmdInfo.commandPool = state.pool;
        cmdInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmdInfo.commandBufferCount = 1 + openfg::kMaxInterpolatedFrames;
        std::array<VkCommandBuffer, 1 + openfg::kMaxInterpolatedFrames> cmds{};
        if (fns.allocateCommandBuffers(fns.device, &cmdInfo, cmds.data()) != VK_SUCCESS) {
            setError(state, "failed to allocate command buffers");
            return false;
        }
        state.sourceCmd = cmds[0];
        for (uint32_t i = 0; i < openfg::kMaxInterpolatedFrames; i++)
            state.destinationCmds[i] = cmds[i + 1];

        state.imageSync.resize(swapchain.images.size());
        for (ImageSync& sync : state.imageSync) {
            if (!createBinarySemaphore(fns, sync.sourceDone)) {
                setError(state, "failed to create semaphores");
                return false;
            }
            for (uint32_t i = 0; i < openfg::kMaxInterpolatedFrames; i++) {
                if (!createBinarySemaphore(fns, sync.acquire[i]) ||
                    !createBinarySemaphore(fns, sync.copyDone[i])) {
                    setError(state, "failed to create semaphores");
                    return false;
                }
            }
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
    return ::enabled();
}

bool debug() {
    openLog();
    return layerConfig().error.empty() && activeConfig().debug;
}

unsigned multiplier() {
    return std::clamp(activeConfig().multiplier, 2u, 4u);
}

bool crossGpu() {
    return activeConfig().crossGpu;
}

bool presentTiming() {
    return activeConfig().presentTiming;
}

void addSwapchain(const LayerDeviceFns&, const SwapchainInfo& info) {
    std::lock_guard lock(g_mutex);
    g_swapchains[info.swapchain] = info;
}

void removeSwapchain(VkDevice device, VkSwapchainKHR swapchain) {
    (void)device;
    std::lock_guard lock(g_mutex);
    auto it = g_states.find(swapchain);
    if (it != g_states.end()) {
        resetState(*it->second);
        g_states.erase(it);
    }
    g_swapchains.erase(swapchain);
}

void destroyDevice(VkDevice device) {
    std::lock_guard lock(g_mutex);
    for (auto it = g_states.begin(); it != g_states.end();) {
        if (it->second->device == device) {
            resetState(*it->second);
            it = g_states.erase(it);
        } else {
            ++it;
        }
    }
    g_instances.erase(device);
    for (auto swapIt = g_swapchains.begin(); swapIt != g_swapchains.end();) {
        if (swapIt->second.device == device)
            swapIt = g_swapchains.erase(swapIt);
        else
            ++swapIt;
    }
}

std::atomic<uint64_t> g_presentIdCounter{0};

VkResult presentNext(
    const LayerDeviceFns& fns,
    VkQueue queue,
    const VkPresentInfoKHR* info,
    PFN_vkQueuePresentKHR next,
    uint64_t* appPresentId = nullptr
) {
    if (appPresentId)
        *appPresentId = 0;
    if (!fns.presentId || !info || info->swapchainCount == 0)
        return next(queue, info);

    // If the application already supplies present IDs, leave them alone.
    for (const auto* p = static_cast<const VkBaseInStructure*>(info->pNext);
         p; p = p->pNext) {
        if (p->sType == VK_STRUCTURE_TYPE_PRESENT_ID_KHR)
            return next(queue, info);
    }

    uint64_t singleId{};
    std::vector<uint64_t> ids;
    uint64_t* idData{&singleId};
    if (info->swapchainCount > 1) {
        ids.resize(info->swapchainCount);
        idData = ids.data();
    }
    for (uint32_t i = 0; i < info->swapchainCount; i++)
        idData[i] = g_presentIdCounter.fetch_add(1, std::memory_order_relaxed) + 1;

    VkPresentIdKHR idInfo{};
    idInfo.sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR;
    idInfo.pNext = const_cast<void*>(info->pNext);
    idInfo.swapchainCount = info->swapchainCount;
    idInfo.pPresentIds = idData;

    VkPresentInfoKHR copy = *info;
    copy.pNext = &idInfo;
    if (appPresentId)
        *appPresentId = idData[0];
    return next(queue, &copy);
}

void pollPresentLatency(FgState& state, const LayerDeviceFns& fns) {
    if (!fns.presentId || !fns.waitForPresent || state.failed)
        return;
    const auto now = std::chrono::steady_clock::now();
    while (!state.adaptive.pendingPresents.empty()) {
        const auto [id, submitted] = state.adaptive.pendingPresents.front();
        const VkResult result =
            fns.waitForPresent(fns.device, state.swapchain, id, 0);
        if (result == VK_TIMEOUT)
            break;
        if (result == VK_SUCCESS) {
            state.adaptive.presentLatencies.push_back(
                std::chrono::duration<double, std::milli>(now - submitted).count());
        }
        state.adaptive.pendingPresents.pop_front();
    }
    while (state.adaptive.pendingPresents.size() > 8)
        state.adaptive.pendingPresents.pop_front();
}

void reportPerf(FgState& state) {
    if (!activeConfig().debug)
        return;
    const auto now = std::chrono::steady_clock::now();
    if (state.adaptive.reportTime == std::chrono::steady_clock::time_point{}) {
        state.adaptive.reportTime = now;
        return;
    }
    const double elapsed =
        std::chrono::duration<double>(now - state.adaptive.reportTime).count();
    if (elapsed < 5.0)
        return;
    const double appFps = static_cast<double>(state.adaptive.reportPresents) / elapsed;
    const double presentedFps = static_cast<double>(
        state.adaptive.reportPresents + state.adaptive.reportGenerated) / elapsed;
    logLine("%s perf: app %.1f fps, presented %.1f fps, overhead %.2f ms, "
        "generating %s\n",
        TAG, appFps, presentedFps, state.adaptive.overheadMs,
        state.adaptive.generating ? "yes" : "no");
    if (!state.adaptive.presentLatencies.empty()) {
        auto samples = state.adaptive.presentLatencies;
        std::sort(samples.begin(), samples.end());
        auto pct = [&](double p) {
            const size_t index = std::min(samples.size() - 1,
                static_cast<size_t>(p * static_cast<double>(samples.size() - 1)));
            return samples[index];
        };
        logLine("%s present latency: p50 %.2f ms, p95 %.2f ms, p99 %.2f ms "
            "(%zu samples)\n", TAG, pct(0.50), pct(0.95), pct(0.99),
            samples.size());
        state.adaptive.presentLatencies.clear();
    }
    state.adaptive.reportTime = now;
    state.adaptive.reportPresents = 0;
    state.adaptive.reportGenerated = 0;
}

VkResult present(
    const LayerDeviceFns& fns,
    VkQueue queue,
    uint32_t queueFamily,
    const VkPresentInfoKHR* presentInfo,
    PFN_vkQueuePresentKHR nextPresent
) {
    if (!enabled() || !presentInfo || presentInfo->swapchainCount != 1 ||
        !presentInfo->pSwapchains || !presentInfo->pImageIndices)
        return presentNext(fns, queue, presentInfo, nextPresent);

    std::lock_guard lock(g_mutex);

    const VkSwapchainKHR swapchain = presentInfo->pSwapchains[0];
    const auto swapIt = g_swapchains.find(swapchain);
    if (swapIt == g_swapchains.end())
        return presentNext(fns, queue, presentInfo, nextPresent);
    const SwapchainInfo& swapInfo = swapIt->second;

    if (swapInfo.queueFamily != queueFamily || swapInfo.images.empty())
        return presentNext(fns, queue, presentInfo, nextPresent);

    FgState* state{};
    auto stateIt = g_states.find(swapchain);
    if (stateIt == g_states.end()) {
        auto newState = std::make_unique<FgState>();
        state = newState.get();
        g_states[swapchain] = std::move(newState);
    } else {
        state = stateIt->second.get();
    }

    state->device = fns.device;
    state->fns = &fns;
    pollPresentLatency(*state, fns);

    if (state->failed)
        return presentNext(fns, queue, presentInfo, nextPresent);

    reportPerf(*state);
    state->adaptive.reportPresents++;

    const auto layerStart = std::chrono::steady_clock::now();

    if (!shouldGenerate(*state, activeConfig())) {
        state->adaptive.overheadMs = 0.0;
        uint64_t appPresentId{};
        const VkResult result = presentNext(
            fns, queue, presentInfo, nextPresent, &appPresentId);
        if (appPresentId)
            state->adaptive.pendingPresents.emplace_back(appPresentId, layerStart);
        return result;
    }

    if (state->adaptive.resetPending) {
        resetState(*state);
        state->device = fns.device;
        state->fns = &fns;
        state->adaptive.resetPending = false;
    }

    if (!state->ready) {
        bool initialized{false};
        try {
            initialized = initializeState(*state, fns, swapInfo);
        } catch (const std::exception& exception) {
            setError(*state, std::string("backend initialization failed: ") + exception.what());
        }
        if (!initialized)
            return presentNext(fns, queue, presentInfo, nextPresent);
    }

    if (state->fencePending) {
        if (fns.waitForFences(fns.device, 1, &state->fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS)
            return presentNext(fns, queue, presentInfo, nextPresent);
        fns.resetFences(fns.device, 1, &state->fence);
        state->fencePending = false;
    }

    const uint32_t requestedCount = std::min(
        state->multiplier - 1, openfg::kMaxInterpolatedFrames);
    const uint32_t appImage = presentInfo->pImageIndices[0];
    if (appImage >= state->imageSync.size())
        return presentNext(fns, queue, presentInfo, nextPresent);
    ImageSync& imageSync = state->imageSync[appImage];

    // Never block inside present: on Wayland a blocking acquire consumes the
    // events the application's own event loop is waiting for. Use timeout 0
    // and generate as many frames as the swapchain can hand out right now.
    std::array<uint32_t, openfg::kMaxInterpolatedFrames> generatedIndices{};
    uint32_t generatedCount{};
    for (uint32_t i = 0; i < requestedCount; i++) {
        const VkResult acquireResult = fns.acquireNextImage(
            fns.device, swapchain, 0, imageSync.acquire[i], VK_NULL_HANDLE,
            &generatedIndices[i]);
        if (acquireResult != VK_SUCCESS && acquireResult != VK_SUBOPTIMAL_KHR)
            break;
        if (generatedIndices[i] >= swapInfo.images.size())
            break;
        generatedCount++;
    }
    if (generatedCount == 0)
        return presentNext(fns, queue, presentInfo, nextPresent);

    const uint32_t layer = state->iteration % 2;
    if (activeConfig().debug)
        std::fprintf(stderr, "%s frame %u: real=%u first-generated=%u (%u/%u frames), layer=%u\n",
            TAG, state->iteration, presentInfo->pImageIndices[0],
            generatedIndices[0], generatedCount, requestedCount, layer);

    recordCopy(fns, state->sourceCmd,
        swapInfo.images[presentInfo->pImageIndices[0]], 0,
        state->source, layer,
        swapInfo.extent, true, false);

    const uint64_t sourceSync = state->syncValue + 1;

    std::vector<uint64_t> waitValues(presentInfo->waitSemaphoreCount, 0);
    std::vector<VkPipelineStageFlags> waitStages(
        presentInfo->waitSemaphoreCount, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);

    VkSemaphore sourceSignals[2]{ imageSync.sourceDone, state->sync };
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
        return presentNext(fns, queue, presentInfo, nextPresent);
    }

    uint64_t generatedSync = sourceSync;
    try {
        state->context->dispatch(generatedCount);

        for (uint32_t i = 0; i < generatedCount; i++) {
            state->context->acquire();
            generatedSync++;

            recordCopy(fns, state->destinationCmds[i],
                state->destination, i,
                swapInfo.images[generatedIndices[i]], 0,
                swapInfo.extent, false, true);

            VkSemaphore copyWaits[2]{ imageSync.acquire[i], state->sync };
            uint64_t copyWaitValues[2]{ 0, generatedSync };

            VkTimelineSemaphoreSubmitInfo copyTimeline{};
            copyTimeline.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
            copyTimeline.waitSemaphoreValueCount = 2;
            copyTimeline.pWaitSemaphoreValues = copyWaitValues;
            copyTimeline.signalSemaphoreValueCount = 1;
            uint64_t zeroSignal{0};
            copyTimeline.pSignalSemaphoreValues = &zeroSignal;

            std::array<VkPipelineStageFlags, 2> copyStages{
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT
            };

            VkSubmitInfo copySubmit{};
            copySubmit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            copySubmit.pNext = &copyTimeline;
            copySubmit.waitSemaphoreCount = 2;
            copySubmit.pWaitSemaphores = copyWaits;
            copySubmit.pWaitDstStageMask = copyStages.data();
            copySubmit.commandBufferCount = 1;
            copySubmit.pCommandBuffers = &state->destinationCmds[i];
            copySubmit.signalSemaphoreCount = 1;
            copySubmit.pSignalSemaphores = &imageSync.copyDone[i];
            const bool last = (i + 1 == generatedCount);
            if (fns.queueSubmit(queue, 1, &copySubmit, last ? state->fence : VK_NULL_HANDLE)
                    != VK_SUCCESS) {
                setError(*state, "failed to submit generated frame copy");
                break;
            }
            if (last)
                state->fencePending = true;

            VkPresentInfoKHR generatedPresent = *presentInfo;
            // The application's pNext chains describe its own present (IDs,
            // damage regions, modes); they do not apply to inserted frames.
            generatedPresent.pNext = nullptr;
            generatedPresent.waitSemaphoreCount = 1;
            generatedPresent.pWaitSemaphores = &imageSync.copyDone[i];
            generatedPresent.swapchainCount = 1;
            generatedPresent.pSwapchains = &swapchain;
            generatedPresent.pImageIndices = &generatedIndices[i];
            generatedPresent.pResults = nullptr;
            const VkResult generatedResult = presentNext(fns, queue, &generatedPresent, nextPresent);
            if (generatedResult != VK_SUCCESS && generatedResult != VK_SUBOPTIMAL_KHR) {
                state->syncValue = generatedSync;
                state->iteration++;
                if (activeConfig().debug)
                    std::fprintf(stderr, "%s frame %u: generated present failed (%d)\n",
                        TAG, state->iteration, generatedResult);
                state->adaptive.overheadMs =
                    std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - layerStart).count();
                VkPresentInfoKHR originalPresent = *presentInfo;
                originalPresent.waitSemaphoreCount = 1;
                originalPresent.pWaitSemaphores = &imageSync.sourceDone;
                presentNext(fns, queue, &originalPresent, nextPresent);
                return generatedResult;
            }
        }
    } catch (const std::exception& exception) {
        setError(*state, std::string("frame generation failed: ") + exception.what());
    }

    state->adaptive.reportGenerated += generatedCount;
    state->adaptive.overheadMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - layerStart).count();

    VkPresentInfoKHR originalPresent = *presentInfo;
    originalPresent.waitSemaphoreCount = 1;
    originalPresent.pWaitSemaphores = &imageSync.sourceDone;
    const VkResult originalResult = presentNext(fns, queue, &originalPresent, nextPresent);

    state->syncValue = generatedSync;
    state->iteration++;

    if (activeConfig().debug)
        std::fprintf(stderr, "%s frame %u: present original=%d\n",
            TAG, state->iteration, originalResult);

    return originalResult;
}

}
