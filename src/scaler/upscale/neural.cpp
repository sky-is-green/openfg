/* SPDX-License-Identifier: MIT */

#include "neural.hpp"
#include "neural_backend.hpp"

#include <filesystem>
#include <stdexcept>
#include <string>

namespace openfg::scaler {

#ifndef OPENFG_SCALER_HAVE_NCNN
std::unique_ptr<NeuralBackend> createNcnnNeuralBackend(
    Runtime&,
    const std::string&,
    std::string& error
) {
    error = "this build has no ncnn support";
    return nullptr;
}
#endif

struct NeuralUpscaler::Impl {
    std::unique_ptr<NeuralBackend> backend;

    Impl(Runtime& runtime, const std::string& modelPath)
    {
        std::filesystem::path path(modelPath);
        std::error_code error;
        if (std::filesystem::is_directory(path, error))
            path /= "model.ofg";

        if (path.extension() == ".param") {
            std::string message;
            backend = createNcnnNeuralBackend(
                runtime, path.string(), message);
            if (!backend) {
                if (message.empty())
                    message = "this build has no ncnn support";
                throw std::runtime_error(
                    message + "; convert the package with "
                    "tools/model-ofg.py and use the .ofg model instead");
            }
        } else {
            backend = createVulkanNeuralBackend(runtime, path.string());
        }
    }
};

NeuralUpscaler::NeuralUpscaler(Runtime& runtime, std::string modelPath)
    : impl_{std::make_unique<Impl>(runtime, modelPath)}
{
}

NeuralUpscaler::~NeuralUpscaler() = default;

void NeuralUpscaler::resize(vk::Extent2D input, vk::Extent2D output)
{
    impl_->backend->resize(input, output);
}

bool NeuralUpscaler::prepare(
    Runtime& runtime,
    const Frame& frame,
    std::string& error
)
{
    return impl_->backend->prepare(runtime, frame, error);
}

void NeuralUpscaler::record(
    vk::CommandBuffer command,
    vk::ImageView input,
    vk::Extent2D inputExtent
)
{
    impl_->backend->record(command, input, inputExtent);
}

vk::ImageView NeuralUpscaler::view() const
{
    return impl_->backend->view();
}

vk::Extent2D NeuralUpscaler::outputExtent() const
{
    return impl_->backend->outputExtent();
}

bool NeuralUpscaler::gpuTimed() const
{
    return impl_->backend->gpuTimed();
}

} // namespace openfg::scaler
