#pragma once

#include "Presenter.h"

#include <EGL/egl.h>
#include <GLES3/gl32.h>
#include <jni.h>
#include <mutex>

namespace ds13r
{

// OpenGL ES 3.2 presenter. Also owns the EGL context that the OpenGL ES DS renderer
// draws with, so both live on the emulation thread.
class GLPresenter : public Presenter
{
public:
    GLPresenter(JavaVM* vm, jobject activity);
    ~GLPresenter() override;

    bool Init() override;
    void Shutdown() override;

    void SetWindow(ANativeWindow* window) override;
    void ReleaseWindow() override;
    bool HasWindow() const override { return surface != EGL_NO_SURFACE && window != nullptr; }

    void SetLayout(const PresentLayout& layout) override;
    void SetSettings(const PresentSettings& settings) override;

    void UploadSoftwareFrame(const void* const* screens, int count, int width, int height, bool bgra) override;
    // Hardware renderer output: a 2-layer texture array, RGBA, `scale` times native size.
    void SetHardwareFrame(GLuint textureArray, int width, int height);

    bool Present() override;
    void SetTargetRefreshRate(float hz) override;

    // Makes the context current (with the offscreen surface if there is no window).
    void MakeCurrent();

    // Reads back one screen of the current frame as RGBA8888 (for screenshots).
    bool ReadScreen(int screen, std::vector<uint32_t>& out, int& width, int& height);

private:
    bool CreateProgram();
    void DestroyWindowSurface();

    JavaVM* vm;
    jobject activity;
    bool swappyReady = false;

    EGLDisplay display = EGL_NO_DISPLAY;
    EGLConfig config = nullptr;
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface pbuffer = EGL_NO_SURFACE;
    EGLSurface surface = EGL_NO_SURFACE;
    ANativeWindow* window = nullptr;
    int surfaceWidth = 0, surfaceHeight = 0;

    GLuint program = 0;
    GLuint vao = 0;
    GLint uRect = -1, uRotation = -1, uLayer = -1, uTexSize = -1, uOutSize = -1;
    GLint uFilter = -1, uColorCorrect = -1, uOpacity = -1, uSwapRB = -1, uFlipY = -1, uTex = -1;

    GLuint softTexture = 0;       // texture array for software-rendered frames
    int softWidth = 0, softHeight = 0, softLayers = 0;
    bool softBgra = true;
    GLuint hwTexture = 0;         // owned by the hardware renderer, not us
    int hwWidth = 0, hwHeight = 0;
    bool useHardware = false;
    bool haveFrame = false;

    std::mutex layoutLock;
    PresentLayout layout;
    PresentSettings settings;

    float targetHz = 60.0f;
};

}
