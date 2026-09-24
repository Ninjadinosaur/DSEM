// Vulkan port of melonDS's GPU3D_Compute.cpp (Copyright 2016-2026 melonDS team,
// GPL-3.0-or-later). CPU-side setup is unchanged; GL compute calls map to Vulkan dispatches.

#include "GPU3D_Vulkan.h"
#include "GPU_Vulkan.h"
#include "Utils.h"
#include "../LogBuffer.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstring>

namespace melonDS
{
using namespace ds13r::vk;
using ds13r::VkBufferResource;
using ds13r::VulkanContext;

namespace
{
// SPIR-V of the 33 converted compute shaders, in melonDS's ShaderCompileStep order.
const uint32_t kInterpSpansZ[] = {
#include "interp_spans_z.comp.inc"
};
const uint32_t kInterpSpansW[] = {
#include "interp_spans_w.comp.inc"
};
const uint32_t kBinCombined[] = {
#include "bin_combined.comp.inc"
};
const uint32_t kDepthBlendZ[] = {
#include "depth_blend_z.comp.inc"
};
const uint32_t kDepthBlendW[] = {
#include "depth_blend_w.comp.inc"
};
const uint32_t kRastNoTexZ[] = {
#include "rasterise_notex_z.comp.inc"
};
const uint32_t kRastNoTexToonZ[] = {
#include "rasterise_notex_toon_z.comp.inc"
};
const uint32_t kRastNoTexHighlightZ[] = {
#include "rasterise_notex_highlight_z.comp.inc"
};
const uint32_t kRastTexDecalZ[] = {
#include "rasterise_tex_decal_z.comp.inc"
};
const uint32_t kRastTexModulateZ[] = {
#include "rasterise_tex_modulate_z.comp.inc"
};
const uint32_t kRastTexToonZ[] = {
#include "rasterise_tex_toon_z.comp.inc"
};
const uint32_t kRastTexHighlightZ[] = {
#include "rasterise_tex_highlight_z.comp.inc"
};
const uint32_t kRastShadowMaskZ[] = {
#include "rasterise_shadow_mask_z.comp.inc"
};
const uint32_t kRastNoTexW[] = {
#include "rasterise_notex_w.comp.inc"
};
const uint32_t kRastNoTexToonW[] = {
#include "rasterise_notex_toon_w.comp.inc"
};
const uint32_t kRastNoTexHighlightW[] = {
#include "rasterise_notex_highlight_w.comp.inc"
};
const uint32_t kRastTexDecalW[] = {
#include "rasterise_tex_decal_w.comp.inc"
};
const uint32_t kRastTexModulateW[] = {
#include "rasterise_tex_modulate_w.comp.inc"
};
const uint32_t kRastTexToonW[] = {
#include "rasterise_tex_toon_w.comp.inc"
};
const uint32_t kRastTexHighlightW[] = {
#include "rasterise_tex_highlight_w.comp.inc"
};
const uint32_t kRastShadowMaskW[] = {
#include "rasterise_shadow_mask_w.comp.inc"
};
const uint32_t kClearCoarseBinMask[] = {
#include "clear_coarse_bin_mask.comp.inc"
};
const uint32_t kClearIndirectWorkCount[] = {
#include "clear_indirect_work_count.comp.inc"
};
const uint32_t kCalcOffsets[] = {
#include "calc_offsets.comp.inc"
};
const uint32_t kSortWork[] = {
#include "sort_work.comp.inc"
};
const uint32_t kFinalPass0[] = {
#include "final_pass_0.comp.inc"
};
const uint32_t kFinalPass1[] = {
#include "final_pass_1.comp.inc"
};
const uint32_t kFinalPass2[] = {
#include "final_pass_2.comp.inc"
};
const uint32_t kFinalPass3[] = {
#include "final_pass_3.comp.inc"
};
const uint32_t kFinalPass4[] = {
#include "final_pass_4.comp.inc"
};
const uint32_t kFinalPass5[] = {
#include "final_pass_5.comp.inc"
};
const uint32_t kFinalPass6[] = {
#include "final_pass_6.comp.inc"
};
const uint32_t kFinalPass7[] = {
#include "final_pass_7.comp.inc"
};

struct ShaderCode
{
    const uint32_t* code;
    size_t bytes;
};
#define SC(x) ShaderCode {x, sizeof(x)}
// Same order as the pipeline enum.
const ShaderCode kShaders[] = {
    SC(kInterpSpansZ), SC(kInterpSpansW),
    SC(kBinCombined),
    SC(kDepthBlendZ), SC(kDepthBlendW),
    SC(kRastNoTexZ), SC(kRastNoTexToonZ), SC(kRastNoTexHighlightZ), SC(kRastTexDecalZ),
    SC(kRastTexModulateZ), SC(kRastTexToonZ), SC(kRastTexHighlightZ), SC(kRastShadowMaskZ),
    SC(kRastNoTexW), SC(kRastNoTexToonW), SC(kRastNoTexHighlightW), SC(kRastTexDecalW),
    SC(kRastTexModulateW), SC(kRastTexToonW), SC(kRastTexHighlightW), SC(kRastShadowMaskW),
    SC(kClearCoarseBinMask), SC(kClearIndirectWorkCount), SC(kCalcOffsets), SC(kSortWork),
    SC(kFinalPass0), SC(kFinalPass1), SC(kFinalPass2), SC(kFinalPass3),
    SC(kFinalPass4), SC(kFinalPass5), SC(kFinalPass6), SC(kFinalPass7),
};
#undef SC

// Bindings of the converted shaders (tools/convert_melonds_compute.py).
enum
{
    B_Polygons = 0, B_XSpans = 1, B_YSpans = 2, B_ColorTiles = 3, B_DepthTiles = 4, B_AttrTiles = 5,
    B_Result = 6, B_BinResult = 7, B_WorkDesc = 8, B_Meta = 9, B_SetupIndices = 10,
    B_CurrentTexture = 11, B_Capture128 = 12, B_Capture256 = 13, B_ClearColor = 14, B_ClearDepth = 15,
    B_FinalFB = 16, B_Count = 17,
};
}

// ---------------------------------------------------------------------------
// Texture cache loader

Texture* TexcacheVulkanLoader::GenerateTexture(u32 width, u32 height, u32 layers)
{
    auto* t = new Texture;
    TextureDesc d;
    d.width = width;
    d.height = height;
    d.layers = layers;
    d.array = true;
    d.format = VK_FORMAT_R8G8B8A8_UINT;
    S->CreateTexture(*t, d);
    return t;
}

void TexcacheVulkanLoader::UploadTexture(Texture* handle, u32 width, u32 height, u32 layer, void* data)
{
    S->UploadTexture(*handle, layer, 0, 0, (int)width, (int)height, data, 4);
}

void TexcacheVulkanLoader::DeleteTexture(Texture* handle)
{
    S->DestroyTexture(*handle);
    delete handle;
}

// ---------------------------------------------------------------------------

VulkanCompute3D::VulkanCompute3D(melonDS::GPU3D& gpu3D, VulkanRenderer& parent)
    : Renderer3D(gpu3D), Parent(parent), S(parent.S), Texcache(gpu3D.GPU, TexcacheVulkanLoader(parent.S))
{
    ClearBitmap[0] = new u32[256*256];
    ClearBitmap[1] = new u32[256*256];
    ClearBitmapDirty = 0x3;
}

VulkanCompute3D::~VulkanCompute3D()
{
    S.Flush(true);
    Texcache.Reset();
    DestroyPipelines();
    VulkanContext& vk = S.Context();
    for (VkBufferResource* b : {&YSpanSetupMemory, &XSpanSetupMemory, &BinResultMemory, &RenderPolygonMemory,
                                &WorkDescMemory, &SetupIndicesMemory, &TileMemory[0], &TileMemory[1], &TileMemory[2],
                                &FinalTileMemory, &MetaUniformMemory})
        b->Destroy(vk);
    S.DestroyTexture(ClearBitmapTex[0]);
    S.DestroyTexture(ClearBitmapTex[1]);
    S.DestroyTexture(DummyTexture);
    S.DestroyTexture(Framebuffer);
    for (VkSampler s : Samplers)
        if (s) vkDestroySampler(vk.Device(), s, nullptr);
    if (PipelineLayout) vkDestroyPipelineLayout(vk.Device(), PipelineLayout, nullptr);
    if (SetLayout) vkDestroyDescriptorSetLayout(vk.Device(), SetLayout, nullptr);
    delete[] ClearBitmap[0];
    delete[] ClearBitmap[1];
}

bool VulkanCompute3D::Init()
{
    VulkanContext& vk = S.Context();
    auto buf = [](uint32_t b) { return LayoutBinding {b, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}; };
    auto tex = [](uint32_t b) { return LayoutBinding {b, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1}; };
    SetLayout = CreateSetLayout(vk, {
        buf(B_Polygons), buf(B_XSpans), buf(B_YSpans), buf(B_ColorTiles), buf(B_DepthTiles), buf(B_AttrTiles),
        buf(B_Result), buf(B_BinResult), buf(B_WorkDesc), {B_Meta, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
        buf(B_SetupIndices), tex(B_CurrentTexture), tex(B_Capture128), tex(B_Capture256), tex(B_ClearColor),
        tex(B_ClearDepth), {B_FinalFB, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1},
    });
    VkPushConstantRange pcr {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushConstants)};
    VkPipelineLayoutCreateInfo pli {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &SetLayout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(vk.Device(), &pli, nullptr, &PipelineLayout) != VK_SUCCESS) return false;

    // Samplers for the texture wrap modes: clamp, repeat, mirrored repeat, per axis.
    const VkSamplerAddressMode modes[3] = {VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_REPEAT,
                                           VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT};
    for (u32 j = 0; j < 3; j++)
        for (u32 i = 0; i < 3; i++)
        {
            VkSamplerCreateInfo si {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
            si.magFilter = si.minFilter = VK_FILTER_NEAREST;
            si.addressModeU = modes[i];
            si.addressModeV = modes[j];
            si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            vkCreateSampler(vk.Device(), &si, nullptr, &Samplers[i + j * 3]);
        }

    // clear bitmap textures
    TextureDesc cd;
    cd.width = 256;
    cd.height = 256;
    cd.format = VK_FORMAT_R32_UINT;
    if (!S.CreateTexture(ClearBitmapTex[0], cd) || !S.CreateTexture(ClearBitmapTex[1], cd)) return false;
    memset(ClearBitmap[0], 0, 256 * 256 * 4);
    S.UploadTexture(ClearBitmapTex[0], 0, 0, 0, 256, 256, ClearBitmap[0], 4);
    S.UploadTexture(ClearBitmapTex[1], 0, 0, 0, 256, 256, ClearBitmap[0], 4);

    TextureDesc dd;
    dd.width = 1;
    dd.height = 1;
    dd.array = true;
    dd.format = VK_FORMAT_R8G8B8A8_UINT;
    if (!S.CreateTexture(DummyTexture, dd)) return false;
    u32 zero = 0;
    S.UploadTexture(DummyTexture, 0, 0, 0, 1, 1, &zero, 4);

    VkBufferUsageFlags ssbo = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (!YSpanSetupMemory.Create(vk, sizeof(SpanSetupY) * MaxYSpanSetups, ssbo, false)) return false;
    if (!RenderPolygonMemory.Create(vk, sizeof(RenderPolygon) * 2048, ssbo, false)) return false;
    if (!MetaUniformMemory.Create(vk, sizeof(MetaUniform), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false))
        return false;
    return true;
}

void VulkanCompute3D::Reset()
{
    Texcache.Reset();
    ClearBitmapDirty = 0x3;
}

void VulkanCompute3D::DestroyPipelines()
{
    VkDevice dev = S.Context().Device();
    for (VkPipeline& p : Pipelines)
    {
        if (p) vkDestroyPipeline(dev, p, nullptr);
        p = VK_NULL_HANDLE;
    }
}

bool VulkanCompute3D::CreatePipelines()
{
    VulkanContext& vk = S.Context();
    // Specialization constants 0..6 (see the converter): the values melonDS #defines per scale.
    int32_t values[7] = {ScreenWidth, ScreenHeight, MaxWorkTiles, TileSize, CoarseTileCountY, CoarseTileArea,
                         ClearCoarseBinMaskLocalSize};
    VkSpecializationMapEntry entries[7];
    for (uint32_t i = 0; i < 7; i++) entries[i] = {i, i * 4, 4};
    VkSpecializationInfo spec {7, entries, sizeof(values), values};

    bool ok = true;
    for (int i = 0; i < P_Count; i++)
    {
        VkShaderModule m = vk.CreateShaderModule(kShaders[i].code, kShaders[i].bytes);
        VkComputePipelineCreateInfo ci {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = m;
        ci.stage.pName = "main";
        ci.stage.pSpecializationInfo = &spec;
        ci.layout = PipelineLayout;
        VkResult r = vkCreateComputePipelines(vk.Device(), vk.PipelineCache(), 1, &ci, nullptr, &Pipelines[i]);
        vkDestroyShaderModule(vk.Device(), m, nullptr);
        if (r != VK_SUCCESS)
        {
            LOGE("Vulkan 3D: compute pipeline %d failed (%s)", i, ds13r::VkResultName(r));
            ok = false;
        }
    }
    vk.SavePipelineCache();
    return ok;
}

void VulkanCompute3D::SetRenderSettings(int scale, bool highResolutionCoordinates)
{
    HiresCoordinates = highResolutionCoordinates;
    if (scale == ScaleFactor) return;

    S.Flush(true);
    DestroyPipelines();

    ScaleFactor = scale;
    ScreenWidth = 256 * ScaleFactor;
    ScreenHeight = 192 * ScaleFactor;

    // Starting at 4.5x we want to double TileSize every time scale doubles
    u8 TileScale = 2 * ScaleFactor / 9;
    TileScale = GetMSBit(TileScale);
    TileScale <<= 1;
    TileScale += TileScale == 0;

    TileSize = std::min(8 * TileScale, 32);
    CoarseTileCountY = TileSize < 32 ? 4 : 6;
    ClearCoarseBinMaskLocalSize = TileSize < 32 ? 64 : 48;
    CoarseTileArea = CoarseTileCountX * CoarseTileCountY;
    CoarseTileW = CoarseTileCountX * TileSize;
    CoarseTileH = CoarseTileCountY * TileSize;

    TilesPerLine = ScreenWidth/TileSize;
    TileLines = ScreenHeight/TileSize;

    MaxWorkTiles = TilesPerLine*TileLines*16;

    VulkanContext& vk = S.Context();
    VkBufferUsageFlags ssbo = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    for (auto& t : TileMemory)
        t.Create(vk, 4 * TileSize * TileSize * MaxWorkTiles, ssbo, false);
    FinalTileMemory.Create(vk, 4 * 3 * 2 * ScreenWidth * ScreenHeight, ssbo, false);

    int binResultSize = sizeof(BinResultHeader)
        + TilesPerLine*TileLines*CoarseBinStride*4 // BinnedMaskCoarse
        + TilesPerLine*TileLines*BinStride*4 // BinnedMask
        + TilesPerLine*TileLines*BinStride*4; // WorkOffsets
    BinResultMemory.Create(vk, binResultSize, ssbo | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, false);
    WorkDescMemory.Create(vk, MaxWorkTiles*2*4*2, ssbo, false);

    TextureDesc fd;
    fd.width = ScreenWidth;
    fd.height = ScreenHeight;
    fd.format = VK_FORMAT_R8G8B8A8_UNORM;
    fd.storage = true;
    fd.transferSrc = true;
    S.CreateTexture(Framebuffer, fd);
    Parent.OutputTex3D = &Framebuffer;
    // Nothing rendered yet: transparent, so the 2D layers show through.
    S.Use(Framebuffer, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, true);
    VkClearColorValue clear {};
    VkImageSubresourceRange range {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(S.Cmd(), Framebuffer.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);

    // eh those are pretty bad guesses
    // though real hw shouldn't be eable to render all 2048 polygons on every line either
    int maxYSpanIndices = 64*2048 * ScaleFactor;
    YSpanIndices.resize(maxYSpanIndices);
    SetupIndicesMemory.Create(vk, maxYSpanIndices * 2 * 4, ssbo, false);
    XSpanSetupMemory.Create(vk, sizeof(SpanSetupX) * maxYSpanIndices, ssbo, false);

    CreatePipelines();
    LOGI("Vulkan 3D: %dx%d, tile %d", ScreenWidth, ScreenHeight, TileSize);
}

void VulkanCompute3D::SetupAttrs(SpanSetupY* span, Polygon* poly, int from, int to)
{
    span->Z0 = poly->FinalZ[from];
    span->W0 = poly->FinalW[from];
    span->Z1 = poly->FinalZ[to];
    span->W1 = poly->FinalW[to];
    span->ColorR0 = poly->Vertices[from]->FinalColor[0];
    span->ColorG0 = poly->Vertices[from]->FinalColor[1];
    span->ColorB0 = poly->Vertices[from]->FinalColor[2];
    span->ColorR1 = poly->Vertices[to]->FinalColor[0];
    span->ColorG1 = poly->Vertices[to]->FinalColor[1];
    span->ColorB1 = poly->Vertices[to]->FinalColor[2];
    span->TexcoordU0 = poly->Vertices[from]->TexCoords[0];
    span->TexcoordV0 = poly->Vertices[from]->TexCoords[1];
    span->TexcoordU1 = poly->Vertices[to]->TexCoords[0];
    span->TexcoordV1 = poly->Vertices[to]->TexCoords[1];
}

void VulkanCompute3D::SetupYSpanDummy(RenderPolygon* rp, SpanSetupY* span, Polygon* poly, int vertex, int side, s32 positions[10][2])
{
    s32 x0 = positions[vertex][0];
    if (side)
    {
        span->DxInitial = -0x40000;
        x0--;
    }
    else
    {
        span->DxInitial = 0;
    }

    span->X0 = span->X1 = x0;
    span->XMin = x0;
    span->XMax = x0;
    span->Y0 = span->Y1 = positions[vertex][1];

    if (span->XMin < rp->XMin)
    {
        rp->XMin = span->XMin;
        rp->XMinY = span->Y0;
    }
    if (span->XMax > rp->XMax)
    {
        rp->XMax = span->XMax;
        rp->XMaxY = span->Y0;
    }

    span->Increment = 0;

    span->I0 = span->I1 = span->IRecip = 0;
    span->Linear = true;

    span->XCovIncr = 0;

    span->IsDummy = true;

    SetupAttrs(span, poly, vertex, vertex);
}

void VulkanCompute3D::SetupYSpan(RenderPolygon* rp, SpanSetupY* span, Polygon* poly, int from, int to, int side, s32 positions[10][2])
{
    span->X0 = positions[from][0];
    span->X1 = positions[to][0];
    span->Y0 = positions[from][1];
    span->Y1 = positions[to][1];

    SetupAttrs(span, poly, from, to);

    s32 minXY, maxXY;
    bool negative = false;
    if (span->X1 > span->X0)
    {
        span->XMin = span->X0;
        span->XMax = span->X1-1;

        minXY = span->Y0;
        maxXY = span->Y1;
    }
    else if (span->X1 < span->X0)
    {
        span->XMin = span->X1;
        span->XMax = span->X0-1;
        negative = true;

        minXY = span->Y1;
        maxXY = span->Y0;
    }
    else
    {
        span->XMin = span->X0;
        if (side) span->XMin--;
        span->XMax = span->XMin;

        // doesn't matter for completely vertical slope
        minXY = span->Y0;
        maxXY = span->Y0;
    }

    if (span->XMin < rp->XMin)
    {
        rp->XMin = span->XMin;
        rp->XMinY = minXY;
    }
    if (span->XMax > rp->XMax)
    {
        rp->XMax = span->XMax;
        rp->XMaxY = maxXY;
    }

    span->IsDummy = false;

    s32 xlen = span->XMax+1 - span->XMin;
    s32 ylen = span->Y1 - span->Y0;

    // slope increment has a 18-bit fractional part
    // note: for some reason, x/y isn't calculated directly,
    // instead, 1/y is calculated and then multiplied by x
    if (ylen == 0)
    {
        span->Increment = 0;
    }
    else if (ylen == xlen)
    {
        span->Increment = 0x40000;
    }
    else
    {
        s32 yrecip = (1<<18) / ylen;
        span->Increment = (span->X1-span->X0) * yrecip;
        if (span->Increment < 0) span->Increment = -span->Increment;
    }

    bool xMajor = (span->Increment > 0x40000);

    if (side)
    {
        // right
        if (xMajor)
            span->DxInitial = negative ? (0x20000 + 0x40000) : (span->Increment - 0x20000);
        else if (span->Increment != 0)
            span->DxInitial = negative ? 0x40000 : 0;
        else
            span->DxInitial = -0x40000;
    }
    else
    {
        // left
        if (xMajor)
            span->DxInitial = negative ? ((span->Increment - 0x20000) + 0x40000) : 0x20000;
        else if (span->Increment != 0)
            span->DxInitial = negative ? 0x40000 : 0;
        else
            span->DxInitial = 0;
    }

    if (xMajor)
    {
        if (side)
        {
            span->I0 = span->X0 - 1;
            span->I1 = span->X1 - 1;
        }
        else
        {
            span->I0 = span->X0;
            span->I1 = span->X1;
        }

        // used for calculating AA coverage
        span->XCovIncr = (ylen << 10) / xlen;
    }
    else
    {
        span->I0 = span->Y0;
        span->I1 = span->Y1;
    }

    if (span->I0 != span->I1)
        span->IRecip = (1<<30) / (span->I1 - span->I0);
    else
        span->IRecip = 0;

    span->Linear = (span->W0 == span->W1) && !(span->W0 & 0x7E) && !(span->W1 & 0x7E);

    if ((span->W0 & 0x1) && !(span->W1 & 0x1))
    {
        span->W0n = (span->W0 - 1) >> 1;
        span->W0d = (span->W0 + 1) >> 1;
        span->W1d = span->W1 >> 1;
    }
    else
    {
        span->W0n = span->W0 >> 1;
        span->W0d = span->W0 >> 1;
        span->W1d = span->W1 >> 1;
    }
}

void VulkanCompute3D::ComputeBarrier()
{
    // glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT)
    S.MemoryBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
                    VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT);
}

void VulkanCompute3D::Dispatch(int pipeline, u32 x, u32 y, u32 z, Texture* texture, VkSampler sampler,
                               const PushConstants* push, VkBuffer indirect, VkDeviceSize indirectOffset)
{
    VkPipelineStageFlags cs = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    Texture* current = texture ? texture : &DummyTexture;
    VkSampler smp = sampler ? sampler : Samplers[0];
    // Make every sampled image readable (inserts barriers only where needed).
    for (Texture* t : {current, &Parent.CaptureOutput128Tex, &Parent.CaptureOutput256Tex, &ClearBitmapTex[0], &ClearBitmapTex[1]})
        S.Use(*t, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, cs, VK_ACCESS_SHADER_READ_BIT, false);
    if (pipeline >= P_FinalPassFirst)
        S.Use(Framebuffer, VK_IMAGE_LAYOUT_GENERAL, cs, VK_ACCESS_SHADER_WRITE_BIT, true);

    VkDescriptorSet set = S.AllocateDescriptorSet(SetLayout);
    VkDescriptorBufferInfo buffers[B_Count];
    VkDescriptorImageInfo images[B_Count];
    VkWriteDescriptorSet writes[B_Count];
    auto bufferAt = [&](uint32_t b, VkBufferResource& r, VkDescriptorType type) {
        buffers[b] = {r.buffer, 0, VK_WHOLE_SIZE};
        writes[b] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, b, 0, 1, type, nullptr, &buffers[b], nullptr};
    };
    auto imageAt = [&](uint32_t b, Texture& t, VkSampler s, VkImageLayout layout, VkDescriptorType type) {
        images[b] = {s, t.view, layout};
        writes[b] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, b, 0, 1, type, &images[b], nullptr, nullptr};
    };
    VkDescriptorType sb = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    VkDescriptorType cis = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    VkImageLayout ro = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    bufferAt(B_Polygons, RenderPolygonMemory, sb);
    bufferAt(B_XSpans, XSpanSetupMemory, sb);
    bufferAt(B_YSpans, YSpanSetupMemory, sb);
    bufferAt(B_ColorTiles, TileMemory[0], sb);
    bufferAt(B_DepthTiles, TileMemory[1], sb);
    bufferAt(B_AttrTiles, TileMemory[2], sb);
    bufferAt(B_Result, FinalTileMemory, sb);
    bufferAt(B_BinResult, BinResultMemory, sb);
    bufferAt(B_WorkDesc, WorkDescMemory, sb);
    bufferAt(B_Meta, MetaUniformMemory, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    bufferAt(B_SetupIndices, SetupIndicesMemory, sb);
    imageAt(B_CurrentTexture, *current, smp, ro, cis);
    imageAt(B_Capture128, Parent.CaptureOutput128Tex, smp, ro, cis);
    imageAt(B_Capture256, Parent.CaptureOutput256Tex, smp, ro, cis);
    imageAt(B_ClearColor, ClearBitmapTex[0], Samplers[4], ro, cis); // repeat/repeat, like GL
    imageAt(B_ClearDepth, ClearBitmapTex[1], Samplers[4], ro, cis);
    imageAt(B_FinalFB, Framebuffer, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
    vkUpdateDescriptorSets(S.Context().Device(), B_Count, writes, 0, nullptr);

    VkCommandBuffer cmd = S.Cmd();
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, Pipelines[pipeline]);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, PipelineLayout, 0, 1, &set, 0, nullptr);
    PushConstants zero {};
    vkCmdPushConstants(cmd, PipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushConstants), push ? push : &zero);
    if (indirect)
        vkCmdDispatchIndirect(cmd, indirect, indirectOffset);
    else if (x && y && z)
        vkCmdDispatch(cmd, x, y, z);
}

