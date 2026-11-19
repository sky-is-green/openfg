/* SPDX-License-Identifier: MIT */
// openfg-scaler: capture, upscale and present the companion scaler pipeline.

#include "scaler/capture/test_pattern.hpp"
#include "scaler/present/presenter.hpp"
#include "scaler/scaler.hpp"
#include "scaler/upscale/easu.hpp"
#include "scaler/vulkan.hpp"

#ifdef OPENFG_SCALER_HAVE_PIPEWIRE
#include "scaler/capture/pipewire.hpp"
#endif

#include "vkutil/vkutil.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace openfg::scaler;

namespace {

struct Options {
    std::string source{"test"};
    std::string capture{"monitor"};
    vk::Extent2D input{1280, 720};
    vk::Extent2D output{};
    vk::Rect2D crop{};
    bool cropSet{false};
    uint32_t scale{2};
    std::string filter{"easu+rcas"};
    float sharpness{4.0F};
    uint64_t frames{0};
    bool headless{false};
    bool fullscreen{false};
    bool vsync{true};
    bool presentTiming{false};
    bool useDmaBuf{true};
    bool animate{true};
    std::string gpu{"auto"};
    std::string dump;
    std::string dumpInput;
    uint32_t stats{0};
    bool help{false};
    bool version{false};
};

void usage(const char* argv0)
{
    std::cout
        << "usage: " << argv0 << " [options]\n\n"
        << "Sources:\n"
        << "  --source test|pipewire   capture backend (default: test)\n"
        << "  --capture monitor|window  PipeWire source type (default: monitor)\n"
        << "  --input WxH              source resolution (default: 1280x720)\n"
        << "  --crop WxH+X+Y           crop the PipeWire capture\n"
        << "  --static                 freeze the test pattern\n\n"
        << "Output:\n"
        << "  --output WxH             upscaled resolution (default: input x scale)\n"
        << "  --scale N                output = input * N (default: 2)\n"
        << "  --filter easu|easu+rcas  spatial filter (default: easu+rcas)\n"
        << "  --sharpness N            RCAS sharpness 0..20 (default: 4; 0=max)\n"
        << "  --fullscreen             present fullscreen instead of a window\n"
        << "  --windowed               force a window (default)\n"
        << "  --vsync on|off           FIFO or immediate/mailbox (default: on)\n"
        << "  --present-timing         diagnostic display wait; reduces throughput\n"
        << "  --no-dmabuf              copy PipeWire frames through shared memory\n"
        << "  --frames N               stop after N frames (0 = until quit)\n"
        << "  --headless               render offscreen, no window\n"
        << "  --dump PATH              write the last headless frame as RGBA8\n"
        << "  --dump-input PATH        write the first source frame as RGBA8\n"
        << "  --stats N                print progress every N frames (0 = off)\n"
        << "  --gpu auto|PCI|name      Vulkan device selection\n"
        << "  --help                   show this text\n"
        << "  --version                print the version and enabled backends\n\n"
        << "Examples:\n"
        << "  " << argv0 << " --source test --windowed --frames 120\n"
        << "  " << argv0 << " --headless --frames 1 --dump frame.rgba\n"
        << "  " << argv0 << " --source pipewire --capture window \\\n"
        << "      --input 1280x720 --output 2560x1440 --fullscreen\n\n"
        << "PipeWire capture uses the desktop ScreenCast portal and requests a\n"
        << "DMA-BUF stream. It never records frames to disk unless --dump-input\n"
        << "is explicitly set.\n";
}

std::string_view nextArg(int argc, char** argv, int& index, const char* name)
{
    if (index + 1 >= argc)
        throw std::runtime_error(std::string(name) + " needs a value");
    return argv[++index];
}

vk::Extent2D parseExtent(std::string_view text)
{
    const auto position = text.find('x');
    if (position == std::string_view::npos)
        throw std::runtime_error(
            "expected a size like 1280x720, got " + std::string(text));
    const auto width = std::stoul(std::string(text.substr(0, position)));
    const auto height = std::stoul(std::string(text.substr(position + 1)));
    if (width == 0 || height == 0 || width > 16384 || height > 16384)
        throw std::runtime_error("size is out of range");
    return {
        static_cast<uint32_t>(width),
        static_cast<uint32_t>(height)
    };
}

bool parseBool(std::string_view text)
{
    if (text == "on" || text == "true" || text == "1")
        return true;
    if (text == "off" || text == "false" || text == "0")
        return false;
    throw std::runtime_error("expected on or off, got " + std::string(text));
}

Options parseOptions(int argc, char** argv)
{
    Options options;
    for (int index = 1; index < argc; index++) {
        const std::string_view argument{argv[index]};
        if (argument == "--help" || argument == "-h") {
            options.help = true;
        } else if (argument == "--version") {
            options.version = true;
        } else if (argument == "--source") {
            options.source = std::string(nextArg(argc, argv, index, "--source"));
        } else if (argument == "--capture") {
            options.capture = std::string(nextArg(argc, argv, index, "--capture"));
        } else if (argument == "--input") {
            options.input = parseExtent(nextArg(argc, argv, index, "--input"));
        } else if (argument == "--output") {
            options.output = parseExtent(nextArg(argc, argv, index, "--output"));
        } else if (argument == "--crop") {
            const auto text = nextArg(argc, argv, index, "--crop");
            const auto first = text.find('x');
            const auto second = text.find('+', first == std::string_view::npos
                ? 0 : first);
            const auto third = second == std::string_view::npos
                ? std::string_view::npos : text.find('+', second + 1);
            if (first == std::string_view::npos ||
                second == std::string_view::npos ||
                third == std::string_view::npos)
                throw std::runtime_error(
                    "--crop expects WxH+X+Y, for example 1280x720+0+28");
            options.crop = vk::Rect2D{
                .offset = {
                    static_cast<int32_t>(std::stoul(std::string(
                        text.substr(second + 1, third - second - 1)))),
                    static_cast<int32_t>(std::stoul(std::string(
                        text.substr(third + 1))))
                },
                .extent = parseExtent(text.substr(0, second))
            };
            options.cropSet = true;
        } else if (argument == "--scale") {
            options.scale = static_cast<uint32_t>(
                std::stoul(std::string(nextArg(argc, argv, index, "--scale"))));
        } else if (argument == "--filter") {
            options.filter = std::string(nextArg(argc, argv, index, "--filter"));
        } else if (argument == "--sharpness") {
            options.sharpness = std::stof(
                std::string(nextArg(argc, argv, index, "--sharpness")));
        } else if (argument == "--frames") {
            options.frames = std::stoull(
                std::string(nextArg(argc, argv, index, "--frames")));
        } else if (argument == "--headless") {
            options.headless = true;
        } else if (argument == "--fullscreen") {
            options.fullscreen = true;
        } else if (argument == "--windowed") {
            options.fullscreen = false;
        } else if (argument == "--vsync") {
            options.vsync = parseBool(
                nextArg(argc, argv, index, "--vsync"));
        } else if (argument == "--present-timing") {
            options.presentTiming = true;
        } else if (argument == "--no-dmabuf") {
            options.useDmaBuf = false;
        } else if (argument == "--static") {
            options.animate = false;
        } else if (argument == "--gpu") {
            options.gpu = std::string(nextArg(argc, argv, index, "--gpu"));
        } else if (argument == "--dump") {
            options.dump = std::string(nextArg(argc, argv, index, "--dump"));
        } else if (argument == "--dump-input") {
            options.dumpInput =
                std::string(nextArg(argc, argv, index, "--dump-input"));
        } else if (argument == "--stats") {
            options.stats = static_cast<uint32_t>(
                std::stoul(std::string(nextArg(argc, argv, index, "--stats"))));
        } else {
            throw std::runtime_error(
                "unknown option " + std::string(argument));
        }
    }

    if (options.source != "test" && options.source != "pipewire")
        throw std::runtime_error(
            "--source must be test or pipewire");
    if (options.capture != "monitor" && options.capture != "window")
        throw std::runtime_error(
            "--capture must be monitor or window");
    if (options.filter != "easu" && options.filter != "easu+rcas")
        throw std::runtime_error(
            "--filter must be easu or easu+rcas");
    if (options.cropSet && options.source != "pipewire")
        throw std::runtime_error(
            "--crop requires --source pipewire");
    if (options.scale < 1 || options.scale > 4)
        throw std::runtime_error("--scale must be 1..4");
    if (options.sharpness < 0.0F || options.sharpness > 20.0F)
        throw std::runtime_error("--sharpness must be 0..20");

    if (options.output.width == 0 || options.output.height == 0)
        options.output = vk::Extent2D{
            options.input.width * options.scale,
            options.input.height * options.scale
        };
    if (options.headless && options.frames == 0)
        options.frames = 1;
    if (options.presentTiming && options.headless)
        throw std::runtime_error(
            "--present-timing requires a window, not --headless");
    return options;
}

void writeFile(const std::string& path, const std::string& data)
{
    std::ofstream file{path, std::ios::binary | std::ios::trunc};
    if (!file)
        throw std::runtime_error("cannot open " + path);
    file.write(data.data(), static_cast<std::streamsize>(data.size()));
    if (!file)
        throw std::runtime_error("cannot write " + path);
}

double percentile(std::vector<double> values, double fraction)
{
    if (values.empty())
        return 0.0;
    std::sort(values.begin(), values.end());
    const size_t index = std::min(
        values.size() - 1,
        static_cast<size_t>(fraction * static_cast<double>(values.size())));
    return values[index];
}

} // namespace

