// Shared declarations for the openfg compute pipeline.
//
// Descriptor set 0 is fixed for every pass:
//   binding 0      sampler
//   binding 1..4   sampled images (2D or 2D array)
//   binding 8..11  storage images (r8, r8, rg16f, rgba8)
// Unused bindings are still valid in the set; shaders only declare what they use.

#extension GL_GOOGLE_include_directive : require

layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 0, binding = 0) uniform sampler uSampler;

layout(set = 0, binding = 1) uniform texture2D uTex0;
layout(set = 0, binding = 2) uniform texture2D uTex1;
layout(set = 0, binding = 3) uniform texture2D uTex2;
layout(set = 0, binding = 4) uniform texture2D uTex3;

layout(set = 0, binding = 5) uniform texture2DArray uTexArray0;

layout(push_constant) uniform PC {
    uint width;
    uint height;
    float t;
    float flowScale;
    int level;
    int radius;
    int flags;
    int pad0;
    uint layerA;
    uint layerB;
    uint dstLayer;
    uint pad2;
} pc;

// Clamp a coordinate for texelFetch/imageLoad style access.
ivec2 clampCoord(ivec2 p, ivec2 size) {
    return clamp(p, ivec2(0), size - ivec2(1));
}

// Bilinear sample of a single-channel R8 storage image.
float sampleR8(image2D img, vec2 p, ivec2 size) {
    vec2 q = clamp(p, vec2(0.0), vec2(size) - vec2(1.0));
    ivec2 i = ivec2(floor(q));
    vec2 f = q - vec2(i);
    ivec2 i1 = min(i + ivec2(1), size - ivec2(1));
    float a = imageLoad(img, i).r;
    float b = imageLoad(img, ivec2(i1.x, i.y)).r;
    float c = imageLoad(img, ivec2(i.x, i1.y)).r;
    float d = imageLoad(img, i1).r;
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

float luma(vec3 c) {
    return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

// Bilinear sample of an RG16F storage image.
vec2 sampleRg16f(image2D img, vec2 p) {
    ivec2 size = imageSize(img);
    vec2 q = clamp(p, vec2(0.0), vec2(size) - 1.0);
    ivec2 i = ivec2(floor(q));
    vec2 f = q - vec2(i);
    ivec2 i1 = min(i + ivec2(1), size - 1);
    vec2 a = imageLoad(img, i).xy;
    vec2 b = imageLoad(img, ivec2(i1.x, i.y)).xy;
    vec2 c = imageLoad(img, ivec2(i.x, i1.y)).xy;
    vec2 d = imageLoad(img, i1).xy;
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}
