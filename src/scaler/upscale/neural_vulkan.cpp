/* SPDX-License-Identifier: MIT */

#include "neural_backend.hpp"

#include "openfg_shaders.hpp"
#include "vkutil/vkutil.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace openfg::scaler {
namespace {

constexpr uint32_t kRelu = 1U;
constexpr uint32_t kResidual = 2U;
constexpr uint32_t kMaxChannels = 20;

struct OfgHeader {
    char magic[8];
    uint32_t version;
    uint32_t scale;
    uint32_t channels;
    uint32_t blocks;
    uint32_t inputChannels;
    uint32_t outputChannels;
};

static_assert(sizeof(OfgHeader) == 32, "unexpected .ofg header size");

struct Layer {
    uint32_t inChannels{};
    uint32_t outChannels{};
    uint32_t weightOffset{};
    uint32_t biasOffset{};
    uint32_t flags{};
};

struct ConvPush {
    uint32_t width;
    uint32_t height;
    uint32_t inChannels;
    uint32_t outChannels;
    uint32_t weightOffset;
    uint32_t biasOffset;
    uint32_t flags;
};

struct PreparePush {
    uint32_t width;
    uint32_t height;
    uint32_t swapRedBlue;
    uint32_t pad;
};

struct ShufflePush {
    uint32_t width;
    uint32_t height;
    uint32_t outWidth;
    uint32_t outHeight;
    uint32_t scale;
    uint32_t pad0;
    uint32_t pad1;
    uint32_t pad2;
};

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

class VulkanNeuralBackend final : public NeuralBackend {
public:
    VulkanNeuralBackend(Runtime& runtime, const std::string& modelPath)
        : runtime_{runtime}
    {
        loadModel(modelPath);
        sampler_ = vkutil::createSampler(
            runtime_.dld, *runtime_.device,
            vk::SamplerAddressMode::eClampToEdge,
            vk::CompareOp::eNever, false);
        createLayouts();
        createDescriptors();
        createPipelines();
    }

    ~VulkanNeuralBackend() override
    {
        if (runtime_.device)
            runtime_.device->waitIdle(runtime_.dld);
        destroyResources();
    }

    void resize(vk::Extent2D input, vk::Extent2D output) override
    {
        if (input == inputExtent_ && output == outputExtent_)
            return;

        runtime_.device->waitIdle(runtime_.dld);
        destroyResources();

        inputExtent_ = input;
        outputExtent_ = output;
        valid_ = input.width != 0 && input.height != 0 &&
            output.width == input.width * 2 &&
            output.height == input.height * 2;
        if (!valid_) {
            error_ = "the v0 neural model is 2x; "
                "output must be input * 2";
            return;
        }

        createBuffers();
        createOutput();
        writeBufferDescriptors();
        resourcesReady_ = true;
    }

    bool prepare(
        Runtime&,
        const Frame& frame,
        std::string& error
    ) override
    {
        if (!valid_ || !resourcesReady_) {
            error = error_;
            return false;
        }
        if (frame.extent != inputExtent_) {
            error = "neural input extent changed";
            return false;
        }
        swapRedBlue_ = frame.format == vk::Format::eB8G8R8A8Unorm;
        return true;
    }

