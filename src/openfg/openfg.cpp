/* SPDX-License-Identifier: MIT */

#include "openfg.hpp"

#include "vkutil/vkutil.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <vk>

using namespace openfg;

namespace {
    constexpr std::array<std::string_view, 9> PIPELINE_NAMES{
        "luma",
        "downsample",
        "blockmatch",
        "refine",
        "median",
        "smooth",
        "tilemask",
        "refine_full",
        "interpolate"
    };

    enum PipelineIndex : size_t {
        LUMA = 0,
        DOWNSAMPLE = 1,
        BLOCKMATCH = 2,
        REFINE = 3,
        MEDIAN = 4,
        SMOOTH = 5,
        TILEMASK = 6,
        REFINE_FULL = 7,
        INTERPOLATE = 8
    };

    constexpr uint32_t LOCAL_SIZE = 8;

    vk::Extent2D halve(vk::Extent2D extent) {
        return {
            std::max(1U, (extent.width + 1) / 2),
            std::max(1U, (extent.height + 1) / 2)
        };
    }
}

Interpolator::Interpolator(
    const vk::detail::DispatchLoaderDynamic& dld,
    const vk::PhysicalDevice& physdev,
    const vk::Device& device,
    vk::Extent2D extent,
    vk::Image source,
    vk::Image destination,
    const ShaderLoader& loader,
    bool performanceMode
) : m_dld(dld), m_physdev(physdev), m_device(device),
    m_extent(extent), m_loader(loader) {
    if (extent.width == 0 || extent.height == 0)
        throw std::runtime_error("openfg: invalid extent");

    this->m_levels.at(0) = extent;
    for (size_t i = 1; i < this->m_levels.size(); i++)
        this->m_levels.at(i) = halve(this->m_levels.at(i - 1));

    this->m_bidirectional = std::getenv("OPENFG_BIDIRECTIONAL") != nullptr;
    this->m_occlusionFlow = std::getenv("OPENFG_OCCLUSION") != nullptr;
    this->m_tileMaskEnabled = std::getenv("OPENFG_NO_TILE_MASK") == nullptr;
    this->m_performanceMode = performanceMode;

    constexpr vk::ImageUsageFlags INTERNAL_USAGE{
        vk::ImageUsageFlagBits::eStorage |
        vk::ImageUsageFlagBits::eSampled |
        vk::ImageUsageFlagBits::eTransferSrc |
        vk::ImageUsageFlagBits::eTransferDst
    };

    for (size_t i = 0; i < this->m_levels.size(); i++) {
        this->m_lumaA.at(i) = this->createImage(
            this->m_levels.at(i), vk::Format::eR8Unorm, INTERNAL_USAGE);
        this->m_lumaB.at(i) = this->createImage(
            this->m_levels.at(i), vk::Format::eR8Unorm, INTERNAL_USAGE);
        this->m_flow.at(i) = this->createImage(
            this->m_levels.at(i), vk::Format::eR16G16Sfloat, INTERNAL_USAGE);
        if (this->m_bidirectional || this->m_occlusionFlow) {
            this->m_flowBA.at(i) = this->createImage(
                this->m_levels.at(i), vk::Format::eR16G16Sfloat, INTERNAL_USAGE);
        }
    }
    for (size_t i = 0; i < this->m_levels.size(); i++) {
        this->m_flowFiltered.at(i) = this->createImage(
            this->m_levels.at(i), vk::Format::eR16G16Sfloat, INTERNAL_USAGE);
        this->m_flowSmooth.at(i) = this->createImage(
            this->m_levels.at(i), vk::Format::eR16G16Sfloat, INTERNAL_USAGE);
        if (this->m_bidirectional || this->m_occlusionFlow) {
            this->m_flowBAFiltered.at(i) = this->createImage(
                this->m_levels.at(i), vk::Format::eR16G16Sfloat, INTERNAL_USAGE);
            this->m_flowBASmooth.at(i) = this->createImage(
                this->m_levels.at(i), vk::Format::eR16G16Sfloat, INTERNAL_USAGE);
        }
    }

    this->m_flowFull = this->createImage(
        extent, vk::Format::eR16G16Sfloat, INTERNAL_USAGE);
    this->m_tileMask = this->createImage(
        { (extent.width + 15) / 16, (extent.height + 15) / 16 },
        vk::Format::eR8Unorm, INTERNAL_USAGE);

    // Dummy 1x1 images keep every descriptor binding valid for passes that do
    // not use all of them.
    this->m_dummyR8 = this->createImage(
        { 1, 1 }, vk::Format::eR8Unorm, vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled);
    this->m_dummyRg16f = this->createImage(
        { 1, 1 }, vk::Format::eR16G16Sfloat, vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled);
    this->m_dummyRgba8 = this->createImage(
        { 1, 1 }, vk::Format::eR8G8B8A8Unorm, vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled);

    this->m_srcView = vkutil::createImageView(
        this->m_dld, this->m_device, source, vk::Format::eR8G8B8A8Unorm, 2);
    this->m_dstView = vkutil::createImageView(
        this->m_dld, this->m_device, destination, vk::Format::eR8G8B8A8Unorm,
        kMaxInterpolatedFrames);

    this->m_sampler = vkutil::createSampler(
        this->m_dld, this->m_device,
        vk::SamplerAddressMode::eClampToEdge,
        vk::CompareOp::eNever,
        false
    );

    this->createPipelines();
    this->createDescriptors();
}

