/* SPDX-License-Identifier: MIT */
#pragma once

#include <array>
#include <cstdint>

std::array<uint32_t, 16> fsr1EasuConstants(
    uint32_t inputWidth, uint32_t inputHeight,
    uint32_t outputWidth, uint32_t outputHeight);

std::array<uint32_t, 4> fsr1RcasConstants(float sharpness);
