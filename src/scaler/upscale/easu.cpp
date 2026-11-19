/* SPDX-License-Identifier: MIT */

#include "easu.hpp"

#include "openfg_shaders.hpp"
#include "scaler/upscale/fsr1_constants.hpp"
#include "vkutil/vkutil.hpp"

#include <algorithm>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace openfg::scaler {
namespace {

    vk::UniquePipeline makePipeline(
        const Runtime& runtime,
        const vk::PipelineLayout& layout,
        std::string_view name
    ) {
        const auto code = openfg::embeddedShader(name);
        if (code.empty())
            throw std::runtime_error(
                "missing embedded shader " + std::string(name));
        auto module =
            vkutil::createShaderModule(runtime.dld, *runtime.device, code);
        const vk::ComputePipelineCreateInfo info{
            .stage = {
                .stage = vk::ShaderStageFlagBits::eCompute,
                .module = *module,
                .pName = "main"
            },
            .layout = layout
        };
        auto result = runtime.device->createComputePipelineUnique(
            nullptr, info, nullptr, runtime.dld);
        if (result.result != vk::Result::eSuccess)
            throw std::runtime_error(
                "pipeline creation failed for " + std::string(name));
        return std::move(result.value);
    }

    std::pair<vk::UniqueImage, vk::UniqueDeviceMemory> createImage(
        const Runtime& runtime,
        vk::Extent2D extent,
        vk::Format format,
        vk::ImageUsageFlags usage
    ) {
        auto image = vkutil::createImage(
            runtime.dld, *runtime.device, extent, format, 1, usage);
        const auto requirements =
            runtime.device->getImageMemoryRequirements(*image, runtime.dld);
        auto memory = vkutil::allocateMemory(
            runtime.dld, *runtime.device, runtime.physical,
            requirements.size, requirements.memoryTypeBits);
        runtime.device->bindImageMemory(
            *image, *memory, 0, runtime.dld);
        return { std::move(image), std::move(memory) };
    }

    void barrier(
        const Runtime& runtime,
        vk::CommandBuffer command,
        vk::Image image,
        vk::ImageLayout oldLayout,
        vk::ImageLayout newLayout,
        vk::AccessFlags srcAccess,
        vk::AccessFlags dstAccess,
        vk::PipelineStageFlags srcStage =
            vk::PipelineStageFlagBits::eComputeShader,
        vk::PipelineStageFlags dstStage =
            vk::PipelineStageFlagBits::eComputeShader
    ) {
        const vk::ImageMemoryBarrier info{
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
                .layerCount = 1
            }
        };
        command.pipelineBarrier(
            srcStage, dstStage, {}, {}, {}, { info }, runtime.dld);
    }

} // namespace

EasuUpscaler::EasuUpscaler(
    Runtime& runtime,
    vk::Extent2D input,
    vk::Extent2D output,
    bool sharpening,
    float sharpness
)
    : runtime_{runtime},
      inputExtent_{input},
      outputExtent_{output},
      sharpening_{sharpening},
      sharpness_{sharpness}
{
    if (input.width == 0 || input.height == 0 ||
        output.width == 0 || output.height == 0)
        throw std::runtime_error("upscaler extents must be non-zero");

    sampler_ = vkutil::createSampler(
        runtime_.dld, *runtime_.device, vk::SamplerAddressMode::eClampToEdge,
        vk::CompareOp::eNever, false);

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
    auto [layout, pipelineLayout] = vkutil::createLayout(
        runtime_.dld, *runtime_.device, { bindings.begin(), bindings.end() },
        sizeof(PushConstants));
    setLayout_ = std::move(layout);
    pipelineLayout_ = std::move(pipelineLayout);

    easuPipeline_ = makePipeline(runtime_, *pipelineLayout_, "fsr1_easu");
    if (sharpening_)
        rcasPipeline_ = makePipeline(
            runtime_, *pipelineLayout_, "fsr1_rcas");

    createImages();
}

EasuUpscaler::~EasuUpscaler() = default;

