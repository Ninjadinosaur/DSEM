#include "GLPresenter.h"
#include "LogBuffer.h"

#include <android/native_window.h>
#include <swappy/swappyGL.h>
#include <swappy/swappyGL_extra.h>

namespace ds13r
{

namespace
{
const char* kVertexShader = R"(#version 320 es
uniform vec4 uRect;      // x0, y0, x1, y1 in NDC
uniform int uRotation;   // 0..3 quarter turns clockwise
uniform int uFlipY;
out vec2 vUV;

const vec2 kCorners[4] = vec2[](vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(0.0, 1.0), vec2(1.0, 1.0));

void main()
{
    vec2 p = kCorners[gl_VertexID];
    gl_Position = vec4(mix(uRect.xy, uRect.zw, p), 0.0, 1.0);

    // p is the corner on screen (0,0 = top-left). Work out which texel corner it shows.
    vec2 uv = p;
    if (uRotation == 1) uv = vec2(p.y, 1.0 - p.x);
    else if (uRotation == 2) uv = vec2(1.0 - p.x, 1.0 - p.y);
    else if (uRotation == 3) uv = vec2(1.0 - p.y, p.x);
    if (uFlipY != 0) uv.y = 1.0 - uv.y;
    vUV = uv;
}
)";

const char* kFragmentShader = R"(#version 320 es
precision highp float;
precision highp sampler2DArray;

in vec2 vUV;
out vec4 fragColor;

uniform sampler2DArray uTex;
uniform float uLayer;
uniform vec2 uTexSize;       // source size in texels
uniform vec2 uOutSize;       // destination size in pixels (after rotation)
uniform int uFilter;
uniform int uColorCorrect;
uniform int uSwapRB;         // software frames are BGRA
uniform float uOpacity;

vec3 fetch(vec2 uv)
{
    vec4 c = texture(uTex, vec3(uv, uLayer));
    return uSwapRB != 0 ? c.bgr : c.rgb;
}

vec3 fetchNearest(vec2 texel)
{
    vec4 c = texelFetch(uTex, ivec3(clamp(texel, vec2(0.0), uTexSize - 1.0), int(uLayer)), 0);
    return uSwapRB != 0 ? c.bgr : c.rgb;
}

// Keeps pixels crisp at non-integer scales while only blending at texel edges.
vec3 sharpBilinear(vec2 uv)
{
    vec2 scale = max(floor(uOutSize / uTexSize), vec2(1.0));
    vec2 texel = uv * uTexSize;
    vec2 base = floor(texel);
    vec2 centerDist = fract(texel) - 0.5;
    vec2 range = 0.5 - 0.5 / scale;
    vec2 f = (centerDist - clamp(centerDist, -range, range)) * scale + 0.5;
    return fetch((base + f) / uTexSize);
}

// Colour response of the original DS LCD (after the libretro "nds-color" shader).
vec3 dsColor(vec3 c)
{
    c = pow(c, vec3(2.2));
    mat3 m = mat3(0.815, 0.215, 0.145,
                  0.105, 0.665, 0.095,
                  -0.025, 0.120, 0.730);
    c = clamp(m * c * 0.905, 0.0, 1.0);
    return pow(c, vec3(1.0 / 2.2));
}

void main()
{
    vec3 c;
    vec2 texel = vUV * uTexSize;

    if (uFilter == 0)
    {
        c = fetchNearest(floor(texel));
    }
    else if (uFilter == 1)
    {
        c = fetch(vUV);
    }
    else if (uFilter == 3)
    {
        // Scanlines: darken the lower part of every source row.
        c = sharpBilinear(vUV);
        float y = fract(texel.y);
        c *= mix(1.0, 0.72, smoothstep(0.55, 0.9, y));
    }
    else if (uFilter == 4)
    {
        // LCD grid: thin dark gaps between pixels, like the DS panel up close.
        c = fetchNearest(floor(texel));
        vec2 f = fract(texel);
        vec2 edge = smoothstep(vec2(0.0), vec2(0.12), f) * smoothstep(vec2(1.0), vec2(0.88), f);
        c *= mix(0.78, 1.0, edge.x * edge.y);
    }
    else
    {
        c = sharpBilinear(vUV);
    }

    if (uColorCorrect != 0) c = dsColor(c);
    fragColor = vec4(c, uOpacity);
}
)";

