#pragma once

// Vulkan port of melonDS's GPU2D_OpenGL (Copyright 2016-2026 melonDS team, GPL-3.0-or-later).
// Same structure and logic; GL calls are replaced by ds13r::vk::Stream operations.

#include "GPU2D.h"
#include "VkStream.h"

namespace melonDS
{
class VulkanRenderer;

class VulkanRenderer2D : public Renderer2D
{
public:
    VulkanRenderer2D(melonDS::GPU2D& gpu2D, VulkanRenderer& parent);
    ~VulkanRenderer2D() override;
    bool Init() override;
    void Reset() override;

    void PostSavestate();
    void SetScaleFactor(int scale);

    void DrawScanline(u32 line) override;
    void DrawSprites(u32 line) override;
    void VBlank() override;
    void VBlankEnd() override;

private:
    friend class VulkanRenderer;
    VulkanRenderer& Parent;
    ds13r::vk::Stream& S;

    int ScaleFactor;
    int ScreenW, ScreenH;

    // base index for a BG layer within the BG texture arrays, based on BG type and size
    const u8 BGBaseIndex[4][4] = {
        {2, 10, 6, 14},     // text mode
        {0, 4, 16, 20},     // rotscale
        {0, 4, 12, 16},     // bitmap
        {18, 19, 12, 16},   // large bitmap
    };

    ds13r::vk::Texture VRAMTex_BG;
    ds13r::vk::Texture VRAMTex_OBJ;
    ds13r::vk::Texture PalTex_BG;
    ds13r::vk::Texture PalTex_OBJ;

    ds13r::vk::Texture AllBGLayerTex[22];
    ds13r::vk::Texture* BGLayerTex[4];

    ds13r::vk::Texture SpriteTex;
    ds13r::vk::Texture OBJLayerTex;
    ds13r::vk::Texture OBJDepthTex;
    ds13r::vk::Texture OutputTex;

    u16 SpritePreVtxData[(3 * 6) * 128];
    u16 SpriteVtxData[(5 * 6) * 256];

    // std140 compliant config struct for the layer shader
    struct sLayerConfig
    {
        u32 uVRAMMask;
        u32 __pad0[3];
        struct sBGConfig
        {
            u32 Size[2];
            u32 Type;
            u32 PalOffset;
            u32 TileOffset;
            u32 MapOffset;
            u32 Clamp;
            u32 __pad0[1];
        } uBGConfig[4];
    } LayerConfig;

    struct sSpriteConfig
    {
        u32 uVRAMMask;
        u32 __pad0[3];
        s32 uRotscale[32][4];
        struct sOAM
        {
            s32 Position[2];
            s32 Flip[2];
            s32 Size[2];
            s32 BoundSize[2];
            u32 OBJMode;
            u32 Type;
            u32 PalOffset;
            u32 TileOffset;
            u32 TileStride;
            u32 Rotscale;
            u32 BGPrio;
            u32 Mosaic;
        } uOAM[128];
    } SpriteConfig;
    int NumSprites;
    bool SpriteUseMosaic;

    struct sScanlineConfig
    {
        struct sScanline
        {
            s32 BGOffset[4][4];     // really [4][2]
            s32 BGRotscale[2][4];
            u32 BackColor;
            u32 WinRegs;
            u32 WinMask;
            u32 __pad0[1];
            s32 WinPos[4];
            u32 BGMosaicEnable[4];
            s32 MosaicSize[4];
        } uScanline[192];
    } ScanlineConfig;

    struct sSpriteScanlineConfig
    {
        s32 uMosaicLine[192];
    } SpriteScanlineConfig;

    struct sCompositorConfig
    {
        u32 uBGPrio[4];
        u32 uEnableOBJ;
        u32 uEnable3D;
        u32 uBlendCnt;
        u32 uBlendEffect;
        u32 uBlendCoef[4];
    } CompositorConfig;

    int LastLine;
    bool UnitEnabled;

    u32 DispCnt;
    u8 LayerEnable;
    u8 OBJEnable;
    u8 ForcedBlank;
    u16 BGCnt[4];
    u16 BlendCnt;
    u8 EVA, EVB, EVY;

    u32 BGVRAMRange[4][4];
    bool LayerConfigDirty;

    int LastSpriteLine;
    u16 OAM[512];

    u32 SpriteDispCnt;
    bool SpriteConfigDirty;
    bool SpriteDirty;

    u16 TempPalBuffer[256 * (1 + (4*16))];

    bool IsScreenOn();
    void UpdateAndRender(int line);
    void UpdateScanlineConfig(int line);
    void UpdateLayerConfig();
    void UpdateOAM(int ystart, int yend);
    void UpdateCompositorConfig();
    void PrerenderSprites();
    void PrerenderLayer(int layer);
    void DoRenderSprites(int line);
    void RenderSprites(bool window, int ystart, int yend, int pass);
    void RenderScreen(int ystart, int yend);
    void UploadVRAMRows(ds13r::vk::Texture& tex, const u8* vram, int start, int end);
};

}
