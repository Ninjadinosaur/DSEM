// Vulkan port of melonDS's GPU_OpenGL.cpp (Copyright 2016-2026 melonDS team,
// GPL-3.0-or-later). The logic is unchanged; each GL operation maps to a Stream call.

#include "GPU_Vulkan.h"
#include "NDS.h"

#include <algorithm>
#include <cstring>

namespace melonDS
{
using namespace ds13r::vk;
using Platform::Log;
using Platform::LogLevel;

TextureDesc DSColorTexture(uint32_t w, uint32_t h, uint32_t layers, bool array); // GPU2D_Vulkan.cpp

namespace
{
// SPIR-V from shaders/ds/*, converted from melonDS's GL shaders.
const uint32_t kLayerPreVert[] = {
#include "layer_pre.vert.inc"
};
const uint32_t kLayerPreFrag[] = {
#include "layer_pre.frag.inc"
};
const uint32_t kSpritePreVert[] = {
#include "sprite_pre.vert.inc"
};
const uint32_t kSpritePreFrag[] = {
#include "sprite_pre.frag.inc"
};
const uint32_t kSpriteVert[] = {
#include "sprite.vert.inc"
};
const uint32_t kSpriteFrag[] = {
#include "sprite.frag.inc"
};
const uint32_t kCompositorVert[] = {
#include "compositor.vert.inc"
};
const uint32_t kCompositorFrag[] = {
#include "compositor.frag.inc"
};
const uint32_t kCaptureVert[] = {
#include "capture.vert.inc"
};
const uint32_t kCaptureFrag[] = {
#include "capture.frag.inc"
};
const uint32_t kCapDownVert[] = {
#include "capture_down.vert.inc"
};
const uint32_t kCapDownFrag[] = {
#include "capture_down.frag.inc"
};
const uint32_t kFinalPassVert[] = {
#include "final_pass.vert.inc"
};
const uint32_t kFinalPassFrag[] = {
#include "final_pass.frag.inc"
};

const float kRectVertices[2 * 2 * 3] = {
    0, 1,   1, 0,   1, 1,
    0, 1,   0, 0,   1, 0,
};

const float kFinalPassVertices[6][2] = {
    {-1, 1}, {1, -1}, {1, 1},
    {-1, 1}, {-1, -1}, {1, -1},
};

constexpr VkColorComponentFlags RGBA = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                       VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

TextureDesc Target(uint32_t w, uint32_t h, uint32_t layers, bool array, bool transferSrc = false)
{
    TextureDesc d;
    d.width = w;
    d.height = h;
    d.layers = layers;
    d.array = array;
    d.format = VK_FORMAT_R8G8B8A8_UNORM;
    d.renderTarget = true;
    d.transferSrc = transferSrc;
    return d;
}
}

// ---------------------------------------------------------------------------
// Placeholder 3D layer (phase 2)

VulkanNull3D::VulkanNull3D(melonDS::GPU3D& gpu3D, VulkanRenderer& parent) : Renderer3D(gpu3D), Parent(parent)
{
}

VulkanNull3D::~VulkanNull3D()
{
    Parent.S.DestroyTexture(Output);
}

void VulkanNull3D::SetScaleFactor(int scale)
{
    if (scale == Scale) return;
    Scale = scale;
    Parent.S.CreateTexture(Output, Target(256 * scale, 192 * scale, 1, false));
    // Fully transparent: the compositor shows the 2D layers through it.
    Parent.S.BeginRendering({{&Output, 0}});
    Parent.S.ClearColor(0, 0, 0, 0, 0);
    Parent.S.EndRendering();
    Parent.OutputTex3D = &Output;
}

// ---------------------------------------------------------------------------

VulkanRenderer::VulkanRenderer(melonDS::NDS& nds, ds13r::VulkanContext& vk)
    : Renderer(nds.GPU), VK(vk), S(vk)
{
    AuxInputBuffer[0] = new u16[256 * 256];
    AuxInputBuffer[1] = new u16[256 * 192];
    memset(AuxInputBuffer[0], 0, 256 * 256 * 2);
    memset(AuxInputBuffer[1], 0, 256 * 192 * 2);

    Rend2D_A = std::make_unique<VulkanRenderer2D>(GPU.GPU2D_A, *this);
    Rend2D_B = std::make_unique<VulkanRenderer2D>(GPU.GPU2D_B, *this);
    Rend3D = std::make_unique<VulkanNull3D>(GPU.GPU3D, *this);

    ScaleFactor = 0;
    ScreenW = ScreenH = 0;
}

VulkanRenderer::~VulkanRenderer()
{
    S.Flush(true);
    Rend2D_A.reset();
    Rend2D_B.reset();
    Rend3D.reset();
    S.DestroyTexture(MosaicTex);
    S.DestroyTexture(AuxInputTex);
    S.DestroyTexture(CaptureVRAMTex);
    S.DestroyTexture(FPOutputTex[0]);
    S.DestroyTexture(FPOutputTex[1]);
    S.DestroyTexture(CaptureOutput256Tex);
    S.DestroyTexture(CaptureOutput128Tex);
    S.DestroyTexture(CaptureSyncTex);
    S.Flush(true);
    DestroyPrograms();
    delete[] AuxInputBuffer[0];
    delete[] AuxInputBuffer[1];
}

bool VulkanRenderer::CreatePrograms()
{
    auto cis = [](uint32_t binding, uint32_t count = 1) {
        return LayoutBinding {binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, count};
    };
    auto ubo = [](uint32_t binding) { return LayoutBinding {binding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}; };

    VkFormat rgba = VK_FORMAT_R8G8B8A8_UNORM;
    VkRenderPass one = S.RenderPassFor({rgba}, VK_FORMAT_UNDEFINED);
    VkRenderPass two = S.RenderPassFor({rgba, rgba}, VK_FORMAT_UNDEFINED);
    VkRenderPass spritePass = S.RenderPassFor({rgba, rgba}, VK_FORMAT_D16_UNORM);

    VkVertexInputAttributeDescription vec2Pos {0, 0, VK_FORMAT_R32G32_SFLOAT, 0};

    // BG layer pre-render
    P.LayerPre.set = CreateSetLayout(VK, {cis(0), cis(1), ubo(2)});
    P.LayerPre.layout = CreatePipelineLayout(VK, P.LayerPre.set, 4);
    {
        PipelineDesc d;
        d.vertCode = kLayerPreVert; d.vertBytes = sizeof(kLayerPreVert);
        d.fragCode = kLayerPreFrag; d.fragBytes = sizeof(kLayerPreFrag);
        d.layout = P.LayerPre.layout;
        d.renderPass = one;
        d.vertexStride = 2 * sizeof(float);
        d.attribs = {vec2Pos};
        d.colorMasks = {RGBA};
        P.LayerPre.pipeline = CreateGraphicsPipeline(VK, d);
    }

    // sprite pre-render: ivec2 position + int sprite index (GL_SHORT integer attributes)
    P.SpritePre.set = CreateSetLayout(VK, {cis(0), cis(1), ubo(2)});
    P.SpritePre.layout = CreatePipelineLayout(VK, P.SpritePre.set, 0);
    {
        PipelineDesc d;
        d.vertCode = kSpritePreVert; d.vertBytes = sizeof(kSpritePreVert);
        d.fragCode = kSpritePreFrag; d.fragBytes = sizeof(kSpritePreFrag);
        d.layout = P.SpritePre.layout;
        d.renderPass = one;
        d.vertexStride = 3 * sizeof(u16);
        d.attribs = {{0, 0, VK_FORMAT_R16G16_SINT, 0}, {1, 0, VK_FORMAT_R16_SINT, 2 * sizeof(u16)}};
        d.colorMasks = {RGBA};
        P.SpritePre.pipeline = CreateGraphicsPipeline(VK, d);
    }

    // sprites: three passes with different colour masks and depth (see DoRenderSprites)
    VkDescriptorSetLayout spriteSet = CreateSetLayout(VK, {cis(0), cis(1), cis(2), ubo(3), ubo(4)});
    VkPipelineLayout spriteLayout = CreatePipelineLayout(VK, spriteSet, 4);
    const VkColorComponentFlags masks[3][2] = {
        {0, VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_A_BIT},                             // mosaic flags
        {0, VK_COLOR_COMPONENT_B_BIT},                                                         // object window
        {RGBA, VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_A_BIT}, // sprites
    };
    for (int i = 0; i < 3; i++)
    {
        PipelineDesc d;
        d.vertCode = kSpriteVert; d.vertBytes = sizeof(kSpriteVert);
        d.fragCode = kSpriteFrag; d.fragBytes = sizeof(kSpriteFrag);
        d.layout = spriteLayout;
        d.renderPass = spritePass;
        d.vertexStride = 5 * sizeof(u16);
        d.attribs = {{0, 0, VK_FORMAT_R16G16_SINT, 0},
                     {1, 0, VK_FORMAT_R16G16_SINT, 2 * sizeof(u16)},
                     {2, 0, VK_FORMAT_R16_SINT, 4 * sizeof(u16)}};
        d.colorMasks = {masks[i][0], masks[i][1]};
        d.depthTest = i == 2;
        d.depthWrite = i == 2;
        P.Sprite[i].set = spriteSet;
        P.Sprite[i].layout = spriteLayout;
        P.Sprite[i].pipeline = CreateGraphicsPipeline(VK, d);
    }

    // compositor
    P.Compositor.set = CreateSetLayout(VK, {cis(0, 4), cis(1), cis(2), cis(3), cis(4), ubo(5), ubo(6), ubo(7)});
    P.Compositor.layout = CreatePipelineLayout(VK, P.Compositor.set, 4);
    {
        PipelineDesc d;
        d.vertCode = kCompositorVert; d.vertBytes = sizeof(kCompositorVert);
        d.fragCode = kCompositorFrag; d.fragBytes = sizeof(kCompositorFrag);
        d.layout = P.Compositor.layout;
        d.renderPass = one;
        d.vertexStride = 2 * sizeof(float);
        d.attribs = {vec2Pos};
        d.colorMasks = {RGBA};
        P.Compositor.pipeline = CreateGraphicsPipeline(VK, d);
    }

    // final pass: both screens at once (two attachments)
    P.FinalPass.set = CreateSetLayout(VK, {cis(0), cis(1), cis(2), ubo(3)});
    P.FinalPass.layout = CreatePipelineLayout(VK, P.FinalPass.set, 0);
    {
        PipelineDesc d;
        d.vertCode = kFinalPassVert; d.vertBytes = sizeof(kFinalPassVert);
        d.fragCode = kFinalPassFrag; d.fragBytes = sizeof(kFinalPassFrag);
        d.layout = P.FinalPass.layout;
        d.renderPass = two;
        d.vertexStride = 2 * sizeof(float);
        d.attribs = {vec2Pos};
        d.colorMasks = {RGBA, RGBA};
        P.FinalPass.pipeline = CreateGraphicsPipeline(VK, d);
    }

    // display capture: ivec2 position + ivec2 texcoord
    P.Capture.set = CreateSetLayout(VK, {cis(0), cis(1), ubo(2)});
    P.Capture.layout = CreatePipelineLayout(VK, P.Capture.set, 0);
    {
        PipelineDesc d;
        d.vertCode = kCaptureVert; d.vertBytes = sizeof(kCaptureVert);
        d.fragCode = kCaptureFrag; d.fragBytes = sizeof(kCaptureFrag);
        d.layout = P.Capture.layout;
        d.renderPass = one;
        d.vertexStride = 4 * sizeof(u16);
        d.attribs = {{0, 0, VK_FORMAT_R16G16_SINT, 0}, {1, 0, VK_FORMAT_R16G16_SINT, 2 * sizeof(u16)}};
        d.colorMasks = {RGBA};
        P.Capture.pipeline = CreateGraphicsPipeline(VK, d);
    }

    // capture downscale (for VRAM sync)
    P.CapDown.set = CreateSetLayout(VK, {cis(0)});
    P.CapDown.layout = CreatePipelineLayout(VK, P.CapDown.set, 4);
    {
        PipelineDesc d;
        d.vertCode = kCapDownVert; d.vertBytes = sizeof(kCapDownVert);
        d.fragCode = kCapDownFrag; d.fragBytes = sizeof(kCapDownFrag);
        d.layout = P.CapDown.layout;
        d.renderPass = one;
        d.vertexStride = 2 * sizeof(float);
        d.attribs = {vec2Pos};
        d.colorMasks = {RGBA};
        P.CapDown.pipeline = CreateGraphicsPipeline(VK, d);
    }

    VK.SavePipelineCache();
    bool ok = P.LayerPre.pipeline && P.SpritePre.pipeline && P.Sprite[0].pipeline && P.Sprite[1].pipeline &&
              P.Sprite[2].pipeline && P.Compositor.pipeline && P.FinalPass.pipeline && P.Capture.pipeline &&
              P.CapDown.pipeline;
    if (!ok) Log(LogLevel::Error, "GPU_Vulkan: pipeline creation failed\n");
    return ok;
}

void VulkanRenderer::DestroyPrograms()
{
    VkDevice dev = VK.Device();
    auto destroy = [&](VulkanPrograms::Program& p, bool ownsLayout) {
        if (p.pipeline) vkDestroyPipeline(dev, p.pipeline, nullptr);
        if (ownsLayout)
        {
            if (p.layout) vkDestroyPipelineLayout(dev, p.layout, nullptr);
            if (p.set) vkDestroyDescriptorSetLayout(dev, p.set, nullptr);
        }
        p = {};
    };
    destroy(P.LayerPre, true);
    destroy(P.SpritePre, true);
    destroy(P.Sprite[0], false);
    destroy(P.Sprite[1], false);
    destroy(P.Sprite[2], true);
    destroy(P.Compositor, true);
    destroy(P.FinalPass, true);
    destroy(P.Capture, true);
    destroy(P.CapDown, true);
}

bool VulkanRenderer::Init()
{
    if (!S.Init()) return false;
    if (!CreatePrograms()) return false;

    // mosaic lookup texture
    u8 mosaic_tex[256 * 16];
    for (int m = 0; m < 16; m++)
    {
        int mosx = 0;
        for (int x = 0; x < 256; x++)
        {
            mosaic_tex[(m * 256) + x] = mosx;
            if (mosx == m)
                mosx = 0;
            else
                mosx++;
        }
    }
    TextureDesc md;
    md.width = 256;
    md.height = 16;
    md.format = VK_FORMAT_R8_SINT;
    if (!S.CreateTexture(MosaicTex, md)) return false;
    S.UploadTexture(MosaicTex, 0, 0, 0, 256, 16, mosaic_tex, 1);

    if (!S.CreateTexture(AuxInputTex, DSColorTexture(256, 256, 2, true))) return false;
    std::vector<u16> zero(256 * 256, 0);
    S.UploadTexture(AuxInputTex, 0, 0, 0, 256, 256, zero.data(), 2);
    S.UploadTexture(AuxInputTex, 1, 0, 0, 256, 256, zero.data(), 2);

    if (!S.CreateTexture(CaptureSyncTex, Target(256, 256, 1, false, true))) return false;

    if (!Rend2D_A->Init()) return false;
    if (!Rend2D_B->Init()) return false;
    if (!Rend3D->Init()) return false;

    // Sized for 1x until the settings arrive, so nothing is ever drawn into a missing image.
    RendererSettings rs {};
    rs.ScaleFactor = 1;
    SetRenderSettings(rs);
    S.Flush(false);
    return true;
}

void VulkanRenderer::Reset()
{
    memset(&FinalPassConfig, 0, sizeof(FinalPassConfig));
    memset(&CaptureConfig, 0, sizeof(CaptureConfig));

    AuxUsageMask = 0;

    DispCntA = 0;
    DispCntB = 0;
    MasterBrightnessA = 0;
    MasterBrightnessB = 0;
    CaptureCnt = 0;

    NeedPartialRender = false;
    LastLine = 0;
    LastCapLine = 0;
    Aux0VRAMCap = -1;

    Rend2D_A->Reset();
    Rend2D_B->Reset();
    Rend3D->Reset();
}

void VulkanRenderer::Stop()
{
}

void VulkanRenderer::PostSavestate()
{
    Reset();
    static_cast<VulkanRenderer2D*>(Rend2D_A.get())->PostSavestate();
    static_cast<VulkanRenderer2D*>(Rend2D_B.get())->PostSavestate();
}

void VulkanRenderer::SetRenderSettings(RendererSettings& settings)
{
    SetScaleFactor(settings.ScaleFactor);
    static_cast<VulkanRenderer2D*>(Rend2D_A.get())->SetScaleFactor(settings.ScaleFactor);
    static_cast<VulkanRenderer2D*>(Rend2D_B.get())->SetScaleFactor(settings.ScaleFactor);
    static_cast<VulkanNull3D*>(Rend3D.get())->SetScaleFactor(settings.ScaleFactor);
}

void VulkanRenderer::SetScaleFactor(int scale)
{
    if (scale == ScaleFactor) return;

    ScaleFactor = scale;
    ScreenW = 256 * scale;
    ScreenH = 192 * scale;

    S.CreateTexture(CaptureOutput256Tex, Target(256 * scale, 256 * scale, 4, true, true));
    S.CreateTexture(CaptureOutput128Tex, Target(128 * scale, 128 * scale, 16, true));
    S.CreateTexture(CaptureVRAMTex, Target(256 * scale, 256 * scale, 1, true));
    for (int i = 0; i < 2; i++)
        S.CreateTexture(FPOutputTex[i], Target(ScreenW, ScreenH, 2, true, true)); // read back for screenshots

    // Start from black, like freshly allocated GL textures.
    for (Texture* t : {&CaptureOutput256Tex, &CaptureOutput128Tex, &CaptureVRAMTex, &FPOutputTex[0], &FPOutputTex[1]})
    {
        for (uint32_t layer = 0; layer < t->layers; layer++)
        {
            S.BeginRendering({{t, layer}});
            S.ClearColor(0, 0, 0, 0, 0);
        }
    }
    S.EndRendering();
}

void VulkanRenderer::DrawScanline(u32 line)
{
    u32 dispcnt_a_diff = DispCntA ^ GPU.GPU2D_A.DispCnt;
    u32 dispcnt_b_diff = DispCntB ^ GPU.GPU2D_B.DispCnt;
    u32 capturecnt_diff = CaptureCnt ^ GPU.CaptureCnt;

    bool need_render = false;
    bool need_capture = false;

    if (dispcnt_a_diff & 0xF0000)
        need_render = true;
    else if (dispcnt_b_diff & 0x10000)
        need_render = true;
    else if (MasterBrightnessA != GPU.MasterBrightnessA ||
             MasterBrightnessB != GPU.MasterBrightnessB)
        need_render = true;

    if (GPU.CaptureEnable && (capturecnt_diff & 0x7FFFFFFF))
    {
        need_render = true;
        need_capture = true;
    }

    NeedPartialRender = need_render;
    Rend2D_A->DrawScanline(line);
    Rend2D_B->DrawScanline(line);

    if (need_render && (line > 0))
    {
        RenderScreen(LastLine, line);
        LastLine = line;
    }

    if (need_capture && (line > 0))
    {
        DoCapture(LastCapLine, line);
        LastCapLine = line;
    }

    DispCntA = GPU.GPU2D_A.DispCnt;
    DispCntB = GPU.GPU2D_B.DispCnt;
    MasterBrightnessA = GPU.MasterBrightnessA;
    MasterBrightnessB = GPU.MasterBrightnessB;
    CaptureCnt = GPU.CaptureCnt;

    FinalPassConfig.uScreenSwap[line] = GPU.ScreenSwap;

    u32 dispcnt = GPU.GPU2D_A.DispCnt;
    u32 dispmode = (dispcnt >> 16) & 0x3;
    u32 capcnt = GPU.CaptureCnt;
    u32 capsel = (capcnt >> 29) & 0x3;
    u32 capA = (capcnt >> 24) & 0x1;
    u32 capB = (capcnt >> 25) & 0x1;
    bool checkcap = GPU.CaptureEnable && (capsel != 0);

    if (GPU.CaptureEnable && (capsel != 1))
    {
        if (capA == 0)
            CaptureConfig.uSrcAOffset[line] = 0;
        else
        {
            int xpos = GPU.GPU3D.GetRenderXPos() & 0x1FF;
            xpos -= ((xpos & 0x100) << 1);
            CaptureConfig.uSrcAOffset[line] = (float)xpos / 256.f;
        }
    }

    if ((dispmode == 2) || (checkcap && (capB == 0)))
    {
        AuxUsageMask |= (1<<0);

        u32 vrambank = (dispcnt >> 18) & 0x3;
        u32 vramoffset = line * 256;
        u32 outoffset = line * 256;
        if (dispmode != 2)
        {
            u32 yoff = ((capcnt >> 26) & 0x3) << 14;
            vramoffset += yoff;
            outoffset += yoff;
        }

        vramoffset &= 0xFFFF;
        outoffset &= 0xFFFF;

        u16* adst = &AuxInputBuffer[0][outoffset];

        if (GPU.VRAMMap_LCDC & (1<<vrambank))
        {
            u16* vram = (u16*)GPU.VRAM[vrambank];
            for (int i = 0; i < 256; i++)
            {
                adst[i] = vram[vramoffset];
                vramoffset++;
            }
        }
        else
        {
            for (int i = 0; i < 256; i++)
                adst[i] = 0;
        }
    }

    if ((dispmode == 3) || (checkcap && (capB == 1)))
    {
        AuxUsageMask |= (1<<1);

        u16* adst = &AuxInputBuffer[1][line * 256];
        for (int i = 0; i < 256; i++)
            adst[i] = GPU.DispFIFOBuffer[i];
    }
}

void VulkanRenderer::DrawSprites(u32 line)
{
    Rend2D_A->DrawSprites(line);
    Rend2D_B->DrawSprites(line);
}

void VulkanRenderer::RenderScreen(int ystart, int yend)
{
    if (yend <= ystart) return;
    int backbuf = BackBuffer;

    int vramcap = -1;
    if (AuxUsageMask & (1<<0))
    {
        u32 vrambank = (DispCntA >> 18) & 0x3;
        if (GPU.VRAMMap_LCDC & (1<<vrambank))
            vramcap = GPU.GetCaptureBlock_LCDC(vrambank << 17);
    }
    Aux0VRAMCap = vramcap;

    if (!GPU.ScreensEnabled)
    {
        S.BeginRendering({{&FPOutputTex[backbuf], 0}, {&FPOutputTex[backbuf], 1}});
        S.SetViewport(0, 0, ScreenW, ScreenH);
        S.SetScissor(0, ystart * ScaleFactor, ScreenW, (yend - ystart) * ScaleFactor);
        S.ClearColor(0, 0, 0, 0, 1);
        S.ClearColor(1, 0, 0, 0, 1);
        return;
    }

    FinalPassConfig.uScaleFactor = ScaleFactor;
    FinalPassConfig.uDispModeA = (DispCntA >> 16) & 0x3;
    FinalPassConfig.uDispModeB = (DispCntB >> 16) & 0x1;
    FinalPassConfig.uBrightModeA = (MasterBrightnessA >> 14) & 0x3;
    FinalPassConfig.uBrightModeB = (MasterBrightnessB >> 14) & 0x3;
    FinalPassConfig.uBrightFactorA = std::min(MasterBrightnessA & 0x1F, 16);
    FinalPassConfig.uBrightFactorB = std::min(MasterBrightnessB & 0x1F, 16);

    if (AuxUsageMask)
    {
        if ((AuxUsageMask & (1<<0)) && (vramcap == -1))
            S.UploadTexture(AuxInputTex, 0, 0, 0, 256, 256, AuxInputBuffer[0], 2);
        if (AuxUsageMask & (1<<1))
            S.UploadTexture(AuxInputTex, 1, 0, 0, 256, 192, AuxInputBuffer[1], 2);
    }

    Texture* aux = &AuxInputTex;
    u32 modeA = (DispCntA >> 16) & 0x3;
    if ((modeA == 2) && (vramcap != -1))
    {
        aux = &CaptureOutput256Tex;
        FinalPassConfig.uAuxLayer = vramcap >> 2;
        FinalPassConfig.uAuxColorFactor = 63.75f;
    }
    else if (modeA >= 2)
    {
        FinalPassConfig.uAuxLayer = (modeA - 2);
        FinalPassConfig.uAuxColorFactor = 62.f;
    }

    S.BeginRendering({{&FPOutputTex[backbuf], 0}, {&FPOutputTex[backbuf], 1}});
    S.SetViewport(0, 0, ScreenW, ScreenH);
    S.SetScissor(0, ystart * ScaleFactor, ScreenW, (yend - ystart) * ScaleFactor);
    S.Draw(P.FinalPass.pipeline, P.FinalPass.layout, P.FinalPass.set,
           {{0, 0, OutputTex2D[0], SamplerMode::NearestClampEdge},
            {1, 0, OutputTex2D[1], SamplerMode::NearestClampEdge},
            {2, 0, aux, SamplerMode::NearestRepeat},
            {3, 0, nullptr, SamplerMode::NearestClampEdge, &FinalPassConfig, sizeof(FinalPassConfig)}},
           nullptr, 0, kFinalPassVertices, sizeof(kFinalPassVertices), 6);
}

void VulkanRenderer::VBlank()
{
    Rend2D_A->VBlank();
    Rend2D_B->VBlank();

    RenderScreen(LastLine, 192);

    if (GPU.CaptureEnable)
        DoCapture(LastCapLine, 192);

    LastLine = 0;
    LastCapLine = 0;
}

void VulkanRenderer::VBlankEnd()
{
    AuxUsageMask = 0;
}

void VulkanRenderer::DoCapture(int ystart, int yend)
{
    u32 dispcnt = DispCntA;
    u32 capcnt = CaptureCnt;
    u32 dispmode = (dispcnt >> 16) & 0x3;
    u32 srcA = (capcnt >> 24) & 0x1;
    u32 srcB = (capcnt >> 25) & 0x1;
    u32 srcBblock = (dispcnt >> 18) & 0x3;
    u32 srcBoffset = (dispmode == 2) ? 0 : ((capcnt >> 26) & 0x3);
    u32 dstblock = (capcnt >> 16) & 0x3;
    u32 dstoffset = (capcnt >> 18) & 0x3;
    u32 capsize = (capcnt >> 20) & 0x3;
    u32 dstmode = (capcnt >> 29) & 0x3;
    u32 eva = std::min(capcnt & 0x1F, 16u);
    u32 evb = std::min((capcnt >> 8) & 0x1F, 16u);

    // determine the region we're going to capture to
    int dstwidth, dstheight;
    if (capsize == 0)
    {
        dstwidth = 128;
        dstheight = 128;
    }
    else
    {
        dstwidth = 256;
        dstheight = 64 * capsize;
    }

    if (ystart >= dstheight)
        return;
    if (yend > dstheight)
        yend = dstheight;
    if (yend <= ystart)
        return;

    Texture* inputA = srcA ? OutputTex3D : OutputTex2D[0];

    bool useSrcB = (dstmode == 1) || (dstmode == 2 && evb > 0);

    Texture* inputB = &AuxInputTex;
    u32 layerB = srcB;
    CaptureConfig.uSrcBColorFactor = 248.f;

    if (useSrcB && (Aux0VRAMCap != -1))
    {
        // hi-res VRAM
        if (dstblock == srcBblock)
        {
            // Reading from the block being captured to: the hardware reads the old VRAM
            // contents, then writes; copy the source rows aside first.
            int blitY0 = (srcBoffset * 64) + ystart;
            int blitY1 = (srcBoffset * 64) + yend;

            if (dstoffset != srcBoffset)
                Log(LogLevel::Error, "GPU_Vulkan: MISMATCHED VRAM OFFSETS ON SAME BANK!!! bank=%d src=%d dst=%d\n",
                    dstblock, srcBoffset, dstoffset);

            auto copyRows = [&](int y0, int y1) {
                if (y1 <= y0) return;
                S.Use(CaptureOutput256Tex, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                      VK_ACCESS_TRANSFER_READ_BIT, false);
                S.Use(CaptureVRAMTex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                      VK_ACCESS_TRANSFER_WRITE_BIT, true);
                VkImageCopy c {};
                c.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, srcBblock, 1};
                c.srcOffset = {0, y0 * ScaleFactor, 0};
                c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                c.dstOffset = {0, y0 * ScaleFactor, 0};
                c.extent = {(uint32_t)(256 * ScaleFactor), (uint32_t)((y1 - y0) * ScaleFactor), 1};
                vkCmdCopyImage(S.Cmd(), CaptureOutput256Tex.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               CaptureVRAMTex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
            };

            if (blitY1 > 256)
            {
                // wraparound
                copyRows(blitY0, 256);
                copyRows(0, blitY1 - 256);
            }
            else
            {
                copyRows(blitY0, blitY1);
            }

            inputB = &CaptureVRAMTex;
            layerB = 0;
        }
        else
        {
            // a different bank can be used as-is
            inputB = &CaptureOutput256Tex;
            layerB = srcBblock;
        }

        CaptureConfig.uSrcBColorFactor = 255.f;
    }

    Texture* target;
    uint32_t targetLayer;
    if (capsize == 0)
    {
        target = &CaptureOutput128Tex;
        targetLayer = (dstblock << 2) | dstoffset;
    }
    else
    {
        target = &CaptureOutput256Tex;
        targetLayer = dstblock;
    }

    CaptureConfig.uInvCaptureSize[0] = 1.f / (float)dstwidth;
    CaptureConfig.uInvCaptureSize[1] = 1.f / (float)dstheight;
    CaptureConfig.uSrcALayer = srcA;
    CaptureConfig.uSrcBOffset = (srcB == 0) ? 64 * srcBoffset : 0;
    CaptureConfig.uSrcBLayer = layerB;
    CaptureConfig.uDstMode = dstmode;
    CaptureConfig.uBlendFactors[0] = eva;
    CaptureConfig.uBlendFactors[1] = evb;

    u16 vtxbuf[12 * 4];
    u16* vptr = vtxbuf;
    int numvtx;

    // y0/y1 = coordinates in destination buffer
    // t0/t1 = coordinates in source buffers
    if (capsize == 0) dstoffset = 0;
    int y0 = (dstoffset * 64) + ystart;
    int y1 = (dstoffset * 64) + yend;
    int t0 = ystart;
    int t1 = yend;

    int bufferheight = (capsize == 0) ? 128 : 256;
    if (y1 > bufferheight)
    {
        // wraparound
        int y2 = bufferheight;
        int t2 = t0 + (y2 - y0);
        *vptr++ = 0;        *vptr++ = y2; *vptr++ = 0;         *vptr++ = t2;
        *vptr++ = dstwidth; *vptr++ = y0; *vptr++ = dstwidth;  *vptr++ = t0;
        *vptr++ = dstwidth; *vptr++ = y2; *vptr++ = dstwidth;  *vptr++ = t2;
        *vptr++ = 0;        *vptr++ = y2; *vptr++ = 0;         *vptr++ = t2;
        *vptr++ = 0;        *vptr++ = y0; *vptr++ = 0;         *vptr++ = t0;
        *vptr++ = dstwidth; *vptr++ = y0; *vptr++ = dstwidth;  *vptr++ = t0;

        y2 = y1 - bufferheight;
        *vptr++ = 0;        *vptr++ = y2; *vptr++ = 0;         *vptr++ = t1;
        *vptr++ = dstwidth; *vptr++ = 0;  *vptr++ = dstwidth;  *vptr++ = t2;
        *vptr++ = dstwidth; *vptr++ = y2; *vptr++ = dstwidth;  *vptr++ = t1;
        *vptr++ = 0;        *vptr++ = y2; *vptr++ = 0;         *vptr++ = t1;
        *vptr++ = 0;        *vptr++ = 0;  *vptr++ = 0;         *vptr++ = t2;
        *vptr++ = dstwidth; *vptr++ = 0;  *vptr++ = dstwidth;  *vptr++ = t2;

        numvtx = 12;
    }
    else
    {
        *vptr++ = 0;        *vptr++ = y1; *vptr++ = 0;         *vptr++ = t1;
        *vptr++ = dstwidth; *vptr++ = y0; *vptr++ = dstwidth;  *vptr++ = t0;
        *vptr++ = dstwidth; *vptr++ = y1; *vptr++ = dstwidth;  *vptr++ = t1;
        *vptr++ = 0;        *vptr++ = y1; *vptr++ = 0;         *vptr++ = t1;
        *vptr++ = 0;        *vptr++ = y0; *vptr++ = 0;         *vptr++ = t0;
        *vptr++ = dstwidth; *vptr++ = y0; *vptr++ = dstwidth;  *vptr++ = t0;

        numvtx = 6;
    }

    S.BeginRendering({{target, targetLayer}});
    int size = (capsize == 0) ? 128 * ScaleFactor : 256 * ScaleFactor;
    S.SetViewport(0, 0, size, size);
    S.SetScissor(0, 0, size, size);
    S.Draw(P.Capture.pipeline, P.Capture.layout, P.Capture.set,
           {{0, 0, inputA, SamplerMode::NearestClampBorder},
            {1, 0, inputB, SamplerMode::NearestRepeat},
            {2, 0, nullptr, SamplerMode::NearestClampEdge, &CaptureConfig, sizeof(CaptureConfig)}},
           nullptr, 0, vtxbuf, numvtx * 4 * sizeof(u16), numvtx);
}

void VulkanRenderer::AllocCapture(u32 bank, u32 start, u32 len)
{
    auto rend2D = static_cast<VulkanRenderer2D*>(Rend2D_A.get());
    rend2D->LayerConfigDirty = true;
    rend2D->SpriteConfigDirty = true;
    rend2D = static_cast<VulkanRenderer2D*>(Rend2D_B.get());
    rend2D->LayerConfigDirty = true;
    rend2D->SpriteConfigDirty = true;
}

void VulkanRenderer::DownscaleCapture(int width, int height, int layer)
{
    // downscale a hi-res capture buffer to 1x, so colour components can be downscaled accurately
    S.BeginRendering({{&CaptureSyncTex, 0}});
    S.SetViewport(0, 0, width, height);
    S.SetScissor(0, 0, width, height);
    int32_t inputLayer = layer;
    Texture* input = (width == 128) ? &CaptureOutput128Tex : &CaptureOutput256Tex;
    S.Draw(P.CapDown.pipeline, P.CapDown.layout, P.CapDown.set,
           {{0, 0, input, SamplerMode::NearestRepeat}},
           &inputLayer, sizeof(inputLayer), kRectVertices, sizeof(kRectVertices), 6);
}

// RGBA8 from the downscale pass (components already reduced to 5 bits) to the DS's RGB555+A.
static void PackCapture(const u32* src, u16* dst, int count)
{
    for (int i = 0; i < count; i++)
    {
        u32 p = src[i];
        u32 r = ((p & 0xFF) * 31 + 127) / 255;
        u32 g = (((p >> 8) & 0xFF) * 31 + 127) / 255;
        u32 b = (((p >> 16) & 0xFF) * 31 + 127) / 255;
        u32 a = (p >> 24) ? 1 : 0;
        dst[i] = (u16)(r | (g << 5) | (b << 10) | (a << 15));
    }
}

void VulkanRenderer::SyncVRAMCapture(u32 bank, u32 start, u32 len, bool complete)
{
    if (!complete)
        Log(LogLevel::Error, "GPU_Vulkan: !!! READING VRAM AS IT IS BEING CAPTURED TO\n");

    u8* vram = GPU.VRAM[bank];
    std::vector<u32> pixels;

    if (len == 0) // 128x128
    {
        DownscaleCapture(128, 128, (bank<<2) | start);
        pixels.resize(128 * 128);
        S.ReadTexture(CaptureSyncTex, 0, 0, 0, 128, 128, pixels.data(), 4);
        PackCapture(pixels.data(), (u16*)&vram[start * 64 * 512], 128 * 128);

        for (u32 j = start * 64; j < (start+1) * 64; j++)
            GPU.VRAMDirty[bank][j] = true;
    }
    else
    {
        DownscaleCapture(256, 256, bank);

        u32 pos = start;
        for (u32 i = 0; i < len;)
        {
            u32 end = pos + len;
            if (end > 4)
                end = 4;

            int rows = (end - pos) * 64;
            pixels.resize((size_t)256 * rows);
            S.ReadTexture(CaptureSyncTex, 0, 0, pos * 64, 256, rows, pixels.data(), 4);
            PackCapture(pixels.data(), (u16*)&vram[pos * 64 * 512], 256 * rows);

            for (u32 j = pos * 64; j < end * 64; j++)
                GPU.VRAMDirty[bank][j] = true;

            i += (end - pos);
            pos += (end - pos);
            pos &= 3;
        }
    }
}

bool VulkanRenderer::GetFramebuffers(void** top, void** bottom)
{
    // Hand the finished frame to the presenter: make it readable and submit the frame's work.
    int frontbuf = BackBuffer ^ 1;
    S.PrepareForExternalRead(FPOutputTex[frontbuf]);
    S.Flush(false);
    *top = &FPOutputTex[frontbuf];
    *bottom = nullptr;
    return false;
}

}
