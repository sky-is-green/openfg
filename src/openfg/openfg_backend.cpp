/* SPDX-License-Identifier: MIT */

#include "openfg_backend.hpp"
#include "openfg.hpp"
#include "openfg_shaders.hpp"

#include "vkutil/vkutil.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <vk>

using namespace openfg;

namespace {
    std::vector<uint32_t> loadSpirv(
        const std::filesystem::path& dir,
        std::string_view name
    ) {
        const std::filesystem::path path{dir / (std::string(name) + ".spv")};
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open())
            throw std::runtime_error("openfg: cannot open shader " + path.string());

        const auto size{static_cast<size_t>(file.tellg())};
        if (size == 0 || size % sizeof(uint32_t) != 0)
            throw std::runtime_error("openfg: invalid shader " + path.string());

        std::vector<uint32_t> words(size / sizeof(uint32_t));
        file.seekg(0, std::ios::beg);
        if (!file.read(reinterpret_cast<char*>(words.data()), static_cast<std::streamsize>(size)))
            throw std::runtime_error("openfg: cannot read shader " + path.string());
        return words;
    }

    std::pair<vk::UniqueDevice, vk::Queue> createDevice(
        vk::detail::DispatchLoaderDynamic& dld,
        const vk::PhysicalDevice& physdev,
        uint32_t qfi,
        bool crossGpu
    ) {
        std::vector<const char*> extensions{
            vk::KHRSynchronization2ExtensionName,
            vk::KHRExternalMemoryFdExtensionName,
            vk::KHRExternalSemaphoreFdExtensionName
        };
        if (crossGpu)
            extensions.push_back(vk::EXTExternalMemoryDmaBufExtensionName);

        vk::PhysicalDeviceSynchronization2Features sync2Info{
            .synchronization2 = vk::True
        };
        const vk::PhysicalDeviceVulkan12Features vk12Info{
            .pNext = &sync2Info,
            .timelineSemaphore = vk::True
        };
        const float queuePriority{1.0F};
        const vk::DeviceQueueCreateInfo queueInfo{
            .queueFamilyIndex = qfi,
            .queueCount = 1,
            .pQueuePriorities = &queuePriority
        };
        const vk::DeviceCreateInfo deviceInfo{
            .pNext = &vk12Info,
            .queueCreateInfoCount = 1,
            .pQueueCreateInfos = &queueInfo,
            .enabledExtensionCount = static_cast<uint32_t>(extensions.size()),
            .ppEnabledExtensionNames = extensions.data()
        };
        auto device{physdev.createDeviceUnique(deviceInfo, nullptr, dld)};
        dld.init(*device);

        return { std::move(device), device->getQueue(qfi, 0, dld) };
    }

    std::pair<vk::UniqueImage, vk::UniqueDeviceMemory> createExternalImage(
        const vk::detail::DispatchLoaderDynamic& dld,
        const vk::PhysicalDevice& physdev,
        const vk::Device& device,
        vk::Extent2D extent,
        vk::ImageUsageFlags usage,
        uint32_t layers,
        bool crossGpu,
        vk::Format format
    ) {
        auto image{vkutil::createImage(
            dld, device, extent,
            format,
            layers, usage,
            crossGpu ? vkutil::ExternalMode::DmaBuf : vkutil::ExternalMode::OpaqueFd
        )};

        const auto requirements{device.getImageMemoryRequirements(*image, dld)};
        const vk::ExportMemoryAllocateInfo exportInfo{
            .handleTypes = crossGpu
                ? vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT
                : vk::ExternalMemoryHandleTypeFlagBits::eOpaqueFd
        };
        const vk::MemoryDedicatedAllocateInfo dedicatedInfo{
            .pNext = &exportInfo,
            .image = *image
        };
        auto memory{vkutil::allocateMemory(
            dld, device, physdev,
            requirements.size,
            requirements.memoryTypeBits,
            false,
            &dedicatedInfo
        )};
        device.bindImageMemory(*image, *memory, 0, dld);

        return { std::move(image), std::move(memory) };
    }
}

