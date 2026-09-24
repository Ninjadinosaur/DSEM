#version 450
#include "present_common.glsl"

layout(location = 0) out vec2 vUV;

const vec2 kCorners[4] = vec2[](vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(0.0, 1.0), vec2(1.0, 1.0));

void main()
{
    vec2 p = kCorners[gl_VertexIndex];
    vec2 pos = mix(pc.rect.xy, pc.rect.zw, p);
    // Pre-rotation: we draw in the panel's native orientation so the compositor doesn't have to.
    gl_Position = vec4(mat2(pc.preRotation.xy, pc.preRotation.zw) * pos, 0.0, 1.0);

    // p is the corner on screen (0,0 = top-left). Work out which texel corner it shows.
    vec2 uv = p;
    if (pc.rotation == 1) uv = vec2(p.y, 1.0 - p.x);
    else if (pc.rotation == 2) uv = vec2(1.0 - p.x, 1.0 - p.y);
    else if (pc.rotation == 3) uv = vec2(1.0 - p.y, p.x);
    if (pc.flipY != 0) uv.y = 1.0 - uv.y;
    vUV = uv;
}