void EasuUpscaler::createImages()
{
    outputImage_ = {};
    outputMemory_ = {};
    outputView_ = {};
    intermediateImage_ = {};
    intermediateMemory_ = {};
    intermediateView_ = {};

    auto output = createImage(runtime_, outputExtent_,
        vk::Format::eR32G32B32A32Sfloat,
        vk::ImageUsageFlagBits::eStorage |
        vk::ImageUsageFlagBits::eSampled |
        vk::ImageUsageFlagBits::eTransferSrc);
    outputImage_ = std::move(output.first);
    outputMemory_ = std::move(output.second);
    outputView_ = vkutil::createImageView(
        runtime_.dld, *runtime_.device, *outputImage_,
        vk::Format::eR32G32B32A32Sfloat, 1);

    if (sharpening_) {
        auto intermediate = createImage(runtime_, outputExtent_,
            vk::Format::eR32G32B32A32Sfloat,
            vk::ImageUsageFlagBits::eStorage |
            vk::ImageUsageFlagBits::eSampled |
            vk::ImageUsageFlagBits::eTransferSrc);
        intermediateImage_ = std::move(intermediate.first);
        intermediateMemory_ = std::move(intermediate.second);
        intermediateView_ = vkutil::createImageView(
            runtime_.dld, *runtime_.device, *intermediateImage_,
            vk::Format::eR32G32B32A32Sfloat, 1);
    }

    auto easu = vkutil::createDescriptorSet(
        runtime_.dld, *runtime_.device, *setLayout_, 1, 0, 1, 1);
    easuPool_ = std::move(easu.first);
    easuSet_ = easu.second;

    if (sharpening_) {
        auto rcas = vkutil::createDescriptorSet(
            runtime_.dld, *runtime_.device, *setLayout_, 1, 0, 1, 1);
        rcasPool_ = std::move(rcas.first);
        rcasSet_ = rcas.second;

        const vk::DescriptorImageInfo inputInfo{
            .imageView = *intermediateView_,
            .imageLayout = vk::ImageLayout::eGeneral
        };
        const vk::DescriptorImageInfo samplerInfo{
            .sampler = *sampler_
        };
        const vk::DescriptorImageInfo outputInfo{
            .imageView = *outputView_,
            .imageLayout = vk::ImageLayout::eGeneral
        };
        const std::array<vk::WriteDescriptorSet, 3> writes{{
            {
                .dstSet = rcasSet_,
                .dstBinding = 0,
                .descriptorCount = 1,
                .descriptorType = vk::DescriptorType::eSampledImage,
                .pImageInfo = &inputInfo
            },
            {
                .dstSet = rcasSet_,
                .dstBinding = 1,
                .descriptorCount = 1,
                .descriptorType = vk::DescriptorType::eSampler,
                .pImageInfo = &samplerInfo
            },
            {
                .dstSet = rcasSet_,
                .dstBinding = 2,
                .descriptorCount = 1,
                .descriptorType = vk::DescriptorType::eStorageImage,
                .pImageInfo = &outputInfo
            }
        }};
        runtime_.device->updateDescriptorSets(
            static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr,
            runtime_.dld);
    }

    initialized_ = false;
}

void EasuUpscaler::destroyImages()
{
    intermediateView_ = {};
    intermediateImage_ = {};
    intermediateMemory_ = {};
    outputView_ = {};
    outputImage_ = {};
    outputMemory_ = {};
    easuPool_ = {};
    rcasPool_ = {};
    initialized_ = false;
}

void EasuUpscaler::updateEasuSet(vk::ImageView input)
{
    const vk::DescriptorImageInfo inputInfo{
        .imageView = input,
        .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal
    };
    const vk::DescriptorImageInfo samplerInfo{
        .sampler = *sampler_
    };
    const vk::DescriptorImageInfo outputInfo{
        .imageView = *(sharpening_ ? intermediateView_ : outputView_),
        .imageLayout = vk::ImageLayout::eGeneral
    };
    const std::array<vk::WriteDescriptorSet, 3> writes{{
        {
            .dstSet = easuSet_,
            .dstBinding = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eSampledImage,
            .pImageInfo = &inputInfo
        },
        {
            .dstSet = easuSet_,
            .dstBinding = 1,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eSampler,
            .pImageInfo = &samplerInfo
        },
        {
            .dstSet = easuSet_,
            .dstBinding = 2,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eStorageImage,
            .pImageInfo = &outputInfo
        }
    }};
    runtime_.device->updateDescriptorSets(
        static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr,
        runtime_.dld);
}

