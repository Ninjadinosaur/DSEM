#version 450
#include "present_common.glsl"

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 fragColor;

layout(set = 0, binding = 0) uniform sampler2DArray uTex;

vec3 fetch(vec2 uv)
{
    vec4 c = texture(uTex, vec3(uv, pc.layer));
    return pc.swapRB != 0 ? c.bgr : c.rgb;
}

vec3 fetchNearest(vec2 texel)
{
    vec4 c = texelFetch(uTex, ivec3(clamp(texel, vec2(0.0), pc.texSize - 1.0), int(pc.layer)), 0);
    return pc.swapRB != 0 ? c.bgr : c.rgb;
}

// Keeps pixels crisp at non-integer scales while only blending at texel edges.
vec3 sharpBilinear(vec2 uv)
{
    vec2 scale = max(floor(pc.outSize / pc.texSize), vec2(1.0));
    vec2 texel = uv * pc.texSize;
    vec2 base = floor(texel);
    vec2 centerDist = fract(texel) - 0.5;
    vec2 range = 0.5 - 0.5 / scale;
    vec2 f = (centerDist - clamp(centerDist, -range, range)) * scale + 0.5;
    return fetch((base + f) / pc.texSize);
}

// Colour response of the original DS LCD (after the libretro "nds-color" shader).
vec3 dsColor(vec3 c)
{
    c = pow(c, vec3(2.2));
    mat3 m = mat3(0.815, 0.215, 0.145,
                  0.105, 0.665, 0.095,
                  -0.025, 0.120, 0.730);
    c = clamp(m * c * 0.905, 0.0, 1.0);
    return pow(c, vec3(1.0 / 2.2));
}

void main()
{
    vec3 c;
    vec2 texel = vUV * pc.texSize;

    if (pc.filterMode == 0)
    {
        c = fetchNearest(floor(texel));
    }
    else if (pc.filterMode == 1)
    {
        c = fetch(vUV);
    }
    else if (pc.filterMode == 3)
    {
        // Scanlines: darken the lower part of every source row.
        c = sharpBilinear(vUV);
        float y = fract(texel.y);
        c *= mix(1.0, 0.72, smoothstep(0.55, 0.9, y));
    }
    else if (pc.filterMode == 4)
    {
        // LCD grid: thin dark gaps between pixels, like the DS panel up close.
        c = fetchNearest(floor(texel));
        vec2 f = fract(texel);
        vec2 edge = smoothstep(vec2(0.0), vec2(0.12), f) * smoothstep(vec2(1.0), vec2(0.88), f);
        c *= mix(0.78, 1.0, edge.x * edge.y);
    }
    else
    {
        c = sharpBilinear(vUV);
    }

    if (pc.colorCorrect != 0) c = dsColor(c);
    fragColor = vec4(c, pc.opacity);
}
