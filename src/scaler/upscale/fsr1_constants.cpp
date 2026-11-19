/* SPDX-License-Identifier: MIT */

#include "fsr1_constants.hpp"

#include <math.h>
#include <stdlib.h>

#define A_CPU 1
#include "fsr1/ffx_a.h"
#include "fsr1/ffx_fsr1.h"

std::array<uint32_t, 16> fsr1EasuConstants(
    uint32_t inputWidth, uint32_t inputHeight,
    uint32_t outputWidth, uint32_t outputHeight
) {
    AU1 c0[4]{};
    AU1 c1[4]{};
    AU1 c2[4]{};
    AU1 c3[4]{};
    FsrEasuCon(c0, c1, c2, c3,
        static_cast<AF1>(inputWidth), static_cast<AF1>(inputHeight),
        static_cast<AF1>(inputWidth), static_cast<AF1>(inputHeight),
        static_cast<AF1>(outputWidth), static_cast<AF1>(outputHeight));
    return {
        c0[0], c0[1], c0[2], c0[3],
        c1[0], c1[1], c1[2], c1[3],
        c2[0], c2[1], c2[2], c2[3],
        c3[0], c3[1], c3[2], c3[3]
    };
}

std::array<uint32_t, 4> fsr1RcasConstants(float sharpness) {
    AU1 c0[4]{};
    FsrRcasCon(c0, static_cast<AF1>(sharpness));
    return { c0[0], c0[1], c0[2], c0[3] };
}