Interpolator::Image Interpolator::createImage(
    vk::Extent2D extent,
    vk::Format format,
    vk::ImageUsageFlags usage
) {
    const vk::ImageCreateInfo imageInfo{
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = { extent.width, extent.height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = usage,
        .sharingMode = vk::SharingMode::eExclusive,
        .initialLayout = vk::ImageLayout::eGeneral
    };

    Image image{};
    image.extent = extent;
    image.format = format;
    image.image = this->m_device.createImageUnique(imageInfo, nullptr, this->m_dld);

    const auto requirements{this->m_device.getImageMemoryRequirements(*image.image, this->m_dld)};
    image.memory = vkutil::allocateMemory(
        this->m_dld,
        this->m_device,
        this->m_physdev,
        requirements.size,
        requirements.memoryTypeBits,
        false
    );
    this->m_device.bindImageMemory(*image.image, *image.memory, 0, this->m_dld);
    image.view = vkutil::createImageView(
        this->m_dld, this->m_device, *image.image, format, 1);

    return image;
}

void Interpolator::createPipelines() {
    std::vector<vk::DescriptorSetLayoutBinding> bindings;
    bindings.push_back({
        .binding = 0,
        .descriptorType = vk::DescriptorType::eSampler,
        .descriptorCount = 1,
        .stageFlags = vk::ShaderStageFlagBits::eCompute
    });
    for (uint32_t b = 1; b <= 5; b++) {
        bindings.push_back({
            .binding = b,
            .descriptorType = vk::DescriptorType::eSampledImage,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute
        });
    }
    for (uint32_t b = 8; b <= 11; b++) {
        bindings.push_back({
            .binding = b,
            .descriptorType = vk::DescriptorType::eStorageImage,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute
        });
    }

    auto [setLayout, pipelineLayout] = vkutil::createLayout(
        this->m_dld, this->m_device, bindings, sizeof(PushConstants));
    this->m_setLayout = std::move(setLayout);
    this->m_pipelineLayout = std::move(pipelineLayout);

    for (size_t i = 0; i < PIPELINE_NAMES.size(); i++) {
        const auto code{this->m_loader(PIPELINE_NAMES.at(i))};
        if (code.empty())
            throw std::runtime_error(
                "openfg: missing shader '" + std::string(PIPELINE_NAMES.at(i)) + "'");

        this->m_modules.push_back(vkutil::createShaderModule(
            this->m_dld, this->m_device, std::span<const uint32_t>(code)));

        const vk::ComputePipelineCreateInfo pipelineInfo{
            .stage = {
                .stage = vk::ShaderStageFlagBits::eCompute,
                .module = *this->m_modules.back(),
                .pName = "main"
            },
            .layout = *this->m_pipelineLayout
        };
        auto result{this->m_device.createComputePipelineUnique(
            nullptr, pipelineInfo, nullptr, this->m_dld)};
        if (result.result != vk::Result::eSuccess)
            throw std::runtime_error(
                "openfg: failed to create pipeline '" + std::string(PIPELINE_NAMES.at(i)) + "'");
        this->m_pipelines.at(i) = std::move(result.value);
    }
}

vk::DescriptorSet Interpolator::makeSet(
    const std::array<vk::ImageView, 5>& sampled,
    const std::array<vk::ImageView, 4>& storage
) {
    const vk::DescriptorSet set{this->m_device.allocateDescriptorSets({
        .descriptorPool = *this->m_pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &*this->m_setLayout
    }, this->m_dld).at(0)};

    const vk::DescriptorImageInfo samplerInfo{
        .sampler = *this->m_sampler
    };

    std::array<vk::DescriptorImageInfo, 5> sampledInfos{};
    for (size_t i = 0; i < sampled.size(); i++) {
        sampledInfos.at(i) = {
            .imageView = sampled.at(i),
            .imageLayout = vk::ImageLayout::eGeneral
        };
    }
    // Every pass gets the tile mask at binding 4; shaders that do not use it
    // ignore the binding.
    sampledInfos.at(3) = {
        .imageView = *this->m_tileMask.view,
        .imageLayout = vk::ImageLayout::eGeneral
    };

    std::array<vk::DescriptorImageInfo, 4> storageInfos{};
    for (size_t i = 0; i < storage.size(); i++) {
        storageInfos.at(i) = {
            .imageView = storage.at(i),
            .imageLayout = vk::ImageLayout::eGeneral
        };
    }

    std::array<vk::WriteDescriptorSet, 10> writes{};
    writes.at(0) = {
        .dstSet = set,
        .dstBinding = 0,
        .descriptorCount = 1,
        .descriptorType = vk::DescriptorType::eSampler,
        .pImageInfo = &samplerInfo
    };
    for (uint32_t i = 0; i < 5; i++) {
        writes.at(1 + i) = {
            .dstSet = set,
            .dstBinding = 1 + i,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eSampledImage,
            .pImageInfo = &sampledInfos.at(i)
        };
    }
    for (uint32_t i = 0; i < 4; i++) {
        writes.at(6 + i) = {
            .dstSet = set,
            .dstBinding = 8 + i,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eStorageImage,
            .pImageInfo = &storageInfos.at(i)
        };
    }

    this->m_device.updateDescriptorSets(writes, {}, this->m_dld);
    return set;
}

void Interpolator::createDescriptors() {
    const uint32_t maxSets{64};
    const std::array<vk::DescriptorPoolSize, 3> poolSizes{{
        { .type = vk::DescriptorType::eSampler, .descriptorCount = maxSets },
        { .type = vk::DescriptorType::eSampledImage, .descriptorCount = maxSets * 5 },
        { .type = vk::DescriptorType::eStorageImage, .descriptorCount = maxSets * 4 }
    }};
    this->m_pool = this->m_device.createDescriptorPoolUnique({
        .flags = vk::DescriptorPoolCreateFlagBits::eUpdateAfterBind,
        .maxSets = maxSets,
        .poolSizeCount = static_cast<uint32_t>(poolSizes.size()),
        .pPoolSizes = poolSizes.data()
    }, nullptr, this->m_dld);

    std::array<vk::ImageView, 5> sampled{{
        *this->m_dummyR8.view,
        *this->m_dummyR8.view,
        *this->m_dummyR8.view,
        *this->m_dummyR8.view,
        *this->m_srcView
    }};
    std::array<vk::ImageView, 4> storage{{
        *this->m_dummyR8.view,
        *this->m_dummyR8.view,
        *this->m_dummyRg16f.view,
        *this->m_dummyRgba8.view
    }};

    // luma A/B
    storage.at(0) = *this->m_lumaA.at(0).view;
    this->m_setLumaA = this->makeSet(sampled, storage);
    storage.at(0) = *this->m_lumaB.at(0).view;
    this->m_setLumaB = this->makeSet(sampled, storage);

    // pyramid downsample
    for (uint32_t level = 1; level < 6; level++) {
        std::array<vk::ImageView, 4> downStorage{{
            *this->m_lumaA.at(level - 1).view,
            *this->m_lumaA.at(level).view,
            *this->m_dummyRg16f.view,
            *this->m_dummyRgba8.view
        }};
        this->m_setDownA.at(level) = this->makeSet(sampled, downStorage);

        downStorage.at(0) = *this->m_lumaB.at(level - 1).view;
        downStorage.at(1) = *this->m_lumaB.at(level).view;
        this->m_setDownB.at(level) = this->makeSet(sampled, downStorage);
    }

    // block matching at 1/16 resolution
    storage.at(0) = *this->m_lumaA.at(4).view;
    storage.at(1) = *this->m_lumaB.at(4).view;
    storage.at(2) = *this->m_flow.at(4).view;
    this->m_setBlock = this->makeSet(sampled, storage);

    // per-level refinement: flow[level+1] and luma B sampled, flow[level] out
    for (uint32_t level = 1; level <= 4; level++) {
        std::array<vk::ImageView, 5> refineSampled{{
            *this->m_dummyR8.view,
            *this->m_dummyR8.view,
            *this->m_dummyR8.view,
            *this->m_dummyR8.view,
            *this->m_srcView
        }};
        refineSampled.at(0) = *this->m_flow.at(level + 1).view;
        refineSampled.at(1) = *this->m_lumaB.at(level).view;

        std::array<vk::ImageView, 4> refineStorage{{
            *this->m_lumaA.at(level).view,
            *this->m_flow.at(level + 1).view,
            *this->m_flow.at(level).view,
            *this->m_dummyRgba8.view
        }};
        this->m_setRefine.at(level) = this->makeSet(refineSampled, refineStorage);
    }

    // per-level flow filter: raw -> median -> guided smooth
    for (uint32_t level = 1; level < 6; level++) {
        storage.at(0) = *this->m_flow.at(level).view;
        storage.at(1) = *this->m_flowFiltered.at(level).view;
        this->m_setMedian.at(level) = this->makeSet(sampled, storage);

        std::array<vk::ImageView, 5> smoothSampled{{
            *this->m_dummyR8.view,
            *this->m_lumaB.at(level).view,
            *this->m_dummyR8.view,
            *this->m_dummyR8.view,
            *this->m_srcView
        }};
        std::array<vk::ImageView, 4> smoothStorage{{
            *this->m_flowFiltered.at(level).view,
            *this->m_flowSmooth.at(level).view,
            *this->m_dummyRg16f.view,
            *this->m_dummyRgba8.view
        }};
        this->m_setSmooth.at(level) = this->makeSet(smoothSampled, smoothStorage);
    }

    // ---- reverse direction (B -> A), optional ----
    if (!this->m_bidirectional && !this->m_occlusionFlow) {
        // Still create the interp set below; reverse flow falls back to a dummy.
    } else {
    storage.at(0) = *this->m_lumaB.at(4).view;
    storage.at(1) = *this->m_lumaA.at(4).view;
    storage.at(2) = *this->m_flowBA.at(4).view;
    this->m_setBlockBA = this->makeSet(sampled, storage);

    for (uint32_t level = 1; level <= 4; level++) {
        std::array<vk::ImageView, 5> refineSampled{{
            *this->m_dummyR8.view,
            *this->m_dummyR8.view,
            *this->m_dummyR8.view,
            *this->m_dummyR8.view,
            *this->m_srcView
        }};
        refineSampled.at(0) = *this->m_flowBA.at(level + 1).view;
        refineSampled.at(1) = *this->m_lumaA.at(level).view;

        std::array<vk::ImageView, 4> refineStorage{{
            *this->m_lumaB.at(level).view,
            *this->m_flowBA.at(level + 1).view,
            *this->m_flowBA.at(level).view,
            *this->m_dummyRgba8.view
        }};
        this->m_setRefineBA.at(level) = this->makeSet(refineSampled, refineStorage);
    }

    for (uint32_t level = 1; level < 6; level++) {
        storage.at(0) = *this->m_flowBA.at(level).view;
        storage.at(1) = *this->m_flowBAFiltered.at(level).view;
        this->m_setMedianBA.at(level) = this->makeSet(sampled, storage);

        std::array<vk::ImageView, 5> smoothSampled{{
            *this->m_dummyR8.view,
            *this->m_lumaA.at(level).view,
            *this->m_dummyR8.view,
            *this->m_dummyR8.view,
            *this->m_srcView
        }};
        std::array<vk::ImageView, 4> smoothStorage{{
            *this->m_flowBAFiltered.at(level).view,
            *this->m_flowBASmooth.at(level).view,
            *this->m_dummyRg16f.view,
            *this->m_dummyRgba8.view
        }};
        this->m_setSmoothBA.at(level) = this->makeSet(smoothSampled, smoothStorage);
    }
    }

    // full-resolution refinement of the half-resolution field
    {
        std::array<vk::ImageView, 5> fullSampled{{
            *this->m_dummyR8.view,
            *this->m_lumaB.at(0).view,
            *this->m_dummyR8.view,
            *this->m_dummyR8.view,
            *this->m_srcView
        }};
        std::array<vk::ImageView, 4> fullStorage{{
            *this->m_lumaA.at(0).view,
            *this->m_flow.at(1).view,
            *this->m_flowFull.view,
            *this->m_dummyRgba8.view
        }};
        this->m_setRefineFull = this->makeSet(fullSampled, fullStorage);
    }

    // tile activity mask from the level-2 luma
    {
        std::array<vk::ImageView, 5> maskSampled{{
            *this->m_lumaA.at(2).view,
            *this->m_lumaB.at(2).view,
            *this->m_dummyR8.view,
            *this->m_dummyR8.view,
            *this->m_srcView
        }};
        std::array<vk::ImageView, 4> maskStorage{{
            *this->m_tileMask.view,
            *this->m_dummyR8.view,
            *this->m_dummyRg16f.view,
            *this->m_dummyRgba8.view
        }};
        this->m_setTileMask = this->makeSet(maskSampled, maskStorage);
    }

    // interpolation
    std::array<vk::ImageView, 5> interpSampled{{
        this->m_bidirectional
            ? *this->m_flowBASmooth.at(1).view
            : (this->m_occlusionFlow
                ? *this->m_flowBASmooth.at(2).view
                : *this->m_dummyRg16f.view),
        *this->m_dummyR8.view,
        this->m_performanceMode
            ? *this->m_flowSmooth.at(1).view
            : *this->m_flowFull.view,
        *this->m_dummyR8.view, // replaced by the tile mask in makeSet
        *this->m_srcView
    }};
    std::array<vk::ImageView, 4> interpStorage{{
        *this->m_dummyR8.view,
        *this->m_dummyR8.view,
        *this->m_dummyRg16f.view,
        *this->m_dstView
    }};
    this->m_setInterp = this->makeSet(interpSampled, interpStorage);
}

void Interpolator::dispatch(
    const vk::CommandBuffer& cmdbuf,
    std::string_view pipeline,
    vk::DescriptorSet set,
    vk::Extent2D extent,
    const PushConstants& pc
) {
    size_t index{0};
    for (size_t i = 0; i < PIPELINE_NAMES.size(); i++) {
        if (PIPELINE_NAMES.at(i) != pipeline)
            continue;
        index = i;
        break;
    }

    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *this->m_pipelines.at(index), this->m_dld);
    cmdbuf.bindDescriptorSets(
        vk::PipelineBindPoint::eCompute,
        *this->m_pipelineLayout,
        0, { set }, {},
        this->m_dld
    );
    cmdbuf.pushConstants(
        *this->m_pipelineLayout,
        vk::ShaderStageFlagBits::eCompute,
        0, sizeof(pc), &pc,
        this->m_dld
    );
    cmdbuf.dispatch(
        (extent.width + LOCAL_SIZE - 1) / LOCAL_SIZE,
        (extent.height + LOCAL_SIZE - 1) / LOCAL_SIZE,
        1,
        this->m_dld
    );
}

