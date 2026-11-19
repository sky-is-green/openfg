/* SPDX-License-Identifier: MIT */

#include "pipeline.hpp"

#include "openfg_shaders.hpp"
#include "vkutil/vkutil.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>

namespace openfg::scaler {
namespace {

    vk::UniqueShaderModule module(
        Runtime& runtime,
        std::string_view name
    ) {
        const auto code = openfg::embeddedShader(name);
        if (code.empty())
            throw std::runtime_error(
                "missing embedded shader " + std::string(name));
        return vkutil::createShaderModule(
            runtime.dld, *runtime.device, code);
    }

} // namespace

void PresentPipeline::init(
    Runtime& runtime,
    vk::Format colorFormat,
    vk::ImageLayout finalLayout
) {
    runtime_ = &runtime;

    const vk::AttachmentDescription color{
        .format = colorFormat,
        .samples = vk::SampleCountFlagBits::e1,
        .loadOp = vk::AttachmentLoadOp::eClear,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .stencilLoadOp = vk::AttachmentLoadOp::eDontCare,
        .stencilStoreOp = vk::AttachmentStoreOp::eDontCare,
        .initialLayout = vk::ImageLayout::eUndefined,
        .finalLayout = finalLayout
    };
    const vk::AttachmentReference colorReference{
        .attachment = 0,
        .layout = vk::ImageLayout::eColorAttachmentOptimal
    };
    const vk::SubpassDescription subpass{
        .pipelineBindPoint = vk::PipelineBindPoint::eGraphics,
        .colorAttachmentCount = 1,
        .pColorAttachments = &colorReference
    };
    const vk::SubpassDependency dependency{
        .srcSubpass = VK_SUBPASS_EXTERNAL,
        .dstSubpass = 0,
        .srcStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput,
        .dstStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput,
        .dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite
    };
    renderPass_ = runtime.device->createRenderPassUnique({
        .attachmentCount = 1,
        .pAttachments = &color,
        .subpassCount = 1,
        .pSubpasses = &subpass,
        .dependencyCount = 1,
        .pDependencies = &dependency
    }, nullptr, runtime.dld);

    const vk::DescriptorSetLayoutBinding binding{
        .binding = 0,
        .descriptorType = vk::DescriptorType::eCombinedImageSampler,
        .descriptorCount = 1,
        .stageFlags = vk::ShaderStageFlagBits::eFragment
    };
    setLayout_ = runtime.device->createDescriptorSetLayoutUnique({
        .bindingCount = 1,
        .pBindings = &binding
    }, nullptr, runtime.dld);
    pipelineLayout_ = runtime.device->createPipelineLayoutUnique({
        .setLayoutCount = 1,
        .pSetLayouts = &*setLayout_
    }, nullptr, runtime.dld);

    const vk::DescriptorPoolSize poolSize{
        .type = vk::DescriptorType::eCombinedImageSampler,
        .descriptorCount = 1
    };
    pool_ = runtime.device->createDescriptorPoolUnique({
        .maxSets = 1,
        .poolSizeCount = 1,
        .pPoolSizes = &poolSize
    }, nullptr, runtime.dld);
    set_ = runtime.device->allocateDescriptorSets({
        .descriptorPool = *pool_,
        .descriptorSetCount = 1,
        .pSetLayouts = &*setLayout_
    }, runtime.dld).front();

    sampler_ = vkutil::createSampler(
        runtime.dld, *runtime.device, vk::SamplerAddressMode::eClampToEdge,
        vk::CompareOp::eNever, false);

    auto vertex = module(runtime, "present_vert");
    auto fragment = module(runtime, "present_frag");
    const std::array<vk::PipelineShaderStageCreateInfo, 2> stages{{
        {
            .stage = vk::ShaderStageFlagBits::eVertex,
            .module = *vertex,
            .pName = "main"
        },
        {
            .stage = vk::ShaderStageFlagBits::eFragment,
            .module = *fragment,
            .pName = "main"
        }
    }};
    const vk::PipelineVertexInputStateCreateInfo vertexInput{};
    const vk::PipelineInputAssemblyStateCreateInfo inputAssembly{
        .topology = vk::PrimitiveTopology::eTriangleList
    };
    const vk::PipelineViewportStateCreateInfo viewport{
        .viewportCount = 1,
        .scissorCount = 1
    };
    const vk::PipelineRasterizationStateCreateInfo rasterizer{
        .polygonMode = vk::PolygonMode::eFill,
        .cullMode = vk::CullModeFlagBits::eNone,
        .frontFace = vk::FrontFace::eCounterClockwise,
        .lineWidth = 1.0F
    };
    const vk::PipelineMultisampleStateCreateInfo multisample{
        .rasterizationSamples = vk::SampleCountFlagBits::e1
    };
    const vk::PipelineColorBlendAttachmentState blendAttachment{
        .colorWriteMask =
            vk::ColorComponentFlagBits::eR |
            vk::ColorComponentFlagBits::eG |
            vk::ColorComponentFlagBits::eB |
            vk::ColorComponentFlagBits::eA
    };
    const vk::PipelineColorBlendStateCreateInfo blend{
        .attachmentCount = 1,
        .pAttachments = &blendAttachment
    };
    const std::array<vk::DynamicState, 2> dynamicStates{
        vk::DynamicState::eViewport,
        vk::DynamicState::eScissor
    };
    const vk::PipelineDynamicStateCreateInfo dynamic{
        .dynamicStateCount = static_cast<uint32_t>(dynamicStates.size()),
        .pDynamicStates = dynamicStates.data()
    };
    const vk::GraphicsPipelineCreateInfo pipelineInfo{
        .stageCount = static_cast<uint32_t>(stages.size()),
        .pStages = stages.data(),
        .pVertexInputState = &vertexInput,
        .pInputAssemblyState = &inputAssembly,
        .pViewportState = &viewport,
        .pRasterizationState = &rasterizer,
        .pMultisampleState = &multisample,
        .pColorBlendState = &blend,
        .pDynamicState = &dynamic,
        .layout = *pipelineLayout_,
        .renderPass = *renderPass_,
        .subpass = 0
    };
    auto result = runtime.device->createGraphicsPipelineUnique(
        nullptr, pipelineInfo, nullptr, runtime.dld);
    if (result.result != vk::Result::eSuccess)
        throw std::runtime_error("present pipeline creation failed");
    pipeline_ = std::move(result.value);
}

void PresentPipeline::record(
    Runtime& runtime,
    vk::CommandBuffer command,
    vk::Framebuffer framebuffer,
    vk::Extent2D targetExtent,
    vk::ImageView source,
    vk::Extent2D sourceExtent
) {
    const vk::DescriptorImageInfo imageInfo{
        .sampler = *sampler_,
        .imageView = source,
        .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal
    };
    const vk::WriteDescriptorSet write{
        .dstSet = set_,
        .dstBinding = 0,
        .descriptorCount = 1,
        .descriptorType = vk::DescriptorType::eCombinedImageSampler,
        .pImageInfo = &imageInfo
    };
    runtime.device->updateDescriptorSets(1, &write, 0, nullptr, runtime.dld);

    const vk::ClearValue clear{
        .color = { .float32 = { { 0.0F, 0.0F, 0.0F, 1.0F } } }
    };
    command.beginRenderPass({
        .renderPass = *renderPass_,
        .framebuffer = framebuffer,
        .renderArea = { { 0, 0 }, targetExtent },
        .clearValueCount = 1,
        .pClearValues = &clear
    }, vk::SubpassContents::eInline, runtime.dld);

    const float targetAspect = static_cast<float>(targetExtent.width) /
        static_cast<float>(targetExtent.height);
    const float sourceAspect = static_cast<float>(sourceExtent.width) /
        static_cast<float>(sourceExtent.height);
    float width = static_cast<float>(targetExtent.width);
    float height = static_cast<float>(targetExtent.height);
    float x = 0.0F;
    float y = 0.0F;
    if (targetAspect > sourceAspect) {
        width = static_cast<float>(targetExtent.height) * sourceAspect;
        x = (static_cast<float>(targetExtent.width) - width) * 0.5F;
    } else {
        height = static_cast<float>(targetExtent.width) / sourceAspect;
        y = (static_cast<float>(targetExtent.height) - height) * 0.5F;
    }
    const vk::Viewport viewport{
        .x = x,
        .y = y,
        .width = width,
        .height = height,
        .minDepth = 0.0F,
        .maxDepth = 1.0F
    };
    const vk::Rect2D scissor{
        .offset = {
            static_cast<int32_t>(x),
            static_cast<int32_t>(y)
        },
        .extent = {
            static_cast<uint32_t>(std::max(1.0F, width)),
            static_cast<uint32_t>(std::max(1.0F, height))
        }
    };
    command.setViewport(0, { viewport }, runtime.dld);
    command.setScissor(0, { scissor }, runtime.dld);
    command.bindPipeline(
        vk::PipelineBindPoint::eGraphics, *pipeline_, runtime.dld);
    command.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
        *pipelineLayout_, 0, { set_ }, {}, runtime.dld);
    command.draw(3, 1, 0, 0, runtime.dld);
    command.endRenderPass(runtime.dld);
}

void PresentPipeline::destroy()
{
    set_ = {};
    pool_ = {};
    sampler_ = {};
    pipeline_ = {};
    pipelineLayout_ = {};
    setLayout_ = {};
    renderPass_ = {};
}

} // namespace openfg::scaler