GLuint Compile(GLenum type, const char* src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char log[2048];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        LOGE("GL: shader compile failed: %s", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}
}

GLPresenter::GLPresenter(JavaVM* vm, jobject activity) : vm(vm), activity(activity)
{
}

GLPresenter::~GLPresenter()
{
    Shutdown();
}

bool GLPresenter::Init()
{
    display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, nullptr, nullptr))
    {
        LOGE("GL: eglInitialize failed");
        return false;
    }

    const EGLint configAttribs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
        EGL_NONE,
    };
    EGLint n = 0;
    if (!eglChooseConfig(display, configAttribs, &config, 1, &n) || n < 1)
    {
        LOGE("GL: no suitable EGL config");
        return false;
    }

    const EGLint contextAttribs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 2,
        EGL_NONE,
    };
    context = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttribs);
    if (context == EGL_NO_CONTEXT)
    {
        LOGE("GL: failed to create an OpenGL ES 3.2 context (0x%x)", eglGetError());
        return false;
    }

    // A tiny offscreen surface keeps the context usable while the app has no window
    // (e.g. in the background), so GPU resources survive app switching.
    const EGLint pbufferAttribs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    pbuffer = eglCreatePbufferSurface(display, config, pbufferAttribs);
    MakeCurrent();

    LOGI("GL: %s / %s / %s", glGetString(GL_VENDOR), glGetString(GL_RENDERER), glGetString(GL_VERSION));

    if (!CreateProgram()) return false;

    glGenVertexArrays(1, &vao);

    // Swappy paces frames against the display's vsync (Android Frame Pacing library).
    JNIEnv* env = nullptr;
    if (vm && activity && vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK)
    {
        swappyReady = SwappyGL_init(env, activity);
        if (swappyReady)
        {
            SwappyGL_setAutoSwapInterval(false);
            SwappyGL_setAutoPipelineMode(false);
            SwappyGL_setSwapIntervalNS(SWAPPY_SWAP_60FPS);
        }
    }
    LOGI("GL: frame pacing %s", swappyReady ? "via Swappy" : "via eglSwapInterval");
    return true;
}

bool GLPresenter::CreateProgram()
{
    GLuint vs = Compile(GL_VERTEX_SHADER, kVertexShader);
    GLuint fs = Compile(GL_FRAGMENT_SHADER, kFragmentShader);
    if (!vs || !fs) return false;

    program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok)
    {
        char log[2048];
        glGetProgramInfoLog(program, sizeof(log), nullptr, log);
        LOGE("GL: program link failed: %s", log);
        return false;
    }

    uRect = glGetUniformLocation(program, "uRect");
    uRotation = glGetUniformLocation(program, "uRotation");
    uFlipY = glGetUniformLocation(program, "uFlipY");
    uTex = glGetUniformLocation(program, "uTex");
    uLayer = glGetUniformLocation(program, "uLayer");
    uTexSize = glGetUniformLocation(program, "uTexSize");
    uOutSize = glGetUniformLocation(program, "uOutSize");
    uFilter = glGetUniformLocation(program, "uFilter");
    uColorCorrect = glGetUniformLocation(program, "uColorCorrect");
    uSwapRB = glGetUniformLocation(program, "uSwapRB");
    uOpacity = glGetUniformLocation(program, "uOpacity");
    return true;
}