    void record(
        vk::CommandBuffer command,
        vk::ImageView input,
        vk::Extent2D inputExtent
    ) override
    {
        if (!valid_ || !resourcesReady_ || inputExtent != inputExtent_)
            return;

        updatePrepareImage(input);

        const vk::ImageMemoryBarrier begin{
            .srcAccessMask = initialized_
                ? vk::AccessFlags{vk::AccessFlagBits::eShaderRead}
                : vk::AccessFlags{},
            .dstAccessMask = vk::AccessFlagBits::eShaderWrite,
            .oldLayout = initialized_
                ? vk::ImageLayout::eShaderReadOnlyOptimal
                : vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eGeneral,
            .srcQueueFamilyIndex = vk::QueueFamilyIgnored,
            .dstQueueFamilyIndex = vk::QueueFamilyIgnored,
            .image = *outputImage_,
            .subresourceRange = {
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .levelCount = 1,
                .layerCount = 1
            }
        };
        command.pipelineBarrier(
            initialized_
                ? vk::PipelineStageFlagBits::eFragmentShader
                : vk::PipelineStageFlagBits::eTopOfPipe,
            vk::PipelineStageFlagBits::eComputeShader,
            {}, {}, {}, { begin }, runtime_.dld);

        PreparePush prepare{
            .width = inputExtent_.width,
            .height = inputExtent_.height,
            .swapRedBlue = swapRedBlue_ ? 1U : 0U,
            .pad = 0
        };
        command.bindPipeline(
            vk::PipelineBindPoint::eCompute, *preparePipeline_,
            runtime_.dld);
        command.bindDescriptorSets(
            vk::PipelineBindPoint::eCompute, *preparePipelineLayout_, 0,
            { prepareSet_ }, {}, runtime_.dld);
        command.pushConstants(
            *preparePipelineLayout_, vk::ShaderStageFlagBits::eCompute,
            0, sizeof(prepare), &prepare, runtime_.dld);
        command.dispatch(
            (inputExtent_.width + 7) / 8,
            (inputExtent_.height + 7) / 8, 1, runtime_.dld);

        const uint32_t inputX = (inputExtent_.width + 7) / 8;
        const uint32_t inputY = (inputExtent_.height + 7) / 8;

        auto conv = [&](const Layer& layer, const vk::DescriptorSet& set) {
            bufferBarrier(command);
            ConvPush push{
                .width = inputExtent_.width,
                .height = inputExtent_.height,
                .inChannels = layer.inChannels,
                .outChannels = layer.outChannels,
                .weightOffset = layer.weightOffset,
                .biasOffset = layer.biasOffset,
                .flags = layer.flags
            };
            command.bindPipeline(
                vk::PipelineBindPoint::eCompute, *convPipeline_,
                runtime_.dld);
            command.bindDescriptorSets(
                vk::PipelineBindPoint::eCompute, *convPipelineLayout_, 0,
                { set }, {}, runtime_.dld);
            command.pushConstants(
                *convPipelineLayout_, vk::ShaderStageFlagBits::eCompute,
                0, sizeof(push), &push, runtime_.dld);
            command.dispatch(inputX, inputY, 1, runtime_.dld);
        };

        conv(layers_.front(), convAB_);
        for (size_t block = 0; block < static_cast<size_t>(blocks_);
             block++) {
            conv(layers_[1 + block * 2], convBA_);
            conv(layers_[2 + block * 2], convAB_);
        }
        conv(layers_.back(), convBA_);

        bufferBarrier(command);

        ShufflePush shuffle{
            .width = inputExtent_.width,
            .height = inputExtent_.height,
            .outWidth = outputExtent_.width,
            .outHeight = outputExtent_.height,
            .scale = 2,
            .pad0 = 0,
            .pad1 = 0,
            .pad2 = 0
        };
        command.bindPipeline(
            vk::PipelineBindPoint::eCompute, *shufflePipeline_,
            runtime_.dld);
        command.bindDescriptorSets(
            vk::PipelineBindPoint::eCompute, *shufflePipelineLayout_, 0,
            { shuffleSet_ }, {}, runtime_.dld);
        command.pushConstants(
            *shufflePipelineLayout_, vk::ShaderStageFlagBits::eCompute,
            0, sizeof(shuffle), &shuffle, runtime_.dld);
        command.dispatch(
            (outputExtent_.width + 7) / 8,
            (outputExtent_.height + 7) / 8, 1, runtime_.dld);

        const vk::ImageMemoryBarrier end{
            .srcAccessMask = vk::AccessFlagBits::eShaderWrite,
            .dstAccessMask = vk::AccessFlagBits::eShaderRead,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .srcQueueFamilyIndex = vk::QueueFamilyIgnored,
            .dstQueueFamilyIndex = vk::QueueFamilyIgnored,
            .image = *outputImage_,
            .subresourceRange = {
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .levelCount = 1,
                .layerCount = 1
            }
        };
        command.pipelineBarrier(
            vk::PipelineStageFlagBits::eComputeShader,
            vk::PipelineStageFlagBits::eFragmentShader,
            {}, {}, {}, { end }, runtime_.dld);
        initialized_ = true;
    }

    vk::ImageView view() const override
    {
        return outputView_ ? *outputView_ : vk::ImageView{};
    }

    vk::Extent2D outputExtent() const override
    {
        return outputExtent_;
    }