void Interpolator::barrier(const vk::CommandBuffer& cmdbuf) const {
    const vk::MemoryBarrier memoryBarrier{
        .srcAccessMask = vk::AccessFlagBits::eShaderWrite,
        .dstAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite
    };
    cmdbuf.pipelineBarrier(
        vk::PipelineStageFlagBits::eComputeShader,
        vk::PipelineStageFlagBits::eComputeShader,
        {}, { memoryBarrier }, {}, {},
        this->m_dld
    );
}

void Interpolator::analyze(const vk::CommandBuffer& cmdbuf, uint32_t layerA, uint32_t layerB) {
    PushConstants pc{};
    pc.width = this->m_extent.width;
    pc.height = this->m_extent.height;
    pc.flowScale = 1.0F;
    pc.layerA = layerA;
    pc.layerB = layerB;

    this->dispatch(cmdbuf, "luma", this->m_setLumaA, this->m_extent, pc);
    this->barrier(cmdbuf);
    pc.layerA = layerB; // the luma shader selects one layer per dispatch
    this->dispatch(cmdbuf, "luma", this->m_setLumaB, this->m_extent, pc);
    this->barrier(cmdbuf);
    pc.layerA = layerA;

    for (uint32_t level = 1; level < 6; level++) {
        pc.width = this->m_levels.at(level).width;
        pc.height = this->m_levels.at(level).height;
        pc.level = static_cast<int32_t>(level);

        this->dispatch(cmdbuf, "downsample", this->m_setDownA.at(level), this->m_levels.at(level), pc);
        this->barrier(cmdbuf);
        this->dispatch(cmdbuf, "downsample", this->m_setDownB.at(level), this->m_levels.at(level), pc);
        this->barrier(cmdbuf);
    }

    // Tile activity mask (and its one-tile dilation).
    pc.width = this->m_tileMask.extent.width;
    pc.height = this->m_tileMask.extent.height;
    pc.level = 2;
    this->dispatch(cmdbuf, "tilemask", this->m_setTileMask, this->m_tileMask.extent, pc);
    this->barrier(cmdbuf);

    pc.width = this->m_levels.at(4).width;
    pc.height = this->m_levels.at(4).height;
    pc.level = 4;
    pc.radius = 8;
    if (this->m_tileMaskEnabled)
        pc.flags |= 256;

    this->dispatch(cmdbuf, "blockmatch", this->m_setBlock, this->m_levels.at(4), pc);
    this->barrier(cmdbuf);
    this->dispatch(cmdbuf, "median", this->m_setMedian.at(4), this->m_levels.at(4), pc);
    this->barrier(cmdbuf);
    this->dispatch(cmdbuf, "smooth", this->m_setSmooth.at(4), this->m_levels.at(4), pc);
    this->barrier(cmdbuf);

    for (int level = 3; level >= 1; level--) {
        pc.width = this->m_levels.at(level).width;
        pc.height = this->m_levels.at(level).height;
        pc.level = level;
        pc.radius = 0;

        this->dispatch(cmdbuf, "refine", this->m_setRefine.at(level),
            this->m_levels.at(level), pc);
        this->barrier(cmdbuf);
        this->dispatch(cmdbuf, "median", this->m_setMedian.at(level),
            this->m_levels.at(level), pc);
        this->barrier(cmdbuf);
        this->dispatch(cmdbuf, "smooth", this->m_setSmooth.at(level),
            this->m_levels.at(level), pc);
        this->barrier(cmdbuf);
    }

    if (!this->m_performanceMode) {
        pc.width = this->m_extent.width;
        pc.height = this->m_extent.height;
        pc.level = 0;
        this->dispatch(cmdbuf, "refine_full", this->m_setRefineFull, this->m_extent, pc);
        this->barrier(cmdbuf);
    }

    if (!this->m_bidirectional && !this->m_occlusionFlow)
        return;

    // Reverse flow B -> A. Full mode refines to half resolution; the default
    // occlusion mode stops at quarter resolution, which is enough to detect
    // disocclusions and costs a fraction of a full reverse pyramid.
    pc.width = this->m_levels.at(4).width;
    pc.height = this->m_levels.at(4).height;
    pc.level = 4;
    pc.radius = 8;
    if (this->m_tileMaskEnabled)
        pc.flags |= 256;
    this->dispatch(cmdbuf, "blockmatch", this->m_setBlockBA, this->m_levels.at(4), pc);
    this->barrier(cmdbuf);
    this->dispatch(cmdbuf, "median", this->m_setMedianBA.at(4), this->m_levels.at(4), pc);
    this->barrier(cmdbuf);
    this->dispatch(cmdbuf, "smooth", this->m_setSmoothBA.at(4), this->m_levels.at(4), pc);
    this->barrier(cmdbuf);

    const int coarsest = this->m_bidirectional ? 1 : 2;
    for (int level = 3; level >= coarsest; level--) {
        pc.width = this->m_levels.at(level).width;
        pc.height = this->m_levels.at(level).height;
        pc.level = level;
        pc.radius = 0;

        this->dispatch(cmdbuf, "refine", this->m_setRefineBA.at(level),
            this->m_levels.at(level), pc);
        this->barrier(cmdbuf);
        this->dispatch(cmdbuf, "median", this->m_setMedianBA.at(level),
            this->m_levels.at(level), pc);
        this->barrier(cmdbuf);
        this->dispatch(cmdbuf, "smooth", this->m_setSmoothBA.at(level),
            this->m_levels.at(level), pc);
        this->barrier(cmdbuf);
    }
}

void Interpolator::generate(
    const vk::CommandBuffer& cmdbuf,
    uint32_t layerA,
    uint32_t layerB,
    float t,
    uint32_t flags,
    uint32_t destinationLayer
) {
    PushConstants pc{};
    pc.width = this->m_extent.width;
    pc.height = this->m_extent.height;
    pc.t = t;
    pc.flowScale = this->m_performanceMode ? 2.0F : 1.0F;
    pc.level = 1;
    pc.flags = static_cast<int32_t>(flags);
    pc.layerA = layerA;
    pc.layerB = layerB;
    pc.dstLayer = destinationLayer;
    if (this->m_bidirectional)
        pc.flags |= 64;
    else if (this->m_occlusionFlow)
        pc.flags |= 128;
    if (this->m_tileMaskEnabled)
        pc.flags |= 256;

    this->dispatch(cmdbuf, "interpolate", this->m_setInterp, this->m_extent, pc);
    this->barrier(cmdbuf);
}

Interpolator::~Interpolator() = default;