void GLPresenter::Shutdown()
{
    if (display == EGL_NO_DISPLAY) return;

    ReleaseWindow();
    MakeCurrent();
    if (softTexture) glDeleteTextures(1, &softTexture);
    if (vao) glDeleteVertexArrays(1, &vao);
    if (program) glDeleteProgram(program);
    softTexture = vao = program = 0;

    if (swappyReady)
    {
        SwappyGL_destroy();
        swappyReady = false;
    }

    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (pbuffer != EGL_NO_SURFACE) eglDestroySurface(display, pbuffer);
    if (context != EGL_NO_CONTEXT) eglDestroyContext(display, context);
    eglTerminate(display);
    pbuffer = EGL_NO_SURFACE;
    context = EGL_NO_CONTEXT;
    display = EGL_NO_DISPLAY;
}

void GLPresenter::MakeCurrent()
{
    EGLSurface s = surface != EGL_NO_SURFACE ? surface : pbuffer;
    if (!eglMakeCurrent(display, s, s, context))
        LOGE("GL: eglMakeCurrent failed (0x%x)", eglGetError());
}

void GLPresenter::SetWindow(ANativeWindow* w)
{
    ReleaseWindow();
    if (!w) return;

    window = w;
    ANativeWindow_acquire(window);
    surface = eglCreateWindowSurface(display, config, window, nullptr);
    if (surface == EGL_NO_SURFACE)
    {
        LOGE("GL: eglCreateWindowSurface failed (0x%x)", eglGetError());
        ANativeWindow_release(window);
        window = nullptr;
        return;
    }
    MakeCurrent();
    eglQuerySurface(display, surface, EGL_WIDTH, &surfaceWidth);
    eglQuerySurface(display, surface, EGL_HEIGHT, &surfaceHeight);

    if (swappyReady)
        SwappyGL_setWindow(window);
    else
        eglSwapInterval(display, 1);

    SetTargetRefreshRate(targetHz);
    LOGI("GL: window surface %dx%d", surfaceWidth, surfaceHeight);
}

void GLPresenter::ReleaseWindow()
{
    DestroyWindowSurface();
}

void GLPresenter::DestroyWindowSurface()
{
    if (surface != EGL_NO_SURFACE)
    {
        eglMakeCurrent(display, pbuffer, pbuffer, context);
        eglDestroySurface(display, surface);
        surface = EGL_NO_SURFACE;
    }
    if (window)
    {
        ANativeWindow_release(window);
        window = nullptr;
    }
}

void GLPresenter::SetLayout(const PresentLayout& l)
{
    std::lock_guard<std::mutex> lock(layoutLock);
    layout = l;
}

void GLPresenter::SetSettings(const PresentSettings& s)
{
    std::lock_guard<std::mutex> lock(layoutLock);
    settings = s;
}

void GLPresenter::UploadSoftwareFrame(const void* const* screens, int count, int width, int height, bool bgra)
{
    if (!softTexture || width != softWidth || height != softHeight || count != softLayers)
    {
        // (Re)allocate for this system's screens: 256x192 x2 for DS, 240x160 x1 for GBA.
        if (softTexture) glDeleteTextures(1, &softTexture);
        glGenTextures(1, &softTexture);
        glBindTexture(GL_TEXTURE_2D_ARRAY, softTexture);
        glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_RGBA8, width, height, count);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        softWidth = width;
        softHeight = height;
        softLayers = count;
    }
    glBindTexture(GL_TEXTURE_2D_ARRAY, softTexture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0); // the GL renderer may have left this set
    for (int i = 0; i < count; i++)
        glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, i, width, height, 1, GL_RGBA, GL_UNSIGNED_BYTE, screens[i]);
    softBgra = bgra;
    useHardware = false;
    haveFrame = true;
}

void GLPresenter::SetHardwareFrame(GLuint textureArray, int width, int height)
{
    hwTexture = textureArray;
    hwWidth = width;
    hwHeight = height;
    useHardware = true;
    haveFrame = true;
}

void GLPresenter::SetTargetRefreshRate(float hz)
{
    targetHz = hz;
    if (!window) return;

    // Ask the LTPO panel for exactly this rate: 60 Hz in play saves power and matches the DS.
    // 0 withdraws the request so the system can run menus at 120 Hz.
    ANativeWindow_setFrameRate(window, hz, ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE);
    if (swappyReady && hz > 0.0f)
        SwappyGL_setSwapIntervalNS((uint64_t)(1e9 / hz));
}

