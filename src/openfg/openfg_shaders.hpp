/* SPDX-License-Identifier: MIT */

#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace openfg {

struct EmbeddedShader {
    std::string_view name;
    const uint32_t* data;
    size_t size;
};

/// Look up a compiled SPIR-V module by pipeline name (e.g. "luma").
[[nodiscard]] std::span<const uint32_t> embeddedShader(std::string_view name);

}