struct Instance::Impl {
    struct Vk {
        std::unique_ptr<vk::detail::DispatchLoaderDynamic> dld;
        vk::UniqueInstance instance;
        vk::PhysicalDevice physdev;
        vk::UniqueDevice device;
        vk::Queue queue;
        uint32_t qfi{};
        bool leakInstance{false};
    } vk;

    std::filesystem::path shaderDir;
    bool crossGpu{false};
    std::function<void(const std::string&)> logCallback;
};

Instance::Instance(
    const std::string& deviceId,
    const std::filesystem::path& shaderDir,
    bool crossGpu,
    bool leakInstance,
    std::function<void(const std::string&)> logCallback
) {
    auto dld{std::make_unique<vk::detail::DispatchLoaderDynamic>()};
    auto instance{vkutil::createInstance(
        *dld,
        "openfg",
        vk::ApiVersion12
    )};
    auto physdev{vkutil::findPhysicalDevice(*dld, *instance, deviceId)};
    const uint32_t qfi{vkutil::findQueueFamilyIndex(
        *dld, physdev, vk::QueueFlagBits::eCompute
    )};

    auto [device, queue] = createDevice(*dld, physdev, qfi, crossGpu);

    this->m_impl = std::make_unique<Impl>(Impl{
        .vk = {
            .dld = std::move(dld),
            .instance = std::move(instance),
            .physdev = physdev,
            .device = std::move(device),
            .queue = queue,
            .qfi = qfi,
            .leakInstance = leakInstance
        },
        .shaderDir = shaderDir,
        .crossGpu = crossGpu,
        .logCallback = std::move(logCallback)
    });

    if (this->m_impl->logCallback)
        this->m_impl->logCallback("openfg: backend initialized on " + deviceId);
}

struct Context::Impl {
    const Instance::Impl* instance{};
    std::unique_ptr<openfg::Interpolator> interpolator;

    vk::UniqueImage sourceImage;
    vk::UniqueDeviceMemory sourceMemory;
    vk::UniqueImage destinationImage;
    vk::UniqueDeviceMemory destinationMemory;

    std::pair<vk::UniqueSemaphore, uint64_t> syncSemaphore;
    std::pair<vk::UniqueSemaphore, uint64_t> internalSemaphore;
    std::pair<vk::UniqueFence, bool> fence;

    vk::UniqueCommandPool pool;
    vk::UniqueCommandBuffer analyzeCmdbuf;
    std::array<vk::UniqueCommandBuffer, 4> generateCmdbufs{};

    uint32_t iteration{};
    uint32_t total{};
    uint32_t index{};
    uint32_t layerA{};
    uint32_t layerB{};
    bool firstIteration{true};
};

