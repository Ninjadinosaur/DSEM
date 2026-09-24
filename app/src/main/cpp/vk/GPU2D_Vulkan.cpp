// Vulkan port of melonDS's GPU2D_OpenGL.cpp (Copyright 2016-2026 melonDS team,
// GPL-3.0-or-later). The logic is unchanged; each GL operation maps to a Stream call.

#include "GPU2D_Vulkan.h"
#include "GPU_Vulkan.h"
#include "GPU.h"
#include "GPU3D.h"
#include "Platform.h"

#include <cstddef>
#include <cstring>

namespace melonDS
{
using namespace ds13r::vk;

namespace
{
const float kRectVertices[2 * 2 * 3] = {
    0, 1,   1, 0,   1, 1,
    0, 1,   0, 0,   1, 0,
};

TextureDesc ColorTarget(uint32_t w, uint32_t h, uint32_t layers = 1, bool array = false)
{
    TextureDesc d;
    d.width = w;
    d.height = h;
    d.layers = layers;
    d.array = array;
    d.format = VK_FORMAT_R8G8B8A8_UNORM;
    d.renderTarget = true;
    return d;
}
}

// DS colours (RGB555 + bit 15) sampled like GL's RGB5_A1 / UNSIGNED_SHORT_1_5_5_5_REV:
// Vulkan's A1R5G5B5 has red and blue the other way round, so the view swaps them back.
TextureDesc DSColorTexture(uint32_t w, uint32_t h, uint32_t layers, bool array)
{
    TextureDesc d;
    d.width = w;
    d.height = h;
    d.layers = layers;
    d.array = array;
    d.format = VK_FORMAT_A1R5G5B5_UNORM_PACK16;
    d.swizzle = {VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_A};
    return d;
}

VulkanRenderer2D::VulkanRenderer2D(melonDS::GPU2D& gpu2D, VulkanRenderer& parent)
    : Renderer2D(gpu2D), Parent(parent), S(parent.S)
{
    ScaleFactor = 0;
    ScreenW = ScreenH = 0;
    for (auto*& t : BGLayerTex) t = nullptr;
}

bool VulkanRenderer2D::Init()
{
    // textures holding raw BG and OBJ VRAM and palettes
    int bgheight = (GPU2D.Num == 0) ? 512 : 128;
    int objheight = (GPU2D.Num == 0) ? 256 : 128;

    TextureDesc vram;
    vram.width = 1024;
    vram.format = VK_FORMAT_R8_UINT;
    vram.height = bgheight;
    if (!S.CreateTexture(VRAMTex_BG, vram)) return false;
    vram.height = objheight;
    if (!S.CreateTexture(VRAMTex_OBJ, vram)) return false;

    if (!S.CreateTexture(PalTex_BG, DSColorTexture(256, 1 + (4 * 16), 1, false))) return false;
    if (!S.CreateTexture(PalTex_OBJ, DSColorTexture(256, 1 + 16, 1, false))) return false;

    // textures holding pre-rendered BG layers
    const u16 bgsizes[8][3] = {
        {128, 128, 2},
        {256, 256, 4},
        {256, 512, 4},
        {512, 256, 4},
        {512, 512, 4},
        {512, 1024, 1},
        {1024, 512, 1},
        {1024, 1024, 2},
    };
    int l = 0;
    for (int j = 0; j < 8; j++)
        for (int k = 0; k < bgsizes[j][2]; k++)
            if (!S.CreateTexture(AllBGLayerTex[l++], ColorTarget(bgsizes[j][0], bgsizes[j][1]))) return false;

    // pre-rendered sprites
    if (!S.CreateTexture(SpriteTex, ColorTarget(1024, 512))) return false;

    // Uploaded once so the textures are never sampled uninitialised.
    std::vector<u8> zero(1024 * 512, 0);
    S.UploadTexture(VRAMTex_BG, 0, 0, 0, 1024, bgheight, zero.data(), 1);
    S.UploadTexture(VRAMTex_OBJ, 0, 0, 0, 1024, objheight, zero.data(), 1);
    S.UploadTexture(PalTex_BG, 0, 0, 0, 256, 1 + 4 * 16, zero.data(), 2);
    S.UploadTexture(PalTex_OBJ, 0, 0, 0, 256, 1 + 16, zero.data(), 2);
    return true;
}

VulkanRenderer2D::~VulkanRenderer2D()
{
    S.DestroyTexture(VRAMTex_BG);
    S.DestroyTexture(VRAMTex_OBJ);
    S.DestroyTexture(PalTex_BG);
    S.DestroyTexture(PalTex_OBJ);
    for (auto& t : AllBGLayerTex) S.DestroyTexture(t);
    S.DestroyTexture(SpriteTex);
    S.DestroyTexture(OBJLayerTex);
    S.DestroyTexture(OBJDepthTex);
    S.DestroyTexture(OutputTex);
}

void VulkanRenderer2D::Reset()
{
    for (auto*& t : BGLayerTex) t = nullptr;

    memset(&LayerConfig, 0, sizeof(LayerConfig));
    memset(&SpriteConfig, 0, sizeof(SpriteConfig));
    memset(&ScanlineConfig, 0, sizeof(ScanlineConfig));
    memset(&SpriteScanlineConfig, 0, sizeof(SpriteScanlineConfig));
    memset(&CompositorConfig, 0, sizeof(CompositorConfig));

    int bgheight = (GPU2D.Num == 0) ? 512 : 128;
    int objheight = (GPU2D.Num == 0) ? 256 : 128;
    LayerConfig.uVRAMMask = bgheight - 1;
    SpriteConfig.uVRAMMask = objheight - 1;

    LastLine = 0;
    UnitEnabled = false;

    DispCnt = 0;
    LayerEnable = 0;
    OBJEnable = 0;
    ForcedBlank = 0;
    memset(BGCnt, 0, sizeof(BGCnt));
    BlendCnt = 0;
    EVA = 0; EVB = 0; EVY = 0;

    memset(BGVRAMRange, 0xFF, sizeof(BGVRAMRange));
    LayerConfigDirty = true;

    LastSpriteLine = 0;
    memset(OAM, 0, sizeof(OAM));
    NumSprites = 0;
    SpriteUseMosaic = false;

    SpriteDispCnt = 0;
    SpriteConfigDirty = true;
    SpriteDirty = true;

    memset(TempPalBuffer, 0, sizeof(TempPalBuffer));
}

void VulkanRenderer2D::PostSavestate()
{
    Reset();
}

void VulkanRenderer2D::SetScaleFactor(int scale)
{
    if (scale == ScaleFactor) return;

    ScaleFactor = scale;
    ScreenW = 256 * scale;
    ScreenH = 192 * scale;

    S.CreateTexture(OBJLayerTex, ColorTarget(ScreenW, ScreenH, 2, true));
    TextureDesc depth;
    depth.width = ScreenW;
    depth.height = ScreenH;
    depth.format = VK_FORMAT_D16_UNORM;
    depth.depth = true;
    depth.renderTarget = true;
    S.CreateTexture(OBJDepthTex, depth);
    S.CreateTexture(OutputTex, ColorTarget(ScreenW, ScreenH));

    Parent.OutputTex2D[GPU2D.Num] = &OutputTex;
}

bool VulkanRenderer2D::IsScreenOn()
{
    if (!GPU.ScreensEnabled) return false;
    if (!GPU2D.Enabled) return false;
    if (GPU2D.ForcedBlank) return false;

    u16 masterbright = GPU2D.Num ? GPU.MasterBrightnessB : GPU.MasterBrightnessA;
    u16 brightmode = masterbright >> 14;
    u16 brightness = masterbright & 0x1F;
    if ((brightmode == 1 || brightmode == 2) && brightness >= 16)
        return false;

    u16 layers = GPU2D.LayerEnable | 0x20;
    u16 bldeffect = (GPU2D.BlendCnt >> 6) & 0x3;
    u16 bldlayers = GPU2D.BlendCnt & layers & 0x3F;
    if ((bldeffect == 2 || bldeffect == 3) && bldlayers == layers && GPU2D.EVY >= 16 &&
        !(GPU2D.DispCnt & 0xE000))
        return false;

    u32 dispmode = (GPU2D.DispCnt >> 16) & 0x3;
    if (dispmode != 1)
    {
        if (GPU2D.Num) return false;
        if (!GPU.CaptureEnable) return false;
    }

    return true;
}

void VulkanRenderer2D::UploadVRAMRows(Texture& tex, const u8* vram, int start, int end)
{
    S.UploadTexture(tex, 0, 0, start, 1024, end - start, &vram[start * 1024], 1);
}

void VulkanRenderer2D::UpdateAndRender(int line)
{
    u32 palmask = 1 << (GPU2D.Num * 2);

    // check if any 'critical' registers were modified
    u32 dispcnt_diff = GPU2D.DispCnt ^ DispCnt;
    u8 layer_diff = GPU2D.LayerEnable ^ LayerEnable;
    u16 bgcnt_diff[4];
    for (int layer = 0; layer < 4; layer++)
        bgcnt_diff[layer] = GPU2D.BGCnt[layer] ^ BGCnt[layer];

    u8 layer_pre_dirty = 0;
    bool comp_dirty = false;
    bool screenon = IsScreenOn();

    if (dispcnt_diff & 0x8)
        layer_pre_dirty |= 0x1;
    if (dispcnt_diff & 0x7)
        layer_pre_dirty |= 0xC;
    if (dispcnt_diff & 0x7F000000)
        layer_pre_dirty |= 0xF;

    if (dispcnt_diff & 0x0000E008)
        comp_dirty = true;
    else if (layer_diff & 0x1F)
        comp_dirty = true;
    else if (UnitEnabled != GPU2D.Enabled)
        comp_dirty = true;
    else if (ForcedBlank != GPU2D.ForcedBlank)
        comp_dirty = true;

    for (int layer = 0; layer < 4; layer++)
    {
        u16 mask = 0xDFBC;
        if (layer < 2) mask |= (1 << 13);
        if (bgcnt_diff[layer] & mask)
            layer_pre_dirty |= (1 << layer);
        if (bgcnt_diff[layer] & (~mask))
            comp_dirty = true;
    }

    if ((GPU2D.BlendCnt != BlendCnt) ||
        (GPU2D.EVA != EVA) ||
        (GPU2D.EVB != EVB) ||
        (GPU2D.EVY != EVY))
        comp_dirty = true;

    // check if VRAM was modified, and flatten it as needed
    static_assert(VRAMDirtyGranularity == 512);
    NonStupidBitField<1024> bgDirty;
    NonStupidBitField<64> bgExtPalDirty;
    NonStupidBitField<16> objExtPalDirty;

    if (screenon)
    {
        if (GPU2D.Num == 0)
        {
            bgDirty = GPU.VRAMDirty_ABG.DeriveState(GPU.VRAMMap_ABG, GPU);
            GPU.MakeVRAMFlat_ABGCoherent(bgDirty);

            bgExtPalDirty = GPU.VRAMDirty_ABGExtPal.DeriveState(GPU.VRAMMap_ABGExtPal, GPU);
            GPU.MakeVRAMFlat_ABGExtPalCoherent(bgExtPalDirty);
            objExtPalDirty = GPU.VRAMDirty_AOBJExtPal.DeriveState(&GPU.VRAMMap_AOBJExtPal, GPU);
            GPU.MakeVRAMFlat_AOBJExtPalCoherent(objExtPalDirty);
        }
        else
        {
            auto _bgDirty = GPU.VRAMDirty_BBG.DeriveState(GPU.VRAMMap_BBG, GPU);
            GPU.MakeVRAMFlat_BBGCoherent(_bgDirty);
            for (int i = 0; i < 1024; i += 256)
                memcpy(&bgDirty.Data[i>>6], _bgDirty.Data, 256>>3);

            bgExtPalDirty = GPU.VRAMDirty_BBGExtPal.DeriveState(GPU.VRAMMap_BBGExtPal, GPU);
            GPU.MakeVRAMFlat_BBGExtPalCoherent(bgExtPalDirty);
            objExtPalDirty = GPU.VRAMDirty_BOBJExtPal.DeriveState(&GPU.VRAMMap_BOBJExtPal, GPU);
            GPU.MakeVRAMFlat_BOBJExtPalCoherent(objExtPalDirty);
        }
    }

    // for each layer, check if the VRAM and palettes involved are dirty
    for (int layer = 0; layer < 4; layer++)
    {
        const u32* rangeinfo = BGVRAMRange[layer];

        for (int r = 0; r < 4; r+=2)
        {
            if (rangeinfo[r] == 0xFFFFFFFF)
                continue;

            bool dirty = false;
            u32 rstart = (rangeinfo[r] >> 9) & 0x3FF;
            u32 rcount = (rangeinfo[r+1] >> 9);
            if ((rstart + rcount) > 1024)
            {
                dirty = bgDirty.CheckRange(rstart, 1024-rstart) ||
                        bgDirty.CheckRange(0, rcount-(1024-rstart));
            }
            else
                dirty = bgDirty.CheckRange(rstart, rcount);

            if (dirty)
                layer_pre_dirty |= (1 << layer);
        }

        auto& cfg = LayerConfig.uBGConfig[layer];
        if ((cfg.Type == 1 || cfg.Type == 3) && (cfg.PalOffset > 0))
        {
            u32 pal = cfg.PalOffset - 1;
            if (bgExtPalDirty.CheckRange(pal, pal + 16))
                layer_pre_dirty |= (1 << layer);
        }
        else if (cfg.Type <= 4)
        {
            if (GPU.PaletteDirty & palmask)
                layer_pre_dirty |= (1 << layer);
        }
    }

    if (layer_pre_dirty)
        comp_dirty = true;

    if (Parent.NeedPartialRender)
        comp_dirty = true;

    // if needed, render sprites
    if ((comp_dirty || SpriteDirty) && (line > 0))
        DoRenderSprites(line);

    // if needed, composite the previous screen section
    if (comp_dirty && (line > 0))
    {
        RenderScreen(LastLine, line);
        LastLine = line;
    }

    // update registers
    UnitEnabled = GPU2D.Enabled;
    DispCnt = GPU2D.DispCnt;
    LayerEnable = GPU2D.LayerEnable;
    OBJEnable = GPU2D.OBJEnable;
    ForcedBlank = GPU2D.ForcedBlank;
    for (int layer = 0; layer < 4; layer++)
        BGCnt[layer] = GPU2D.BGCnt[layer];
    BlendCnt = GPU2D.BlendCnt;
    EVA = GPU2D.EVA;
    EVB = GPU2D.EVB;
    EVY = GPU2D.EVY;

    if (layer_pre_dirty || LayerConfigDirty)
        UpdateLayerConfig();

    UpdateScanlineConfig(line);

    // update VRAM and palettes
    int dirtybits = GPU2D.Num ? 256 : 1024;
    if (bgDirty.CheckRange(0, dirtybits))
    {
        u8* vram;
        u32 vrammask;
        GPU2D.GetBGVRAM(vram, vrammask);

        int texlen = dirtybits >> 6;
        for (int i = 0; i < texlen; )
        {
            if (!bgDirty.Data[i])
            {
                i++;
                continue;
            }

            int start = i * 32;
            for (;;)
            {
                i++;
                if (i >= texlen) break;
                if (!bgDirty.Data[i]) break;
            }
            int end = i * 32;

            UploadVRAMRows(VRAMTex_BG, vram, start, end);
        }
    }

    if ((GPU.PaletteDirty & palmask) || bgExtPalDirty.CheckRange(0, 64))
    {
        memcpy(&TempPalBuffer[0], &GPU.Palette[GPU2D.Num ? 0x400 : 0], 256*2);
        for (int s = 0; s < 4; s++)
        {
            for (int p = 0; p < 16; p++)
            {
                u16* pal = GPU2D.GetBGExtPal(s, p);
                memcpy(&TempPalBuffer[(1 + ((s*16)+p)) * 256], pal, 256*2);
            }
        }

        S.UploadTexture(PalTex_BG, 0, 0, 0, 256, 1 + (4*16), TempPalBuffer, 2);
    }

    GPU.PaletteDirty &= ~palmask;

    if (layer_pre_dirty)
    {
        // pre-render BG layers with the new settings
        for (int layer = 0; layer < 4; layer++)
        {
            if (!(layer_pre_dirty & (1 << layer)))
                continue;

            PrerenderLayer(layer);
        }
    }

    if (SpriteDirty)
    {
        // OAM and VRAM have already been updated prior; the palette is updated here
        NumSprites = 0;
        SpriteUseMosaic = false;
        UpdateOAM(0, 192);

        memcpy(&TempPalBuffer[0], &GPU.Palette[GPU2D.Num ? 0x600 : 0x200], 256*2);
        {
            u16* pal = GPU2D.GetOBJExtPal();
            memcpy(&TempPalBuffer[256], pal, 256*16*2);
        }

        S.UploadTexture(PalTex_OBJ, 0, 0, 0, 256, 1 + 16, TempPalBuffer, 2);

        PrerenderSprites();

        LastSpriteLine = line;
    }

    LayerConfigDirty = false;
    SpriteDirty = false;
}

void VulkanRenderer2D::DrawScanline(u32 line)
{
    UpdateAndRender(line);
}

void VulkanRenderer2D::VBlank()
{
    DoRenderSprites(192);
    RenderScreen(LastLine, 192);

    LastSpriteLine = 0;
    LastLine = 0;
}

void VulkanRenderer2D::VBlankEnd()
{
}

void VulkanRenderer2D::UpdateScanlineConfig(int line)
{
    auto& cfg = ScanlineConfig.uScanline[line];

    // update BG layer coordinates
    // Y coordinates are adjusted to account for vertical mosaic
    // horizontal mosaic will be done during compositing

    u32 bgmode = DispCnt & 0x7;
    bool xmosaic = (GPU2D.BGMosaicSize[0] > 0);

    if (DispCnt & (1<<3))
    {
        // 3D layer
        int xpos = GPU.GPU3D.GetRenderXPos() & 0x1FF;
        cfg.BGOffset[0][0] = xpos - ((xpos & 0x100) << 1);
        cfg.BGOffset[0][1] = line;
        cfg.BGMosaicEnable[0] = false;
    }
    else
    {
        // text layer
        cfg.BGOffset[0][0] = GPU2D.BGXPos[0];
        if (GPU2D.BGCnt[0] & (1<<6))
        {
            cfg.BGOffset[0][1] = GPU2D.BGYPos[0] + GPU2D.BGMosaicLine;
            cfg.BGMosaicEnable[0] = xmosaic;
        }
        else
        {
            cfg.BGOffset[0][1] = GPU2D.BGYPos[0] + line;
            cfg.BGMosaicEnable[0] = false;
        }
    }

    // always a text layer
    cfg.BGOffset[1][0] = GPU2D.BGXPos[1];
    if (GPU2D.BGCnt[1] & (1<<6))
    {
        cfg.BGOffset[1][1] = GPU2D.BGYPos[1] + GPU2D.BGMosaicLine;
        cfg.BGMosaicEnable[1] = xmosaic;
    }
    else
    {
        cfg.BGOffset[1][1] = GPU2D.BGYPos[1] + line;
        cfg.BGMosaicEnable[1] = false;
    }

    if ((bgmode == 2) || (bgmode >= 4 && bgmode <= 6))
    {
        // rotscale layer
        cfg.BGOffset[2][0] = GPU2D.BGXRefInternal[0];
        cfg.BGOffset[2][1] = GPU2D.BGYRefInternal[0];
        cfg.BGRotscale[0][0] = GPU2D.BGRotA[0];
        cfg.BGRotscale[0][1] = GPU2D.BGRotB[0];
        cfg.BGRotscale[0][2] = GPU2D.BGRotC[0];
        cfg.BGRotscale[0][3] = GPU2D.BGRotD[0];
    }
    else
    {
        // text layer
        cfg.BGOffset[2][0] = GPU2D.BGXPos[2];
        if (GPU2D.BGCnt[2] & (1<<6))
            cfg.BGOffset[2][1] = GPU2D.BGYPos[2] + GPU2D.BGMosaicLine;
        else
            cfg.BGOffset[2][1] = GPU2D.BGYPos[2] + line;
    }

    if (GPU2D.BGCnt[2] & (1<<6))
        cfg.BGMosaicEnable[2] = xmosaic;
    else
        cfg.BGMosaicEnable[2] = false;

    if (bgmode >= 1 && bgmode <= 5)
    {
        // rotscale layer
        cfg.BGOffset[3][0] = GPU2D.BGXRefInternal[1];
        cfg.BGOffset[3][1] = GPU2D.BGYRefInternal[1];
        cfg.BGRotscale[1][0] = GPU2D.BGRotA[1];
        cfg.BGRotscale[1][1] = GPU2D.BGRotB[1];
        cfg.BGRotscale[1][2] = GPU2D.BGRotC[1];
        cfg.BGRotscale[1][3] = GPU2D.BGRotD[1];
    }
    else
    {
        // text layer
        cfg.BGOffset[3][0] = GPU2D.BGXPos[3];
        if (GPU2D.BGCnt[3] & (1<<6))
            cfg.BGOffset[3][1] = GPU2D.BGYPos[3] + GPU2D.BGMosaicLine;
        else
            cfg.BGOffset[3][1] = GPU2D.BGYPos[3] + line;
    }

    if (GPU2D.BGCnt[3] & (1<<6))
        cfg.BGMosaicEnable[3] = xmosaic;
    else
        cfg.BGMosaicEnable[3] = false;

    u16* pal = (u16*)&GPU.Palette[GPU2D.Num ? 0x400 : 0];
    cfg.BackColor = pal[0];

    // mosaic
    cfg.MosaicSize[0] = GPU2D.BGMosaicSize[0];
    cfg.MosaicSize[1] = GPU2D.BGMosaicSize[1];
    cfg.MosaicSize[2] = GPU2D.OBJMosaicSize[0];
    cfg.MosaicSize[3] = GPU2D.OBJMosaicSize[1];

    // windows
    if (GPU2D.DispCnt & 0xE000)
        cfg.WinRegs = GPU2D.WinCnt[2];
    else
        cfg.WinRegs = 0xFF;

    if (GPU2D.DispCnt & (1<<15))
        cfg.WinRegs |= (GPU2D.WinCnt[3] << 8);
    else
        cfg.WinRegs |= 0xFF00;

    if (GPU2D.DispCnt & (1<<14))
        cfg.WinRegs |= (GPU2D.WinCnt[1] << 16);
    else
        cfg.WinRegs |= 0xFF0000;

    if (GPU2D.DispCnt & (1<<13))
        cfg.WinRegs |= (GPU2D.WinCnt[0] << 24);
    else
        cfg.WinRegs |= 0xFF000000;

    cfg.WinMask = 0;

    if ((GPU2D.DispCnt & (1<<13)) && (GPU2D.Win0Active & 0x1))
    {
        int x0 = GPU2D.Win0Coords[0];
        int x1 = GPU2D.Win0Coords[1];

        if (x0 <= x1)
        {
            cfg.WinPos[0] = x0;
            cfg.WinPos[1] = x1;
            if (GPU2D.Win0Active == 0x3)
                cfg.WinMask |= (1<<0);
            cfg.WinMask |= (1<<1);
            GPU2D.Win0Active &= ~0x2;
        }
        else
        {
            cfg.WinPos[0] = x1;
            cfg.WinPos[1] = x0;
            if (GPU2D.Win0Active == 0x3)
                cfg.WinMask |= (1<<0);
            cfg.WinMask |= (1<<2);
            GPU2D.Win0Active |= 0x2;
        }
    }
    else
    {
        cfg.WinPos[0] = 256;
        cfg.WinPos[1] = 256;
    }

    if ((GPU2D.DispCnt & (1<<14)) && (GPU2D.Win1Active & 0x1))
    {
        int x0 = GPU2D.Win1Coords[0];
        int x1 = GPU2D.Win1Coords[1];

        if (x0 <= x1)
        {
            cfg.WinPos[2] = x0;
            cfg.WinPos[3] = x1;
            if (GPU2D.Win1Active == 0x3)
                cfg.WinMask |= (1<<3);
            cfg.WinMask |= (1<<4);
            GPU2D.Win1Active &= ~0x2;
        }
        else
        {
            cfg.WinPos[2] = x1;
            cfg.WinPos[3] = x0;
            if (GPU2D.Win1Active == 0x3)
                cfg.WinMask |= (1<<3);
            cfg.WinMask |= (1<<5);
            GPU2D.Win1Active |= 0x2;
        }
    }
    else
    {
        cfg.WinPos[2] = 256;
        cfg.WinPos[3] = 256;
    }
}

void VulkanRenderer2D::UpdateLayerConfig()
{
    // determine which parts of VRAM were used for captures
    int capturemask = GPU2D.Num ? 0x7 : 0x1F;
    int captureinfo[32];
    GPU2D.GetCaptureInfo_BG(captureinfo);

    u32 tilebase, mapbase;
    if (!GPU2D.Num)
    {
        tilebase = ((GPU2D.DispCnt >> 24) & 0x7) << 16;
        mapbase = ((GPU2D.DispCnt >> 27) & 0x7) << 16;
    }
    else
    {
        tilebase = 0;
        mapbase = 0;
    }

    int layertype[4] = {1, 1, 0, 0};
    switch (GPU2D.DispCnt & 0x7)
    {
        case 0: layertype[2] = 1; layertype[3] = 1; break;
        case 1: layertype[2] = 1; layertype[3] = 2; break;
        case 2: layertype[2] = 2; layertype[3] = 2; break;
        case 3: layertype[2] = 1; layertype[3] = 3; break;
        case 4: layertype[2] = 2; layertype[3] = 3; break;
        case 5: layertype[2] = 3; layertype[3] = 3; break;
        case 6: layertype[0] = 0; layertype[1] = 0;
                layertype[2] = 4; layertype[3] = 0; break;
        case 7: layertype[2] = 0; layertype[3] = 0; break;
    }

    for (int layer = 0; layer < 4; layer++)
    {
        int type = layertype[layer];
        if (!type)
            continue;

        u16 bgcnt = GPU2D.BGCnt[layer];
        auto& cfg = LayerConfig.uBGConfig[layer];

        cfg.TileOffset = tilebase + (((bgcnt >> 2) & 0xF) << 14);
        cfg.MapOffset = mapbase + (((bgcnt >> 8) & 0x1F) << 11);
        cfg.PalOffset = 0;

        BGVRAMRange[layer][0] = cfg.TileOffset;
        BGVRAMRange[layer][2] = cfg.MapOffset;

        if ((layer == 0) && (GPU2D.DispCnt & (1<<3)))
        {
            // 3D layer
            cfg.Size[0] = 256; cfg.Size[1] = 192;
            cfg.Type = 6;
            cfg.Clamp = 1;

            BGVRAMRange[layer][0] = 0xFFFFFFFF;
            BGVRAMRange[layer][1] = 0xFFFFFFFF;
            BGVRAMRange[layer][2] = 0xFFFFFFFF;
            BGVRAMRange[layer][3] = 0xFFFFFFFF;
        }
        else if (type == 1)
        {
            // text layer
            u32 tilesz, mapsz = 0;
            switch (bgcnt >> 14)
            {
                case 0: cfg.Size[0] = 256; cfg.Size[1] = 256; mapsz = 0x800; break;
                case 1: cfg.Size[0] = 512; cfg.Size[1] = 256; mapsz = 0x1000; break;
                case 2: cfg.Size[0] = 256; cfg.Size[1] = 512; mapsz = 0x1000; break;
                case 3: cfg.Size[0] = 512; cfg.Size[1] = 512; mapsz = 0x2000; break;
            }

            if (bgcnt & (1<<7))
            {
                // 256-color
                cfg.Type = 1;
                if (DispCnt & (1<<30))
                {
                    // extended palette
                    int paloff = layer;
                    if ((layer < 2) && (bgcnt & (1<<13)))
                        paloff += 2;
                    cfg.PalOffset = 1 + (16 * paloff);
                }

                tilesz = 0x10000;
            }
            else
            {
                // 16-color
                cfg.Type = 0;
                tilesz = 0x8000;
            }

            cfg.Clamp = 0;

            int n = BGBaseIndex[0][bgcnt >> 14] + layer;
            BGLayerTex[layer] = &AllBGLayerTex[n];

            BGVRAMRange[layer][1] = tilesz;
            BGVRAMRange[layer][3] = mapsz;
        }
        else if (type == 2)
        {
            // affine layer
            u32 mapsz = 0;
            switch (bgcnt >> 14)
            {
                case 0: cfg.Size[0] = 128; cfg.Size[1] = 128; mapsz = 0x100; break;
                case 1: cfg.Size[0] = 256; cfg.Size[1] = 256; mapsz = 0x400; break;
                case 2: cfg.Size[0] = 512; cfg.Size[1] = 512; mapsz = 0x1000; break;
                case 3: cfg.Size[0] = 1024; cfg.Size[1] = 1024; mapsz = 0x4000; break;
            }

            cfg.Type = 2;
            cfg.Clamp = !(bgcnt & (1<<13));

            int n = BGBaseIndex[1][bgcnt >> 14] + layer - 2;
            BGLayerTex[layer] = &AllBGLayerTex[n];

            BGVRAMRange[layer][1] = 0x4000;
            BGVRAMRange[layer][3] = mapsz;
        }
        else if (type == 3)
        {
            // extended layer
            if (bgcnt & (1<<7))
            {
                // bitmap modes
                u32 mapsz = 0;
                switch (bgcnt >> 14)
                {
                    case 0: cfg.Size[0] = 128; cfg.Size[1] = 128; mapsz = 0x4000; break;
                    case 1: cfg.Size[0] = 256; cfg.Size[1] = 256; mapsz = 0x10000; break;
                    case 2: cfg.Size[0] = 512; cfg.Size[1] = 256; mapsz = 0x20000; break;
                    case 3: cfg.Size[0] = 512; cfg.Size[1] = 512; mapsz = 0x40000; break;
                }

                u32 tileoffset = 0;
                u32 mapoffset = ((bgcnt >> 8) & 0x1F) << 14;

                BGVRAMRange[layer][0] = 0xFFFFFFFF;
                BGVRAMRange[layer][1] = 0xFFFFFFFF;
                BGVRAMRange[layer][2] = mapoffset;
                BGVRAMRange[layer][3] = mapsz;

                if (bgcnt & (1<<2))
                {
                    mapsz <<= 1;

                    int capblock = -1;
                    if ((cfg.Size[0] == 128) || (cfg.Size[0] == 256))
                    {
                        // if this is a direct color bitmap, and the width is 128 or 256
                        // then it might be a display capture
                        u32 startaddr = mapoffset;
                        u32 endaddr = startaddr + mapsz;

                        startaddr >>= 14;
                        endaddr = (endaddr + 0x3FFF) >> 14;

                        for (u32 b = startaddr; b < endaddr; b++)
                        {
                            int blk = captureinfo[b & capturemask];
                            if (blk == -1) continue;

                            capblock = blk;
                        }
                    }

                    if (capblock != -1)
                    {
                        if (cfg.Size[0] == 128)
                        {
                            cfg.Type = 7;
                            tileoffset = capblock;
                            mapoffset = (mapoffset >> 8) & 0x7F;
                        }
                        else
                        {
                            cfg.Type = 8;
                            tileoffset = capblock >> 2;
                            mapoffset = (mapoffset >> 9) & 0xFF;
                        }
                    }
                    else
                        cfg.Type = 5;
                }
                else
                    cfg.Type = 4;

                cfg.TileOffset = tileoffset;
                cfg.MapOffset = mapoffset;

                int n = BGBaseIndex[2][bgcnt >> 14] + layer - 2;
                BGLayerTex[layer] = &AllBGLayerTex[n];
            }
            else
            {
                // rotscale w/ tiles
                u32 mapsz = 0;
                switch (bgcnt >> 14)
                {
                    case 0: cfg.Size[0] = 128; cfg.Size[1] = 128; mapsz = 0x200; break;
                    case 1: cfg.Size[0] = 256; cfg.Size[1] = 256; mapsz = 0x800; break;
                    case 2: cfg.Size[0] = 512; cfg.Size[1] = 512; mapsz = 0x2000; break;
                    case 3: cfg.Size[0] = 1024; cfg.Size[1] = 1024; mapsz = 0x8000; break;
                }

                // this layer type is always 256-color
                cfg.Type = 3;
                if (DispCnt & (1<<30))
                {
                    // extended palette
                    int paloff = layer;
                    if ((layer < 2) && (bgcnt & (1<<13)))
                        paloff += 2;
                    cfg.PalOffset = 1 + (16 * paloff);
                }

                int n = BGBaseIndex[1][bgcnt >> 14] + layer - 2;
                BGLayerTex[layer] = &AllBGLayerTex[n];

                BGVRAMRange[layer][1] = 0x10000;
                BGVRAMRange[layer][3] = mapsz;
            }

            cfg.Clamp = !(bgcnt & (1<<13));
        }
        else //if (type == 4)
        {
            // large layer
            u32 mapsz = 0;
            switch (bgcnt >> 14)
            {
                case 0: cfg.Size[0] = 512; cfg.Size[1] = 1024; mapsz = 0x80000; break;
                case 1: cfg.Size[0] = 1024; cfg.Size[1] = 512; mapsz = 0x80000; break;
                case 2: cfg.Size[0] = 512; cfg.Size[1] = 256; mapsz = 0x20000; break;
                case 3: cfg.Size[0] = 512; cfg.Size[1] = 512; mapsz = 0x40000; break;
            }

            cfg.Type = 4;
            cfg.TileOffset = 0;
            cfg.MapOffset = 0;
            cfg.Clamp = !(bgcnt & (1<<13));

            int n = BGBaseIndex[3][bgcnt >> 14];
            BGLayerTex[layer] = &AllBGLayerTex[n];

            BGVRAMRange[layer][0] = 0xFFFFFFFF;
            BGVRAMRange[layer][1] = 0xFFFFFFFF;
            BGVRAMRange[layer][3] = mapsz;
        }
    }
    // (GL uploaded the UBO here; the Vulkan port uploads the current copy with each draw.)
}

void VulkanRenderer2D::UpdateOAM(int ystart, int yend)
{
    auto& cfg = SpriteConfig;
    u16* oam = OAM;

    // determine which parts of VRAM were used for captures
    int capturemask = GPU2D.Num ? 0x7 : 0xF;
    int captureinfo[16];
    GPU2D.GetCaptureInfo_OBJ(captureinfo);

    for (int i = 0; i < 32; i++)
    {
        s16* rotscale = (s16*)&oam[(i * 16) + 3];
        auto& rotdst = cfg.uRotscale[i];

        rotdst[0] = rotscale[0];
        rotdst[1] = rotscale[4];
        rotdst[2] = rotscale[8];
        rotdst[3] = rotscale[12];
    }

    const u8 spritewidth[16] =
    {
        8, 16, 8, 8,
        16, 32, 8, 8,
        32, 32, 16, 8,
        64, 64, 32, 8
    };
    const u8 spriteheight[16] =
    {
        8, 8, 16, 8,
        16, 8, 32, 8,
        32, 16, 32, 8,
        64, 32, 64, 8
    };

    for (int sprnum = 0; sprnum < 128; sprnum++)
    {
        u16* attrib = &oam[sprnum * 4];

        u32 sprtype = (attrib[0] >> 8) & 0x3;
        if (sprtype == 2) // sprite disabled
            continue;

        // X > 255 is interpreted as negative (-256..-1)
        // Y > 127 is interpreted as both positive (128..255) and negative (-128..-1)
        s32 xpos = (s32)(attrib[1] << 23) >> 23;
        s32 ypos = (s32)(attrib[0] << 24) >> 24;

        u32 sizeparam = (attrib[0] >> 14) | ((attrib[1] & 0xC000) >> 12);
        s32 width = spritewidth[sizeparam];
        s32 height = spriteheight[sizeparam];
        s32 boundwidth = width;
        s32 boundheight = height;

        if (sprtype == 3)
        {
            // double-size rotscale sprite
            boundwidth <<= 1;
            boundheight <<= 1;
        }

        if (xpos <= -boundwidth)
            continue;

        bool yc0 = ((ypos + boundheight) > ystart) && (ypos < yend);
        bool yc1 = (((ypos&0xFF) + boundheight) > ystart) && ((ypos&0xFF) < yend);
        if (!(yc0 || yc1))
            continue;

        u32 sprmode = (attrib[0] >> 10) & 0x3;
        if (sprmode == 3)
        {
            if ((GPU2D.DispCnt & 0x60) == 0x60)
                continue;
            if ((attrib[2] >> 12) == 0)
                continue;
        }

        if (NumSprites >= 128)
        {
            Platform::Log(Platform::LogLevel::Error, "GPU2D_Vulkan: SPRITE BUFFER IS FULL!!!!!\n");
            break;
        }

        auto& sprcfg = cfg.uOAM[NumSprites];

        sprcfg.Position[0] = (u32)xpos;
        sprcfg.Position[1] = (u32)ypos;
        sprcfg.Size[0] = width;
        sprcfg.Size[1] = height;
        sprcfg.BoundSize[0] = boundwidth;
        sprcfg.BoundSize[1] = boundheight;

        if (sprtype & 1)
        {
            sprcfg.Flip[0] = 0;
            sprcfg.Flip[1] = 0;
            sprcfg.Rotscale = (attrib[1] >> 9) & 0x1F;
        }
        else
        {
            sprcfg.Flip[0] = !!(attrib[1] & (1<<12));
            sprcfg.Flip[1] = !!(attrib[1] & (1<<13));
            sprcfg.Rotscale = (u32)-1;
        }

        sprcfg.OBJMode = sprmode;
        sprcfg.Mosaic = !!(attrib[0] & (1<<12)) && (sprmode != 2);
        sprcfg.BGPrio = (attrib[2] >> 10) & 0x3;

        u32 tilenum = attrib[2] & 0x3FF;

        if (sprmode == 3)
        {
            // bitmap sprite
            sprcfg.Type = 2;

            if (GPU2D.DispCnt & (1<<6))
            {
                // 1D mapping
                sprcfg.TileOffset = tilenum << (7 + ((GPU2D.DispCnt >> 22) & 0x1));
                sprcfg.TileStride = width * 2;
            }
            else
            {
                bool is256 = !!(GPU2D.DispCnt & (1<<5));
                int capblock = -1;

                u32 tileoffset, tilestride;
                if (is256)
                {
                    // 2D mapping, 256 pixels
                    tileoffset = ((tilenum & 0x01F) << 4) + ((tilenum & 0x3E0) << 7);
                    tilestride = 256 * 2;
                }
                else
                {
                    // 2D mapping, 128 pixels
                    tileoffset = ((tilenum & 0x00F) << 4) + ((tilenum & 0x3F0) << 7);
                    tilestride = 128 * 2;
                }

                // if this is a direct color bitmap, and the width is 128 or 256
                // then it might be a display capture
                u32 startaddr = tileoffset;
                u32 endaddr = startaddr + (height * tilestride);

                startaddr >>= 14;
                endaddr = (endaddr + 0x3FFF) >> 14;

                for (u32 b = startaddr; b < endaddr; b++)
                {
                    int blk = captureinfo[b & capturemask];
                    if (blk == -1) continue;

                    capblock = blk;
                }

                if (capblock != -1)
                {
                    if (!is256)
                    {
                        sprcfg.Type = 3;
                        tilestride = capblock;
                        tileoffset &= 0x7FFF;
                    }
                    else
                    {
                        sprcfg.Type = 4;
                        tilestride = capblock >> 2;
                        tileoffset &= 0x1FFFF;
                    }
                }

                sprcfg.TileOffset = tileoffset;
                sprcfg.TileStride = tilestride;
            }

            sprcfg.PalOffset = 1 + (attrib[2] >> 12); // alpha
        }
        else
        {
            if (GPU2D.DispCnt & (1<<4))
            {
                // 1D mapping
                sprcfg.TileOffset = tilenum << (5 + ((GPU2D.DispCnt >> 20) & 0x3));
                sprcfg.TileStride = (width >> 3) * 32;
                if (attrib[0] & (1<<13))
                    sprcfg.TileStride <<= 1;
            }
            else
            {
                // 2D mapping
                sprcfg.TileOffset = tilenum << 5;
                sprcfg.TileStride = 32 * 32;
            }

            if (attrib[0] & (1<<13))
            {
                // 256-color sprite
                sprcfg.Type = 1;
                if (GPU2D.DispCnt & (1<<31))
                    sprcfg.PalOffset = 1 + (attrib[2] >> 12);
                else
                    sprcfg.PalOffset = 0;
            }
            else
            {
                // 16-color sprite
                sprcfg.Type = 0;
                sprcfg.PalOffset = (attrib[2] >> 12) << 4;
            }
        }

        NumSprites++;

        if (sprcfg.Mosaic && (GPU2D.OBJMosaicSize[0] > 0))
            SpriteUseMosaic = true;
    }
}

void VulkanRenderer2D::UpdateCompositorConfig()
{
    for (int i = 0; i < 4; i++)
        CompositorConfig.uBGPrio[i] = -1;

    for (int layer = 0; layer < 4; layer++)
    {
        if (!(LayerEnable & (1 << layer)))
            continue;

        int prio = BGCnt[layer] & 0x3;
        CompositorConfig.uBGPrio[layer] = prio;
    }

    CompositorConfig.uEnableOBJ = !!(LayerEnable & (1<<4));
    CompositorConfig.uEnable3D = !!(DispCnt & (1<<3));

    CompositorConfig.uBlendCnt = BlendCnt;
    CompositorConfig.uBlendEffect = (BlendCnt >> 6) & 0x3;
    CompositorConfig.uBlendCoef[0] = EVA;
    CompositorConfig.uBlendCoef[1] = EVB;
    CompositorConfig.uBlendCoef[2] = EVY;
}

void VulkanRenderer2D::PrerenderSprites()
{
    u16* vtxbuf = SpritePreVtxData;
    int vtxnum = 0;

    for (int i = 0; i < NumSprites; i++)
    {
        auto& sprite = SpriteConfig.uOAM[i];
        if (sprite.Type >= 3)
            continue;

        *vtxbuf++ = 0; *vtxbuf++ = 1; *vtxbuf++ = i;
        *vtxbuf++ = 1; *vtxbuf++ = 0; *vtxbuf++ = i;
        *vtxbuf++ = 1; *vtxbuf++ = 1; *vtxbuf++ = i;
        *vtxbuf++ = 0; *vtxbuf++ = 1; *vtxbuf++ = i;
        *vtxbuf++ = 0; *vtxbuf++ = 0; *vtxbuf++ = i;
        *vtxbuf++ = 1; *vtxbuf++ = 0; *vtxbuf++ = i;
        vtxnum += 6;
    }

    if (vtxnum == 0) return;

    S.BeginRendering({{&SpriteTex, 0}});
    S.SetViewport(0, 0, 1024, 512);
    S.SetScissor(0, 0, 1024, 512);
    const auto& prog = Parent.P.SpritePre;
    S.Draw(prog.pipeline, prog.layout, prog.set,
           {{0, 0, &VRAMTex_OBJ, SamplerMode::NearestClampEdge},
            {1, 0, &PalTex_OBJ, SamplerMode::NearestClampEdge},
            {2, 0, nullptr, SamplerMode::NearestClampEdge, &SpriteConfig, sizeof(SpriteConfig)}},
           nullptr, 0, SpritePreVtxData, vtxnum * 3 * sizeof(u16), vtxnum);
}

void VulkanRenderer2D::PrerenderLayer(int layer)
{
    auto& cfg = LayerConfig.uBGConfig[layer];

    if (cfg.Type >= 6)
        return;
    if (!BGLayerTex[layer])
        return;

    S.BeginRendering({{BGLayerTex[layer], 0}});
    // set layer size
    S.SetViewport(0, 0, cfg.Size[0], cfg.Size[1]);
    S.SetScissor(0, 0, cfg.Size[0], cfg.Size[1]);

    int32_t curBG = layer;
    const auto& prog = Parent.P.LayerPre;
    S.Draw(prog.pipeline, prog.layout, prog.set,
           {{0, 0, &VRAMTex_BG, SamplerMode::NearestClampEdge},
            {1, 0, &PalTex_BG, SamplerMode::NearestClampEdge},
            {2, 0, nullptr, SamplerMode::NearestClampEdge, &LayerConfig, sizeof(LayerConfig)}},
           &curBG, sizeof(curBG), kRectVertices, sizeof(kRectVertices), 6);
}

void VulkanRenderer2D::DoRenderSprites(int line)
{
    int ystart = LastSpriteLine;
    int yend = line;
    if (yend <= ystart) return;

    S.BeginRendering({{&OBJLayerTex, 0}, {&OBJLayerTex, 1}}, {&OBJDepthTex, 0});
    S.SetViewport(0, 0, ScreenW, ScreenH);
    S.SetScissor(0, ystart * ScaleFactor, ScreenW, (yend - ystart) * ScaleFactor);

    // NOTE
    // this requires two passes for mosaic emulation, because mosaic flags get set for
    // transparent pixels too, and priority is only checked against opaque pixels
    S.ClearColor(0, 0, 0, 0, 0);
    S.ClearColor(1, 0, 0, 0, 0);
    S.ClearDepth(1.0f);

    if (SpriteUseMosaic)
        RenderSprites(false, ystart, yend, 0);

    RenderSprites(true, ystart, yend, 1);

    RenderSprites(false, ystart, yend, 2);
}

void VulkanRenderer2D::RenderSprites(bool window, int ystart, int yend, int pass)
{
    if (window)
    {
        if (!(GPU2D.DispCnt & (1<<15)))
            return;
    }

    u16* vtxbuf = SpriteVtxData;
    int vtxnum = 0;

    for (int i = 0; i < NumSprites; i++)
    {
        auto& sprite = SpriteConfig.uOAM[i];

        bool iswin = (sprite.OBJMode == 2);
        if (iswin != window)
            continue;

        s32 xpos = sprite.Position[0];
        s32 ypos = sprite.Position[1];
        s32 boundwidth = sprite.BoundSize[0];
        s32 boundheight = sprite.BoundSize[1];

        bool yc0 = ((ypos + boundheight) > ystart) && (ypos < yend);
        bool yc1 = (((ypos&0xFF) + boundheight) > ystart) && ((ypos&0xFF) < yend);

        if (yc0)
        {
            s32 x0 = xpos, x1 = xpos + boundwidth;
            s32 y0 = ypos, y1 = ypos + boundheight;

            *vtxbuf++ = x0; *vtxbuf++ = y1; *vtxbuf++ = 0; *vtxbuf++ = 1; *vtxbuf++ = i;
            *vtxbuf++ = x1; *vtxbuf++ = y0; *vtxbuf++ = 1; *vtxbuf++ = 0; *vtxbuf++ = i;
            *vtxbuf++ = x1; *vtxbuf++ = y1; *vtxbuf++ = 1; *vtxbuf++ = 1; *vtxbuf++ = i;
            *vtxbuf++ = x0; *vtxbuf++ = y1; *vtxbuf++ = 0; *vtxbuf++ = 1; *vtxbuf++ = i;
            *vtxbuf++ = x0; *vtxbuf++ = y0; *vtxbuf++ = 0; *vtxbuf++ = 0; *vtxbuf++ = i;
            *vtxbuf++ = x1; *vtxbuf++ = y0; *vtxbuf++ = 1; *vtxbuf++ = 0; *vtxbuf++ = i;
            vtxnum += 6;
        }

        if (yc1)
        {
            ypos &= 0xFF;
            s32 x0 = xpos, x1 = xpos + boundwidth;
            s32 y0 = ypos, y1 = ypos + boundheight;

            *vtxbuf++ = x0; *vtxbuf++ = y1; *vtxbuf++ = 0; *vtxbuf++ = 1; *vtxbuf++ = i;
            *vtxbuf++ = x1; *vtxbuf++ = y0; *vtxbuf++ = 1; *vtxbuf++ = 0; *vtxbuf++ = i;
            *vtxbuf++ = x1; *vtxbuf++ = y1; *vtxbuf++ = 1; *vtxbuf++ = 1; *vtxbuf++ = i;
            *vtxbuf++ = x0; *vtxbuf++ = y1; *vtxbuf++ = 0; *vtxbuf++ = 1; *vtxbuf++ = i;
            *vtxbuf++ = x0; *vtxbuf++ = y0; *vtxbuf++ = 0; *vtxbuf++ = 0; *vtxbuf++ = i;
            *vtxbuf++ = x1; *vtxbuf++ = y0; *vtxbuf++ = 1; *vtxbuf++ = 0; *vtxbuf++ = i;
            vtxnum += 6;
        }
    }

    if (vtxnum == 0) return;

    // pass 0: mosaic flags for transparent pixels; 1: object window; 2: sprites with priority
    int32_t renderTransparent = (pass == 0) ? 1 : 0;
    const auto& prog = Parent.P.Sprite[pass];
    S.Draw(prog.pipeline, prog.layout, prog.set,
           {{0, 0, &SpriteTex, SamplerMode::NearestClampEdge},
            {1, 0, &Parent.CaptureOutput128Tex, SamplerMode::NearestRepeat},
            {2, 0, &Parent.CaptureOutput256Tex, SamplerMode::NearestRepeat},
            {3, 0, nullptr, SamplerMode::NearestClampEdge, &SpriteConfig, sizeof(SpriteConfig)},
            {4, 0, nullptr, SamplerMode::NearestClampEdge, &SpriteScanlineConfig, sizeof(SpriteScanlineConfig)}},
           &renderTransparent, sizeof(renderTransparent), SpriteVtxData, vtxnum * 5 * sizeof(u16), vtxnum);
}

void VulkanRenderer2D::RenderScreen(int ystart, int yend)
{
    if (yend <= ystart) return;
    S.BeginRendering({{&OutputTex, 0}});
    S.SetViewport(0, 0, ScreenW, ScreenH);
    S.SetScissor(0, ystart * ScaleFactor, ScreenW, (yend - ystart) * ScaleFactor);

    if (ForcedBlank || !UnitEnabled)
    {
        if (!UnitEnabled && !GPU2D.Num)
            S.ClearColor(0, 0, 0, 0, 1);
        else
            S.ClearColor(0, 1, 1, 1, 1);
        return;
    }

    UpdateCompositorConfig();

    std::vector<Binding> bindings;
    for (int i = 0; i < 4; i++)
    {
        Texture* tex = BGLayerTex[i];
        if ((i == 0) && (DispCnt & (1<<3)))
            tex = Parent.OutputTex3D;
        if (!tex) tex = &AllBGLayerTex[0]; // unused layer: any valid image
        SamplerMode mode = LayerConfig.uBGConfig[i].Clamp ? SamplerMode::NearestClampBorder : SamplerMode::NearestRepeat;
        bindings.push_back({0, (uint32_t)i, tex, mode});
    }
    bindings.push_back({1, 0, &OBJLayerTex, SamplerMode::NearestClampEdge});
    bindings.push_back({2, 0, &Parent.CaptureOutput128Tex, SamplerMode::NearestRepeat});
    bindings.push_back({3, 0, &Parent.CaptureOutput256Tex, SamplerMode::NearestRepeat});
    bindings.push_back({4, 0, &Parent.MosaicTex, SamplerMode::NearestClampEdge});
    bindings.push_back({5, 0, nullptr, SamplerMode::NearestClampEdge, &LayerConfig, sizeof(LayerConfig)});
    bindings.push_back({6, 0, nullptr, SamplerMode::NearestClampEdge, &ScanlineConfig, sizeof(ScanlineConfig)});
    bindings.push_back({7, 0, nullptr, SamplerMode::NearestClampEdge, &CompositorConfig, sizeof(CompositorConfig)});

    int32_t scale = ScaleFactor;
    const auto& prog = Parent.P.Compositor;
    S.Draw(prog.pipeline, prog.layout, prog.set, bindings, &scale, sizeof(scale), kRectVertices, sizeof(kRectVertices), 6);
}

void VulkanRenderer2D::DrawSprites(u32 line)
{
    u32 oammask = 1 << GPU2D.Num;
    bool dirty = false;
    bool screenon = IsScreenOn();

    SpriteScanlineConfig.uMosaicLine[line] = GPU2D.OBJMosaicLine;

    u32 dispcnt_diff = GPU2D.DispCnt ^ SpriteDispCnt;
    SpriteDispCnt = GPU2D.DispCnt;
    if (dispcnt_diff & 0x80F000F0)
        dirty = true;

    static_assert(VRAMDirtyGranularity == 512);
    NonStupidBitField<512> objDirty;

    if (screenon)
    {
        if (GPU2D.Num == 0)
        {
            objDirty = GPU.VRAMDirty_AOBJ.DeriveState(GPU.VRAMMap_AOBJ, GPU);
            GPU.MakeVRAMFlat_AOBJCoherent(objDirty);
        }
        else
        {
            auto _objDirty = GPU.VRAMDirty_BOBJ.DeriveState(GPU.VRAMMap_BOBJ, GPU);
            GPU.MakeVRAMFlat_BOBJCoherent(_objDirty);
            memcpy(objDirty.Data, _objDirty.Data, 256>>3);
        }
    }

    u8* vram; u32 vrammask;
    GPU2D.GetOBJVRAM(vram, vrammask);

    int texlen = (GPU2D.Num ? 256 : 512) >> 6;
    for (int i = 0; i < texlen; )
    {
        if (!objDirty.Data[i])
        {
            i++;
            continue;
        }

        int start = i * 32;
        for (;;)
        {
            i++;
            if (i >= texlen) break;
            if (!objDirty.Data[i]) break;
        }
        int end = i * 32;

        UploadVRAMRows(VRAMTex_OBJ, vram, start, end);
        dirty = true;
    }

    if ((GPU.OAMDirty & oammask) || SpriteConfigDirty)
    {
        memcpy(OAM, &GPU.OAM[GPU2D.Num ? 0x400 : 0], 0x400);
        GPU.OAMDirty &= ~oammask;
        SpriteConfigDirty = false;
        dirty = true;
    }

    // DrawScanline() for the next scanline will be called after this
    // so it will be able to do the actual sprite rendering
    if (dirty)
        SpriteDirty = true;
}

}
