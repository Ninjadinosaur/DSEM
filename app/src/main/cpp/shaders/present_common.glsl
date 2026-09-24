// Shared by present.vert and present.frag: one draw per DS/GBA screen.
layout(push_constant) uniform Params
{
    vec4 rect;          // x0, y0, x1, y1 in Vulkan NDC (y down), before pre-rotation
    vec4 preRotation;   // 2x2 matrix (column-major) undoing the display's rotation
    vec2 texSize;       // source size in texels
    vec2 outSize;       // destination size in pixels (after screen rotation)
    float layer;        // which screen in the texture array
    float opacity;
    int rotation;       // 0..3 quarter turns clockwise
    int flipY;
    int filterMode;         // 0 nearest, 1 bilinear, 2 sharp bilinear, 3 scanlines, 4 LCD grid
    int colorCorrect;
    int swapRB;         // melonDS software frames are BGRA
} pc;
