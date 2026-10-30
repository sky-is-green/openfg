/* SPDX-License-Identifier: MIT */

// fg-lab: standalone harness for the openfg interpolation pipeline.
//
//   fg-lab --a frame_a.rgba --b frame_b.rgba --out frame_t.rgba \
//          --width 2560 --height 1440 --t 0.5 [--gt frame_gt.rgba]
//
// Frames are raw RGBA8. The harness times analyze+generate on the selected
// GPU and optionally reports PSNR against a ground-truth frame.

#include "openfg.hpp"

#include "vkutil/vkutil.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>
#include <vk>

#include <getopt.h>

namespace {
    struct Options {
        std::filesystem::path a;
        std::filesystem::path b;
        std::filesystem::path out;
        std::filesystem::path gt;
        std::filesystem::path dumpFlow;
        int dumpFlowLevel{1};
        bool dumpFlowRaw{false};
        bool dumpFlowReverse{false};
        std::filesystem::path dumpLuma;
        int dumpLumaLevel{-1};
        std::string gpu;
        std::filesystem::path shaders{OPENFG_SHADER_DIR};
        uint32_t width{0};
        uint32_t height{0};
        uint32_t iterations{20};
        uint32_t debugFlags{0};
        float t{0.5F};
    };

    std::vector<uint8_t> readFile(const std::filesystem::path& path, size_t expected) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file)
            throw std::runtime_error("cannot open " + path.string());
        const auto size{static_cast<size_t>(file.tellg())};
        if (expected != 0 && size != expected)
            throw std::runtime_error(
                path.string() + ": expected " + std::to_string(expected) +
                " bytes, got " + std::to_string(size));
        file.seekg(0);
        std::vector<uint8_t> data(size);
        if (!file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size)))
            throw std::runtime_error("cannot read " + path.string());
        return data;
    }

    void writeFile(const std::filesystem::path& path, const std::vector<uint8_t>& data) {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file)
            throw std::runtime_error("cannot open " + path.string() + " for writing");
        file.write(reinterpret_cast<const char*>(data.data()),
            static_cast<std::streamsize>(data.size()));
    }

    std::vector<uint32_t> loadSpirv(const std::filesystem::path& dir, std::string_view name) {
        const auto path{dir / (std::string(name) + ".spv")};
        auto bytes{readFile(path, 0)};
        if (bytes.size() % sizeof(uint32_t) != 0)
            throw std::runtime_error(path.string() + ": not valid SPIR-V");

        std::vector<uint32_t> words(bytes.size() / sizeof(uint32_t));
        std::memcpy(words.data(), bytes.data(), bytes.size());
        return words;
    }

    struct LabImage {
        vk::UniqueImage image;
        vk::UniqueDeviceMemory memory;
        vk::UniqueImageView view;
    };

    LabImage createImage(
        const vk::detail::DispatchLoaderDynamic& dld,
        const vk::PhysicalDevice& physdev,
        const vk::Device& device,
        vk::Extent2D extent,
        vk::Format format,
        uint32_t layers,
        vk::ImageUsageFlags usage
    ) {
        const vk::ImageCreateInfo info{
            .imageType = vk::ImageType::e2D,
            .format = format,
            .extent = { extent.width, extent.height, 1 },
            .mipLevels = 1,
            .arrayLayers = layers,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = usage,
            .sharingMode = vk::SharingMode::eExclusive,
            .initialLayout = vk::ImageLayout::eGeneral
        };

        LabImage result{};
        result.image = device.createImageUnique(info, nullptr, dld);
        const auto requirements{device.getImageMemoryRequirements(*result.image, dld)};
        result.memory = vkutil::allocateMemory(
            dld, device, physdev, requirements.size, requirements.memoryTypeBits, false);
        device.bindImageMemory(*result.image, *result.memory, 0, dld);
        result.view = vkutil::createImageView(dld, device, *result.image, format, layers);
        return result;
    }

    struct StagingBuffer {
        vk::UniqueBuffer buffer;
        vk::UniqueDeviceMemory memory;
        vk::DeviceSize size{};
    };

    StagingBuffer makeStaging(
        const vk::detail::DispatchLoaderDynamic& dld,
        const vk::PhysicalDevice& physdev,
        const vk::Device& device,
        vk::BufferUsageFlags usage,
        const void* data,
        size_t size
    ) {
        auto [buffer, memory] = vkutil::createBuffer(
            dld, device, physdev, usage, data, size);
        return { std::move(buffer), std::move(memory), size };
    }

    void uploadLayer(
        const vk::detail::DispatchLoaderDynamic& dld,
        const vk::Device& device,
        const vk::CommandBuffer& cmdbuf,
        vk::Buffer staging,
        vk::Image image,
        uint32_t width,
        uint32_t height,
        uint32_t layer
    ) {
        const vk::BufferImageCopy region{
            .bufferOffset = 0,
            .bufferRowLength = 0,
            .bufferImageHeight = 0,
            .imageSubresource = {
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = layer,
                .layerCount = 1
            },
            .imageOffset = { 0, 0, 0 },
            .imageExtent = { width, height, 1 }
        };
        cmdbuf.copyBufferToImage(
            staging, image, vk::ImageLayout::eGeneral, { region }, dld);

        const vk::ImageMemoryBarrier barrier{
            .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
            .dstAccessMask = vk::AccessFlagBits::eShaderRead,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eGeneral,
            .srcQueueFamilyIndex = vk::QueueFamilyIgnored,
            .dstQueueFamilyIndex = vk::QueueFamilyIgnored,
            .image = image,
            .subresourceRange = {
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .levelCount = 1,
                .baseArrayLayer = layer,
                .layerCount = 1
            }
        };
        cmdbuf.pipelineBarrier(
            vk::PipelineStageFlagBits::eTransfer,
            vk::PipelineStageFlagBits::eComputeShader,
            {}, {}, {}, { barrier }, dld);
    }

    StagingBuffer uploadImage(
        const vk::detail::DispatchLoaderDynamic& dld,
        const vk::PhysicalDevice& physdev,
        const vk::Device& device,
        const vk::CommandBuffer& cmdbuf,
        vk::Image image,
        const std::vector<uint8_t>& data,
        uint32_t width,
        uint32_t height,
        uint32_t layer
    ) {
        auto staging{makeStaging(
            dld, physdev, device,
            vk::BufferUsageFlagBits::eTransferSrc,
            data.data(), data.size())};
        uploadLayer(dld, device, cmdbuf, *staging.buffer, image, width, height, layer);
        return staging;
    }

    StagingBuffer prepareDownload(
        const vk::detail::DispatchLoaderDynamic& dld,
        const vk::PhysicalDevice& physdev,
        const vk::Device& device,
        const vk::CommandBuffer& cmdbuf,
        vk::Image image,
        uint32_t width,
        uint32_t height
    ) {
        const size_t size{static_cast<size_t>(width) * height * 4};
        auto staging{makeStaging(
            dld, physdev, device,
            vk::BufferUsageFlagBits::eTransferDst,
            nullptr, size)};

        const vk::ImageMemoryBarrier toTransfer{
            .srcAccessMask = vk::AccessFlagBits::eShaderWrite,
            .dstAccessMask = vk::AccessFlagBits::eTransferRead,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eGeneral,
            .srcQueueFamilyIndex = vk::QueueFamilyIgnored,
            .dstQueueFamilyIndex = vk::QueueFamilyIgnored,
            .image = image,
            .subresourceRange = {
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1
            }
        };
        cmdbuf.pipelineBarrier(
            vk::PipelineStageFlagBits::eComputeShader,
            vk::PipelineStageFlagBits::eTransfer,
            {}, {}, {}, { toTransfer }, dld);

        const vk::BufferImageCopy region{
            .bufferOffset = 0,
            .bufferRowLength = 0,
            .bufferImageHeight = 0,
            .imageSubresource = {
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = 1
            },
            .imageOffset = { 0, 0, 0 },
            .imageExtent = { width, height, 1 }
        };
        cmdbuf.copyImageToBuffer(
            image, vk::ImageLayout::eGeneral, *staging.buffer, { region }, dld);

        const vk::MemoryBarrier hostBarrier{
            .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
            .dstAccessMask = vk::AccessFlagBits::eHostRead
        };
        cmdbuf.pipelineBarrier(
            vk::PipelineStageFlagBits::eTransfer,
            vk::PipelineStageFlagBits::eHost,
            {}, { hostBarrier }, {}, {}, dld);

        return staging;
    }

    std::vector<uint8_t> readback(
        const vk::detail::DispatchLoaderDynamic& dld,
        const vk::Device& device,
        const StagingBuffer& staging
    ) {
        std::vector<uint8_t> result(static_cast<size_t>(staging.size));
        void* mapped{device.mapMemory(*staging.memory, 0, staging.size, {}, dld)};
        std::memcpy(result.data(), mapped, static_cast<size_t>(staging.size));
        device.unmapMemory(*staging.memory, dld);
        return result;
    }

    std::vector<uint8_t> readImage(
        const vk::detail::DispatchLoaderDynamic& dld,
        const vk::PhysicalDevice& physdev,
        const vk::Device& device,
        const vk::Queue& queue,
        const vk::CommandPool& cmdPool,
        vk::Image image,
        vk::Extent2D extent,
        uint32_t bytesPerPixel
    ) {
        const size_t size{static_cast<size_t>(extent.width) * extent.height * bytesPerPixel};
        auto staging{makeStaging(
            dld, physdev, device,
            vk::BufferUsageFlagBits::eTransferDst,
            nullptr, size)};

        auto cmdbuf{vkutil::createCommandBuffer(dld, device, cmdPool)};
        auto fence{vkutil::createFence(dld, device)};

        cmdbuf->begin({ .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit }, dld);

        const vk::ImageMemoryBarrier toTransfer{
            .srcAccessMask = vk::AccessFlagBits::eShaderWrite,
            .dstAccessMask = vk::AccessFlagBits::eTransferRead,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eGeneral,
            .srcQueueFamilyIndex = vk::QueueFamilyIgnored,
            .dstQueueFamilyIndex = vk::QueueFamilyIgnored,
            .image = image,
            .subresourceRange = {
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1
            }
        };
        cmdbuf->pipelineBarrier(
            vk::PipelineStageFlagBits::eComputeShader,
            vk::PipelineStageFlagBits::eTransfer,
            {}, {}, {}, { toTransfer }, dld);

        const vk::BufferImageCopy region{
            .bufferOffset = 0,
            .bufferRowLength = 0,
            .bufferImageHeight = 0,
            .imageSubresource = {
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = 1
            },
            .imageOffset = { 0, 0, 0 },
            .imageExtent = { extent.width, extent.height, 1 }
        };
        cmdbuf->copyImageToBuffer(
            image, vk::ImageLayout::eGeneral, *staging.buffer, { region }, dld);

        const vk::MemoryBarrier hostBarrier{
            .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
            .dstAccessMask = vk::AccessFlagBits::eHostRead
        };
        cmdbuf->pipelineBarrier(
            vk::PipelineStageFlagBits::eTransfer,
            vk::PipelineStageFlagBits::eHost,
            {}, { hostBarrier }, {}, {}, dld);

        cmdbuf->end(dld);
        queue.submit({{
            .commandBufferCount = 1,
            .pCommandBuffers = &*cmdbuf
        }}, *fence, dld);
        if (device.waitForFences(*fence, true, UINT64_MAX, dld) != vk::Result::eSuccess)
            throw std::runtime_error("flow readback fence failed");

        return readback(dld, device, staging);
    }

    double psnr(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
        if (a.size() != b.size() || a.size() % 4 != 0)
            throw std::runtime_error("PSNR: size mismatch");
        double mse = 0.0;
        size_t count = 0;
        for (size_t i = 0; i + 3 < a.size(); i += 4) {
            for (size_t c = 0; c < 3; c++) {
                const double d{static_cast<double>(a[i + c]) - static_cast<double>(b[i + c])};
                mse += d * d;
                count++;
            }
        }
        mse /= static_cast<double>(count);
        if (mse <= 1e-12)
            return 99.0;
        return 10.0 * std::log10((255.0 * 255.0) / mse);
    }

    void usage(const char* prog) {
        std::cerr <<
R"(openfg interpolation lab

USAGE:
    )" << prog << R"( --a A.rgba --b B.rgba --out C.rgba --width W --height H [options]

OPTIONS:
    -a, --a <PATH>          Previous frame (raw RGBA8)
    -b, --b <PATH>          Current frame (raw RGBA8)
    -o, --out <PATH>        Output frame (raw RGBA8)
    -g, --gt <PATH>         Ground truth for PSNR
    -w, --width <INT>       Frame width
    -h, --height <INT>      Frame height
    -t, --time <FLOAT>      Interpolation time (default 0.5)
    -n, --iterations <INT>  Timed iterations (default 20)
        --gpu <STRING>      Physical device (name, vendor:device, PCI id)
        --shaders <PATH>    Directory containing compiled .spv files
        --dump-flow <PATH>  Write the half-resolution RG16F flow field
        --debug-source      Output the current source layer (sanity check)
        --dump-luma <LVL>   Write R8 luma pyramids at this level to PATH.a/.b
)";
    }
}

