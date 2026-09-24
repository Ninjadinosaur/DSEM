#pragma once

// Vulkan port of melonDS's compute 3D renderer (GPU3D_Compute, Copyright 2016-2026 melonDS
// team, GPL-3.0-or-later). The CPU-side polygon/span setup is unchanged; the GL compute
// dispatches become Vulkan compute dispatches with the same shaders (converted by
// tools/convert_melonds_compute.py; screen and tile sizes are specialization constants).

#include "GPU3D.h"
#include "GPU3D_Texcache.h"
#include "VkStream.h"

#include <vector>

namespace melonDS
{
class VulkanRenderer;

// Loads decoded DS textures into Vulkan array textures (RGBA8 integer, like the GL loader).
class TexcacheVulkanLoader
{
public:
    explicit TexcacheVulkanLoader(ds13r::vk::Stream& s) : S(&s) {}
    ds13r::vk::Texture* GenerateTexture(u32 width, u32 height, u32 layers);
    void UploadTexture(ds13r::vk::Texture* handle, u32 width, u32 height, u32 layer, void* data);
    void DeleteTexture(ds13r::vk::Texture* handle);

private:
    ds13r::vk::Stream* S;
};
using TexcacheVulkan = Texcache<TexcacheVulkanLoader, ds13r::vk::Texture*>;

class VulkanCompute3D : public Renderer3D
{
public:
    VulkanCompute3D(melonDS::GPU3D& gpu3D, VulkanRenderer& parent);
    ~VulkanCompute3D() override;
    bool Init() override;
    void Reset() override;

    void SetRenderSettings(int scale, bool highResolutionCoordinates);

    void RenderFrame() override;
    void RestartFrame() override {}
    u32* GetLine(int line) override { return nullptr; }

private:
    VulkanRenderer& Parent;
    ds13r::vk::Stream& S;

    // ---- pipelines (index = ShaderCompileStep order in melonDS)
    enum
    {
        P_InterpXSpansZ, P_InterpXSpansW,
        P_BinCombined,
        P_DepthBlendZ, P_DepthBlendW,
        P_RasteriseFirst, // 16 rasteriser variants follow, Z then W (see RasteriseIndex)
        P_ClearCoarseBinMask = P_RasteriseFirst + 16,
        P_ClearIndirectWorkCount,
        P_CalculateWorkListOffset,
        P_SortWork,
        P_FinalPassFirst,
        P_Count = P_FinalPassFirst + 8,
    };
    enum RasteriseKind
    {
        R_NoTexture, R_NoTextureToon, R_NoTextureHighlight,
        R_UseTextureDecal, R_UseTextureModulate, R_UseTextureToon, R_UseTextureHighlight,
        R_ShadowMask,
    };
    static int RasteriseIndex(RasteriseKind kind, bool wbuffer) { return P_RasteriseFirst + (wbuffer ? 8 : 0) + kind; }

    VkDescriptorSetLayout SetLayout = VK_NULL_HANDLE;
    VkPipelineLayout PipelineLayout = VK_NULL_HANDLE;
    VkPipeline Pipelines[P_Count] {};
    VkSampler Samplers[9] {};

    // ---- buffers
    ds13r::VkBufferResource YSpanSetupMemory;
    ds13r::VkBufferResource XSpanSetupMemory;
    ds13r::VkBufferResource BinResultMemory;
    ds13r::VkBufferResource RenderPolygonMemory;
    ds13r::VkBufferResource WorkDescMemory;
    ds13r::VkBufferResource SetupIndicesMemory;
    ds13r::VkBufferResource TileMemory[3];
    ds13r::VkBufferResource FinalTileMemory;
    ds13r::VkBufferResource MetaUniformMemory;

    ds13r::vk::Texture ClearBitmapTex[2];
    ds13r::vk::Texture DummyTexture;  // bound where no DS texture is used
    ds13r::vk::Texture Framebuffer;   // the 3D layer the 2D compositor samples

