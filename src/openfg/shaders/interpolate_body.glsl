// Motion-compensated interpolation between two colour frames.
//
// Flow is stored at half resolution in full-resolution pixel units, so the
// shader samples it at the same normalised coordinate and scales by pc.flowScale.
//
// This file is included by interpolate.comp (RGBA8) and interpolate_hdr.comp
// (RGBA16F). The only format-dependent line is the output image declaration.
#include "common.glsl"

#if OPENFG_HDR
layout(set = 0, binding = 11, rgba16f) uniform image2DArray uOut;
#else
layout(set = 0, binding = 11, rgba8) uniform image2DArray uOut;
#endif

void main() {
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = ivec2(pc.width, pc.height);
    if (any(greaterThanEqual(p, size)))
        return;

    vec2 uv = (vec2(p) + 0.5) / vec2(size);

    if ((pc.flags & 512) != 0) {
        ivec2 ts = textureSize(sampler2D(uTex3, uSampler), 0);
        ivec2 tile = clamp(p / 16, ivec2(0), ts - 1);
        imageStore(uOut, ivec3(p, int(pc.dstLayer)), vec4(vec3(texelFetch(sampler2D(uTex3, uSampler), tile, 0).r), 1.0));
        return;
    }

    if ((pc.flags & 256) != 0) {
        ivec2 ts = textureSize(sampler2D(uTex3, uSampler), 0);
        ivec2 tile = clamp(p / 16, ivec2(0), ts - 1);
        if (texelFetch(sampler2D(uTex3, uSampler), tile, 0).r < 0.5) {
            imageStore(uOut, ivec3(p, int(pc.dstLayer)), texture(sampler2DArray(uTexArray0, uSampler),
                vec3(uv, float(pc.layerB))));
            return;
        }
    }

    // Debug: dump the current source layer unchanged.
    if ((pc.flags & 2) != 0) {
        imageStore(uOut, ivec3(p, int(pc.dstLayer)), texture(sampler2DArray(uTexArray0, uSampler),
            vec3(uv, float(pc.layerB))));
        return;
    }

    vec2 flow = texture(sampler2D(uTex2, uSampler), uv).xy * pc.flowScale;
    if (length(flow) > 192.0)
        flow = vec2(0.0);
    vec2 duv = flow / vec2(size);
    float t = pc.t;

    // Reverse flow B -> A. Full mode refines to half resolution; the default
    // occlusion mode stops at quarter resolution. Either way it is only used
    // for the consistency test, never to warp.
    bool hasReverse = (pc.flags & (64 | 128)) != 0;
    float reverseScale = ((pc.flags & 64) != 0) ? 2.0 : 4.0;
    vec2 bpos = clamp(uv + (1.0 - t) * duv, vec2(0.0), vec2(1.0));
    vec2 flowBA = hasReverse
        ? texture(sampler2D(uTex0, uSampler), bpos).xy * reverseScale
        : vec2(0.0);
    float consistency = hasReverse ? length(flow + flowBA) : 0.0;

    vec2 uva = clamp(uv - t * duv, vec2(0.0), vec2(1.0));
    vec2 uvb = bpos;

    vec4 ca = texture(sampler2DArray(uTexArray0, uSampler), vec3(uva, float(pc.layerA)));
    vec4 cb = texture(sampler2DArray(uTexArray0, uSampler), vec3(uvb, float(pc.layerB)));

    if ((pc.flags & 8) != 0) {
        imageStore(uOut, ivec3(p, int(pc.dstLayer)), ca);
        return;
    }
    if ((pc.flags & 16) != 0) {
        imageStore(uOut, ivec3(p, int(pc.dstLayer)), cb);
        return;
    }

    // Unwarped samples, used both for static detail and as a fallback where
    // the flow is not trustworthy.
    vec4 a0 = texture(sampler2DArray(uTexArray0, uSampler), vec3(uv, float(pc.layerA)));
    vec4 b0 = texture(sampler2DArray(uTexArray0, uSampler), vec3(uv, float(pc.layerB)));

    // Per-side photo-consistency: compare each warped sample with the other
    // frame at the output position. The two errors differ where one side is
    // occluded, which is what makes the visibility weights meaningful.
    float ea = length(ca.rgb - b0.rgb);
    float eb = length(cb.rgb - a0.rgb);

    // Video-safe fallback. The cut score is the mean flow-compensated
    // residual over the whole frame, so a hard cut switches the entire frame
    // to the current one instead of leaving a blend patchwork.
    float cutScore = texelFetch(sampler2D(uTex1, uSampler), ivec2(0), 0).r;
    if ((pc.flags & 8192) != 0) {
        imageStore(uOut, ivec3(p, int(pc.dstLayer)), vec4(vec3(cutScore), 1.0));
        return;
    }
    if ((pc.flags & 4096) != 0 && cutScore > 0.08) {
        imageStore(uOut, ivec3(p, int(pc.dstLayer)), b0);
        return;
    }

    // Soft visibility. Each side is compared with the other frame at the
    // output position, so the two errors differ at occlusions and the blend
    // leans toward the side that is consistent. The reverse-flow term is
    // optional (it costs a second flow field for very little gain).
    if ((pc.flags & 2048) != 0) {
        imageStore(uOut, ivec3(p, int(pc.dstLayer)), vec4(vec3(clamp(consistency / 20.0, 0.0, 1.0)), 1.0));
        return;
    }
    float flowConf = hasReverse ? smoothstep(10.0, 2.0, consistency) : 1.0;
    float confA = flowConf * smoothstep(0.25, 0.03, ea);
    float confB = flowConf * smoothstep(0.25, 0.03, eb);

    float wa = (1.0 - t) / (ea + 0.02) * (0.05 + confA);
    float wb = t / (eb + 0.02) * (0.05 + confB);
    // Limit how far the photo-consistency term can pull the blend. Letting a
    // single side win outright doubles edges in the ghost scene.
    float ratio = clamp(wa / max(wb, 1e-6), 0.5, 2.0);

    // A second weighting uses luma only, which follows edges more closely.
    // Averaging the two blend ratios keeps colour detail without bringing the
    // doubled edges back.
    float la = abs(luma(ca.rgb) - luma(b0.rgb));
    float lb = abs(luma(cb.rgb) - luma(a0.rgb));
    float waL = (1.0 - t) / (la + 0.02);
    float wbL = t / (lb + 0.02);
    float ratioL = clamp(waL / max(wbL, 1e-6), 0.5, 2.0);

    float blend = 0.5 * (1.0 / (1.0 + ratio) + 1.0 / (1.0 + ratioL));
    vec4 c = mix(ca, cb, blend);

    float err = min(ea, eb);
    if ((pc.flags & 32) != 0) {
        imageStore(uOut, ivec3(p, int(pc.dstLayer)), vec4(vec3(clamp(err * 4.0, 0.0, 1.0)), 1.0));
        return;
    }
    if ((pc.flags & 1024) != 0) {
        imageStore(uOut, ivec3(p, int(pc.dstLayer)), vec4(vec3(max(confA, confB)), 1.0));
        return;
    }

    // The hard "fill holes with the current frame" fallback was tested and
    // removed: it moved whole objects to frame B and cost 1-3 dB. The soft
    // confidences above are the useful part.

    // Static detail (HUD, text, flat UI) is copied from the current frame so it
    // cannot shimmer from tiny flow errors.
    if (length(a0.rgb - b0.rgb) < 0.004)
        c = b0;

    c.a = 1.0;
    imageStore(uOut, ivec3(p, int(pc.dstLayer)), c);
}
