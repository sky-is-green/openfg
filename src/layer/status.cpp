/* SPDX-License-Identifier: MIT */

#include "status.hpp"

#include "config/config.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

namespace openfg_layer {
namespace {

    std::filesystem::path statusPath() {
        if (const char* runtime = std::getenv("XDG_RUNTIME_DIR");
            runtime && *runtime)
            return std::filesystem::path(runtime) / "openfg" / "status.json";
        if (const char* state = std::getenv("XDG_STATE_HOME"); state && *state)
            return std::filesystem::path(state) / "openfg" / "status.json";
        if (const char* home = std::getenv("HOME"); home && *home)
            return std::filesystem::path(home) / ".local" / "state" /
                "openfg" / "status.json";
        return "/tmp/openfg-status.json";
    }

    std::string escape(const std::string& value) {
        std::string result;
        result.reserve(value.size());
        for (const char c : value) {
            switch (c) {
                case '\\': result += "\\\\"; break;
                case '"': result += "\\\""; break;
                case '\n': result += "\\n"; break;
                case '\r': result += "\\r"; break;
                case '\t': result += "\\t"; break;
                default: result += c; break;
            }
        }
        return result;
    }

    std::string processName() {
        const auto names = openfg::config::processNames();
        for (const auto& name : names) {
            if (name.size() > 4 &&
                (name.ends_with(".exe") || name.ends_with(".EXE")))
                return name;
        }
        return names.empty() ? "unknown" : names.front();
    }

} // namespace

void writeStatus(const std::string& state, unsigned multiplier,
                 double appFps, double presentedFps, double overheadMs
) {
    const auto path = statusPath();
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error)
        return;

    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const auto temporary = path.string() + ".tmp";
    std::ofstream out{temporary, std::ios::trunc};
    if (!out)
        return;
    out << "{\n"
        << "  \"pid\": " << static_cast<long long>(::getpid()) << ",\n"
        << "  \"process\": \"" << escape(processName()) << "\",\n"
        << "  \"state\": \"" << escape(state) << "\",\n"
        << "  \"multiplier\": " << multiplier << ",\n"
        << "  \"app_fps\": " << appFps << ",\n"
        << "  \"presented_fps\": " << presentedFps << ",\n"
        << "  \"overhead_ms\": " << overheadMs << ",\n"
        << "  \"updated\": " << now << "\n"
        << "}\n";
    out.close();
    if (!out)
        return;
    std::filesystem::rename(temporary, path, error);
    if (error)
        std::filesystem::remove(temporary, error);
}

} // namespace openfg_layer