void EasuUpscaler::resize(vk::Extent2D input, vk::Extent2D output)
{
    if (input == inputExtent_ && output == outputExtent_)
        return;
    if (input.width == 0 || input.height == 0 ||
        output.width == 0 || output.height == 0)
        return;

    runtime_.device->waitIdle(runtime_.dld);
    destroyImages();
    inputExtent_ = input;
    outputExtent_ = output;
    createImages();
}

void EasuUpscaler::record(
    vk::CommandBuffer command,
    vk::ImageView input,
    vk::Extent2D inputExtent
)
{
    if (inputExtent != inputExtent_)
        return;

    updateEasuSet(input);

    const auto& easuConstants = fsr1EasuConstants(
        inputExtent_.width, inputExtent_.height,
        outputExtent_.width, outputExtent_.height);
    const auto& rcasConstants = fsr1RcasConstants(sharpness_);
    PushConstants easuPush{};
    std::copy_n(easuConstants.begin(), 16, easuPush.c0.begin());
    PushConstants rcasPush{};
    std::copy_n(rcasConstants.begin(), 4, rcasPush.c0.begin());

    barrier(runtime_, command, *outputImage_,
        initialized_ ? vk::ImageLayout::eShaderReadOnlyOptimal
                     : vk::ImageLayout::eUndefined,
        vk::ImageLayout::eGeneral,
        initialized_ ? vk::AccessFlags{vk::AccessFlagBits::eShaderRead}
                     : vk::AccessFlags{},
        vk::AccessFlagBits::eShaderWrite,
        vk::PipelineStageFlagBits::eFragmentShader,
        vk::PipelineStageFlagBits::eComputeShader);

    if (sharpening_) {
        barrier(runtime_, command, *intermediateImage_,
            initialized_ ? vk::ImageLayout::eGeneral
                         : vk::ImageLayout::eUndefined,
            vk::ImageLayout::eGeneral,
            initialized_ ? vk::AccessFlags{vk::AccessFlagBits::eShaderRead}
                         : vk::AccessFlags{},
            vk::AccessFlagBits::eShaderWrite);
    }

    const uint32_t groupX = (outputExtent_.width + 7) / 8;
    const uint32_t groupY = (outputExtent_.height + 7) / 8;

    command.bindPipeline(
        vk::PipelineBindPoint::eCompute, *easuPipeline_, runtime_.dld);
    command.bindDescriptorSets(vk::PipelineBindPoint::eCompute,
        *pipelineLayout_, 0, { easuSet_ }, {}, runtime_.dld);
    command.pushConstants(*pipelineLayout_,
        vk::ShaderStageFlagBits::eCompute, 0, sizeof(easuPush), &easuPush,
        runtime_.dld);
    command.dispatch(groupX, groupY, 1, runtime_.dld);

    if (sharpening_) {
        barrier(runtime_, command, *intermediateImage_,
            vk::ImageLayout::eGeneral, vk::ImageLayout::eGeneral,
            vk::AccessFlagBits::eShaderWrite,
            vk::AccessFlagBits::eShaderRead);

        command.bindPipeline(
            vk::PipelineBindPoint::eCompute, *rcasPipeline_, runtime_.dld);
        command.bindDescriptorSets(vk::PipelineBindPoint::eCompute,
            *pipelineLayout_, 0, { rcasSet_ }, {}, runtime_.dld);
        command.pushConstants(*pipelineLayout_,
            vk::ShaderStageFlagBits::eCompute, 0, sizeof(rcasPush), &rcasPush,
            runtime_.dld);
        command.dispatch(groupX, groupY, 1, runtime_.dld);
    }

    barrier(runtime_, command, *outputImage_,
        vk::ImageLayout::eGeneral,
        vk::ImageLayout::eShaderReadOnlyOptimal,
        vk::AccessFlagBits::eShaderWrite,
        vk::AccessFlagBits::eShaderRead,
        vk::PipelineStageFlagBits::eComputeShader,
        vk::PipelineStageFlagBits::eFragmentShader);
    initialized_ = true;
}

} // namespace openfg::scaler