Context::Context(
    const Instance& instance,
    uint32_t width,
    uint32_t height,
    float flowScale,
    bool performanceMode,
    bool videoSafe,
    vk::Format colorFormat
) {
    (void)flowScale;

    const auto& vk{instance.m_impl->vk};
    const vk::Extent2D extent{width, height};

    auto [sourceImage, sourceMemory] = createExternalImage(
        *vk.dld, vk.physdev, *vk.device, extent,
        vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
        2, instance.m_impl->crossGpu, colorFormat
    );
    auto [destinationImage, destinationMemory] = createExternalImage(
        *vk.dld, vk.physdev, *vk.device, extent,
        vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
        vk::ImageUsageFlagBits::eTransferSrc,
        openfg::kMaxInterpolatedFrames, instance.m_impl->crossGpu, colorFormat
    );

    const auto shaderDir{instance.m_impl->shaderDir};
    auto interpolator{std::make_unique<openfg::Interpolator>(
        *vk.dld,
        vk.physdev,
        *vk.device,
        extent,
        *sourceImage,
        *destinationImage,
        [shaderDir](std::string_view name) {
            if (!shaderDir.empty())
                return loadSpirv(shaderDir, name);

            const auto shader{openfg::embeddedShader(name)};
            if (shader.empty())
                throw std::runtime_error(
                    "openfg: missing embedded shader '" + std::string(name) + "'");
            return std::vector<uint32_t>(shader.begin(), shader.end());
        },
        performanceMode,
        videoSafe,
        colorFormat
    )};

    auto pool{vkutil::createCommandPool(*vk.dld, *vk.device, vk.qfi, true)};
    auto analyze{vkutil::createCommandBuffer(*vk.dld, *vk.device, *pool)};
    std::array<vk::UniqueCommandBuffer, 4> generate{};
    for (auto& cmdbuf : generate)
        cmdbuf = vkutil::createCommandBuffer(*vk.dld, *vk.device, *pool);

    this->m_impl = std::make_unique<Impl>(Impl{
        .instance = instance.m_impl.get(),
        .interpolator = std::move(interpolator),
        .sourceImage = std::move(sourceImage),
        .sourceMemory = std::move(sourceMemory),
        .destinationImage = std::move(destinationImage),
        .destinationMemory = std::move(destinationMemory),
        .syncSemaphore = {
            vkutil::createSemaphore(*vk.dld, *vk.device, true, true), 0
        },
        .internalSemaphore = {
            vkutil::createSemaphore(*vk.dld, *vk.device, true), 0
        },
        .fence = {
            vkutil::createFence(*vk.dld, *vk.device), false
        },
        .pool = std::move(pool),
        .analyzeCmdbuf = std::move(analyze),
        .generateCmdbufs = std::move(generate)
    });
}

FileDescriptors Context::exportFds() const {
    const auto& vk{this->m_impl->instance->vk};
    const auto mode{this->m_impl->instance->crossGpu
        ? vkutil::ExternalMode::DmaBuf
        : vkutil::ExternalMode::OpaqueFd};

    return {
        .sourceFd = vkutil::exportMemoryFd(
            *vk.dld, *vk.device, *this->m_impl->sourceMemory, mode),
        .destinationFd = vkutil::exportMemoryFd(
            *vk.dld, *vk.device, *this->m_impl->destinationMemory, mode),
        .syncFd = vkutil::exportSemaphoreFd(
            *vk.dld, *vk.device, *this->m_impl->syncSemaphore.first)
    };
}

void Context::dispatch(uint32_t total, bool unsignaled) {
    auto& ctx{*this->m_impl};
    const auto& vk{ctx.instance->vk};

    if (ctx.firstIteration) {
        ctx.firstIteration = false;
        ctx.iteration = 0;
    } else {
        const auto result{vk.device->waitForFences(
            *ctx.fence.first, true, UINT64_MAX, *vk.dld)};
        if (result != vk::Result::eSuccess)
            throw std::runtime_error("openfg: failed waiting for previous frame");
        vk.device->resetFences(*ctx.fence.first, *vk.dld);
        ctx.fence.second = false;
        ctx.iteration++;
    }

    ctx.layerB = ctx.iteration % 2;
    ctx.layerA = ctx.iteration == 0 ? ctx.layerB : (ctx.iteration + 1) % 2;

    ctx.analyzeCmdbuf->reset({}, *vk.dld);
    ctx.analyzeCmdbuf->begin(
        { .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit }, *vk.dld);
    ctx.interpolator->analyze(*ctx.analyzeCmdbuf, ctx.layerA, ctx.layerB);
    ctx.analyzeCmdbuf->end(*vk.dld);

    auto& sync{ctx.syncSemaphore};
    auto& internal{ctx.internalSemaphore};
    sync.second++;
    internal.second++;

    const vk::TimelineSemaphoreSubmitInfo timelineInfo{
        .waitSemaphoreValueCount = unsignaled ? 0U : 1U,
        .pWaitSemaphoreValues = &sync.second,
        .signalSemaphoreValueCount = 1,
        .pSignalSemaphoreValues = &internal.second
    };
    const vk::PipelineStageFlags waitStage{vk::PipelineStageFlagBits::eTopOfPipe};

    vk.queue.submit(
        {{
            .pNext = &timelineInfo,
            .waitSemaphoreCount = timelineInfo.waitSemaphoreValueCount,
            .pWaitSemaphores = &*sync.first,
            .pWaitDstStageMask = &waitStage,
            .commandBufferCount = 1U,
            .pCommandBuffers = &*ctx.analyzeCmdbuf,
            .signalSemaphoreCount = 1,
            .pSignalSemaphores = &*internal.first
        }},
        nullptr,
        *vk.dld
    );

    ctx.total = total;
    ctx.index = 0;
}

