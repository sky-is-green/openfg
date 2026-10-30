/* SPDX-License-Identifier: MIT */

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vk>

#include "limits.hpp"

namespace openfg {

/// File descriptors exported by a context. The caller closes them.
struct FileDescriptors {
    /// Two-layer source image (RGBA8 or RGBA16F), layer = iteration % 2.
    int sourceFd;
    /// Three-layer destination image; generated frame i is written to layer i.
    int destinationFd;
    /// External timeline semaphore.
    int syncFd;
};

///
/// Open frame-generation backend.
///
/// It owns a Vulkan device on the selected GPU and exports a two-layer RGBA8
/// source image, a three-layer RGBA8 destination image and an external
/// timeline semaphore. `dispatch(n)` analyses one frame pair; each `acquire()`
/// renders one intermediate frame into the next destination layer.
///
class Instance {
public:
    Instance(
        const std::string& deviceId,
        const std::filesystem::path& shaderDir,
        bool crossGpu,
        bool leakInstance = false,
        std::function<void(const std::string&)> logCallback = nullptr
    );

    Instance(const Instance&) = delete;
    Instance& operator=(const Instance&) = delete;
    Instance(Instance&&) = delete;
    Instance& operator=(Instance&&) = delete;
    ~Instance();

private:
    friend class Context;
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

class Context {
public:
    Context(
        const Instance& instance,
        uint32_t width,
        uint32_t height,
        float flowScale,
        bool performanceMode,
        bool videoSafe = false,
        vk::Format colorFormat = vk::Format::eR8G8B8A8Unorm
    );

    [[nodiscard]] FileDescriptors exportFds() const;
    void dispatch(uint32_t total, bool unsignaled = false);
    void acquire(bool unsignaled = false);
    void idle();

    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
    Context(Context&&) = delete;
    Context& operator=(Context&&) = delete;
    ~Context();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}
