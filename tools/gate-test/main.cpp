/* SPDX-License-Identifier: MIT */
// Standalone Vulkan gate-feature extractor for the 31-feature test vector.

#include "scaler/gate/gate_features.hpp"
#include "scaler/vulkan.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr std::array<const char*, 31> kFeatureNames = {
    "low_luma_mean", "low_luma_std", "low_luma_p10", "low_luma_p90",
    "low_grad_mean", "low_grad_std", "low_grad_p90", "low_edge_density",
    "low_lap_var", "low_hf_mean", "low_hf_p90",
    "low_local_std_mean", "low_local_std_std",
    "low_color_std_mean", "low_sat_mean", "low_sat_std",
    "low_dark_frac", "low_bright_frac", "low_grad_anisotropy",
    "easu_luma_mean", "easu_luma_std", "easu_grad_mean", "easu_grad_p90",
    "easu_lap_var", "easu_hf_mean", "easu_hf_p90",
    "easu_bicubic_diff_mean", "easu_bicubic_diff_p90",
    "easu_bicubic_diff_std", "easu_bicubic_grad_mean",
    "easu_clip_frac",
};

std::vector<uint8_t> readFile(const std::string& path, size_t expected)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        throw std::runtime_error("cannot open " + path);
    file.seekg(0, std::ios::end);
    const auto size = static_cast<size_t>(file.tellg());
    file.seekg(0, std::ios::beg);
    if (expected != 0 && size != expected)
        throw std::runtime_error(
            path + ": expected " + std::to_string(expected) +
            " bytes, got " + std::to_string(size));
    std::vector<uint8_t> data(size);
    file.read(reinterpret_cast<char*>(data.data()),
        static_cast<std::streamsize>(size));
    return data;
}

} // namespace

int main(int argc, char** argv)
{
    std::string lowPath;
    std::string easuPath;
    std::string outPath;
    std::string gpu;
    uint32_t width{};
    uint32_t height{};
    uint32_t outWidth{};
    uint32_t outHeight{};
    uint32_t tile = 128;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto next = [&]() -> std::string {
            if (i + 1 >= argc)
                throw std::runtime_error("missing value for " + arg);
            return argv[++i];
        };
        if (arg == "--low") lowPath = next();
        else if (arg == "--easu") easuPath = next();
        else if (arg == "--out") outPath = next();
        else if (arg == "--gpu") gpu = next();
        else if (arg == "--width") width = std::stoul(next());
        else if (arg == "--height") height = std::stoul(next());
        else if (arg == "--out-width") outWidth = std::stoul(next());
        else if (arg == "--out-height") outHeight = std::stoul(next());
        else if (arg == "--tile") tile = std::stoul(next());
        else if (arg == "--help") {
            std::cout
                << "usage: openfg-gate-test --low low.rgba --easu easu.rgba "
                   "--width W --height H [--out-width OW --out-height OH] "
                   "[--tile N] --out features.csv\n";
            return 0;
        } else {
            throw std::runtime_error("unknown argument: " + arg);
        }
    }
    if (lowPath.empty() || easuPath.empty() || outPath.empty() ||
        width == 0 || height == 0)
        throw std::runtime_error("missing required arguments; use --help");
    if (outWidth == 0) outWidth = width * 2;
    if (outHeight == 0) outHeight = height * 2;

    try {
        const auto low = readFile(
            lowPath, static_cast<size_t>(width) * height * 4);
        const auto easu = readFile(
            easuPath, static_cast<size_t>(outWidth) * outHeight * 4);
        const uint32_t tileCount =
            ((width + tile - 1) / tile) * ((height + tile - 1) / tile);

        auto runtime = openfg::scaler::createRuntime(
            "openfg-gate-test",
            std::span<const char* const>{},
            std::span<const char* const>{},
            gpu);

        std::vector<float> features;
        std::vector<openfg::scaler::GateTile> tiles;
        std::string error;
        double gpuMilliseconds = 0.0;
        const auto started = std::chrono::steady_clock::now();
        if (!openfg::scaler::extractGateFeatures(
                *runtime, low.data(), width, height, easu.data(),
                outWidth, outHeight, tile, features, tiles, error,
                &gpuMilliseconds))
            throw std::runtime_error(error);

        std::ofstream out(outPath);
        if (!out)
            throw std::runtime_error("cannot write " + outPath);
        out << "x,y,w,h";
        for (const auto* name : kFeatureNames)
            out << ',' << name;
        out << '\n';
        out << std::setprecision(9);
        for (size_t i = 0; i < tiles.size(); ++i) {
            const auto& t = tiles[i];
            out << t.x << ',' << t.y << ',' << t.width << ','
                << t.height;
            for (uint32_t f = 0; f < 31; ++f)
                out << ',' << features[i * 31 + f];
            out << '\n';
        }
        const auto elapsed = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count();
        std::cout << tiles.size() << " tiles/" << tileCount
                  << " expected, " << elapsed << " ms wall, "
                  << gpuMilliseconds << " ms GPU\n";
        std::cout << "device: " << runtime->deviceName << "\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "openfg-gate-test: " << exception.what() << "\n";
        return 1;
    }
}
