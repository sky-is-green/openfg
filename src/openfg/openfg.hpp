/* SPDX-License-Identifier: MIT */

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <vk>

#include "limits.hpp"

namespace openfg {

/// Image-based frame-generation method. Off is handled by the layer and never
/// reaches the backend.
enum class Generator {
    Flow,
    Blend,
    Hold,
    Ffx,
    Off
};

/// Loads a named shader as SPIR-V words. Implementations may read from disk
/// (fg-lab) or from an embedded table (the layer).
using ShaderLoader = std::function<std::vector<uint32_t>(std::string_view)>;

/// Push constants shared by every pass (48 bytes).
struct PushConstants {
    uint32_t width;
    uint32_t height;
    float t;
    float flowScale;
    int32_t level;
    int32_t radius;
    int32_t flags;
    int32_t pad0;
    uint32_t layerA;
    uint32_t layerB;
    uint32_t dstLayer;
    uint32_t pad2;
};
static_assert(sizeof(PushConstants) == 48);

///
/// GPU motion-estimation + interpolation pipeline.
///
/// The caller owns the Vulkan device and the external source/destination
/// images. Source is a two-layer RGBA8 array (previous/current); destination
/// is a three-layer RGBA8 array. Each generated frame is written to the layer
/// selected by `destinationLayer` (`kMaxInterpolatedFrames` layers cover 4x).
///
class Interpolator {
public:
    Interpolator(
        const vk::detail::DispatchLoaderDynamic& dld,
        const vk::PhysicalDevice& physdev,
        const vk::Device& device,
        vk::Extent2D extent,
        vk::Image source,
        vk::Image destination,
        const ShaderLoader& loader,
        bool performanceMode = false,
        bool videoSafe = false,
        vk::Format colorFormat = vk::Format::eR8G8B8A8Unorm,
        Generator generator = Generator::Flow
    );
    ~Interpolator();

    Interpolator(const Interpolator&) = delete;
    Interpolator& operator=(const Interpolator&) = delete;

    /// Build the luma pyramid and the flow field for one frame pair.
    void analyze(const vk::CommandBuffer& cmdbuf, uint32_t layerA, uint32_t layerB);

    /// Interpolate one output frame at time t into the destination image.
    void generate(const vk::CommandBuffer& cmdbuf, uint32_t layerA, uint32_t layerB, float t,
        uint32_t flags = 0, uint32_t destinationLayer = 0);

    /// Expose the internal flow image for debugging/inspection.
    [[nodiscard]] vk::Image flowImage(uint32_t level = 1) const {
        return *this->m_flowSmooth.at(level).image;
    }
    [[nodiscard]] vk::Image flowRawImage(uint32_t level = 1) const {
        return *this->m_flow.at(level).image;
    }
    [[nodiscard]] vk::Image flowReverseImage(uint32_t level = 1) const {
        return this->m_bidirectional
            ? *this->m_flowBASmooth.at(level).image
            : *this->m_dummyRg16f.image;
    }
    [[nodiscard]] vk::Extent2D flowExtent(uint32_t level = 1) const {
        return this->m_levels.at(level);
    }
    [[nodiscard]] vk::Image lumaImage(uint32_t level, bool current) const {
        return current ? *this->m_lumaB.at(level).image : *this->m_lumaA.at(level).image;
    }
    [[nodiscard]] vk::Extent2D levelExtent(uint32_t level) const {
        return this->m_levels.at(level);
    }
    [[nodiscard]] vk::Image ffxDenseImage() const {
        return *this->m_ffxDense.image;
    }
    [[nodiscard]] vk::Extent2D ffxDenseExtent() const {
        return this->m_ffxDense.extent;
    }
    [[nodiscard]] vk::Image ffxFlowOutImage() const {
        return *this->m_ffxFlowOut.image;
    }
    [[nodiscard]] vk::Extent2D ffxFlowOutExtent() const {
        return this->m_ffxFlowOut.extent;
    }
    [[nodiscard]] vk::Image ffxLumaImage(uint32_t level, bool current) const {
        return current
            ? *this->m_ffxLumaCurrent.at(level).image
            : *this->m_ffxLumaPrevious.at(level).image;
    }
    [[nodiscard]] vk::Extent2D ffxLumaExtent(uint32_t level) const {
        return this->m_ffxLumaExtent.at(level);
    }
    [[nodiscard]] vk::Image ffxFlowImage(uint32_t level, bool second) const {
        return second
            ? *this->m_ffxFlowB.at(level).image
            : *this->m_ffxFlowA.at(level).image;
    }
    [[nodiscard]] vk::Extent2D ffxFlowExtent(uint32_t level) const {
        return this->m_ffxFlowExtent.at(level);
    }

private:
    struct Image {
        vk::UniqueImage image;
        vk::UniqueDeviceMemory memory;
        vk::UniqueImageView view;
        vk::Extent2D extent;
        vk::Format format;
    };

