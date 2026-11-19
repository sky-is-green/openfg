/* SPDX-License-Identifier: MIT */

#include "layer/status.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

int main() {
    const auto directory = std::filesystem::temp_directory_path() /
        ("openfg-status-test-" + std::to_string(::getpid()));
    std::filesystem::remove_all(directory);
    setenv("XDG_RUNTIME_DIR", directory.c_str(), 1);
    openfg_layer::writeStatus("generating", 2, 30.0, 60.0, 0.75);
    const auto path = directory / "openfg" / "status.json";
    std::ifstream input{path};
    std::string text((std::istreambuf_iterator<char>(input)),
                     std::istreambuf_iterator<char>());
    const bool ok = std::filesystem::is_regular_file(path) &&
        text.find("\"state\": \"generating\"") != std::string::npos &&
        text.find("\"app_fps\": 30") != std::string::npos &&
        text.find("\"multiplier\": 2") != std::string::npos;
    std::filesystem::remove_all(directory);
    if (!ok) {
        std::fprintf(stderr, "status checks failed\n");
        return 1;
    }
    std::printf("status checks passed\n");
    return 0;
}
