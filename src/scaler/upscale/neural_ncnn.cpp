/* SPDX-License-Identifier: MIT */

#include "neural_ncnn.hpp"

#include <ncnn/net.h>

#include <algorithm>
#include <memory>
#include <stdexcept>

namespace openfg::scaler {

struct NcnnModel {
    ncnn::Net net;
};

NcnnModel* loadNcnnModel(
    const std::string& paramPath,
    std::string& error
) {
    std::string binPath = paramPath;
    if (binPath.size() > 6 && binPath.substr(binPath.size() - 6) == ".param")
        binPath = binPath.substr(0, binPath.size() - 6) + ".bin";
    else
        binPath += ".bin";

    if (ncnn::create_gpu_instance() != 0 || ncnn::get_gpu_count() == 0) {
        error = "ncnn could not initialize a Vulkan device";
        return nullptr;
    }

    auto model = std::make_unique<NcnnModel>();
    model->net.opt.use_vulkan_compute = true;
    model->net.set_vulkan_device(0);
    model->net.opt.use_fp16_packed = true;
    model->net.opt.use_fp16_storage = true;
    model->net.opt.use_fp16_arithmetic = true;
    if (model->net.load_param(paramPath.c_str()) != 0) {
        error = "cannot load ncnn param " + paramPath;
        return nullptr;
    }
    if (model->net.load_model(binPath.c_str()) != 0) {
        error = "cannot load ncnn weights " + binPath;
        return nullptr;
    }
    return model.release();
}

void destroyNcnnModel(NcnnModel* model)
{
    delete model;
}

bool runNcnnModel(
    NcnnModel& model,
    int width,
    int height,
    const uint8_t* pixels,
    bool bgr,
    int& outputWidth,
    int& outputHeight,
    std::vector<uint8_t>& output,
    std::string& error
) {
    if (!pixels || width <= 0 || height <= 0) {
        error = "neural input is empty";
        return false;
    }

    ncnn::Mat image(width, height, 3);
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            const auto* p = pixels +
                (static_cast<size_t>(y) * width + x) * 4;
            image.channel(0).row(y)[x] = p[bgr ? 2 : 0] / 255.0F;
            image.channel(1).row(y)[x] = p[1] / 255.0F;
            image.channel(2).row(y)[x] = p[bgr ? 0 : 2] / 255.0F;
        }
    }

    ncnn::Extractor extractor = model.net.create_extractor();
    if (extractor.input("low", image) != 0) {
        error = "ncnn rejected the input";
        return false;
    }
    ncnn::Mat result;
    if (extractor.extract("high", result) != 0) {
        error = "ncnn inference failed";
        return false;
    }
    if (result.c != 3 || result.w <= 0 || result.h <= 0) {
        error = "ncnn returned an unexpected output shape";
        return false;
    }
    outputWidth = result.w;
    outputHeight = result.h;
    output.resize(static_cast<size_t>(4) * result.w * result.h);
    auto clamp = [](float value) {
        return static_cast<uint8_t>(std::clamp(
            value, 0.0F, 1.0F) * 255.0F + 0.5F);
    };
    for (int y = 0; y < result.h; y++) {
        for (int x = 0; x < result.w; x++) {
            const size_t index = static_cast<size_t>(y) * result.w + x;
            uint8_t* p = output.data() + index * 4;
            p[0] = clamp(result.channel(0).row(y)[x]);
            p[1] = clamp(result.channel(1).row(y)[x]);
            p[2] = clamp(result.channel(2).row(y)[x]);
            p[3] = 255;
        }
    }
    return true;
}

} // namespace openfg::scaler