bool GLPresenter::Present()
{
    if (!HasWindow()) return false;

    PresentLayout l;
    PresentSettings s;
    {
        std::lock_guard<std::mutex> lock(layoutLock);
        l = layout;
        s = settings;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, surfaceWidth, surfaceHeight);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    float a = ((s.backgroundArgb >> 24) & 0xFF) / 255.0f;
    float r = ((s.backgroundArgb >> 16) & 0xFF) / 255.0f;
    float g = ((s.backgroundArgb >> 8) & 0xFF) / 255.0f;
    float b = (s.backgroundArgb & 0xFF) / 255.0f;
    glClearColor(r, g, b, a);
    glClear(GL_COLOR_BUFFER_BIT);

    if (haveFrame)
    {
        glUseProgram(program);
        glBindVertexArray(vao);
        glActiveTexture(GL_TEXTURE0);
        GLuint tex = useHardware ? hwTexture : softTexture;
        glBindTexture(GL_TEXTURE_2D_ARRAY, tex);
        bool linear = s.filter != ScreenFilter::Nearest && s.filter != ScreenFilter::LcdGrid;
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);

        float texW = useHardware ? (float)hwWidth : (float)softWidth;
        float texH = useHardware ? (float)hwHeight : (float)softHeight;

        glUniform1i(uTex, 0);
        glUniform2f(uTexSize, texW, texH);
        glUniform1i(uFilter, (int)s.filter);
        glUniform1i(uColorCorrect, s.colorCorrection ? 1 : 0);
        glUniform1i(uSwapRB, !useHardware && softBgra ? 1 : 0);
        glUniform1i(uFlipY, 0);

        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        for (const ScreenQuad& q : l.quads)
        {
            if (q.w <= 0 || q.h <= 0 || q.opacity <= 0.0f) continue;
            float x0 = q.x / surfaceWidth * 2.0f - 1.0f;
            float x1 = (q.x + q.w) / surfaceWidth * 2.0f - 1.0f;
            float y0 = 1.0f - q.y / surfaceHeight * 2.0f;
            float y1 = 1.0f - (q.y + q.h) / surfaceHeight * 2.0f;
            int rot = ((q.rotation / 90) % 4 + 4) % 4;
            // Output size measured along the texture's own axes.
            bool sideways = rot == 1 || rot == 3;

            glUniform4f(uRect, x0, y0, x1, y1);
            glUniform1i(uRotation, rot);
            glUniform1f(uLayer, (float)q.screen);
            glUniform2f(uOutSize, sideways ? q.h : q.w, sideways ? q.w : q.h);
            glUniform1f(uOpacity, q.opacity);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        }
        glDisable(GL_BLEND);
    }

    bool ok = swappyReady ? SwappyGL_swap(display, surface) : eglSwapBuffers(display, surface);
    if (!ok)
    {
        EGLint err = eglGetError();
        LOGW("GL: swap failed (0x%x)", err);
        if (err == EGL_BAD_SURFACE || err == EGL_BAD_NATIVE_WINDOW) DestroyWindowSurface();
    }
    return ok;
}

bool GLPresenter::ReadScreen(int screen, std::vector<uint32_t>& out, int& width, int& height)
{
    GLuint tex = useHardware ? hwTexture : softTexture;
    width = useHardware ? hwWidth : softWidth;
    height = useHardware ? hwHeight : softHeight;
    if (!tex || !haveFrame) return false;

    GLuint fbo;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, tex, 0, screen);
    out.resize((size_t)width * height);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, out.data());
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);

    for (uint32_t& p : out)
    {
        // melonDS software frames are BGRA; convert to RGBA. Neither console has alpha.
        if (!useHardware && softBgra) p = (p & 0x0000FF00) | ((p >> 16) & 0xFF) | ((p & 0xFF) << 16);
        p |= 0xFF000000;
    }
    return true;
}

}
