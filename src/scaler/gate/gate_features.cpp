/* SPDX-License-Identifier: MIT */

#include "gate_features.hpp"

#include "openfg_shaders.hpp"
#include "vkutil/vkutil.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace openfg::scaler {
namespace {

constexpr uint32_t kFeatureCount = 31;

struct Buffer {
    vk::UniqueBuffer buffer;
    vk::UniqueDeviceMemory memory;
    vk::DeviceSize size{};
};

struct Pass {
    vk::UniqueDescriptorSetLayout setLayout;
    vk::UniquePipelineLayout pipelineLayout;
    vk::UniquePipeline pipeline;
    vk::DescriptorSet set;
};

struct DimsPush {
    uint32_t width;
    uint32_t height;
};

struct BlurPush {
    uint32_t width;
    uint32_t height;
    uint32_t radius;
    uint32_t pad;
    float weights[15];
};

struct TilePush {
    uint32_t width;
    uint32_t height;
    uint32_t tile;
    uint32_t pad;
};

struct BicubicPush {
    uint32_t lowWidth;
    uint32_t lowHeight;
    uint32_t highWidth;
    uint32_t highHeight;
};

struct Gaussian {
    uint32_t radius{};
    float weights[15]{};
};

Gaussian gaussianWeights(float sigma)
{
    Gaussian result;
    result.radius = static_cast<uint32_t>(
        std::ceil(4.0F * sigma + 0.5F));
    const int radius = static_cast<int>(result.radius);
    std::array<float, 15> values{};
    float sum = 0.0F;
    for (int i = -radius; i <= radius; ++i) {
        const float value = std::exp(
            -static_cast<float>(i * i) / (2.0F * sigma * sigma));
        values[static_cast<size_t>(i + radius)] = value;
        sum += value;
    }
    for (int i = 0; i <= radius * 2; ++i)
        result.weights[i] = values[static_cast<size_t>(i)] / sum;
    return result;
}

vk::DescriptorSetLayoutBinding storageBinding(uint32_t binding)
{
    return {
        .binding = binding,
        .descriptorType = vk::DescriptorType::eStorageBuffer,
        .descriptorCount = 1,
        .stageFlags = vk::ShaderStageFlagBits::eCompute
    };
}

Buffer createBuffer(
    Runtime& runtime,
    vk::DeviceSize size,
    bool hostVisible,
    const void* data = nullptr
) {
    const vk::BufferCreateInfo info{
        .size = size,
        .usage = vk::BufferUsageFlagBits::eStorageBuffer,
        .sharingMode = vk::SharingMode::eExclusive
    };
    Buffer result;
    result.buffer = runtime.device->createBufferUnique(
        info, nullptr, runtime.dld);
    const auto requirements =
        runtime.device->getBufferMemoryRequirements(
            *result.buffer, runtime.dld);
    result.memory = vkutil::allocateMemory(
        runtime.dld, *runtime.device, runtime.physical,
        requirements.size, requirements.memoryTypeBits, hostVisible);
    runtime.device->bindBufferMemory(
        *result.buffer, *result.memory, 0, runtime.dld);
    result.size = size;
    if (data) {
        void* mapped{};
        if (runtime.device->mapMemory(
                *result.memory, 0, size, {}, &mapped,
                runtime.dld) != vk::Result::eSuccess)
            throw std::runtime_error("cannot map gate buffer");
        std::memcpy(mapped, data, static_cast<size_t>(size));
        runtime.device->unmapMemory(*result.memory, runtime.dld);
    }
    return result;
}

Pass makePass(
    Runtime& runtime,
    const char* shader,
    std::vector<vk::DescriptorSetLayoutBinding> bindings,
    uint32_t pushSize,
    vk::DescriptorPool pool
) {
    auto [setLayout, pipelineLayout] = vkutil::createLayout(
        runtime.dld, *runtime.device, bindings, pushSize);
    const auto code = openfg::embeddedShader(shader);
    if (code.empty())
        throw std::runtime_error(
            "missing embedded shader " + std::string(shader));
    auto module = vkutil::createShaderModule(
        runtime.dld, *runtime.device, code);
    const vk::ComputePipelineCreateInfo info{
        .stage = {
            .stage = vk::ShaderStageFlagBits::eCompute,
            .module = *module,
            .pName = "main"
        },
        .layout = *pipelineLayout
    };
    auto result = runtime.device->createComputePipelineUnique(
        nullptr, info, nullptr, runtime.dld);
    if (result.result != vk::Result::eSuccess)
        throw std::runtime_error(
            "pipeline creation failed for " + std::string(shader));
    const vk::DescriptorSet set = runtime.device->allocateDescriptorSets({
        .descriptorPool = pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &*setLayout
    }, runtime.dld).front();
    return {
        std::move(setLayout),
        std::move(pipelineLayout),
        std::move(result.value),
        set
    };
}

void writeSet(
    Runtime& runtime,
    vk::DescriptorSet set,
    const std::vector<vk::Buffer>& buffers
) {
    std::vector<vk::DescriptorBufferInfo> infos;
    infos.reserve(buffers.size());
    for (const auto buffer : buffers) {
        infos.push_back({
            .buffer = buffer,
            .offset = 0,
            .range = VK_WHOLE_SIZE
        });
    }
    std::vector<vk::WriteDescriptorSet> writes;
    writes.reserve(buffers.size());
    for (size_t i = 0; i < buffers.size(); ++i) {
        writes.push_back({
            .dstSet = set,
            .dstBinding = static_cast<uint32_t>(i),
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .pBufferInfo = &infos[i]
        });
    }
    runtime.device->updateDescriptorSets(
        static_cast<uint32_t>(writes.size()), writes.data(),
        0, nullptr, runtime.dld);
}

void bufferBarrier(vk::CommandBuffer command, Runtime& runtime)
{
    const vk::MemoryBarrier barrier{
        .srcAccessMask = vk::AccessFlagBits::eShaderWrite,
        .dstAccessMask = vk::AccessFlagBits::eShaderRead |
            vk::AccessFlagBits::eShaderWrite
    };
    command.pipelineBarrier(
        vk::PipelineStageFlagBits::eComputeShader,
        vk::PipelineStageFlagBits::eComputeShader,
        {}, { barrier }, {}, {}, runtime.dld);
}

void hostBarrier(vk::CommandBuffer command, Runtime& runtime)
{
    const vk::MemoryBarrier barrier{
        .srcAccessMask = vk::AccessFlagBits::eShaderWrite,
        .dstAccessMask = vk::AccessFlagBits::eHostRead
    };
    command.pipelineBarrier(
        vk::PipelineStageFlagBits::eComputeShader,
        vk::PipelineStageFlagBits::eHost,
        {}, { barrier }, {}, {}, runtime.dld);
}

} // namespace

