/* SPDX-License-Identifier: MIT */

// Small dependency-free checks for the config parser and the [video] profile.

#include "config/config.hpp"

#include <cstdio>
#include <string>

namespace {

    int failures{0};

    void expect(bool condition, const char* message) {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }

    constexpr const char* CONFIG = R"(
[openfg]
enabled = true
gpu = "auto"
cross_gpu = false
multiplier = 2
target_fps = 120
performance_mode = false
debug = false

[processes]
include = ["game.exe"]
exclude = ["blocked.exe"]

[video]
processes = ["mpv"]
multiplier = 4
target_fps = 0
performance_mode = false
safe = true
)";

}

int main() {
    using namespace openfg::config;

    const auto parsed = parse(CONFIG, "config-test.toml");
    expect(parsed.error.empty(), "the sample config parses");
    if (!parsed.error.empty())
        return 1;

    expect(parsed.config.video.processes.size() == 1, "video processes parsed");
    expect(parsed.config.video.multiplier == 4, "video multiplier parsed");
    expect(parsed.config.video.safe, "video safe parsed");
    expect(parsed.config.multiplier == 2, "global multiplier stays separate");

    const auto video = withVideoProfile(parsed.config, { "mpv" });
    expect(video.multiplier == 4, "video profile overrides the multiplier");
    expect(video.targetFps == 0, "video profile overrides target_fps");
    expect(video.videoSafe, "video profile enables safe mode");

    const auto game = withVideoProfile(parsed.config, { "game.exe" });
    expect(game.multiplier == 2, "games keep the global multiplier");
    expect(!game.videoSafe, "games do not get the cut guard");

    expect(matchesName(parsed.config, "game.exe"), "include matches a game");
    expect(matchesName(parsed.config, "mpv"),
        "video.processes also activates the layer");
    expect(!matchesName(parsed.config, "blocked.exe"), "exclude wins");
    expect(!matchesName(parsed.config, "other.exe"), "unlisted process does not match");

    const auto roundTrip = parse(toToml(video), "round-trip.toml");
    expect(roundTrip.error.empty(), "toToml output parses");
    expect(roundTrip.config.video.multiplier == 4, "round-trip keeps the video multiplier");
    expect(roundTrip.config.video.safe, "round-trip keeps the cut guard");

    std::string broken{CONFIG};
    const auto position = broken.find("multiplier = 4");
    broken.replace(position, std::string("multiplier = 4").size(), "multiplier = 5");
    const auto invalid = parse(broken, "invalid.toml");
    expect(!invalid.error.empty(), "an invalid video multiplier is rejected");

    if (failures == 0)
        std::printf("config checks passed\n");
    return failures == 0 ? 0 : 1;
}