    bool gpuTimed() const override
    {
        return true;
    }

private:
    void loadModel(const std::string& path)
    {
        std::ifstream input(path, std::ios::binary);
        if (!input)
            throw std::runtime_error("cannot open neural model " + path);

        OfgHeader header{};
        input.read(
            reinterpret_cast<char*>(&header), sizeof(header));
        if (input.gcount() != static_cast<std::streamsize>(sizeof(header)))
            throw std::runtime_error("truncated neural model " + path);
        if (std::memcmp(header.magic, "OFGNEUR1", 8) != 0)
            throw std::runtime_error(
                "not an .ofg model: " + path);
        if (header.version != 1)
            throw std::runtime_error(
                "unsupported .ofg version " +
                std::to_string(header.version));
        if (header.scale != 2 ||
            header.inputChannels != 3 ||
            header.outputChannels != 12 ||
            header.channels < 1 || header.channels > kMaxChannels)
            throw std::runtime_error(
                "unsupported .ofg architecture in " + path);

        channels_ = header.channels;
        blocks_ = header.blocks;

        size_t expected = static_cast<size_t>(header.channels) * 3 * 9 +
            header.channels;
        for (uint32_t block = 0; block < blocks_; block++)
            expected += 2 * (
                static_cast<size_t>(header.channels) * header.channels * 9 +
                header.channels);
        expected += static_cast<size_t>(header.channels) * 12 * 9 + 12;

        weights_.resize(expected);
        input.read(
            reinterpret_cast<char*>(weights_.data()),
            static_cast<std::streamsize>(expected * sizeof(float)));
        if (input.gcount() !=
            static_cast<std::streamsize>(expected * sizeof(float)))
            throw std::runtime_error("truncated neural weights " + path);
        if (input.peek() != std::char_traits<char>::eof())
            throw std::runtime_error(
                "unexpected data after neural weights " + path);

        uint32_t offset = 0;
        auto addLayer = [&](uint32_t in, uint32_t out, uint32_t flags) {
            Layer layer;
            layer.inChannels = in;
            layer.outChannels = out;
            layer.weightOffset = offset;
            offset += out * in * 9;
            layer.biasOffset = offset;
            offset += out;
            layer.flags = flags;
            layers_.push_back(layer);
        };
        addLayer(3, channels_, 0);
        for (uint32_t block = 0; block < blocks_; block++) {
            addLayer(channels_, channels_, kRelu);
            addLayer(channels_, channels_, kResidual);
        }
        addLayer(channels_, 12, 0);
        if (offset != expected)
            throw std::runtime_error("invalid .ofg layer layout");
    }

    void createLayouts()
    {
        const std::array<vk::DescriptorSetLayoutBinding, 3> prepareBindings{{
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
                .descriptorType = vk::DescriptorType::eStorageBuffer,
                .descriptorCount = 1,
                .stageFlags = vk::ShaderStageFlagBits::eCompute
            }
        }};
        auto prepare = vkutil::createLayout(
            runtime_.dld, *runtime_.device,
            { prepareBindings.begin(), prepareBindings.end() },
            sizeof(PreparePush));
        prepareLayout_ = std::move(prepare.first);
        preparePipelineLayout_ = std::move(prepare.second);

        const std::array<vk::DescriptorSetLayoutBinding, 4> convBindings{{
            {
                .binding = 0,
                .descriptorType = vk::DescriptorType::eStorageBuffer,
                .descriptorCount = 1,
                .stageFlags = vk::ShaderStageFlagBits::eCompute
            },
            {
                .binding = 1,
                .descriptorType = vk::DescriptorType::eStorageBuffer,
                .descriptorCount = 1,
                .stageFlags = vk::ShaderStageFlagBits::eCompute
            },
            {
                .binding = 2,
                .descriptorType = vk::DescriptorType::eStorageBuffer,
                .descriptorCount = 1,
                .stageFlags = vk::ShaderStageFlagBits::eCompute
            },
            {
                .binding = 3,
                .descriptorType = vk::DescriptorType::eStorageBuffer,
                .descriptorCount = 1,
                .stageFlags = vk::ShaderStageFlagBits::eCompute
            }
        }};
        auto conv = vkutil::createLayout(
            runtime_.dld, *runtime_.device,
            { convBindings.begin(), convBindings.end() },
            sizeof(ConvPush));
        convLayout_ = std::move(conv.first);
        convPipelineLayout_ = std::move(conv.second);