int main(int argc, char** argv)
{
    Options options;
    try {
        options = parseOptions(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "openfg-scaler: " << error.what() << "\n";
        return EXIT_FAILURE;
    }
    if (options.help) {
        usage(argv[0]);
        return EXIT_SUCCESS;
    }
    if (options.version) {
        std::cout << "openfg-scaler 0.1.0\n"
                  << "backends: test";
#ifdef OPENFG_SCALER_HAVE_SDL3
        std::cout << ", sdl3";
#endif
#ifdef OPENFG_SCALER_HAVE_PIPEWIRE
        std::cout << ", pipewire";
#endif
        std::cout << "\n";
        return EXIT_SUCCESS;
    }

    try {
        PresentOptions presentOptions{
            .size = (options.headless || options.fullscreen)
                ? options.output
                : vk::Extent2D{1280, 720},
            .fullscreen = options.fullscreen,
            .vsync = options.vsync,
            .presentTiming = options.presentTiming,
            .headless = options.headless,
            .hidden = options.source == "pipewire",
            .title = "OpenFG Scaler"
        };
        std::unique_ptr<Runtime> runtime;
        auto presenter = createPresenter(presentOptions);

        std::vector<const char*> instanceExtensions =
            presenter->instanceExtensions();
        std::vector<const char*> deviceExtensions;
        if (presenter->wantsSurface())
            deviceExtensions.push_back(vk::KHRSwapchainExtensionName);
        if (options.presentTiming) {
            deviceExtensions.push_back(vk::KHRPresentIdExtensionName);
            deviceExtensions.push_back(vk::KHRPresentWaitExtensionName);
        }
        if (options.source == "pipewire") {
#ifdef OPENFG_SCALER_HAVE_PIPEWIRE
            deviceExtensions.push_back(vk::EXTExternalMemoryDmaBufExtensionName);
            deviceExtensions.push_back(
                vk::EXTImageDrmFormatModifierExtensionName);
#else
            throw std::runtime_error(
                "this build has no PipeWire capture support; use --source test");
#endif
        }

        SurfaceFactory surfaceFactory;
        if (presenter->wantsSurface()) {
            surfaceFactory = [&presenter](
                vk::Instance instance, vk::SurfaceKHR& surface) {
                return presenter->createSurface(instance, surface);
            };
        }

        runtime = createRuntime(
            "openfg-scaler", instanceExtensions, deviceExtensions,
            options.gpu, surfaceFactory, options.presentTiming);
        std::string error;
        if (!presenter->init(*runtime, error))
            throw std::runtime_error(error);

        std::unique_ptr<CaptureSource> source;
        if (options.source == "test") {
            source = std::make_unique<TestPatternSource>(
                *runtime, options.input, options.animate);
        } else {
#ifdef OPENFG_SCALER_HAVE_PIPEWIRE
            source = std::make_unique<PipeWireSource>(
                *runtime, options.input, options.capture,
                options.crop, options.cropSet, options.useDmaBuf);
#else
            throw std::runtime_error("PipeWire support is not built");
#endif
        }
        if (!source->start(error))
            throw std::runtime_error(error);
        presenter->show();

        auto upscaler = std::make_unique<EasuUpscaler>(
            *runtime, options.input, options.output,
            options.filter == "easu+rcas", options.sharpness);
        auto command = vkutil::createCommandBuffer(
            runtime->dld, *runtime->device, *runtime->commandPool);

        const auto queueProperties =
            runtime->physical.getQueueFamilyProperties(runtime->dld);
        const bool gpuTiming = queueProperties[runtime->queueFamily]
            .timestampValidBits != 0;
        vk::UniqueQueryPool queryPool;
        if (gpuTiming) {
            queryPool = runtime->device->createQueryPoolUnique({
                .queryType = vk::QueryType::eTimestamp,
                .queryCount = 4
            }, nullptr, runtime->dld);
        }

        std::cout << "device: " << runtime->deviceName << "\n"
                  << "source: " << source->description() << "\n"
                  << "output: " << options.output.width << "x"
                  << options.output.height << "\n"
                  << "filter: " << options.filter << "\n"
                  << "present: " << (options.headless ? "headless"
                      : options.fullscreen ? "fullscreen" : "window")
                  << "\n";

        const auto started = std::chrono::steady_clock::now();
        std::vector<double> frameTimes;
        std::vector<double> captureLatencies;
        std::vector<double> captureWaits;
        std::vector<double> acquireTimes;
        std::vector<double> submitTimes;
        std::vector<double> sourceIntervals;
        uint64_t lastSourceTimestamp = 0;
        std::vector<double> upscaleGpuTimes;
        std::vector<double> presentGpuTimes;
        uint64_t frameCount = 0;
        bool dumpedInput = false;

        while (!presenter->shouldClose() &&
               (options.frames == 0 || frameCount < options.frames)) {
            presenter->pollEvents();
            const auto frameStart = std::chrono::steady_clock::now();

            Frame frame;
            if (!source->next(frame, error))
                throw std::runtime_error(error);
            const auto afterCapture = std::chrono::steady_clock::now();
            captureWaits.push_back(
                std::chrono::duration<double, std::milli>(
                    afterCapture - frameStart).count());
            if (lastSourceTimestamp != 0 &&
                frame.timestampNs > lastSourceTimestamp)
                sourceIntervals.push_back(
                    static_cast<double>(
                        frame.timestampNs - lastSourceTimestamp) / 1.0e6);
            lastSourceTimestamp = frame.timestampNs;

            if (!options.dumpInput.empty() && !dumpedInput) {
                std::string data;
                if (!readbackRgba8(*runtime, frame.image, frame.extent,
                        vk::ImageLayout::eShaderReadOnlyOptimal, data, error))
                    throw std::runtime_error(error);
                writeFile(options.dumpInput, data);
                dumpedInput = true;
            }

            upscaler->resize(frame.extent, options.output);

            PresentTarget target;
            if (!presenter->acquire(target, error))
                throw std::runtime_error(error);
            (void)target;
            const auto afterAcquire = std::chrono::steady_clock::now();
            acquireTimes.push_back(
                std::chrono::duration<double, std::milli>(
                    afterAcquire - afterCapture).count());

            command->reset({}, runtime->dld);
            command->begin(
                { .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit },
                runtime->dld);
            if (gpuTiming) {
                command->resetQueryPool(*queryPool, 0, 4, runtime->dld);
                command->writeTimestamp(vk::PipelineStageFlagBits::eTopOfPipe,
                    *queryPool, 0, runtime->dld);
            }
            upscaler->record(*command, frame.view, frame.extent);
            if (gpuTiming)
                command->writeTimestamp(
                    vk::PipelineStageFlagBits::eComputeShader,
                    *queryPool, 1, runtime->dld);
            if (gpuTiming)
                command->writeTimestamp(
                    vk::PipelineStageFlagBits::eComputeShader,
                    *queryPool, 2, runtime->dld);
            presenter->record(
                *runtime, *command, upscaler->view(), upscaler->outputExtent());
            if (gpuTiming)
                command->writeTimestamp(
                    vk::PipelineStageFlagBits::eBottomOfPipe,
                    *queryPool, 3, runtime->dld);
            if (!presenter->submit(*command, error))
                throw std::runtime_error(error);
            const auto afterSubmit = std::chrono::steady_clock::now();
            submitTimes.push_back(
                std::chrono::duration<double, std::milli>(
                    afterSubmit - afterAcquire).count());
            if (gpuTiming) {
                std::array<uint64_t, 4> timestamps{};
                if (runtime->device->getQueryPoolResults(*queryPool, 0, 4,
                        sizeof(timestamps), timestamps.data(),
                        sizeof(uint64_t),
                        vk::QueryResultFlagBits::e64, runtime->dld) ==
                    vk::Result::eSuccess) {
                    const double period =
                        static_cast<double>(runtime->timestampPeriod);
                    upscaleGpuTimes.push_back(
                        static_cast<double>(timestamps[1] - timestamps[0]) *
                        period / 1.0e6);
                    presentGpuTimes.push_back(
                        static_cast<double>(timestamps[3] - timestamps[2]) *
                        period / 1.0e6);
                }
            }

            const auto end = std::chrono::steady_clock::now();
            frameTimes.push_back(
                std::chrono::duration<double, std::milli>(end - frameStart)
                    .count());
            const uint64_t endNs = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    end.time_since_epoch()).count());
            if (frame.timestampNs != 0 && endNs > frame.timestampNs)
                captureLatencies.push_back(
                    static_cast<double>(endNs - frame.timestampNs) / 1.0e6);
            frameCount++;

            if (options.stats != 0 && frameCount % options.stats == 0) {
                const double elapsed =
                    std::chrono::duration<double>(end - started).count();
                std::cerr << "frames: " << frameCount
                          << "  fps: " << static_cast<double>(frameCount) /
                              std::max(0.001, elapsed)
                          << "  cpu: " << frameTimes.back() << " ms\n";
            }
        }

        source->stop();
        const auto ended = std::chrono::steady_clock::now();
        const double elapsed =
            std::chrono::duration<double>(ended - started).count();
        std::cout << "frames: " << frameCount << "\n"
                  << "time: " << elapsed << " s\n"
                  << "fps: " << static_cast<double>(frameCount) /
                      std::max(0.001, elapsed) << "\n";
        if (!frameTimes.empty()) {
            const double total = std::accumulate(
                frameTimes.begin(), frameTimes.end(), 0.0);
            std::cout << "cpu: " << total /
                    static_cast<double>(frameTimes.size())
                      << " ms avg, " << percentile(frameTimes, 0.50)
                      << " p50, " << percentile(frameTimes, 0.95)
                      << " p95\n";
        }
        if (!captureWaits.empty()) {
            std::cout << "capture wait: "
                      << percentile(captureWaits, 0.50) << " p50, "
                      << percentile(captureWaits, 0.95) << " p95; "
                      << "acquire: " << percentile(acquireTimes, 0.50)
                      << " p50, " << percentile(acquireTimes, 0.95)
                      << " p95; submit: " << percentile(submitTimes, 0.50)
                      << " p50, " << percentile(submitTimes, 0.95)
                      << " p95\n";
            if (!sourceIntervals.empty())
                std::cout << "source interval: "
                          << percentile(sourceIntervals, 0.50) << " p50, "
                          << percentile(sourceIntervals, 0.95) << " p95\n";
        }
        if (!captureLatencies.empty()) {
            const double total = std::accumulate(
                captureLatencies.begin(), captureLatencies.end(), 0.0);
            std::cout << (options.presentTiming
                    ? "capture to display: "
                    : "capture to present: ")
                      << total / static_cast<double>(captureLatencies.size())
                      << " ms avg, " << percentile(captureLatencies, 0.50)
                      << " p50, " << percentile(captureLatencies, 0.95)
                      << " p95\n";
        }
        if (!upscaleGpuTimes.empty()) {
            const double upscale = std::accumulate(
                upscaleGpuTimes.begin(), upscaleGpuTimes.end(), 0.0) /
                static_cast<double>(upscaleGpuTimes.size());
            const double present = std::accumulate(
                presentGpuTimes.begin(), presentGpuTimes.end(), 0.0) /
                static_cast<double>(presentGpuTimes.size());
            std::cout << "gpu upscale: " << upscale << " ms avg, "
                      << percentile(upscaleGpuTimes, 0.50) << " p50, "
                      << percentile(upscaleGpuTimes, 0.95) << " p95\n"
                      << "gpu present: " << present << " ms avg, "
                      << percentile(presentGpuTimes, 0.50) << " p50, "
                      << percentile(presentGpuTimes, 0.95) << " p95\n";
        }

        if (!options.dump.empty()) {
            if (!presenter->saveFrame(options.dump, error))
                throw std::runtime_error(error);
            std::cout << "wrote " << options.dump << "\n";
        }

        upscaler.reset();
        source.reset();
        queryPool.reset();
        command.reset();
        presenter.reset();
        runtime.reset();
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "openfg-scaler: " << error.what() << "\n";
        return EXIT_FAILURE;
    }
}