namespace
{
struct Variant
{
    Texture* Tex;      // texture cache array (nullptr: none, or a display capture)
    int Capture;       // 0 = not a capture, 1 = 128-wide capture, 2 = 256-wide capture
    VkSampler Sampler;
    u16 Width, Height;
    u8 BlendMode;
    int CaptureYOffset;

    bool operator==(const Variant& other) const
    {
        return Tex == other.Tex && Capture == other.Capture && Sampler == other.Sampler &&
               BlendMode == other.BlendMode && CaptureYOffset == other.CaptureYOffset;
    }
    bool Textured() const { return Tex != nullptr || Capture != 0; }
};
}

void VulkanCompute3D::RenderFrame()
{
    if (!Pipelines[P_FinalPassFirst]) return;
    u8 clrBitmapDirty;
    if (!Texcache.Update(clrBitmapDirty) && GPU3D.RenderFrameIdentical)
        return;

    // figure out which chunks of texture memory contain display captures
    int captureinfo[16];
    GPU.GetCaptureInfo_Texture(captureinfo);

    // if we're using a clear bitmap, set that up
    ClearBitmapDirty |= clrBitmapDirty;
    if (GPU3D.RenderDispCnt & (1<<14))
    {
        if (ClearBitmapDirty & (1<<0))
        {
            u16* vram = (u16*)&GPU.VRAMFlat_Texture[0x40000];
            for (int i = 0; i < 256*256; i++)
            {
                u16 color = vram[i];
                u32 r = (color << 1) & 0x3E; if (r) r++;
                u32 g = (color >> 4) & 0x3E; if (g) g++;
                u32 b = (color >> 9) & 0x3E; if (b) b++;
                u32 a = (color & 0x8000) ? 31 : 0;

                ClearBitmap[0][i] = r | (g << 8) | (b << 16) | (a << 24);
            }
            S.UploadTexture(ClearBitmapTex[0], 0, 0, 0, 256, 256, ClearBitmap[0], 4);
        }

        if (ClearBitmapDirty & (1<<1))
        {
            u16* vram = (u16*)&GPU.VRAMFlat_Texture[0x60000];
            for (int i = 0; i < 256*256; i++)
            {
                u16 val = vram[i];
                u32 depth = ((val & 0x7FFF) * 0x200) + 0x1FF;
                u32 fog = (val & 0x8000) << 9;

                ClearBitmap[1][i] = depth | fog;
            }
            S.UploadTexture(ClearBitmapTex[1], 0, 0, 0, 256, 256, ClearBitmap[1], 4);
        }

        ClearBitmapDirty = 0;
    }

    int numYSpans = 0;
    int numSetupIndices = 0;

    // Textures of the same size share array textures, so polygons batch into few variants.
    u32 numVariants = 0, prevVariant = 0, prevTexLayer = 0;
    Variant variants[MaxVariants];
    u32 capLastVariant[16] = {0};

    bool enableTextureMaps = GPU3D.RenderDispCnt & (1<<0);

    for (u32 i = 0; i < GPU3D.RenderNumPolygons; i++)
    {
        Polygon* polygon = GPU3D.RenderPolygonRAM[i];

        u32 nverts = polygon->NumVertices;
        u32 vtop = polygon->VTop, vbot = polygon->VBottom;

        u32 curVL = vtop, curVR = vtop;
        u32 nextVL, nextVR;

        RenderPolygons[i].FirstXSpan = numSetupIndices;
        RenderPolygons[i].Attr = polygon->Attr;

        bool foundVariant = false;
        if (i > 0)
        {
            // if the whole texture attribute matches
            // the texture layer will also match
            Polygon* prevPolygon = GPU3D.RenderPolygonRAM[i - 1];
            foundVariant = prevPolygon->TexParam == polygon->TexParam
                && prevPolygon->TexPalette == polygon->TexPalette
                && (prevPolygon->Attr & 0x30) == (polygon->Attr & 0x30)
                && prevPolygon->IsShadowMask == polygon->IsShadowMask;
        }

        if (!foundVariant)
        {
            Variant variant;
            variant.BlendMode = polygon->IsShadowMask ? 4 : ((polygon->Attr >> 4) & 0x3);
            variant.Tex = nullptr;
            variant.Capture = 0;
            variant.Sampler = VK_NULL_HANDLE;
            variant.CaptureYOffset = -1;
            variant.Width = variant.Height = 0;
            u32* textureLastVariant = nullptr;
            // we always need to look up the texture to get the layer of the array texture
            u32 textype = (polygon->TexParam >> 26) & 0x7;
            if (enableTextureMaps && textype)
            {
                u32 texaddr = polygon->TexParam & 0xFFFF;
                u32 texwidth = TextureWidth(polygon->TexParam);
                u32 texheight = TextureHeight(polygon->TexParam);
                int capblock = -1;
                if ((textype == 7) && ((texwidth == 128) || (texwidth == 256)))
                {
                    // if this is a direct color texture, and the width is 128 or 256
                    // then it might be a display capture
                    u32 startaddr = texaddr << 3;
                    u32 endaddr = startaddr + (texheight * texwidth * 2);

                    startaddr >>= 15;
                    endaddr = (endaddr + 0x7FFF) >> 15;

                    for (u32 b = startaddr; b < endaddr; b++)
                    {
                        int blk = captureinfo[b];
                        if (blk == -1) continue;

                        capblock = blk;
                    }
                }

                if (capblock != -1)
                {
                    if (texwidth == 128)
                    {
                        variant.Capture = 1;
                        variant.CaptureYOffset = (int)((texaddr >> 5) & 0x7F);
                        prevTexLayer = capblock;
                    }
                    else
                    {
                        variant.Capture = 2;
                        variant.CaptureYOffset = (int)((texaddr >> 6) & 0xFF);
                        prevTexLayer = capblock >> 2;
                    }

                    textureLastVariant = &capLastVariant[capblock];
                }
                else
                {
                    Texcache.GetTexture(polygon->TexParam, polygon->TexPalette, variant.Tex, prevTexLayer, textureLastVariant);
                    variant.CaptureYOffset = -1;
                }

                bool wrapS = (polygon->TexParam >> 16) & 1;
                bool wrapT = (polygon->TexParam >> 17) & 1;
                bool mirrorS = (polygon->TexParam >> 18) & 1;
                bool mirrorT = (polygon->TexParam >> 19) & 1;
                variant.Sampler = Samplers[(wrapS ? (mirrorS ? 2 : 1) : 0) + (wrapT ? (mirrorT ? 2 : 1) : 0) * 3];

                if (*textureLastVariant < numVariants && variants[*textureLastVariant] == variant)
                {
                    foundVariant = true;
                    prevVariant = *textureLastVariant;
                }
            }

            if (!foundVariant)
            {
                for (int j = numVariants - 1; j >= 0; j--)
                {
                    if (variants[j] == variant)
                    {
                        foundVariant = true;
                        prevVariant = j;
                        goto foundVariant;
                    }
                }

                prevVariant = numVariants;
                variants[numVariants] = variant;
                variants[numVariants].Width = TextureWidth(polygon->TexParam);
                variants[numVariants].Height = TextureHeight(polygon->TexParam);
                numVariants++;
                assert(numVariants <= MaxVariants);
            foundVariant:;

                if (textureLastVariant)
                    *textureLastVariant = prevVariant;
            }
        }
        RenderPolygons[i].Variant = prevVariant;
        RenderPolygons[i].TextureLayer = (float)prevTexLayer;

        if (polygon->FacingView)
        {
            nextVL = curVL + 1;
            if (nextVL >= nverts) nextVL = 0;
            nextVR = curVR - 1;
            if ((s32)nextVR < 0) nextVR = nverts - 1;
        }
        else
        {
            nextVL = curVL - 1;
            if ((s32)nextVL < 0) nextVL = nverts - 1;
            nextVR = curVR + 1;
            if (nextVR >= nverts) nextVR = 0;
        }

        s32 scaledPositions[10][2];
        s32 ytop = ScreenHeight, ybot = 0;
        for (u32 j = 0; j < polygon->NumVertices; j++)
        {
            if (HiresCoordinates)
            {
                scaledPositions[j][0] = (polygon->Vertices[j]->HiresPosition[0] * ScaleFactor) >> 4;
                scaledPositions[j][1] = (polygon->Vertices[j]->HiresPosition[1] * ScaleFactor) >> 4;
            }
            else
            {
                scaledPositions[j][0] = polygon->Vertices[j]->FinalPosition[0] * ScaleFactor;
                scaledPositions[j][1] = polygon->Vertices[j]->FinalPosition[1] * ScaleFactor;
            }
            ytop = std::min(scaledPositions[j][1], ytop);
            ybot = std::max(scaledPositions[j][1], ybot);
        }
        RenderPolygons[i].YTop = ytop;
        RenderPolygons[i].YBot = ybot;
        RenderPolygons[i].XMin = ScreenWidth;
        RenderPolygons[i].XMax = 0;

        if (ybot == ytop)
        {
            vtop = 0; vbot = 0;

            RenderPolygons[i].YBot++;

            int j = 1;
            if (scaledPositions[j][0] < scaledPositions[vtop][0]) vtop = j;
            if (scaledPositions[j][0] > scaledPositions[vbot][0]) vbot = j;

            j = nverts - 1;
            if (scaledPositions[j][0] < scaledPositions[vtop][0]) vtop = j;
            if (scaledPositions[j][0] > scaledPositions[vbot][0]) vbot = j;

            assert(numYSpans < MaxYSpanSetups);
            u32 curSpanL = numYSpans;
            SetupYSpanDummy(&RenderPolygons[i], &YSpanSetups[numYSpans++], polygon, vtop, 0, scaledPositions);
            assert(numYSpans < MaxYSpanSetups);
            u32 curSpanR = numYSpans;
            SetupYSpanDummy(&RenderPolygons[i], &YSpanSetups[numYSpans++], polygon, vbot, 1, scaledPositions);

            YSpanIndices[numSetupIndices].PolyIdx = i;
            YSpanIndices[numSetupIndices].SpanIdxL = curSpanL;
            YSpanIndices[numSetupIndices].SpanIdxR = curSpanR;
            YSpanIndices[numSetupIndices].Y = ytop;
            numSetupIndices++;
        }
        else
        {
            u32 curSpanL = numYSpans;
            assert(numYSpans < MaxYSpanSetups);
            SetupYSpan(&RenderPolygons[i], &YSpanSetups[numYSpans++], polygon, curVL, nextVL, 0, scaledPositions);
            u32 curSpanR = numYSpans;
            assert(numYSpans < MaxYSpanSetups);
            SetupYSpan(&RenderPolygons[i], &YSpanSetups[numYSpans++], polygon, curVR, nextVR, 1, scaledPositions);

            for (s32 y = ytop; y < ybot; y++)
            {
                if (y >= scaledPositions[nextVL][1] && curVL != polygon->VBottom)
                {
                    while (y >= scaledPositions[nextVL][1] && curVL != polygon->VBottom)
                    {
                        curVL = nextVL;
                        if (polygon->FacingView)
                        {
                            nextVL = curVL + 1;
                            if (nextVL >= nverts)
                                nextVL = 0;
                        }
                        else
                        {
                            nextVL = curVL - 1;
                            if ((s32)nextVL < 0)
                                nextVL = nverts - 1;
                        }
                    }

                    assert(numYSpans < MaxYSpanSetups);
                    curSpanL = numYSpans;
                    SetupYSpan(&RenderPolygons[i], &YSpanSetups[numYSpans++], polygon, curVL, nextVL, 0, scaledPositions);
                }
                if (y >= scaledPositions[nextVR][1] && curVR != polygon->VBottom)
                {
                    while (y >= scaledPositions[nextVR][1] && curVR != polygon->VBottom)
                    {
                        curVR = nextVR;
                        if (polygon->FacingView)
                        {
                            nextVR = curVR - 1;
                            if ((s32)nextVR < 0)
                                nextVR = nverts - 1;
                        }
                        else
                        {
                            nextVR = curVR + 1;
                            if (nextVR >= nverts)
                                nextVR = 0;
                        }
                    }

                    assert(numYSpans < MaxYSpanSetups);
                    curSpanR = numYSpans;
                    SetupYSpan(&RenderPolygons[i] ,&YSpanSetups[numYSpans++], polygon, curVR, nextVR, 1, scaledPositions);
                }

                YSpanIndices[numSetupIndices].PolyIdx = i;
                YSpanIndices[numSetupIndices].SpanIdxL = curSpanL;
                YSpanIndices[numSetupIndices].SpanIdxR = curSpanR;
                YSpanIndices[numSetupIndices].Y = y;
                numSetupIndices++;
            }
        }
    }

    if (numYSpans > 0)
    {
        S.UploadBuffer(YSpanSetupMemory.buffer, 0, YSpanSetups, sizeof(SpanSetupY) * numYSpans);
        S.UploadBuffer(SetupIndicesMemory.buffer, 0, YSpanIndices.data(), numSetupIndices * 4 * 2);
        S.UploadBuffer(RenderPolygonMemory.buffer, 0, RenderPolygons, GPU3D.RenderNumPolygons * sizeof(RenderPolygon));
    }

    MetaUniform meta;
    meta.DispCnt = GPU3D.RenderDispCnt;
    meta.NumPolygons = GPU3D.RenderNumPolygons;
    meta.NumVariants = numVariants;
    meta.AlphaRef = GPU3D.RenderAlphaRef;
    {
        u32 r = (GPU3D.RenderClearAttr1 << 1) & 0x3E; if (r) r++;
        u32 g = (GPU3D.RenderClearAttr1 >> 4) & 0x3E; if (g) g++;
        u32 b = (GPU3D.RenderClearAttr1 >> 9) & 0x3E; if (b) b++;
        u32 a = (GPU3D.RenderClearAttr1 >> 16) & 0x1F;
        meta.ClearColor = r | (g << 8) | (b << 16) | (a << 24);
        meta.ClearDepth = ((GPU3D.RenderClearAttr2 & 0x7FFF) * 0x200) + 0x1FF;
        meta.ClearAttr = GPU3D.RenderClearAttr1 & 0x3F008000;

        u8 xoff = (GPU3D.RenderClearAttr2 >> 16) & 0xFF;
        u8 yoff = (GPU3D.RenderClearAttr2 >> 24) & 0xFF;
        meta.ClearBitmapOffset[0] = (float)xoff / 256.0;
        meta.ClearBitmapOffset[1] = (float)yoff / 256.0;
    }
    for (u32 i = 0; i < 32; i++)
    {
        u32 color = GPU3D.RenderToonTable[i];
        u32 r = (color << 1) & 0x3E;
        u32 g = (color >> 4) & 0x3E;
        u32 b = (color >> 9) & 0x3E;
        if (r) r++;
        if (g) g++;
        if (b) b++;

        meta.ToonTable[i*4+0] = r | (g << 8) | (b << 16);
    }
    for (u32 i = 0; i < 34; i++)
    {
        meta.ToonTable[i*4+1] = GPU3D.RenderFogDensityTable[i];
    }
    for (u32 i = 0; i < 8; i++)
    {
        u32 color = GPU3D.RenderEdgeTable[i];
        u32 r = (color << 1) & 0x3E;
        u32 g = (color >> 4) & 0x3E;
        u32 b = (color >> 9) & 0x3E;
        if (r) r++;
        if (g) g++;
        if (b) b++;

        meta.ToonTable[i*4+2] = r | (g << 8) | (b << 16);
    }
    meta.FogOffset = GPU3D.RenderFogOffset;
    meta.FogShift = GPU3D.RenderFogShift;
    {
        u32 fogR = (GPU3D.RenderFogColor << 1) & 0x3E; if (fogR) fogR++;
        u32 fogG = (GPU3D.RenderFogColor >> 4) & 0x3E; if (fogG) fogG++;
        u32 fogB = (GPU3D.RenderFogColor >> 9) & 0x3E; if (fogB) fogB++;
        u32 fogA = (GPU3D.RenderFogColor >> 16) & 0x1F;
        meta.FogColor = fogR | (fogG << 8) | (fogB << 16) | (fogA << 24);
    }
    S.UploadBuffer(MetaUniformMemory.buffer, 0, &meta, sizeof(meta));

    Dispatch(P_ClearCoarseBinMask, TilesPerLine*TileLines/ClearCoarseBinMaskLocalSize, 1, 1);

    bool wbuffer = false;
    if (numYSpans > 0)
    {
        wbuffer = GPU3D.RenderPolygonRAM[0]->WBuffer;

        Dispatch(P_ClearIndirectWorkCount, (numVariants+31)/32, 1, 1);

        // calculate x-spans
        Dispatch(wbuffer ? P_InterpXSpansW : P_InterpXSpansZ, (numSetupIndices + 31) / 32, 1, 1);
        ComputeBarrier();

        // bin polygons
        Dispatch(P_BinCombined, (GPU3D.RenderNumPolygons + 31) / 32, ScreenWidth/CoarseTileW, ScreenHeight/CoarseTileH);
        ComputeBarrier();

        // calculate list offsets
        Dispatch(P_CalculateWorkListOffset, (numVariants + 31) / 32, 1, 1);
        ComputeBarrier();

        // sort shader work
        Dispatch(P_SortWork, 0, 0, 0, nullptr, VK_NULL_HANDLE, nullptr, BinResultMemory.buffer,
                 offsetof(BinResultHeader, SortWorkWorkCount));
        ComputeBarrier();

        // rasterise
        bool highLightMode = GPU3D.RenderDispCnt & (1<<1);
        const RasteriseKind noTexture[] = {
            R_NoTexture, R_NoTexture, highLightMode ? R_NoTextureHighlight : R_NoTextureToon, R_NoTexture, R_ShadowMask,
        };
        const RasteriseKind useTexture[] = {
            R_UseTextureModulate, R_UseTextureDecal, highLightMode ? R_UseTextureHighlight : R_UseTextureToon,
            R_UseTextureDecal, R_ShadowMask,
        };

        for (u32 i = 0; i < numVariants; i++)
        {
            const Variant& v = variants[i];
            RasteriseKind kind = v.Textured() ? useTexture[v.BlendMode] : noTexture[v.BlendMode];

            PushConstants pc {};
            pc.CurVariant = i;
            pc.InvTextureSize[0] = v.Width ? 1.f / v.Width : 0.f;
            pc.InvTextureSize[1] = v.Height ? 1.f / v.Height : 0.f;
            if (v.CaptureYOffset != -1)
            {
                pc.TexIsCapture = (v.Width == 128) ? 1 : 2;
                pc.CaptureYOffset = (float)v.CaptureYOffset / (float)v.Height;
            }
            else
            {
                pc.TexIsCapture = 0;
            }
            Dispatch(RasteriseIndex(kind, wbuffer), 0, 0, 0, v.Tex, v.Sampler, &pc, BinResultMemory.buffer,
                     offsetof(BinResultHeader, VariantWorkCount) + i*4*4);
        }
    }
    ComputeBarrier();

    // compose final image
    Dispatch(wbuffer ? P_DepthBlendW : P_DepthBlendZ, ScreenWidth/TileSize, ScreenHeight/TileSize, 1);
    ComputeBarrier();

    u32 finalPassShader = 0;
    if (GPU3D.RenderDispCnt & (1<<4))
        finalPassShader |= 0x4;
    if (GPU3D.RenderDispCnt & (1<<7))
        finalPassShader |= 0x2;
    if (GPU3D.RenderDispCnt & (1<<5))
        finalPassShader |= 0x1;

    Dispatch(P_FinalPassFirst + finalPassShader, ScreenWidth/32, ScreenHeight, 1);
    // The 2D compositor samples the result next (Stream::Use inserts that barrier).
}

}