        const std::array<vk::DescriptorSetLayoutBinding, 2> shuffleBindings{{
            {
                .binding = 0,
                .descriptorType = vk::DescriptorType::eStorageBuffer,
                .descriptorCount = 1,
                .stageFlags = vk::ShaderStageFlagBits::eCompute
            },
            {
                .binding = 1,
                .descriptorType = vk::DescriptorType::eStorageImage,
                .descriptorCount = 1,
                .stageFlags = vk::ShaderStageFlagBits::eCompute
            }
        }};
        auto shuffle = vkutil::createLayout(
            runtime_.dld, *runtime_.device,
            { shuffleBindings.begin(), shuffleBindings.end() },
            sizeof(ShufflePush));
        shuffleLayout_ = std::move(shuffle.first);
        shufflePipelineLayout_ = std::move(shuffle.second);
    }

    void createDescriptors()
    {
        const std::array<vk::DescriptorPoolSize, 4> sizes{{
            {
                .type = vk::DescriptorType::eStorageBuffer,
                .descriptorCount = 10
            },
            {
                .type = vk::DescriptorType::eSampler,
                .descriptorCount = 1
            },
            {
                .type = vk::DescriptorType::eSampledImage,
                .descriptorCount = 1
            },
            {
                .type = vk::DescriptorType::eStorageImage,
                .descriptorCount = 1
            }
        }};
        descriptorPool_ = runtime_.device->createDescriptorPoolUnique({
            .maxSets = 4,
            .poolSizeCount = static_cast<uint32_t>(sizes.size()),
            .pPoolSizes = sizes.data()
        }, nullptr, runtime_.dld);

        const std::array<vk::DescriptorSetLayout, 4> layouts{{
            *prepareLayout_, *convLayout_, *convLayout_, *shuffleLayout_
        }};
        const auto sets = runtime_.device->allocateDescriptorSets({
            .descriptorPool = *descriptorPool_,
            .descriptorSetCount = static_cast<uint32_t>(layouts.size()),
            .pSetLayouts = layouts.data()
        }, runtime_.dld);
        prepareSet_ = sets[0];
        convAB_ = sets[1];
        convBA_ = sets[2];
        shuffleSet_ = sets[3];

        const vk::DescriptorImageInfo samplerInfo{
            .sampler = *sampler_
        };
        const vk::WriteDescriptorSet samplerWrite{
            .dstSet = prepareSet_,
            .dstBinding = 1,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eSampler,
            .pImageInfo = &samplerInfo
        };
        runtime_.device->updateDescriptorSets(
            1, &samplerWrite, 0, nullptr, runtime_.dld);
    }

    void createPipelines()
    {
        preparePipeline_ = makePipeline(
            runtime_, *preparePipelineLayout_, "neural_prepare");
        convPipeline_ = makePipeline(
            runtime_, *convPipelineLayout_, "neural_conv");
        shufflePipeline_ = makePipeline(
            runtime_, *shufflePipelineLayout_, "neural_shuffle");
    }

    void createBuffers()
    {
        const size_t spatial = static_cast<size_t>(inputExtent_.width) *
            inputExtent_.height;
        const size_t bufferChannels =
            std::max<size_t>(channels_, 12);
        const auto size = static_cast<vk::DeviceSize>(
            spatial * bufferChannels * sizeof(float));

        auto a = createStorageBuffer(size);
        planesA_ = std::move(a.first);
        planesAMemory_ = std::move(a.second);
        auto b = createStorageBuffer(size);
        planesB_ = std::move(b.first);
        planesBMemory_ = std::move(b.second);

        weightBufferSize_ = static_cast<vk::DeviceSize>(
            weights_.size() * sizeof(float));
        auto weights = vkutil::createBuffer(
            runtime_.dld, *runtime_.device, runtime_.physical,
            vk::BufferUsageFlagBits::eStorageBuffer,
            weights_.data(), static_cast<size_t>(weightBufferSize_));
        weightBuffer_ = std::move(weights.first);
        weightMemory_ = std::move(weights.second);
    }

    std::pair<vk::UniqueBuffer, vk::UniqueDeviceMemory> createStorageBuffer(
        vk::DeviceSize size
    ) {
        const vk::BufferCreateInfo info{
            .size = size,
            .usage = vk::BufferUsageFlagBits::eStorageBuffer |
                vk::BufferUsageFlagBits::eTransferSrc |
                vk::BufferUsageFlagBits::eTransferDst,
            .sharingMode = vk::SharingMode::eExclusive
        };
        auto buffer =
            runtime_.device->createBufferUnique(info, nullptr, runtime_.dld);
        const auto requirements =
            runtime_.device->getBufferMemoryRequirements(
                *buffer, runtime_.dld);
        auto memory = vkutil::allocateMemory(
            runtime_.dld, *runtime_.device, runtime_.physical,
            requirements.size, requirements.memoryTypeBits, false);
        runtime_.device->bindBufferMemory(
            *buffer, *memory, 0, runtime_.dld);
        return { std::move(buffer), std::move(memory) };
    }

