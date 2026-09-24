#pragma once

// Vulkan port of melonDS's GPU_OpenGL (Copyright 2016-2026 melonDS team, GPL-3.0-or-later):
// the DS screens rendered on the GPU (2D layers, sprites, compositing, final pass with master
// brightness, and display capture at the upscaled resolution). Same structure and logic as the
// GL renderer; GL calls are replaced by ds13r::vk::Stream operations.

#include "GPU.h"
#include "GPU3D.h"
#include "GPU2D_Vulkan.h"
#include "VkStream.h"

#include <memory>

namespace melonDS
{
class NDS;

// Programs (pipelines) shared by both 2D engines and the parent renderer.
struct VulkanPrograms
{
    struct Program
    {
        VkDescriptorSetLayout set = VK_NULL_HANDLE;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;
    };
    Program LayerPre, SpritePre, Compositor, FinalPass, Capture, CapDown;
    Program Sprite[3]; // 0 mosaic pass, 1 window pass, 2 main pass (share set/layout)
};

// The 3D layer for phase 2 of the Vulkan port: a transparent image the compositor can sample.
// Replaced by the Vulkan compute 3D renderer.
class VulkanNull3D : public Renderer3D
{
public:
    VulkanNull3D(melonDS::GPU3D& gpu3D, VulkanRenderer& parent);
    ~VulkanNull3D() override;
    void Reset() override {}
    void RenderFrame() override {}
    u32* GetLine(int line) override { return nullptr; }
    void SetScaleFactor(int scale);

private:
    VulkanRenderer& Parent;
    ds13r::vk::Texture Output;
    int Scale = 0;
};

class VulkanRenderer : public Renderer
{
public:
    VulkanRenderer(melonDS::NDS& nds, ds13r::VulkanContext& vk);
    ~VulkanRenderer() override;
    bool Init() override;
    void Reset() override;
    void Stop() override;

    void PostSavestate() override;

    void SetRenderSettings(RendererSettings& settings) override;

    void DrawScanline(u32 line) override;
    void DrawSprites(u32 line) override;

    void VBlank() override;
    void VBlankEnd() override;

    void AllocCapture(u32 bank, u32 start, u32 len) override;
    void SyncVRAMCapture(u32 bank, u32 start, u32 len, bool complete) override;

    // *top receives a ds13r::vk::Texture* (2-layer array: top and bottom screen); returns false
    // like the GL renderer, meaning "not RAM buffers".
    bool GetFramebuffers(void** top, void** bottom) override;

    int ScaleFactorValue() const { return ScaleFactor; }

private:
    friend class VulkanRenderer2D;
    friend class VulkanNull3D;

    ds13r::VulkanContext& VK;
    ds13r::vk::Stream S;
    VulkanPrograms P;

    int ScaleFactor;
    int ScreenW, ScreenH;

    ds13r::vk::Texture* OutputTex3D = nullptr;
    ds13r::vk::Texture* OutputTex2D[2] = {nullptr, nullptr};

    ds13r::vk::Texture MosaicTex;     // shared by both 2D engines

    struct sFinalPassConfig
    {
        u32 uScreenSwap[192];
        u32 uScaleFactor;
        u32 uAuxLayer;
        u32 uDispModeA;
        u32 uDispModeB;
        u32 uBrightModeA;
        u32 uBrightModeB;
        u32 uBrightFactorA;
        u32 uBrightFactorB;
        float uAuxColorFactor;
        u32 __pad0[3];
    } FinalPassConfig;

    ds13r::vk::Texture AuxInputTex;   // aux input (VRAM and mainmem FIFO)
    ds13r::vk::Texture CaptureVRAMTex;
    ds13r::vk::Texture FPOutputTex[2]; // final output (2 layers: top, bottom)

    struct sCaptureConfig
    {
        float uInvCaptureSize[2];
        u32 uSrcALayer;
        u32 uSrcBLayer;
        u32 uSrcBOffset;
        u32 uDstMode;
        u32 uBlendFactors[2];
        float uSrcAOffset[192];
        float uSrcBColorFactor;
        u32 __pad0[3];
    } CaptureConfig;

    ds13r::vk::Texture CaptureOutput256Tex;
    ds13r::vk::Texture CaptureOutput128Tex;
    ds13r::vk::Texture CaptureSyncTex;

    u16* AuxInputBuffer[2];
    u8 AuxUsageMask;

    u32 DispCntA, DispCntB;
    u16 MasterBrightnessA, MasterBrightnessB;
    u32 CaptureCnt;

    bool NeedPartialRender;
    int LastLine;
    int LastCapLine;
    int Aux0VRAMCap;

    bool CreatePrograms();
    void DestroyPrograms();
    void SetScaleFactor(int scale);

    void RenderScreen(int ystart, int yend);
    void DoCapture(int ystart, int yend);
    void DownscaleCapture(int width, int height, int layer);
};

}
