/* SPDX-License-Identifier: MIT */

#include "config/config.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

    void usage() {
        std::fprintf(stderr,
            "usage: openfg-cli <command> [path]\n"
            "\n"
            "  validate [path]  check the config file (default: %s)\n"
            "  show [path]      print the effective settings\n"
            "  test <name>...   test process names against include/exclude\n"
            "  path             print the config path\n"
            "  init [--force]   write the default config file\n"
            "\n"
            "OPENFG_CONFIG overrides the path. OPENFG_* environment variables\n"
            "override individual settings.\n",
            openfg::config::defaultPath().c_str());
    }

    std::string join(const std::vector<std::string>& values) {
        std::string result;
        for (size_t i = 0; i < values.size(); i++) {
            if (i)
                result += ", ";
            result += values[i];
        }
        return result;
    }

    int validate(const std::filesystem::path& path) {
        const auto result = openfg::config::load(path);
        for (const auto& warning : result.warnings)
            std::fprintf(stderr, "warning: %s\n", warning.c_str());

        if (!result.found) {
            std::printf("no config at %s; defaults are valid\n", result.path.c_str());
            return 0;
        }
        if (!result.error.empty()) {
            std::fprintf(stderr, "error: %s\n", result.error.c_str());
            return 1;
        }
        std::printf("ok: %s\n", result.path.c_str());
        return 0;
    }

    int testNames(const std::vector<std::string>& names) {
        const auto result = openfg::config::load();
        for (const auto& warning : result.warnings)
            std::fprintf(stderr, "warning: %s\n", warning.c_str());
        if (!result.error.empty()) {
            std::fprintf(stderr, "error: %s\n", result.error.c_str());
            return 1;
        }
        for (const auto& name : names) {
            const bool matches = openfg::config::matchesName(result.config, name);
            const auto effective = openfg::config::withVideoProfile(
                result.config, { name });
            std::printf("%s: %s", name.c_str(), matches ? "yes" : "no");
            if (matches && effective.videoSafe)
                std::printf(" (video profile: %ux, safe)",
                    effective.multiplier);
            else if (matches && !result.config.video.processes.empty())
                std::printf(" (%ux)", effective.multiplier);
            std::printf("\n");
        }
        return 0;
    }

    int show(const std::filesystem::path& path) {
        const auto result = openfg::config::load(path);
        for (const auto& warning : result.warnings)
            std::fprintf(stderr, "warning: %s\n", warning.c_str());
        if (!result.error.empty()) {
            std::fprintf(stderr, "error: %s\n", result.error.c_str());
            return 1;
        }

        std::printf("# source: %s%s\n",
            result.path.c_str(), result.found ? "" : " (missing, defaults)");
        const std::string toml = openfg::config::toToml(result.config);
        std::fwrite(toml.data(), 1, toml.size(), stdout);
        if (result.found)
            std::printf("\n# config: %s\n", result.path.c_str());

        const auto names = openfg::config::processNames();
        std::printf("\n# process names: %s\n", join(names).c_str());
        std::printf("# this process matches: %s\n",
            openfg::config::processMatches(result.config) ? "yes" : "no");
        const auto effective = openfg::config::withVideoProfile(
            result.config, names);
        std::string presentedCap{"off"};
        if (effective.maxPresentedFps < 0)
            presentedCap = "auto (display refresh)";
        else if (effective.maxPresentedFps > 0)
            presentedCap = std::to_string(effective.maxPresentedFps) + " fps";
        std::printf("# effective: %ux, target %u fps, max presented %s, "
            "performance %s, safe %s\n",
            effective.multiplier, effective.targetFps, presentedCap.c_str(),
            effective.performanceMode ? "yes" : "no",
            effective.videoSafe ? "yes" : "no");
        return 0;
    }

    int init(const std::filesystem::path& path, bool force) {
        std::error_code ec;
        if (!force && std::filesystem::exists(path, ec)) {
            std::fprintf(stderr, "error: %s already exists (use --force to replace)\n",
                path.c_str());
            return 1;
        }
        if (path.has_parent_path())
            std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) {
            std::fprintf(stderr, "error: cannot create %s: %s\n",
                path.parent_path().c_str(), ec.message().c_str());
            return 1;
        }

        std::ofstream file(path, std::ios::trunc);
        if (!file.is_open()) {
            std::fprintf(stderr, "error: cannot write %s\n", path.c_str());
            return 1;
        }
        file << openfg::config::defaultFile();
        if (!file.good()) {
            std::fprintf(stderr, "error: failed writing %s\n", path.c_str());
            return 1;
        }
        std::printf("wrote %s\n", path.c_str());
        return 0;
    }

}

int main(int argc, char** argv) {
    if (argc < 2) {
        usage();
        return 2;
    }

    std::vector<std::string> args(argv + 1, argv + argc);
    const std::string command = args.front();
    args.erase(args.begin());

    if (command == "path") {
        std::printf("%s\n", openfg::config::defaultPath().c_str());
        return 0;
    }

    if (command == "init") {
        bool force{false};
        for (const auto& arg : args) {
            if (arg == "--force")
                force = true;
            else {
                usage();
                return 2;
            }
        }
        return init(openfg::config::defaultPath(), force);
    }

    if (command == "test") {
        if (args.empty()) {
            usage();
            return 2;
        }
        return testNames(args);
    }

    if (command == "validate" || command == "show") {
        if (args.size() > 1) {
            usage();
            return 2;
        }
        const std::filesystem::path path = args.empty()
            ? openfg::config::defaultPath()
            : std::filesystem::path(args.front());
        return command == "validate" ? validate(path) : show(path);
    }

    if (command == "--help" || command == "-h" || command == "help") {
        usage();
        return 0;
    }

    usage();
    return 2;
}
