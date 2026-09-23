#pragma once

#include <cstdint>
#include <vector>

struct ANativeWindow;

// Draws the two DS screens onto the phone's surface (features.md §3).
// The layout itself (where each screen goes, rotation, swap) is computed on the Kotlin side,
// which also owns touch mapping, so both always agree on where the screens are.
namespace ds13r
{

enum class ScreenFilter : int
{
    Nearest = 0,
    Bilinear = 1,
    SharpBilinear = 2,
    Scanlines = 3,
    LcdGrid = 4,
    Xbrz = 5,
    Crt = 6,
};

struct ScreenQuad
{
    int screen = 0;          // 0 = top DS screen, 1 = bottom
    float x = 0, y = 0;      // destination rect in surface pixels, top-left origin
    float w = 0, h = 0;
    int rotation = 0;        // clockwise degrees: 0, 90, 180, 270
    float opacity = 1.0f;
};

struct PresentLayout
{
    std::vector<ScreenQuad> quads;
};

struct PresentSettings
{
    ScreenFilter filter = ScreenFilter::SharpBilinear;
    bool colorCorrection = false;
    // Background drawn behind and between the screens. True black keeps AMOLED pixels off.
    uint32_t backgroundArgb = 0xFF000000;
};

class Presenter
{
public:
    virtual ~Presenter() = default;

    // Called on the emulation thread, which owns the graphics context.
    virtual bool Init() = 0;
    virtual void Shutdown() = 0;

    virtual void SetWindow(ANativeWindow* window) = 0;
    virtual void ReleaseWindow() = 0;
    virtual bool HasWindow() const = 0;

    virtual void SetLayout(const PresentLayout& layout) = 0;
    virtual void SetSettings(const PresentSettings& settings) = 0;

    // Software-rendered frame: `count` screens of width x height 32-bit pixels
    // (BGRA from melonDS, RGBA from mGBA).
    virtual void UploadSoftwareFrame(const void* const* screens, int count, int width, int height, bool bgra) = 0;

    // Draws the current frame and hands it to the display. Returns false if nothing was shown.
    virtual bool Present() = 0;

    // Target presentation rate: 60 Hz during play, up to 120 Hz while fast-forwarding,
    // 0 = no preference (paused, so menus can use the panel's full 120 Hz).
    virtual void SetTargetRefreshRate(float hz) = 0;
};

}