    struct FfxPass {
        vk::UniqueDescriptorSetLayout setLayout;
        vk::UniquePipelineLayout pipelineLayout;
        vk::UniquePipeline pipeline;
    };

    Image createImage(vk::Extent2D extent, vk::Format format, vk::ImageUsageFlags usage);
    void createPipelines();
    void createDescriptors();
    void createFfxResources();
    void createFfxPipelines();
    void createFfxDescriptors();
    vk::DescriptorSet makeSet(
        const std::array<vk::ImageView, 5>& sampled,
        const std::array<vk::ImageView, 4>& storage
    );
    vk::DescriptorSet allocFfxSet(vk::DescriptorSetLayout layout);
    void dispatch(
        const vk::CommandBuffer& cmdbuf,
        std::string_view pipeline,
        vk::DescriptorSet set,
        vk::Extent2D extent,
        const PushConstants& pc
    );
    void dispatchFfx(
        const vk::CommandBuffer& cmdbuf,
        const vk::Pipeline& pipeline,
        const vk::PipelineLayout& layout,
        vk::DescriptorSet set,
        const std::vector<uint32_t>& offsets,
        vk::Extent3D groups
    );
    void barrier(const vk::CommandBuffer& cmdbuf) const;
    void analyzeFfx(
        const vk::CommandBuffer& cmdbuf,
        uint32_t layerA,
        uint32_t layerB
    );
    void generateFfx(
        const vk::CommandBuffer& cmdbuf,
        uint32_t layerA,
        uint32_t layerB,
        float t,
        uint32_t flags,
        uint32_t destinationLayer
    );

    const vk::detail::DispatchLoaderDynamic& m_dld;
    const vk::PhysicalDevice m_physdev;
    const vk::Device m_device;
    const vk::Extent2D m_extent;
    const ShaderLoader m_loader;
    bool m_bidirectional{false};
    bool m_occlusionFlow{true};
    bool m_tileMaskEnabled{true};
    bool m_performanceMode{false};
    bool m_videoSafe{false};
    int m_blockRadius{8};
    int m_refineIterations{4};
    int m_refineRadius{2};
    int m_smoothRadius{3};
    vk::Format m_colorFormat{vk::Format::eR8G8B8A8Unorm};
    bool m_hdr{false};
    Generator m_generator{Generator::Flow};

    std::array<vk::Extent2D, 6> m_levels{};
    std::array<Image, 6> m_lumaA{};
    std::array<Image, 6> m_lumaB{};
    std::array<Image, 6> m_flow{};
    std::array<Image, 6> m_flowFiltered{};
    std::array<Image, 6> m_flowSmooth{};
    Image m_flowFull;
    Image m_tileMask;
    Image m_cutScore;
    std::array<Image, 6> m_flowBA{};
    std::array<Image, 6> m_flowBAFiltered{};
    std::array<Image, 6> m_flowBASmooth{};

    vk::UniqueImageView m_srcView;
    vk::UniqueImageView m_dstView;
    std::array<vk::UniqueImageView, 2> m_srcLayerView{};

    Image m_dummyR8;
    Image m_dummyRg16f;
    Image m_dummyRgba8;

