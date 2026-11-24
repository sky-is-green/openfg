// SPDX-License-Identifier: MIT
// Shared helpers for the tile-gate feature pass.

#ifndef OPENFG_GATE_COMMON_GLSL
#define OPENFG_GATE_COMMON_GLSL

int gateReflect(int index, int size)
{
    if (size <= 1)
        return 0;
    while (index < 0 || index >= size) {
        if (index < 0)
            index = -index - 1;
        else
            index = 2 * size - index - 1;
    }
    return index;
}

int gateClamp(int index, int size)
{
    return clamp(index, 0, size - 1);
}

float gateCubic(float x)
{
    const float a = -0.5;
    x = abs(x);
    if (x <= 1.0)
        return (a + 2.0) * x * x * x - (a + 3.0) * x * x + 1.0;
    if (x < 2.0)
        return a * x * x * x - 5.0 * a * x * x + 8.0 * a * x - 4.0 * a;
    return 0.0;
}

float gateLuma(vec3 color)
{
    return dot(color, vec3(0.2126, 0.7152, 0.0722));
}

vec3 gateRgb(uint packed)
{
    return vec3(
        float(packed & 255u),
        float((packed >> 8) & 255u),
        float((packed >> 16) & 255u)) / 255.0;
}

#endif