    struct SpanSetupY
    {
        s32 Z0, Z1, W0, W1;
        s32 ColorR0, ColorG0, ColorB0;
        s32 ColorR1, ColorG1, ColorB1;
        s32 TexcoordU0, TexcoordV0;
        s32 TexcoordU1, TexcoordV1;
        s32 I0, I1;
        s32 Linear;
        s32 IRecip;
        s32 W0n, W0d, W1d;
        s32 Increment;
        s32 X0, X1, Y0, Y1;
        s32 XMin, XMax;
        s32 DxInitial;
        s32 XCovIncr;
        u32 IsDummy;
    };
    struct SpanSetupX
    {
        s32 X0, X1;
        s32 EdgeLenL, EdgeLenR, EdgeCovL, EdgeCovR;
        s32 XRecip;
        u32 Flags;
        s32 Z0, Z1, W0, W1;
        s32 ColorR0, ColorG0, ColorB0;
        s32 ColorR1, ColorG1, ColorB1;
        s32 TexcoordU0, TexcoordV0;
        s32 TexcoordU1, TexcoordV1;
        s32 CovLInitial, CovRInitial;
    };
    struct SetupIndices
    {
        u16 PolyIdx, SpanIdxL, SpanIdxR, Y;
    };
    struct RenderPolygon
    {
        u32 FirstXSpan;
        s32 YTop, YBot;
        s32 XMin, XMax;
        s32 XMinY, XMaxY;
        u32 Variant;
        u32 Attr;
        float TextureLayer;
    };

    int TileSize = 8;
    static constexpr int CoarseTileCountX = 8;
    int CoarseTileCountY = 4;
    int CoarseTileArea = 32;
    int CoarseTileW = 64;
    int CoarseTileH = 32;
    int ClearCoarseBinMaskLocalSize = 64;

    static constexpr int BinStride = 2048/32;
    static constexpr int CoarseBinStride = BinStride/32;
    static constexpr int MaxVariants = 256;

    struct BinResultHeader
    {
        u32 VariantWorkCount[MaxVariants*4];
        u32 SortedWorkOffset[MaxVariants];
        u32 SortWorkWorkCount[4];
    };

    static const int MaxYSpanSetups = 6144*2;
    std::vector<SetupIndices> YSpanIndices;
    SpanSetupY YSpanSetups[MaxYSpanSetups];
    RenderPolygon RenderPolygons[2048];

    TexcacheVulkan Texcache;

    struct MetaUniform
    {
        u32 NumPolygons;
        u32 NumVariants;
        u32 AlphaRef;
        u32 DispCnt;
        u32 ToonTable[4*34];
        u32 ClearColor, ClearDepth, ClearAttr;
        u32 FogOffset, FogShift, FogColor;
        float ClearBitmapOffset[2];
    };

    // Must match the push-constant block of the converted rasteriser shaders.
    struct PushConstants
    {
        u32 CurVariant;
        s32 TexIsCapture;
        float CaptureYOffset;
        float Pad;
        float InvTextureSize[2];
    };

    u32* ClearBitmap[2];
    u8 ClearBitmapDirty;

    int ScreenWidth = 256, ScreenHeight = 192;
    int TilesPerLine = 32, TileLines = 24;
    int ScaleFactor = -1;
    int MaxWorkTiles = 0;
    bool HiresCoordinates = false;

    bool CreatePipelines();
    void DestroyPipelines();
    void Dispatch(int pipeline, u32 x, u32 y, u32 z, ds13r::vk::Texture* texture = nullptr, VkSampler sampler = VK_NULL_HANDLE,
                  const PushConstants* push = nullptr, VkBuffer indirect = VK_NULL_HANDLE, VkDeviceSize indirectOffset = 0);
    void ComputeBarrier();

    void SetupAttrs(SpanSetupY* span, Polygon* poly, int from, int to);
    void SetupYSpan(RenderPolygon* rp, SpanSetupY* span, Polygon* poly, int from, int to, int side, s32 positions[10][2]);
    void SetupYSpanDummy(RenderPolygon* rp, SpanSetupY* span, Polygon* poly, int vertex, int side, s32 positions[10][2]);

    friend class VulkanRenderer;
};

}