    // FidelityFX Optical Flow state. Both luma pyramids are rebuilt from the
    // two source layers on each analyze call; the scene-change detector keeps
    // its previous histogram between calls.
    vk::Extent2D m_ffxExtent{};
    uint32_t m_ffxFlowScale{1};
    uint32_t m_ffxLevels{5};
    std::array<vk::Extent2D, 7> m_ffxLumaExtent{};
    std::array<vk::Extent2D, 7> m_ffxFlowExtent{};
    std::array<Image, 7> m_ffxLumaCurrent{};
    std::array<Image, 7> m_ffxLumaPrevious{};
    std::array<Image, 7> m_ffxFlowA{};
    std::array<Image, 7> m_ffxFlowB{};
    Image m_ffxFlowOut;
    Image m_ffxDense;
    Image m_ffxScd;
    Image m_ffxScdHistogram;
    Image m_ffxScdPreviousHistogram;
    Image m_ffxScdTemp;
    Image m_ffxColorHalf;
    bool m_ffxHalfColor{false};
    bool m_ffxInitialized{false};

    vk::UniqueBuffer m_ffxCb;
    vk::UniqueDeviceMemory m_ffxCbMemory;
    void* m_ffxCbMapped{};
    uint32_t m_ffxCbStride{};

    FfxPass m_ffxPrepare;
    FfxPass m_ffxPyramid;
    FfxPass m_ffxSearch;
    FfxPass m_ffxFilter;
    FfxPass m_ffxScale;
    FfxPass m_ffxScdHistogramPass;
    FfxPass m_ffxScdDivergencePass;
    vk::UniqueDescriptorPool m_ffxPool;
    std::array<vk::DescriptorSet, 2> m_ffxPrepareSetCurrent{};
    std::array<vk::DescriptorSet, 2> m_ffxPrepareSetPrevious{};
    vk::DescriptorSet m_ffxPrepareHalfSetCurrent{};
    vk::DescriptorSet m_ffxPrepareHalfSetPrevious{};
    std::array<vk::DescriptorSet, 2> m_setFfxDownsample{};
    vk::DescriptorSet m_ffxPyramidSetCurrent{};
    vk::DescriptorSet m_ffxPyramidSetPrevious{};
    std::array<vk::DescriptorSet, 7> m_ffxSearchSet{};
    std::array<vk::DescriptorSet, 7> m_ffxFilterSet{};
    std::array<vk::DescriptorSet, 7> m_ffxScaleSet{};
    vk::DescriptorSet m_ffxScdHistogramSet{};
    vk::DescriptorSet m_ffxScdDivergenceSet{};

    vk::UniqueSampler m_sampler;
    vk::UniqueDescriptorSetLayout m_setLayout;
    vk::UniquePipelineLayout m_pipelineLayout;
    vk::UniqueDescriptorPool m_pool;
    std::array<vk::UniquePipeline, 15> m_pipelines{};
    std::vector<vk::UniqueShaderModule> m_modules;

    vk::DescriptorSet m_setLumaA{};
    vk::DescriptorSet m_setLumaB{};
    std::array<vk::DescriptorSet, 6> m_setDownA{};
    std::array<vk::DescriptorSet, 6> m_setDownB{};
    vk::DescriptorSet m_setBlock{};
    vk::DescriptorSet m_setBlockBA{};
    std::array<vk::DescriptorSet, 6> m_setRefine{};
    std::array<vk::DescriptorSet, 6> m_setRefineBA{};
    vk::DescriptorSet m_setInterp{};
    vk::DescriptorSet m_setInterpFfx{};
    vk::DescriptorSet m_setFfxDense{};
    vk::DescriptorSet m_setFfxCut{};
    vk::DescriptorSet m_setRefineFull{};
    vk::DescriptorSet m_setTileMask{};
    vk::DescriptorSet m_setCut{};
    std::array<vk::DescriptorSet, 6> m_setMedian{};
    std::array<vk::DescriptorSet, 6> m_setMedianBA{};
    std::array<vk::DescriptorSet, 6> m_setSmooth{};
    std::array<vk::DescriptorSet, 6> m_setSmoothBA{};
};

}
