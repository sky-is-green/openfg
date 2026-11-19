/* SPDX-License-Identifier: MIT */
// scaler-gpu: standalone EASU/RCAS harness for the companion scaler work.

#include "fsr1_constants.hpp"
#include "openfg_shaders.hpp"
#include "vkutil/vkutil.hpp"

#include <vk>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>
#include <getopt.h>

namespace {

struct Options {
    std::string input;
    std::string output;
    uint32_t width{};
    uint32_t height{};
    uint32_t outputWidth{};
    uint32_t outputHeight{};
    std::string filter{"easu+rcas"};
    std::string gpu{"auto"};
    float sharpness{0.0F};
    uint32_t iterations{1};
};

struct Image {
    vk::UniqueImage image;
    vk::UniqueDeviceMemory memory;
    vk::UniqueImageView view;
};

struct Staging {
    vk::UniqueBuffer buffer;
    vk::UniqueDeviceMemory memory;
    vk::DeviceSize size{};
};

struct PushConstants {
    std::array<uint32_t, 4> c0{};
    std::array<uint32_t, 4> c1{};
    std::array<uint32_t, 4> c2{};
    std::array<uint32_t, 4> c3{};
    std::array<uint32_t, 4> sample{};
};

void usage(const char* argv0) {
    std::cout
        << "usage: " << argv0 << " --input low.rgba --output high.rgba\n"
        << "       --width W --height H [--out-width OW --out-height OH]\n"
        << "       [--filter easu|rcas|easu+rcas] [--sharpness 0..20]\n"
        << "       [--gpu auto|PCI|name] [--iterations N]\n\n"
        << "Input and output are tightly packed RGBA8. The output is RGBA8\n"
        << "at --out-width x --out-height (default 2x the input).\n";
}

std::vector<uint8_t> readFile(const std::string& path, size_t expected) {
    std::ifstream file{path, std::ios::binary};
    if (!file)
        throw std::runtime_error("cannot open " + path);
    file.seekg(0, std::ios::end);
    const auto size = static_cast<size_t>(file.tellg());
    file.seekg(0, std::ios::beg);
    if (expected != 0 && size != expected)
        throw std::runtime_error(path + ": expected " +
            std::to_string(expected) + " bytes, got " + std::to_string(size));
    std::vector<uint8_t> data(size);
    file.read(reinterpret_cast<char*>(data.data()),
              static_cast<std::streamsize>(size));
    return data;
}

void writeFile(const std::string& path, const std::vector<uint8_t>& data) {
    std::ofstream file{path, std::ios::binary};
    if (!file)
        throw std::runtime_error("cannot write " + path);
    file.write(reinterpret_cast<const char*>(data.data()),
               static_cast<std::streamsize>(data.size()));
}

Image createImage(
    const vk::detail::DispatchLoaderDynamic& dld,
    const vk::PhysicalDevice& physdev,
    const vk::Device& device,
    vk::Extent2D extent,
    vk::Format format,
    vk::ImageUsageFlags usage
) {
    const vk::ImageCreateInfo info{
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = { extent.width, extent.height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = usage,
        .sharingMode = vk::SharingMode::eExclusive,
        .initialLayout = vk::ImageLayout::eUndefined
    };
    Image result;
    result.image = device.createImageUnique(info, nullptr, dld);
    const auto requirements{device.getImageMemoryRequirements(*result.image, dld)};
    result.memory = vkutil::allocateMemory(
        dld, device, physdev, requirements.size, requirements.memoryTypeBits);
    device.bindImageMemory(*result.image, *result.memory, 0, dld);
    result.view = vkutil::createImageView(dld, device, *result.image, format, 1);
    return result;
}

Staging makeStaging(
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

void imageBarrier(
    const vk::detail::DispatchLoaderDynamic& dld,
    const vk::CommandBuffer& cmdbuf,
    vk::Image image,
    vk::ImageLayout oldLayout,
    vk::ImageLayout newLayout,
    vk::AccessFlags srcAccess,
    vk::AccessFlags dstAccess,
    vk::PipelineStageFlags srcStage,
    vk::PipelineStageFlags dstStage
) {
    const vk::ImageMemoryBarrier barrier{
        .srcAccessMask = srcAccess,
        .dstAccessMask = dstAccess,
        .oldLayout = oldLayout,
        .newLayout = newLayout,
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
    cmdbuf.pipelineBarrier(srcStage, dstStage, {}, {}, {}, { barrier }, dld);
}

Staging uploadImage(
    const vk::detail::DispatchLoaderDynamic& dld,
    const vk::PhysicalDevice& physdev,
    const vk::Device& device,
    const vk::CommandBuffer& cmdbuf,
    vk::Image image,
    const std::vector<uint8_t>& data,
    uint32_t width,
    uint32_t height
) {
    auto staging{makeStaging(dld, physdev, device,
        vk::BufferUsageFlagBits::eTransferSrc, data.data(), data.size())};
    imageBarrier(dld, cmdbuf, image,
        vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
        {}, vk::AccessFlagBits::eTransferWrite,
        vk::PipelineStageFlagBits::eTopOfPipe,
        vk::PipelineStageFlagBits::eTransfer);
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
    cmdbuf.copyBufferToImage(*staging.buffer, image,
        vk::ImageLayout::eTransferDstOptimal, { region }, dld);
    imageBarrier(dld, cmdbuf, image,
        vk::ImageLayout::eTransferDstOptimal,
        vk::ImageLayout::eShaderReadOnlyOptimal,
        vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eShaderRead,
        vk::PipelineStageFlagBits::eTransfer,
        vk::PipelineStageFlagBits::eComputeShader);
    return staging;
}

Staging downloadImage(
    const vk::detail::DispatchLoaderDynamic& dld,
    const vk::PhysicalDevice& physdev,
    const vk::Device& device,
    const vk::CommandBuffer& cmdbuf,
    vk::Image image,
    uint32_t width,
    uint32_t height
) {
    const size_t size{static_cast<size_t>(width) * height * 16};
    auto staging{makeStaging(dld, physdev, device,
        vk::BufferUsageFlagBits::eTransferDst, nullptr, size)};
    imageBarrier(dld, cmdbuf, image,
        vk::ImageLayout::eGeneral, vk::ImageLayout::eTransferSrcOptimal,
        vk::AccessFlagBits::eShaderWrite, vk::AccessFlagBits::eTransferRead,
        vk::PipelineStageFlagBits::eComputeShader,
        vk::PipelineStageFlagBits::eTransfer);
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
    cmdbuf.copyImageToBuffer(image, vk::ImageLayout::eTransferSrcOptimal,
        *staging.buffer, { region }, dld);
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

std::vector<uint8_t> readStaging(
    const vk::detail::DispatchLoaderDynamic& dld,
    const vk::Device& device,
    const Staging& staging
) {
    std::vector<uint8_t> data(static_cast<size_t>(staging.size));
    void* mapped{device.mapMemory(*staging.memory, 0, staging.size, {}, dld)};
    std::memcpy(data.data(), mapped, data.size());
    device.unmapMemory(*staging.memory, dld);
    return data;
}

void updateSet(
    const vk::detail::DispatchLoaderDynamic& dld,
    const vk::Device& device,
    vk::DescriptorSet set,
    vk::ImageView input,
    vk::ImageLayout inputLayout,
    vk::Sampler sampler,
    vk::ImageView output
) {
    const vk::DescriptorImageInfo inputInfo{
        .sampler = nullptr,
        .imageView = input,
        .imageLayout = inputLayout
    };
    const vk::DescriptorImageInfo samplerInfo{
        .sampler = sampler,
        .imageView = nullptr,
        .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal
    };
    const vk::DescriptorImageInfo outputInfo{
        .sampler = nullptr,
        .imageView = output,
        .imageLayout = vk::ImageLayout::eGeneral
    };
    const std::array<vk::WriteDescriptorSet, 3> writes{{
        {
            .dstSet = set,
            .dstBinding = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eSampledImage,
            .pImageInfo = &inputInfo
        },
        {
            .dstSet = set,
            .dstBinding = 1,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eSampler,
            .pImageInfo = &samplerInfo
        },
        {
            .dstSet = set,
            .dstBinding = 2,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eStorageImage,
            .pImageInfo = &outputInfo
        }
    }};
    device.updateDescriptorSets(static_cast<uint32_t>(writes.size()),
        writes.data(), 0, nullptr, dld);
}

vk::UniquePipeline makePipeline(
    const vk::detail::DispatchLoaderDynamic& dld,
    const vk::Device& device,
    const vk::PipelineLayout& layout,
    std::string_view name
) {
    const auto code{openfg::embeddedShader(name)};
    if (code.empty())
        throw std::runtime_error("missing embedded shader " + std::string(name));
    auto module{vkutil::createShaderModule(dld, device, code)};
    const vk::ComputePipelineCreateInfo info{
        .stage = {
            .stage = vk::ShaderStageFlagBits::eCompute,
            .module = *module,
            .pName = "main"
        },
        .layout = layout
    };
    auto result{device.createComputePipelineUnique(nullptr, info, nullptr, dld)};
    if (result.result != vk::Result::eSuccess)
        throw std::runtime_error("pipeline creation failed for " +
            std::string(name));
    return std::move(result.value);
}

std::vector<uint8_t> floatsToRgba8(const std::vector<uint8_t>& raw) {
    if (raw.size() % 16 != 0)
        throw std::runtime_error("float readback is not RGBA32F");
    const size_t pixels{raw.size() / 16};
    std::vector<float> values(pixels * 4);
    std::memcpy(values.data(), raw.data(), raw.size());
    std::vector<uint8_t> rgba(pixels * 4);
    for (size_t i = 0; i < values.size(); i++) {
        const float value = std::clamp(values[i], 0.0F, 1.0F);
        rgba[i] = static_cast<uint8_t>(value * 255.0F + 0.5F);
    }
    return rgba;
}

} // namespace

int main(int argc, char** argv) {
    Options opts;
    enum {
        OPT_OUT_WIDTH = 1000,
        OPT_OUT_HEIGHT,
        OPT_FILTER,
        OPT_SHARPNESS,
        OPT_GPU,
        OPT_ITERATIONS
    };
    const option longOptions[] = {
        { "input", required_argument, nullptr, 'i' },
        { "output", required_argument, nullptr, 'o' },
        { "width", required_argument, nullptr, 'w' },
        { "height", required_argument, nullptr, 'h' },
        { "out-width", required_argument, nullptr, OPT_OUT_WIDTH },
        { "out-height", required_argument, nullptr, OPT_OUT_HEIGHT },
        { "filter", required_argument, nullptr, OPT_FILTER },
        { "sharpness", required_argument, nullptr, OPT_SHARPNESS },
        { "gpu", required_argument, nullptr, OPT_GPU },
        { "iterations", required_argument, nullptr, OPT_ITERATIONS },
        { "help", no_argument, nullptr, '?' },
        { nullptr, 0, nullptr, 0 }
    };
    int index{0};
    while (true) {
        const int c{getopt_long(argc, argv, "i:o:w:h:", longOptions, &index)};
        if (c == -1)
            break;
        switch (c) {
            case 'i': opts.input = optarg; break;
            case 'o': opts.output = optarg; break;
            case 'w': opts.width = static_cast<uint32_t>(std::stoul(optarg)); break;
            case 'h': opts.height = static_cast<uint32_t>(std::stoul(optarg)); break;
            case OPT_OUT_WIDTH:
                opts.outputWidth = static_cast<uint32_t>(std::stoul(optarg));
                break;
            case OPT_OUT_HEIGHT:
                opts.outputHeight = static_cast<uint32_t>(std::stoul(optarg));
                break;
            case OPT_FILTER: opts.filter = optarg; break;
            case OPT_SHARPNESS: opts.sharpness = std::stof(optarg); break;
            case OPT_GPU: opts.gpu = optarg; break;
            case OPT_ITERATIONS:
                opts.iterations = static_cast<uint32_t>(std::stoul(optarg));
                break;
            default: usage(argv[0]); return EXIT_FAILURE;
        }
    }
    if (opts.input.empty() || opts.output.empty() ||
        opts.width == 0 || opts.height == 0) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }
    if (opts.outputWidth == 0)
        opts.outputWidth = opts.width * 2;
    if (opts.outputHeight == 0)
        opts.outputHeight = opts.height * 2;
    if (opts.iterations == 0)
        opts.iterations = 1;
    const bool doEasu{opts.filter == "easu" || opts.filter == "easu+rcas"};
    const bool doRcas{opts.filter == "rcas" || opts.filter == "easu+rcas"};
    if (!doEasu && !doRcas)
        throw std::runtime_error("unknown filter: " + opts.filter);
    if (doRcas && !doEasu &&
        (opts.outputWidth != opts.width || opts.outputHeight != opts.height))
        throw std::runtime_error(
            "rcas does not scale; use easu+rcas or matching output size");

    try {
        const auto input{readFile(opts.input,
            static_cast<size_t>(opts.width) * opts.height * 4)};

        vk::detail::DispatchLoaderDynamic dld{};
        auto instance{vkutil::createInstance(dld, "scaler-gpu", vk::ApiVersion13)};
        const auto physdev{vkutil::findPhysicalDevice(
            dld, *instance, opts.gpu == "auto" ? "" : opts.gpu)};
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

        const vk::Extent2D inputExtent{opts.width, opts.height};
        const vk::Extent2D outputExtent{opts.outputWidth, opts.outputHeight};
        const auto inputFormat{vk::Format::eR8G8B8A8Unorm};
        const auto outputFormat{vk::Format::eR32G32B32A32Sfloat};

        auto source{createImage(dld, physdev, *device, inputExtent, inputFormat,
            vk::ImageUsageFlagBits::eSampled |
            vk::ImageUsageFlagBits::eTransferDst)};
        auto finalImage{createImage(dld, physdev, *device, outputExtent,
            outputFormat,
            vk::ImageUsageFlagBits::eStorage |
            vk::ImageUsageFlagBits::eTransferSrc)};
        Image intermediate;
        if (doEasu && doRcas) {
            intermediate = createImage(dld, physdev, *device, outputExtent,
                outputFormat,
                vk::ImageUsageFlagBits::eStorage |
                vk::ImageUsageFlagBits::eSampled |
                vk::ImageUsageFlagBits::eTransferSrc);
        }

        auto sampler{vkutil::createSampler(
            dld, *device, vk::SamplerAddressMode::eClampToEdge,
            vk::CompareOp::eNever, false)};
        const std::array<vk::DescriptorSetLayoutBinding, 3> bindings{{
            {
                .binding = 0,
                .descriptorType = vk::DescriptorType::eSampledImage,
                .descriptorCount = 1,
                .stageFlags = vk::ShaderStageFlagBits::eCompute
            },
            {
                .binding = 1,
                .descriptorType = vk::DescriptorType::eSampler,
                .descriptorCount = 1,
                .stageFlags = vk::ShaderStageFlagBits::eCompute
            },
            {
                .binding = 2,
                .descriptorType = vk::DescriptorType::eStorageImage,
                .descriptorCount = 1,
                .stageFlags = vk::ShaderStageFlagBits::eCompute
            }
        }};
        auto [setLayout, pipelineLayout] = vkutil::createLayout(
            dld, *device, { bindings.begin(), bindings.end() },
            sizeof(PushConstants));

        vk::UniqueDescriptorPool easuPool;
        vk::DescriptorSet easuSet;
        vk::UniqueDescriptorPool rcasPool;
        vk::DescriptorSet rcasSet;
        if (doEasu) {
            auto [pool, set] = vkutil::createDescriptorSet(
                dld, *device, *setLayout, 1, 0, 1, 1);
            easuPool = std::move(pool);
            easuSet = set;
            updateSet(dld, *device, easuSet, *source.view,
                vk::ImageLayout::eShaderReadOnlyOptimal, *sampler,
                *(doRcas ? intermediate.view : finalImage.view));
        }
        if (doRcas) {
            auto [pool, set] = vkutil::createDescriptorSet(
                dld, *device, *setLayout, 1, 0, 1, 1);
            rcasPool = std::move(pool);
            rcasSet = set;
            const auto rcasInputLayout{doEasu
                ? vk::ImageLayout::eGeneral
                : vk::ImageLayout::eShaderReadOnlyOptimal};
            updateSet(dld, *device, rcasSet,
                *(doEasu ? intermediate.view : source.view), rcasInputLayout,
                *sampler, *finalImage.view);
        }

        auto easuPipeline{doEasu ? makePipeline(
            dld, *device, *pipelineLayout, "fsr1_easu") : vk::UniquePipeline{}};
        auto rcasPipeline{doRcas ? makePipeline(
            dld, *device, *pipelineLayout, "fsr1_rcas") : vk::UniquePipeline{}};

        auto pool{vkutil::createCommandPool(dld, *device, qfi, false)};
        auto cmdbuf{vkutil::createCommandBuffer(dld, *device, *pool)};
        auto fence{vkutil::createFence(dld, *device)};

        const uint32_t passes{static_cast<uint32_t>(
            (doEasu ? 1U : 0U) + (doRcas ? 1U : 0U))};
        const uint32_t queryCount{opts.iterations * passes * 2};
        auto queries{device->createQueryPoolUnique({
            .queryType = vk::QueryType::eTimestamp,
            .queryCount = queryCount
        }, nullptr, dld)};

        const auto easuConstants{fsr1EasuConstants(
            opts.width, opts.height, opts.outputWidth, opts.outputHeight)};
        const auto rcasConstants{fsr1RcasConstants(opts.sharpness)};
        PushConstants easuPush{};
        std::copy_n(easuConstants.begin(), 16, easuPush.c0.begin());
        PushConstants rcasPush{};
        std::copy_n(rcasConstants.begin(), 4, rcasPush.c0.begin());

        cmdbuf->begin({ .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit }, dld);
        auto uploadStaging{uploadImage(dld, physdev, *device, *cmdbuf,
            *source.image, input, opts.width, opts.height)};
        imageBarrier(dld, *cmdbuf, *finalImage.image,
            vk::ImageLayout::eUndefined, vk::ImageLayout::eGeneral,
            {}, vk::AccessFlagBits::eShaderWrite,
            vk::PipelineStageFlagBits::eTopOfPipe,
            vk::PipelineStageFlagBits::eComputeShader);
        if (doEasu && doRcas) {
            imageBarrier(dld, *cmdbuf, *intermediate.image,
                vk::ImageLayout::eUndefined, vk::ImageLayout::eGeneral,
                {}, vk::AccessFlagBits::eShaderWrite,
                vk::PipelineStageFlagBits::eTopOfPipe,
                vk::PipelineStageFlagBits::eComputeShader);
        }

        uint32_t query{0};
        const uint32_t groupX{(opts.outputWidth + 7) / 8};
        const uint32_t groupY{(opts.outputHeight + 7) / 8};
        for (uint32_t iteration = 0; iteration <= opts.iterations; iteration++) {
            const bool timed{iteration > 0};
            if (doEasu) {
                if (doRcas && iteration > 0) {
                    imageBarrier(dld, *cmdbuf, *intermediate.image,
                        vk::ImageLayout::eGeneral,
                        vk::ImageLayout::eGeneral,
                        vk::AccessFlagBits::eShaderRead,
                        vk::AccessFlagBits::eShaderWrite,
                        vk::PipelineStageFlagBits::eComputeShader,
                        vk::PipelineStageFlagBits::eComputeShader);
                }
                cmdbuf->bindPipeline(vk::PipelineBindPoint::eCompute,
                    *easuPipeline, dld);
                cmdbuf->bindDescriptorSets(vk::PipelineBindPoint::eCompute,
                    *pipelineLayout, 0, { easuSet }, {}, dld);
                cmdbuf->pushConstants(*pipelineLayout,
                    vk::ShaderStageFlagBits::eCompute, 0,
                    sizeof(easuPush), &easuPush, dld);
                if (timed) cmdbuf->writeTimestamp(
                    vk::PipelineStageFlagBits::eComputeShader,
                    *queries, query++, dld);
                cmdbuf->dispatch(groupX, groupY, 1, dld);
                if (timed) cmdbuf->writeTimestamp(
                    vk::PipelineStageFlagBits::eComputeShader,
                    *queries, query++, dld);
                if (doRcas) {
                    imageBarrier(dld, *cmdbuf, *intermediate.image,
                        vk::ImageLayout::eGeneral,
                        vk::ImageLayout::eGeneral,
                        vk::AccessFlagBits::eShaderWrite,
                        vk::AccessFlagBits::eShaderRead,
                        vk::PipelineStageFlagBits::eComputeShader,
                        vk::PipelineStageFlagBits::eComputeShader);
                }
            }
            if (doRcas) {
                cmdbuf->bindPipeline(vk::PipelineBindPoint::eCompute,
                    *rcasPipeline, dld);
                cmdbuf->bindDescriptorSets(vk::PipelineBindPoint::eCompute,
                    *pipelineLayout, 0, { rcasSet }, {}, dld);
                cmdbuf->pushConstants(*pipelineLayout,
                    vk::ShaderStageFlagBits::eCompute, 0,
                    sizeof(rcasPush), &rcasPush, dld);
                if (timed) cmdbuf->writeTimestamp(
                    vk::PipelineStageFlagBits::eComputeShader,
                    *queries, query++, dld);
                cmdbuf->dispatch(groupX, groupY, 1, dld);
                if (timed) cmdbuf->writeTimestamp(
                    vk::PipelineStageFlagBits::eComputeShader,
                    *queries, query++, dld);
            }
        }

        auto staging{downloadImage(dld, physdev, *device, *cmdbuf,
            *finalImage.image, opts.outputWidth, opts.outputHeight)};
        cmdbuf->end(dld);

        const auto start{std::chrono::steady_clock::now()};
        queue.submit({{
            .commandBufferCount = 1,
            .pCommandBuffers = &*cmdbuf
        }}, *fence, dld);
        if (device->waitForFences(*fence, true, UINT64_MAX, dld) !=
            vk::Result::eSuccess)
            throw std::runtime_error("fence wait failed");
        const auto end{std::chrono::steady_clock::now()};
        (void)uploadStaging;

        std::vector<uint64_t> timestamps(queryCount);
        if (device->getQueryPoolResults(*queries, 0, queryCount,
                timestamps.size() * sizeof(uint64_t), timestamps.data(),
                sizeof(uint64_t), vk::QueryResultFlagBits::e64, dld) !=
            vk::Result::eSuccess)
            throw std::runtime_error("query readback failed");
        const auto props{physdev.getProperties(dld)};
        const double period{static_cast<double>(props.limits.timestampPeriod)};
        double totalMs{0.0};
        double minMs{1e9};
        for (uint32_t i = 0; i < opts.iterations * passes; i++) {
            const double ms{
                static_cast<double>(timestamps[i * 2 + 1] - timestamps[i * 2]) *
                period / 1e6};
            totalMs += ms;
            minMs = std::min(minMs, ms);
        }
        const double wallMs{
            std::chrono::duration<double, std::milli>(end - start).count() /
            static_cast<double>(opts.iterations)};

        auto raw{readStaging(dld, *device, staging)};
        writeFile(opts.output, floatsToRgba8(raw));

        const double averageMs{totalMs / (opts.iterations * passes)};
        std::cout << "filter: " << opts.filter << "\n"
                  << "input: " << opts.width << "x" << opts.height << "\n"
                  << "output: " << opts.outputWidth << "x" << opts.outputHeight << "\n"
                  << "gpu: " << props.deviceName.data() << "\n"
                  << "gpu avg: " << averageMs << " ms\n"
                  << "gpu min: " << minMs << " ms\n"
                  << "wall avg (incl. warm-up): " << wallMs << " ms\n"
                  << "iterations: " << opts.iterations << "\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "scaler-gpu: " << error.what() << "\n";
        return EXIT_FAILURE;
    }
}