    void createOutput()
    {
        outputImage_ = vkutil::createImage(
            runtime_.dld, *runtime_.device, outputExtent_,
            vk::Format::eR8G8B8A8Unorm, 1,
            vk::ImageUsageFlagBits::eSampled |
            vk::ImageUsageFlagBits::eStorage);
        const auto requirements =
            runtime_.device->getImageMemoryRequirements(
                *outputImage_, runtime_.dld);
        outputMemory_ = vkutil::allocateMemory(
            runtime_.dld, *runtime_.device, runtime_.physical,
            requirements.size, requirements.memoryTypeBits);
        runtime_.device->bindImageMemory(
            *outputImage_, *outputMemory_, 0, runtime_.dld);
        outputView_ = vkutil::createImageView(
            runtime_.dld, *runtime_.device, *outputImage_,
            vk::Format::eR8G8B8A8Unorm, 1);
        initialized_ = false;
    }

    void writeBufferDescriptors()
    {
        const vk::DescriptorBufferInfo weightInfo{
            .buffer = *weightBuffer_,
            .offset = 0,
            .range = weightBufferSize_
        };
        const vk::DescriptorBufferInfo aInfo{
            .buffer = *planesA_, .offset = 0, .range = VK_WHOLE_SIZE
        };
        const vk::DescriptorBufferInfo bInfo{
            .buffer = *planesB_, .offset = 0, .range = VK_WHOLE_SIZE
        };

        const std::array<vk::WriteDescriptorSet, 8> writes{{
            {
                .dstSet = convAB_,
                .dstBinding = 0,
                .descriptorCount = 1,
                .descriptorType = vk::DescriptorType::eStorageBuffer,
                .pBufferInfo = &weightInfo
            },
            {
                .dstSet = convAB_,
                .dstBinding = 1,
                .descriptorCount = 1,
                .descriptorType = vk::DescriptorType::eStorageBuffer,
                .pBufferInfo = &aInfo
            },
            {
                .dstSet = convAB_,
                .dstBinding = 2,
                .descriptorCount = 1,
                .descriptorType = vk::DescriptorType::eStorageBuffer,
                .pBufferInfo = &bInfo
            },
            {
                .dstSet = convAB_,
                .dstBinding = 3,
                .descriptorCount = 1,
                .descriptorType = vk::DescriptorType::eStorageBuffer,
                .pBufferInfo = &bInfo
            },
            {
                .dstSet = convBA_,
                .dstBinding = 0,
                .descriptorCount = 1,
                .descriptorType = vk::DescriptorType::eStorageBuffer,
                .pBufferInfo = &weightInfo
            },
            {
                .dstSet = convBA_,
                .dstBinding = 1,
                .descriptorCount = 1,
                .descriptorType = vk::DescriptorType::eStorageBuffer,
                .pBufferInfo = &bInfo
            },
            {
                .dstSet = convBA_,
                .dstBinding = 2,
                .descriptorCount = 1,
                .descriptorType = vk::DescriptorType::eStorageBuffer,
                .pBufferInfo = &aInfo
            },
            {
                .dstSet = convBA_,
                .dstBinding = 3,
                .descriptorCount = 1,
                .descriptorType = vk::DescriptorType::eStorageBuffer,
                .pBufferInfo = &aInfo
            }
        }};
        runtime_.device->updateDescriptorSets(
            static_cast<uint32_t>(writes.size()), writes.data(),
            0, nullptr, runtime_.dld);

        const vk::DescriptorBufferInfo prepareOutput{
            .buffer = *planesA_, .offset = 0, .range = VK_WHOLE_SIZE
        };
        const vk::WriteDescriptorSet prepareWrite{
            .dstSet = prepareSet_,
            .dstBinding = 2,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .pBufferInfo = &prepareOutput
        };
        runtime_.device->updateDescriptorSets(
            1, &prepareWrite, 0, nullptr, runtime_.dld);

        const vk::DescriptorBufferInfo shuffleInput{
            .buffer = *planesA_, .offset = 0, .range = VK_WHOLE_SIZE
        };
        const vk::DescriptorImageInfo outputInfo{
            .imageView = *outputView_,
            .imageLayout = vk::ImageLayout::eGeneral
        };
        const std::array<vk::WriteDescriptorSet, 2> shuffleWrites{{
            {
                .dstSet = shuffleSet_,
                .dstBinding = 0,
                .descriptorCount = 1,
                .descriptorType = vk::DescriptorType::eStorageBuffer,
                .pBufferInfo = &shuffleInput
            },
            {
                .dstSet = shuffleSet_,
                .dstBinding = 1,
                .descriptorCount = 1,
                .descriptorType = vk::DescriptorType::eStorageImage,
                .pImageInfo = &outputInfo
            }
        }};
        runtime_.device->updateDescriptorSets(
            static_cast<uint32_t>(shuffleWrites.size()),
            shuffleWrites.data(), 0, nullptr, runtime_.dld);
    }

