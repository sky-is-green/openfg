// Shared body for the half-resolution colour downsample used by FidelityFX
// Optical Flow's performance mode.
//
// The FidelityFX luma pass expects its input colour image to have the same
// dimensions as the flow field, so the source is sampled bilinearly at 1/2
// scale before it is fed to the pyramid.

layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 0, binding = 0) uniform sampler uSampler;
layout(set = 0, binding = 1) uniform texture2D uSource;
#if OPENFG_HDR
layout(set = 0, binding = 8, rgba16f) uniform image2D uOut;
#else
layout(set = 0, binding = 8, rgba8) uniform image2D uOut;
#endif

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

void main() {
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = ivec2(pc.width, pc.height);
    if (any(greaterThanEqual(p, size)))
        return;

    vec2 uv = (vec2(p) + 0.5) / vec2(size);
    imageStore(uOut, p, texture(sampler2D(uSource, uSampler), uv));
}