bool extractGateFeatures(
    Runtime& runtime,
    const uint8_t* lowRgba,
    uint32_t lowWidth,
    uint32_t lowHeight,
    const uint8_t* easuRgba,
    uint32_t highWidth,
    uint32_t highHeight,
    uint32_t tile,
    std::vector<float>& features,
    std::vector<GateTile>& tiles,
    std::string& error,
    double* gpuMilliseconds
) {
    try {
        if (!lowRgba || !easuRgba || lowWidth == 0 || lowHeight == 0 ||
            tile == 0 || highWidth != lowWidth * 2 ||
            highHeight != lowHeight * 2)
            throw std::runtime_error(
                "gate features need non-zero 2x low/EASU images");

        const uint32_t tilesX = (lowWidth + tile - 1) / tile;
        const uint32_t tilesY = (lowHeight + tile - 1) / tile;
        const uint32_t tileCount = tilesX * tilesY;
        const size_t lowPixels =
            static_cast<size_t>(lowWidth) * lowHeight;
        const size_t highPixels =
            static_cast<size_t>(highWidth) * highHeight;
        const vk::DeviceSize lowBytes =
            static_cast<vk::DeviceSize>(lowPixels * sizeof(float));
        const vk::DeviceSize highBytes =
            static_cast<vk::DeviceSize>(highPixels * sizeof(float));

        tiles.clear();
        tiles.reserve(tileCount);
        for (uint32_t ty = 0; ty < tilesY; ++ty) {
            for (uint32_t tx = 0; tx < tilesX; ++tx) {
                tiles.push_back({
                    tx * tile,
                    ty * tile,
                    std::min(tile, lowWidth - tx * tile),
                    std::min(tile, lowHeight - ty * tile)
                });
            }
        }
        features.assign(
            static_cast<size_t>(tileCount) * kFeatureCount, 0.0F);

        const vk::DescriptorPoolSize poolSize{
            .type = vk::DescriptorType::eStorageBuffer,
            .descriptorCount = 128
        };
        auto descriptorPool =
            runtime.device->createDescriptorPoolUnique({
                .maxSets = 20,
                .poolSizeCount = 1,
                .pPoolSizes = &poolSize
            }, nullptr, runtime.dld);

        Buffer lowInput = createBuffer(
            runtime, lowPixels * 4, true, lowRgba);
        Buffer easuInput = createBuffer(
            runtime, highPixels * 4, true, easuRgba);
        Buffer lowLuma = createBuffer(runtime, lowBytes, false);
        Buffer lowBlurHBuffer = createBuffer(runtime, lowBytes, false);
        Buffer lowHf = createBuffer(runtime, lowBytes, false);
        Buffer lowBoxLuma = createBuffer(runtime, lowBytes, false);
        Buffer lowBoxSquare = createBuffer(runtime, lowBytes, false);
        Buffer lowLocalVariance = createBuffer(runtime, lowBytes, false);
        Buffer lowGx = createBuffer(runtime, lowBytes, false);
        Buffer lowGy = createBuffer(runtime, lowBytes, false);
        Buffer lowGrad = createBuffer(runtime, lowBytes, false);
        Buffer lowLap = createBuffer(runtime, lowBytes, false);
        Buffer lowColor = createBuffer(runtime, lowBytes, false);
        Buffer lowSat = createBuffer(runtime, lowBytes, false);
        Buffer easuLuma = createBuffer(runtime, highBytes, false);
        Buffer easuBlurH = createBuffer(runtime, highBytes, false);
        Buffer easuHf = createBuffer(runtime, highBytes, false);
        Buffer easuGrad = createBuffer(runtime, highBytes, false);
        Buffer easuLap = createBuffer(runtime, highBytes, false);
        Buffer diff = createBuffer(runtime, highBytes, false);
        Buffer diffGrad = createBuffer(runtime, highBytes, false);
        Buffer featureBuffer = createBuffer(
            runtime, static_cast<vk::DeviceSize>(
                tileCount * kFeatureCount * sizeof(float)), true);

        const auto h = [](const Buffer& buffer) { return *buffer.buffer; };

        Pass lowLumaPass = makePass(
            runtime, "gate_luma",
            { storageBinding(0), storageBinding(1) },
            sizeof(DimsPush), *descriptorPool);
        writeSet(runtime, lowLumaPass.set,
            { h(lowInput), h(lowLuma) });

        Pass highLumaPass = makePass(
            runtime, "gate_luma",
            { storageBinding(0), storageBinding(1) },
            sizeof(DimsPush), *descriptorPool);
        writeSet(runtime, highLumaPass.set,
            { h(easuInput), h(easuLuma) });

        Pass lowBlurH = makePass(
            runtime, "gate_blur_h",
            { storageBinding(0), storageBinding(1) },
            sizeof(BlurPush), *descriptorPool);
        writeSet(runtime, lowBlurH.set, { h(lowLuma), h(lowBlurHBuffer) });

        Pass lowBlurV = makePass(
            runtime, "gate_blur_v",
            { storageBinding(0), storageBinding(1), storageBinding(2) },
            sizeof(BlurPush), *descriptorPool);
        writeSet(runtime, lowBlurV.set,
            { h(lowBlurHBuffer), h(lowLuma), h(lowHf) });

        Pass highBlurH = makePass(
            runtime, "gate_blur_h",
            { storageBinding(0), storageBinding(1) },
            sizeof(BlurPush), *descriptorPool);
        writeSet(runtime, highBlurH.set, { h(easuLuma), h(easuBlurH) });

        Pass highBlurV = makePass(
            runtime, "gate_blur_v",
            { storageBinding(0), storageBinding(1), storageBinding(2) },
            sizeof(BlurPush), *descriptorPool);
        writeSet(runtime, highBlurV.set,
            { h(easuBlurH), h(easuLuma), h(easuHf) });

        Pass lowBoxH = makePass(
            runtime, "gate_box_h",
            { storageBinding(0), storageBinding(1), storageBinding(2) },
            sizeof(DimsPush), *descriptorPool);
        writeSet(runtime, lowBoxH.set,
            { h(lowLuma), h(lowBoxLuma), h(lowBoxSquare) });

        Pass lowBoxV = makePass(
            runtime, "gate_box_v",
            { storageBinding(0), storageBinding(1), storageBinding(2) },
            sizeof(DimsPush), *descriptorPool);
        writeSet(runtime, lowBoxV.set,
            { h(lowBoxLuma), h(lowBoxSquare), h(lowLocalVariance) });

        Pass lowMaps = makePass(
            runtime, "gate_low_maps",
            {
                storageBinding(0), storageBinding(1), storageBinding(2),
                storageBinding(3), storageBinding(4), storageBinding(5),
                storageBinding(6), storageBinding(7)
            },
            sizeof(DimsPush), *descriptorPool);
        writeSet(runtime, lowMaps.set,
            {
                h(lowInput), h(lowLuma), h(lowGx), h(lowGy),
                h(lowGrad), h(lowLap), h(lowColor), h(lowSat)
            });

        Pass easuMaps = makePass(
            runtime, "gate_easu_maps",
            { storageBinding(0), storageBinding(1), storageBinding(2) },
            sizeof(DimsPush), *descriptorPool);
        writeSet(runtime, easuMaps.set,
            { h(easuLuma), h(easuGrad), h(easuLap) });

        Pass bicubic = makePass(
            runtime, "gate_bicubic_diff",
            { storageBinding(0), storageBinding(1), storageBinding(2) },
            sizeof(BicubicPush), *descriptorPool);
        writeSet(runtime, bicubic.set,
            { h(lowLuma), h(easuLuma), h(diff) });

        Pass diffGradPass = makePass(
            runtime, "gate_diff_grad",
            { storageBinding(0), storageBinding(1) },
            sizeof(DimsPush), *descriptorPool);
        writeSet(runtime, diffGradPass.set, { h(diff), h(diffGrad) });

        Pass reduceStats = makePass(
            runtime, "gate_reduce_stats",
            {
                storageBinding(0), storageBinding(1), storageBinding(2),
                storageBinding(3), storageBinding(4), storageBinding(5),
                storageBinding(6), storageBinding(7), storageBinding(8),
                storageBinding(9), storageBinding(10), storageBinding(11),
                storageBinding(12), storageBinding(13), storageBinding(14),
                storageBinding(15)
            },
            sizeof(TilePush), *descriptorPool);
        writeSet(runtime, reduceStats.set,
            {
                h(lowLuma), h(lowGrad), h(lowGx), h(lowGy),
                h(lowLap), h(lowHf), h(lowLocalVariance),
                h(lowColor), h(lowSat), h(easuLuma), h(easuGrad),
                h(easuLap), h(easuHf), h(diff), h(diffGrad),
                h(featureBuffer)
            });

        Pass reducePct = makePass(
            runtime, "gate_reduce_pct",
            {
                storageBinding(0), storageBinding(1), storageBinding(2),
                storageBinding(3), storageBinding(4), storageBinding(5),
                storageBinding(6)
            },
            sizeof(TilePush), *descriptorPool);
        writeSet(runtime, reducePct.set,
            {
                h(lowLuma), h(lowGrad), h(lowHf), h(easuGrad),
                h(easuHf), h(diff), h(featureBuffer)
            });

        vk::UniqueQueryPool queryPool;
        if (gpuMilliseconds != nullptr) {
            const auto queueProperties =
                runtime.physical.getQueueFamilyProperties(runtime.dld);
            if (queueProperties[runtime.queueFamily].timestampValidBits != 0)
                queryPool = runtime.device->createQueryPoolUnique({
                    .queryType = vk::QueryType::eTimestamp,
                    .queryCount = 2
                }, nullptr, runtime.dld);
        }

        auto commandPool = vkutil::createCommandPool(
            runtime.dld, *runtime.device, runtime.queueFamily, true);
        auto command = vkutil::createCommandBuffer(
            runtime.dld, *runtime.device, *commandPool);
        auto fence = vkutil::createFence(
            runtime.dld, *runtime.device);

        command->begin(
            { .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit },
            runtime.dld);
        if (queryPool) {
            command->resetQueryPool(*queryPool, 0, 2, runtime.dld);
            command->writeTimestamp(
                vk::PipelineStageFlagBits::eTopOfPipe,
                *queryPool, 0, runtime.dld);
        }

        const auto dispatch = [&](
            Pass& pass,
            uint32_t groupsX,
            uint32_t groupsY,
            uint32_t groupsZ,
            const void* push,
            uint32_t pushSize
        ) {
            command->bindPipeline(
                vk::PipelineBindPoint::eCompute, *pass.pipeline,
                runtime.dld);
            command->bindDescriptorSets(
                vk::PipelineBindPoint::eCompute, *pass.pipelineLayout, 0,
                { pass.set }, {}, runtime.dld);
            command->pushConstants(
                *pass.pipelineLayout, vk::ShaderStageFlagBits::eCompute,
                0, pushSize, push, runtime.dld);
            command->dispatch(groupsX, groupsY, groupsZ, runtime.dld);
            bufferBarrier(*command, runtime);
        };

        const uint32_t lowX = (lowWidth + 7) / 8;
        const uint32_t lowY = (lowHeight + 7) / 8;
        const uint32_t highX = (highWidth + 7) / 8;
        const uint32_t highY = (highHeight + 7) / 8;

        const DimsPush lowDims{lowWidth, lowHeight};
        const DimsPush highDims{highWidth, highHeight};
        const Gaussian lowGaussian = gaussianWeights(1.0F);
        const Gaussian highGaussian = gaussianWeights(1.5F);

        BlurPush lowBlur{
            lowWidth, lowHeight, lowGaussian.radius, 0, {}
        };
        std::memcpy(lowBlur.weights, lowGaussian.weights,
            sizeof(lowBlur.weights));
        BlurPush highBlur{
            highWidth, highHeight, highGaussian.radius, 0, {}
        };
        std::memcpy(highBlur.weights, highGaussian.weights,
            sizeof(highBlur.weights));

        const TilePush tilePush{lowWidth, lowHeight, tile, 0};
        const BicubicPush bicubicPush{
            lowWidth, lowHeight, highWidth, highHeight
        };

        dispatch(lowLumaPass, lowX, lowY, 1,
            &lowDims, sizeof(lowDims));
        dispatch(highLumaPass, highX, highY, 1,
            &highDims, sizeof(highDims));
        dispatch(lowBlurH, lowX, lowY, 1,
            &lowBlur, sizeof(lowBlur));
        dispatch(lowBlurV, lowX, lowY, 1,
            &lowBlur, sizeof(lowBlur));
        dispatch(highBlurH, highX, highY, 1,
            &highBlur, sizeof(highBlur));
        dispatch(highBlurV, highX, highY, 1,
            &highBlur, sizeof(highBlur));
        dispatch(lowBoxH, lowX, lowY, 1,
            &lowDims, sizeof(lowDims));
        dispatch(lowBoxV, lowX, lowY, 1,
            &lowDims, sizeof(lowDims));
        dispatch(lowMaps, lowX, lowY, 1,
            &lowDims, sizeof(lowDims));
        dispatch(easuMaps, highX, highY, 1,
            &highDims, sizeof(highDims));
        dispatch(bicubic, highX, highY, 1,
            &bicubicPush, sizeof(bicubicPush));
        dispatch(diffGradPass, highX, highY, 1,
            &highDims, sizeof(highDims));
        dispatch(reduceStats, tileCount, 1, 1,
            &tilePush, sizeof(tilePush));
        dispatch(reducePct, tileCount, 1, 1,
            &tilePush, sizeof(tilePush));

        if (queryPool)
            command->writeTimestamp(
                vk::PipelineStageFlagBits::eBottomOfPipe,
                *queryPool, 1, runtime.dld);
        hostBarrier(*command, runtime);
        command->end(runtime.dld);

        runtime.device->resetFences(*fence, runtime.dld);
        runtime.queue.submit({{
            .commandBufferCount = 1,
            .pCommandBuffers = &*command
        }}, *fence, runtime.dld);
        if (runtime.device->waitForFences(
                *fence, true, UINT64_MAX, runtime.dld) !=
            vk::Result::eSuccess)
            throw std::runtime_error("gate feature fence wait failed");

        void* mapped{};
        if (runtime.device->mapMemory(
                *featureBuffer.memory, 0, featureBuffer.size, {},
                &mapped, runtime.dld) != vk::Result::eSuccess)
            throw std::runtime_error("cannot map gate features");
        std::memcpy(features.data(), mapped, features.size() * sizeof(float));
        runtime.device->unmapMemory(*featureBuffer.memory, runtime.dld);

        if (gpuMilliseconds != nullptr) {
            *gpuMilliseconds = 0.0;
            if (queryPool) {
                std::array<uint64_t, 2> timestamps{};
                if (runtime.device->getQueryPoolResults(
                        *queryPool, 0, 2, sizeof(timestamps),
                        timestamps.data(), sizeof(uint64_t),
                        vk::QueryResultFlagBits::e64,
                        runtime.dld) == vk::Result::eSuccess)
                    *gpuMilliseconds =
                        static_cast<double>(timestamps[1] - timestamps[0]) *
                        runtime.timestampPeriod / 1.0e6;
            }
        }
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

} // namespace openfg::scaler