    void updatePrepareImage(vk::ImageView input)
    {
        const vk::DescriptorImageInfo inputInfo{
            .imageView = input,
            .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal
        };
        const vk::WriteDescriptorSet write{
            .dstSet = prepareSet_,
            .dstBinding = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eSampledImage,
            .pImageInfo = &inputInfo
        };
        runtime_.device->updateDescriptorSets(
            1, &write, 0, nullptr, runtime_.dld);
    }

    void bufferBarrier(vk::CommandBuffer command)
    {
        const vk::MemoryBarrier barrier{
            .srcAccessMask = vk::AccessFlagBits::eShaderWrite,
            .dstAccessMask = vk::AccessFlagBits::eShaderRead |
                vk::AccessFlagBits::eShaderWrite
        };
        command.pipelineBarrier(
            vk::PipelineStageFlagBits::eComputeShader,
            vk::PipelineStageFlagBits::eComputeShader,
            {}, { barrier }, {}, {}, runtime_.dld);
    }

    void destroyResources()
    {
        resourcesReady_ = false;
        outputView_ = {};
        outputImage_ = {};
        outputMemory_ = {};
        planesB_ = {};
        planesBMemory_ = {};
        planesA_ = {};
        planesAMemory_ = {};
        weightBuffer_ = {};
        weightMemory_ = {};
        initialized_ = false;
    }

    Runtime& runtime_;
    uint32_t channels_{};
    uint32_t blocks_{};
    std::vector<float> weights_;
    std::vector<Layer> layers_;

    vk::UniqueDescriptorSetLayout prepareLayout_;
    vk::UniquePipelineLayout preparePipelineLayout_;
    vk::UniqueDescriptorSetLayout convLayout_;
    vk::UniquePipelineLayout convPipelineLayout_;
    vk::UniqueDescriptorSetLayout shuffleLayout_;
    vk::UniquePipelineLayout shufflePipelineLayout_;
    vk::UniqueDescriptorPool descriptorPool_;

    vk::DescriptorSet prepareSet_{};
    vk::DescriptorSet convAB_{};
    vk::DescriptorSet convBA_{};
    vk::DescriptorSet shuffleSet_{};

    vk::UniquePipeline preparePipeline_;
    vk::UniquePipeline convPipeline_;
    vk::UniquePipeline shufflePipeline_;
    vk::UniqueSampler sampler_;

    vk::UniqueBuffer weightBuffer_;
    vk::UniqueDeviceMemory weightMemory_;
    vk::DeviceSize weightBufferSize_{};

    vk::UniqueBuffer planesA_;
    vk::UniqueDeviceMemory planesAMemory_;
    vk::UniqueBuffer planesB_;
    vk::UniqueDeviceMemory planesBMemory_;

    vk::UniqueImage outputImage_;
    vk::UniqueDeviceMemory outputMemory_;
    vk::UniqueImageView outputView_;

    vk::Extent2D inputExtent_{};
    vk::Extent2D outputExtent_{};
    bool swapRedBlue_{};
    bool initialized_{};
    bool resourcesReady_{};
    bool valid_{true};
    std::string error_;
};

} // namespace

std::unique_ptr<NeuralBackend> createVulkanNeuralBackend(
    Runtime& runtime,
    const std::string& modelPath
) {
    return std::make_unique<VulkanNeuralBackend>(runtime, modelPath);
}

} // namespace openfg::scaler