int main(int argc, char** argv) {
    Options opts{};

    const std::array<option, 18> GETOPT{{
        { "a", required_argument, nullptr, 'a' },
        { "b", required_argument, nullptr, 'b' },
        { "out", required_argument, nullptr, 'o' },
        { "gt", required_argument, nullptr, 'g' },
        { "width", required_argument, nullptr, 'w' },
        { "height", required_argument, nullptr, 'h' },
        { "time", required_argument, nullptr, 't' },
        { "iterations", required_argument, nullptr, 'n' },
        { "gpu", required_argument, nullptr, 1 },
        { "shaders", required_argument, nullptr, 2 },
        { "dump-flow", required_argument, nullptr, 3 },
        { "debug-source", no_argument, nullptr, 4 },
        { "dump-luma", required_argument, nullptr, 5 },
        { "flow-level", required_argument, nullptr, 6 },
        { "flow-raw", no_argument, nullptr, 7 },
        { "debug-warp", required_argument, nullptr, 8 },
        { "flow-reverse", no_argument, nullptr, 9 },
        { nullptr, 0, nullptr, 0 }
    }};

    int c{0};
    while ((c = getopt_long(argc, argv, "a:b:o:g:w:h:t:n:", GETOPT.data(), nullptr)) != -1) {
        switch (c) {
            case 'a': opts.a = optarg; break;
            case 'b': opts.b = optarg; break;
            case 'o': opts.out = optarg; break;
            case 'g': opts.gt = optarg; break;
            case 'w': opts.width = static_cast<uint32_t>(std::stoul(optarg)); break;
            case 'h': opts.height = static_cast<uint32_t>(std::stoul(optarg)); break;
            case 't': opts.t = std::stof(optarg); break;
            case 'n': opts.iterations = static_cast<uint32_t>(std::stoul(optarg)); break;
            case 1: opts.gpu = optarg; break;
            case 2: opts.shaders = optarg; break;
            case 3: opts.dumpFlow = optarg; break;
            case 4: opts.debugFlags |= 2; break;
            case 5:
                opts.dumpLuma = optarg;
                opts.dumpLumaLevel = std::stoi(optarg);
                break;
            case 6: opts.dumpFlowLevel = std::stoi(optarg); break;
            case 7: opts.dumpFlowRaw = true; break;
            case 8: opts.debugFlags |= static_cast<uint32_t>(std::stoul(optarg)); break;
            case 9: opts.dumpFlowReverse = true; break;
            default:
                usage(argv[0]);
                return EXIT_FAILURE;
        }
    }

    if (opts.a.empty() || opts.b.empty() || opts.out.empty() ||
        opts.width == 0 || opts.height == 0) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }
    if (opts.t < 0.0F || opts.t > 1.0F) {
        std::cerr << "time must be in [0,1]\n";
        return EXIT_FAILURE;
    }

    try {
        const size_t frameSize{static_cast<size_t>(opts.width) * opts.height * 4};
        const auto frameA{readFile(opts.a, frameSize)};
        const auto frameB{readFile(opts.b, frameSize)};
        const auto groundTruth{opts.gt.empty()
            ? std::vector<uint8_t>{}
            : readFile(opts.gt, frameSize)};

        vk::detail::DispatchLoaderDynamic dld{};
        auto instance{vkutil::createInstance(dld, "fg-lab", vk::ApiVersion13)};
        const auto physdev{vkutil::findPhysicalDevice(dld, *instance, opts.gpu)};
        const uint32_t qfi{vkutil::findQueueFamilyIndex(
            dld, physdev, vk::QueueFlagBits::eCompute)};

        const float priority{1.0F};
        const vk::DeviceQueueCreateInfo queueInfo{
            .queueFamilyIndex = qfi,
            .queueCount = 1,
            .pQueuePriorities = &priority
        };
        const vk::DeviceCreateInfo deviceInfo{
            .queueCreateInfoCount = 1,
            .pQueueCreateInfos = &queueInfo
        };
        auto device{physdev.createDeviceUnique(deviceInfo, nullptr, dld)};
        dld.init(*device);
        const vk::Queue queue{device->getQueue(qfi, 0, dld)};

        const vk::Extent2D extent{opts.width, opts.height};
        constexpr vk::ImageUsageFlags SOURCE_USAGE{
            vk::ImageUsageFlagBits::eSampled |
            vk::ImageUsageFlagBits::eTransferDst
        };
        constexpr vk::ImageUsageFlags DEST_USAGE{
            vk::ImageUsageFlagBits::eStorage |
            vk::ImageUsageFlagBits::eTransferSrc
        };

        auto source{createImage(dld, physdev, *device, extent,
            vk::Format::eR8G8B8A8Unorm, 2, SOURCE_USAGE)};
        auto dest{createImage(dld, physdev, *device, extent,
            vk::Format::eR8G8B8A8Unorm, 1, DEST_USAGE)};

        auto pool{vkutil::createCommandPool(dld, *device, qfi, false)};
        auto cmdbuf{vkutil::createCommandBuffer(dld, *device, *pool)};
        auto fence{vkutil::createFence(dld, *device)};

        const uint32_t queryCount{opts.iterations * 2};
        auto queryPool{device->createQueryPoolUnique({
            .queryType = vk::QueryType::eTimestamp,
            .queryCount = queryCount
        }, nullptr, dld)};

        openfg::Interpolator interpolator(
            dld, physdev, *device, extent,
            *source.image, *dest.image,
            [&opts](std::string_view name) {
                return loadSpirv(opts.shaders, name);
            });

        cmdbuf->begin({ .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit }, dld);
        auto stagingA{uploadImage(dld, physdev, *device, *cmdbuf, *source.image,
            frameA, opts.width, opts.height, 0)};
        auto stagingB{uploadImage(dld, physdev, *device, *cmdbuf, *source.image,
            frameB, opts.width, opts.height, 1)};

        const vk::MemoryBarrier startBarrier{
            .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
            .dstAccessMask = vk::AccessFlagBits::eShaderRead
        };
        cmdbuf->pipelineBarrier(
            vk::PipelineStageFlagBits::eTransfer,
            vk::PipelineStageFlagBits::eComputeShader,
            {}, { startBarrier }, {}, {}, dld);

        for (uint32_t i = 0; i < opts.iterations; i++) {
            cmdbuf->writeTimestamp(
                vk::PipelineStageFlagBits::eComputeShader,
                *queryPool, i * 2, dld);
            interpolator.analyze(*cmdbuf, 0, 1);
            interpolator.generate(*cmdbuf, 0, 1, opts.t, opts.debugFlags);
            cmdbuf->writeTimestamp(
                vk::PipelineStageFlagBits::eComputeShader,
                *queryPool, i * 2 + 1, dld);
        }

        auto readbackBuffer{prepareDownload(dld, physdev, *device, *cmdbuf, *dest.image,
            opts.width, opts.height)};
        cmdbuf->end(dld);

        const auto start{std::chrono::steady_clock::now()};
        queue.submit({{
            .commandBufferCount = 1,
            .pCommandBuffers = &*cmdbuf
        }}, *fence, dld);
        const auto waitResult{device->waitForFences(*fence, true, UINT64_MAX, dld)};
        if (waitResult != vk::Result::eSuccess)
            throw std::runtime_error("fence wait failed");
        const auto end{std::chrono::steady_clock::now()};

        std::vector<uint64_t> timestamps(queryCount);
        const auto queryResult{device->getQueryPoolResults(
            *queryPool,
            0, queryCount,
            timestamps.size() * sizeof(uint64_t),
            timestamps.data(),
            sizeof(uint64_t),
            vk::QueryResultFlagBits::e64,
            dld)};
        if (queryResult != vk::Result::eSuccess)
            throw std::runtime_error("query readback failed");

        const auto props{physdev.getProperties(dld)};
        const double period{static_cast<double>(props.limits.timestampPeriod)};
        double totalMs{0.0};
        double minMs{1e9};
        for (uint32_t i = 0; i < opts.iterations; i++) {
            const double ms{static_cast<double>(timestamps.at(i * 2 + 1) - timestamps.at(i * 2))
                * period / 1e6};
            totalMs += ms;
            minMs = std::min(minMs, ms);
        }

        const double wallMs{
            std::chrono::duration<double, std::milli>(end - start).count() /
            static_cast<double>(opts.iterations)
        };

        const auto output{readback(dld, *device, readbackBuffer)};
        writeFile(opts.out, output);

        if (!opts.dumpFlow.empty()) {
            const auto level{static_cast<uint32_t>(opts.dumpFlowLevel)};
            const auto image{opts.dumpFlowReverse
                ? interpolator.flowReverseImage(level)
                : (opts.dumpFlowRaw
                    ? interpolator.flowRawImage(level)
                    : interpolator.flowImage(level))};
            const auto flow{readImage(dld, physdev, *device, queue,
                *pool, image, interpolator.flowExtent(level), 4)};
            writeFile(opts.dumpFlow, flow);
        }

        if (opts.dumpLumaLevel >= 0) {
            const auto level{static_cast<uint32_t>(opts.dumpLumaLevel)};
            const auto extent{interpolator.levelExtent(level)};
            const auto a{readImage(dld, physdev, *device, queue, *pool,
                interpolator.lumaImage(level, false), extent, 1)};
            const auto b{readImage(dld, physdev, *device, queue, *pool,
                interpolator.lumaImage(level, true), extent, 1)};
            writeFile("/tmp/opencode/luma_a.r8", a);
            writeFile("/tmp/opencode/luma_b.r8", b);
            std::cout << "luma        : " << extent.width << "x" << extent.height
                      << " level " << level << " -> /tmp/opencode/luma_[ab].r8\n";
        }

        std::cout << "device      : " << props.deviceName.data() << '\n'
                  << "extent      : " << opts.width << "x" << opts.height << '\n'
                  << "t           : " << opts.t << '\n'
                  << "gpu avg     : " << (totalMs / opts.iterations) << " ms\n"
                  << "gpu min     : " << minMs << " ms\n"
                  << "wall avg    : " << wallMs << " ms\n"
                  << "output      : " << opts.out << '\n';

        if (!groundTruth.empty())
            std::cout << "psnr (RGB)  : " << psnr(output, groundTruth) << " dB\n";
    } catch (const std::exception& e) {
        std::cerr << "fg-lab: " << e.what() << '\n';
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