void Context::acquire(bool unsignaled) {
    auto& ctx{*this->m_impl};
    const auto& vk{ctx.instance->vk};

    if (ctx.total == 0 || ctx.index >= ctx.total)
        throw std::runtime_error("openfg: no more frames to acquire");

    const float t{static_cast<float>(ctx.index + 1) /
        static_cast<float>(ctx.total + 1)};

    auto& generate{*ctx.generateCmdbufs.at(ctx.index)};
    generate.reset({}, *vk.dld);
    generate.begin(
        { .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit }, *vk.dld);
    ctx.interpolator->generate(
        generate, ctx.layerA, ctx.layerB, t, 0, ctx.index);
    generate.end(*vk.dld);

    auto& sync{ctx.syncSemaphore};
    auto& internal{ctx.internalSemaphore};

    const uint64_t waitValue{internal.second};
    sync.second++;
    const uint64_t signalValue{sync.second};

    const vk::TimelineSemaphoreSubmitInfo timelineInfo{
        .waitSemaphoreValueCount = 1,
        .pWaitSemaphoreValues = &waitValue,
        .signalSemaphoreValueCount = unsignaled ? 0U : 1U,
        .pSignalSemaphoreValues = &signalValue
    };
    const vk::PipelineStageFlags waitStage{vk::PipelineStageFlagBits::eTopOfPipe};

    vk.queue.submit(
        {{
            .pNext = &timelineInfo,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &*internal.first,
            .pWaitDstStageMask = &waitStage,
            .commandBufferCount = 1,
            .pCommandBuffers = &generate,
            .signalSemaphoreCount = unsignaled ? 0U : 1U,
            .pSignalSemaphores = &*sync.first
        }},
        ctx.index == (ctx.total - 1) ? *ctx.fence.first : nullptr,
        *vk.dld
    );

    if (ctx.index == (ctx.total - 1))
        ctx.fence.second = true;

    ctx.index++;
}

void Context::idle() {
    if (!this->m_impl->fence.second)
        return;

    auto& ctx{*this->m_impl};
    const auto& vk{ctx.instance->vk};
    if (vk.device->waitForFences(*ctx.fence.first, true, UINT64_MAX, *vk.dld)
            != vk::Result::eSuccess)
        throw std::runtime_error("openfg: failed to idle");
}

Context::~Context() {
    try {
        this->idle();
    } catch (...) { // NOLINT
    }
}

Instance::~Instance() {
    if (!this->m_impl->vk.leakInstance) {
        try {
            this->m_impl->vk.device->waitIdle(*this->m_impl->vk.dld);
        } catch (...) { // NOLINT
        }
        return;
    }

    try {
        new std::unique_ptr<vk::detail::DispatchLoaderDynamic>(
            std::move(this->m_impl->vk.dld));
        new vk::UniqueDevice(std::move(this->m_impl->vk.device));
        new vk::UniqueInstance(std::move(this->m_impl->vk.instance));
    } catch (...) { // NOLINT
        if (!this->m_impl->logCallback)
            return;
        this->m_impl->logCallback("openfg: failed to leak Vulkan instance");
    }
}
