#include "ThreeDsCore.h"
#include "ConfigStore.h"
#include "GLPresenter.h"
#include "IoWorker.h"
#include "LogBuffer.h"

#include <libretro.h>

#include <EGL/egl.h>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstring>
#include <cerrno>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

// libretro leaves this opaque for the frontend to define.
struct retro_vfs_file_handle
{
    int fd = -1;
    int64_t pos = 0;
    std::string path;
};

namespace ds13r
{

// The libretro entry points, resolved from libazahar_libretro.so.
struct LibretroApi
{
    void* handle = nullptr;
#define RETRO_FN(ret, name, args) ret(*name) args = nullptr;
    RETRO_FN(void, retro_set_environment, (retro_environment_t))
    RETRO_FN(void, retro_set_video_refresh, (retro_video_refresh_t))
    RETRO_FN(void, retro_set_audio_sample, (retro_audio_sample_t))
    RETRO_FN(void, retro_set_audio_sample_batch, (retro_audio_sample_batch_t))
    RETRO_FN(void, retro_set_input_poll, (retro_input_poll_t))
    RETRO_FN(void, retro_set_input_state, (retro_input_state_t))
    RETRO_FN(void, retro_init, (void))
    RETRO_FN(void, retro_deinit, (void))
    RETRO_FN(void, retro_get_system_av_info, (retro_system_av_info*))
    RETRO_FN(void, retro_reset, (void))
    RETRO_FN(void, retro_run, (void))
    RETRO_FN(size_t, retro_serialize_size, (void))
    RETRO_FN(bool, retro_serialize, (void*, size_t))
    RETRO_FN(bool, retro_unserialize, (const void*, size_t))
    RETRO_FN(bool, retro_load_game, (const retro_game_info*))
    RETRO_FN(void, retro_unload_game, (void))
    RETRO_FN(void, retro_cheat_reset, (void))
    RETRO_FN(void, retro_cheat_set, (unsigned, bool, const char*))
#undef RETRO_FN
};

struct ThreeDsCore::HwRender
{
    retro_hw_render_callback cb {};
    bool requested = false;
};

namespace
{
// libretro callbacks are plain C functions with no user pointer: route them to the active core.
ThreeDsCore* gActive = nullptr;

bool EnvTrampoline(unsigned cmd, void* data) { return gActive && gActive->Environment(cmd, data); }
void VideoTrampoline(const void* data, unsigned w, unsigned h, size_t pitch)
{
    if (gActive) gActive->VideoRefresh(data, w, h, pitch);
}
void AudioSampleTrampoline(int16_t l, int16_t r)
{
    int16_t s[2] = {l, r};
    if (gActive) gActive->AudioBatch(s, 1);
}
size_t AudioBatchTrampoline(const int16_t* data, size_t frames) { return gActive ? gActive->AudioBatch(data, frames) : frames; }
void InputPollTrampoline() {}
int16_t InputStateTrampoline(unsigned port, unsigned device, unsigned index, unsigned id)
{
    return gActive ? gActive->InputState(port, device, index, id) : 0;
}
uintptr_t FramebufferTrampoline() { return gActive ? gActive->CurrentFramebuffer() : 0; }
retro_proc_address_t ProcAddress(const char* sym)
{
    auto p = reinterpret_cast<retro_proc_address_t>(eglGetProcAddress(sym));
    if (!p)
    {
        static void* gles = dlopen("libGLESv3.so", RTLD_NOW | RTLD_LOCAL);
        if (gles) p = reinterpret_cast<retro_proc_address_t>(dlsym(gles, sym));
    }
    return p;
}

void LogCallback(enum retro_log_level level, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    LogLevel l = level >= RETRO_LOG_ERROR ? LogLevel::Error : level == RETRO_LOG_WARN ? LogLevel::Warn
               : level == RETRO_LOG_INFO ? LogLevel::Info : LogLevel::Debug;
    char buf[1024];
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    size_t n = strlen(buf);
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
    LogLine(l, "3DS %s", buf);
}

bool SensorState(unsigned, enum retro_sensor_action, unsigned) { return true; }
float SensorInput(unsigned port, unsigned id)
{
    if (!gActive) return 0.0f;
    // Phone accelerometer (m/s^2) and gyroscope (rad/s) stand in for the 3DS's own.
    switch (id)
    {
    case RETRO_SENSOR_ACCELEROMETER_X: return gActive->Host().MotionQuery(0) / 9.81f;
    case RETRO_SENSOR_ACCELEROMETER_Y: return gActive->Host().MotionQuery(1) / 9.81f;
    case RETRO_SENSOR_ACCELEROMETER_Z: return gActive->Host().MotionQuery(2) / 9.81f;
    case RETRO_SENSOR_GYROSCOPE_X: return gActive->Host().MotionQuery(3);
    case RETRO_SENSOR_GYROSCOPE_Y: return gActive->Host().MotionQuery(4);
    case RETRO_SENSOR_GYROSCOPE_Z: return gActive->Host().MotionQuery(5);
    default: return 0.0f;
    }
}

// ---- File access (libretro VFS) ----
// Games picked through Android's document system arrive as an open file descriptor; the path
// behind it can't be reopened by us (Android checks storage permission on every open, even via
// /proc/self/fd). So the engine's file access comes through here: the game's path is served
// from our descriptor, everything else (saves, system data) is an ordinary file in app storage.
// Reads and writes use pread/pwrite with a per-handle position, so duplicated descriptors,
// which share one kernel file offset, never disturb each other.
const char* VfsGetPath(retro_vfs_file_handle* h) { return h->path.c_str(); }

retro_vfs_file_handle* VfsOpen(const char* path, unsigned mode, unsigned)
{
    if (!path) return nullptr;
    int fd = -1;
    if (gActive && gActive->IsRomPath(path))
    {
        if (mode & RETRO_VFS_FILE_ACCESS_WRITE) return nullptr; // the game file is read-only
        fd = dup(gActive->RomFd());
    }
    else
    {
        int flags;
        bool write = mode & RETRO_VFS_FILE_ACCESS_WRITE;
        bool read = mode & RETRO_VFS_FILE_ACCESS_READ;
        if (!write) flags = O_RDONLY;
        else flags = (read ? O_RDWR : O_WRONLY) | O_CREAT | ((mode & RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING) ? 0 : O_TRUNC);
        fd = open(path, flags | O_CLOEXEC, 0644);
    }
    if (fd < 0) return nullptr;
    auto* h = new retro_vfs_file_handle;
    h->fd = fd;
    h->path = path;
    return h;
}

int VfsClose(retro_vfs_file_handle* h)
{
    if (!h) return -1;
    int r = close(h->fd);
    delete h;
    return r;
}

int64_t VfsSize(retro_vfs_file_handle* h)
{
    struct stat st;
    return fstat(h->fd, &st) == 0 ? (int64_t)st.st_size : -1;
}

int64_t VfsTruncate(retro_vfs_file_handle* h, int64_t length) { return ftruncate(h->fd, length) == 0 ? 0 : -1; }
int64_t VfsTell(retro_vfs_file_handle* h) { return h->pos; }

int64_t VfsSeek(retro_vfs_file_handle* h, int64_t offset, int whence)
{
    int64_t base = whence == RETRO_VFS_SEEK_POSITION_CURRENT ? h->pos
                 : whence == RETRO_VFS_SEEK_POSITION_END ? VfsSize(h) : 0;
    if (base < 0 || base + offset < 0) return -1;
    h->pos = base + offset;
    // 0 on success, like fseek: libretro's own implementation does this and Azahar relies on it.
    return 0;
}

int64_t VfsRead(retro_vfs_file_handle* h, void* buf, uint64_t len)
{
    uint64_t done = 0;
    while (done < len)
    {
        ssize_t n = pread(h->fd, static_cast<uint8_t*>(buf) + done, len - done, h->pos + (int64_t)done);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return done ? (int64_t)done : -1;
        if (n == 0) break;
        done += (uint64_t)n;
    }
    h->pos += (int64_t)done;
    return (int64_t)done;
}

int64_t VfsWrite(retro_vfs_file_handle* h, const void* buf, uint64_t len)
{
    uint64_t done = 0;
    while (done < len)
    {
        ssize_t n = pwrite(h->fd, static_cast<const uint8_t*>(buf) + done, len - done, h->pos + (int64_t)done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return done ? (int64_t)done : -1;
        done += (uint64_t)n;
    }
    h->pos += (int64_t)done;
    return (int64_t)done;
}

int VfsFlush(retro_vfs_file_handle*) { return 0; } // unbuffered: writes are already in the kernel
int VfsRemove(const char* path) { return unlink(path) == 0 || rmdir(path) == 0 ? 0 : -1; }
int VfsRename(const char* from, const char* to) { return rename(from, to); }

retro_vfs_interface gVfs = {
    VfsGetPath, VfsOpen, VfsClose, VfsSize, VfsTell, VfsSeek, VfsRead, VfsWrite, VfsFlush, VfsRemove, VfsRename,
    VfsTruncate,
    // v3 (stat, directories) is not offered: Azahar only asks for v1 file access.
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
};

constexpr size_t kRingFrames = 16384;
constexpr double kOutputRate = 48000.0;
}

ThreeDsCore::ThreeDsCore(CoreHost& h) : host(h), api(std::make_unique<LibretroApi>()), hw(std::make_unique<HwRender>())
{
    ring.resize(kRingFrames * 2);
    systemDir = host.FilesDir() + "/3ds/system";
    saveDir = host.FilesDir() + "/3ds/saves";
    MakeDirs(systemDir);
    MakeDirs(saveDir);
}

ThreeDsCore::~ThreeDsCore()
{
    Shutdown();
}

bool ThreeDsCore::LoadLibrary()
{
    // RTLD_LOCAL keeps its symbols (its own teakra, xxHash, libc++...) away from ours.
    api->handle = dlopen("libazahar_libretro.so", RTLD_NOW | RTLD_LOCAL);
    if (!api->handle)
    {
        LOGE("3DS: could not load the 3DS engine: %s", dlerror());
        return false;
    }
    bool ok = true;
#define RESOLVE(name)                                                                            \
    api->name = reinterpret_cast<decltype(api->name)>(dlsym(api->handle, #name));                \
    if (!api->name) { LOGE("3DS: missing %s", #name); ok = false; }
    RESOLVE(retro_set_environment) RESOLVE(retro_set_video_refresh) RESOLVE(retro_set_audio_sample)
    RESOLVE(retro_set_audio_sample_batch) RESOLVE(retro_set_input_poll) RESOLVE(retro_set_input_state)
    RESOLVE(retro_init) RESOLVE(retro_deinit) RESOLVE(retro_get_system_av_info) RESOLVE(retro_reset)
    RESOLVE(retro_run) RESOLVE(retro_serialize_size) RESOLVE(retro_serialize) RESOLVE(retro_unserialize)
    RESOLVE(retro_load_game) RESOLVE(retro_unload_game) RESOLVE(retro_cheat_reset) RESOLVE(retro_cheat_set)
#undef RESOLVE
    return ok;
}

std::string ThreeDsCore::LoadGame(int fd, const std::string& extension, const std::string& gameKey)
{
    // The engine opens the game by path. Give it a named link to our open file descriptor, so
    // games picked through Android's document system (no real path) work, extension included.
    romFd = dup(fd);
    if (romFd < 0) return "The game file could not be opened.";
    romLink = host.FilesDir() + "/3ds/current" + extension;
    unlink(romLink.c_str());
    std::string target = "/proc/self/fd/" + std::to_string(romFd);
    if (symlink(target.c_str(), romLink.c_str()) != 0)
    {
        LOGW("3DS: symlink failed (%s), using the descriptor path", strerror(errno));
        romLink = target;
    }
    const std::string& path = romLink;

    if (!LoadLibrary()) return "The 3DS engine could not be loaded.";
    gActive = this;

    api->retro_set_environment(EnvTrampoline);
    api->retro_set_video_refresh(VideoTrampoline);
    api->retro_set_audio_sample(AudioSampleTrampoline);
    api->retro_set_audio_sample_batch(AudioBatchTrampoline);
    api->retro_set_input_poll(InputPollTrampoline);
    api->retro_set_input_state(InputStateTrampoline);
    api->retro_init();

    retro_game_info info {};
    info.path = path.c_str();
    if (!api->retro_load_game(&info))
    {
        std::string msg = lastMessage.empty() ? "This 3DS game could not be loaded." : lastMessage;
        api->retro_deinit();
        return msg;
    }

    retro_system_av_info av {};
    api->retro_get_system_av_info(&av);
    frameW = (int)av.geometry.base_width;
    frameH = (int)av.geometry.base_height;
    if (av.timing.sample_rate > 0) coreSampleRate = av.timing.sample_rate;

    if (hw->requested)
    {
        // Our GL context is current on this (the emulation) thread; the core draws into our FBO.
        if (host.Presenter()) host.Presenter()->MakeCurrent();
        CreateFramebuffer((int)av.geometry.max_width, (int)av.geometry.max_height);
        // The engine boots the game here, once it has a graphics context. It reports any
        // failure only as an on-screen message.
        lastMessage.clear();
        if (hw->cb.context_reset) hw->cb.context_reset();
        if (!lastMessage.empty())
        {
            api->retro_unload_game();
            api->retro_deinit();
            DestroyFramebuffer();
            return lastMessage;
        }
    }

    gameLoaded = true;
    LOGI("3DS: loaded %s (%dx%d frame, max %ux%u, audio %.0f Hz)", gameKey.c_str(), frameW, frameH,
         av.geometry.max_width, av.geometry.max_height, coreSampleRate);
    return {};
}

void ThreeDsCore::CreateFramebuffer(int width, int height)
{
    DestroyFramebuffer();
    fboW = std::max(width, 1);
    fboH = std::max(height, 1);
    glGenTextures(1, &fboColor);
    glBindTexture(GL_TEXTURE_2D, fboColor);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, fboW, fboH);
    glGenRenderbuffers(1, &fboDepth);
    glBindRenderbuffer(GL_RENDERBUFFER, fboDepth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, fboW, fboH);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fboColor, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, fboDepth);
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) LOGE("3DS: framebuffer incomplete (0x%x)", status);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void ThreeDsCore::DestroyFramebuffer()
{
    if (fbo) glDeleteFramebuffers(1, &fbo);
    if (fboColor) glDeleteTextures(1, &fboColor);
    if (fboDepth) glDeleteRenderbuffers(1, &fboDepth);
    if (outTexture) glDeleteTextures(1, &outTexture);
    fbo = fboColor = fboDepth = outTexture = 0;
    outW = outH = 0;
}

std::string ThreeDsCore::OptionOverride(const std::string& key) const
{
    const ConfigStore& config = host.Config();
    // Our settings, mapped onto Azahar's core options.
    if (key == "citra_graphics_api") return "OpenGL"; // the presenter's context is OpenGL ES
    if (key == "citra_resolution_factor")
    {
        int scale = (int)config.GetInt("3ds.scale", 3);
        int cap = (int)config.GetInt("video.scaleCap", 0);
        if (cap > 0) scale = std::min(scale, cap);
        return std::to_string(std::clamp(scale, 1, 8));
    }
    if (key == "citra_layout_option")
    {
        // Portrait always stacks the screens; landscape follows the player's choice.
        if (!config.GetBool("3ds.landscape", false)) return "default";
        switch (config.GetInt("3ds.landscapeLayout", 0))
        {
        case 1: return "side_by_side";
        case 2: return "large_screen";
        default: return "default";
        }
    }
    if (key == "citra_swap_screen") return config.GetBool("3ds.swap", false) ? "Bottom" : "Top";
    if (key == "citra_texture_filter")
    {
        static const char* const kFilters[] = {"none", "xBRZ", "ScaleForce", "MMPX", "Bicubic"};
        int i = (int)config.GetInt("3ds.textureFilter", 0);
        return kFilters[i >= 0 && i < 5 ? i : 0];
    }
    if (key == "citra_is_new_3ds") return config.GetBool("3ds.new3ds", true) ? "New 3DS" : "Old 3DS";
    if (key == "citra_use_cpu_jit") return "enabled";
    // CPU vertex shading (used only for draws the GPU driver can't link) must use the
    // interpreter: Azahar's ARM64 shader JIT mirrors and garbles geometry in Pokemon X
    // (verified on the 13R: same save state renders correctly with the interpreter).
    if (key == "citra_use_shader_jit") return "disabled";
    // Touch arrives as absolute positions; the right stick is a real C-stick, not a mouse.
    if (key == "citra_enable_touch_touchscreen") return "enabled";
    if (key == "citra_enable_mouse_touchscreen") return "disabled";
    if (key == "citra_enable_touch_pointer_timeout") return "enabled";
    if (key == "citra_analog_function") return "c_stick";
    if (key == "citra_enable_motion") return "enabled";
    return {};
}

bool ThreeDsCore::Environment(unsigned cmd, void* data)
{
    // Match the full command: some (e.g. GET_VFS_INTERFACE) include the EXPERIMENTAL bit in their value.
    switch (cmd)
    {
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        static_cast<retro_log_callback*>(data)->log = LogCallback;
        return true;
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
        *static_cast<const char**>(data) = systemDir.c_str();
        return true;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        *static_cast<const char**>(data) = saveDir.c_str();
        return true;
    case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
        *static_cast<unsigned*>(data) = 2;
        return true;
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
    {
        auto* opts = static_cast<const retro_core_options_v2*>(data);
        for (auto* d = opts ? opts->definitions : nullptr; d && d->key; d++)
            options[d->key] = d->default_value ? d->default_value : (d->values[0].value ? d->values[0].value : "");
        return true;
    }
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS:
    {
        for (auto* d = static_cast<const retro_core_option_definition*>(data); d && d->key; d++)
            options[d->key] = d->default_value ? d->default_value : (d->values[0].value ? d->values[0].value : "");
        return true;
    }
    case RETRO_ENVIRONMENT_SET_VARIABLES:
    {
        // v0 format: "Description; default|other|..."
        for (auto* v = static_cast<const retro_variable*>(data); v && v->key; v++)
        {
            std::string s = v->value ? v->value : "";
            size_t semi = s.find("; ");
            std::string values = semi == std::string::npos ? s : s.substr(semi + 2);
            options[v->key] = values.substr(0, values.find('|'));
        }
        return true;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE:
    {
        auto* v = static_cast<retro_variable*>(data);
        if (!v || !v->key) return false;
        std::string over = OptionOverride(v->key);
        if (!over.empty()) options[v->key] = over;
        auto it = options.find(v->key);
        if (it == options.end()) return false;
        v->value = it->second.c_str();
        return true;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        *static_cast<bool*>(data) = optionsChanged;
        optionsChanged = false;
        return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        return *static_cast<retro_pixel_format*>(data) == RETRO_PIXEL_FORMAT_XRGB8888;
    case RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER:
        *static_cast<unsigned*>(data) = RETRO_HW_CONTEXT_OPENGLES3;
        return true;
    case RETRO_ENVIRONMENT_SET_HW_RENDER:
    {
        auto* cb = static_cast<retro_hw_render_callback*>(data);
        if (cb->context_type != RETRO_HW_CONTEXT_OPENGLES3 && cb->context_type != RETRO_HW_CONTEXT_OPENGLES_VERSION)
        {
            LOGW("3DS: core asked for an unsupported renderer (%d)", (int)cb->context_type);
            return false;
        }
        cb->get_current_framebuffer = FramebufferTrampoline;
        cb->get_proc_address = ProcAddress;
        hw->cb = *cb;
        hw->requested = true;
        return true;
    }
    case RETRO_ENVIRONMENT_SET_HW_SHARED_CONTEXT:
    case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS:
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
    case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
        return true;
    case RETRO_ENVIRONMENT_GET_VFS_INTERFACE:
    {
        auto* info = static_cast<retro_vfs_interface_info*>(data);
        if (info->required_interface_version > 2) return false;
        info->required_interface_version = 2;
        info->iface = &gVfs;
        LOGI("3DS: file access through the app (VFS v2)");
        return true;
    }
    case RETRO_ENVIRONMENT_GET_JIT_CAPABLE:
        *static_cast<bool*>(data) = true;
        return true;
    case RETRO_ENVIRONMENT_GET_SENSOR_INTERFACE:
    {
        auto* s = static_cast<retro_sensor_interface*>(data);
        s->set_sensor_state = SensorState;
        s->get_sensor_input = SensorInput;
        return true;
    }
    case RETRO_ENVIRONMENT_SET_GEOMETRY:
    {
        // Azahar passes a retro_system_av_info here; its geometry comes first, so this reads either.
        auto* g = static_cast<const retro_game_geometry*>(data);
        frameW = (int)g->base_width;
        frameH = (int)g->base_height;
        if (fbo && ((int)g->max_width > fboW || (int)g->max_height > fboH))
            CreateFramebuffer(std::max((int)g->max_width, fboW), std::max((int)g->max_height, fboH));
        return true;
    }
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
    {
        auto* av = static_cast<const retro_system_av_info*>(data);
        frameW = (int)av->geometry.base_width;
        frameH = (int)av->geometry.base_height;
        if ((int)av->geometry.max_width > fboW || (int)av->geometry.max_height > fboH)
            CreateFramebuffer((int)av->geometry.max_width, (int)av->geometry.max_height);
        return true;
    }
    case RETRO_ENVIRONMENT_SET_MESSAGE:
    {
        auto* m = static_cast<const retro_message*>(data);
        if (m && m->msg)
        {
            lastMessage = m->msg;
            LOGW("3DS: %s", m->msg);
        }
        return true;
    }
    case RETRO_ENVIRONMENT_SHUTDOWN:
        host.OnCoreStopped(0);
        return true;
    default:
        return false;
    }
}

void ThreeDsCore::ApplySettings()
{
    // The core re-reads its options on the next frame.
    optionsChanged = true;
}

int ThreeDsCore::RenderScale() const
{
    std::string v = OptionOverride("citra_resolution_factor");
    return v.empty() ? 1 : std::atoi(v.c_str());
}

void ThreeDsCore::RunFrame(const CoreInput& in)
{
    if (!gameLoaded) return;
    input = in;
    gActive = this;
    api->retro_run();
}

void ThreeDsCore::VideoRefresh(const void* data, unsigned width, unsigned height, size_t pitch)
{
    if (!data || width == 0 || height == 0) return; // duplicate frame
    frameW = (int)width;
    frameH = (int)height;
    if (data == RETRO_HW_FRAME_BUFFER_VALID)
    {
        CopyToPresenterTexture(width, height);
        softwareFrame = false;
    }
    else
    {
        // Software renderer: XRGB8888, i.e. B,G,R,X in memory.
        softPixels.resize((size_t)width * height);
        for (unsigned y = 0; y < height; y++)
            memcpy(&softPixels[(size_t)y * width], static_cast<const uint8_t*>(data) + y * pitch, width * 4);
        softwareFrame = true;
    }
    haveFrame = true;
}

// The core draws bottom-left-origin into our FBO; copy (flipped) into a 1-layer texture array,
// which is what the presenter samples for hardware frames.
void ThreeDsCore::CopyToPresenterTexture(unsigned width, unsigned height)
{
    if (!outTexture || (int)width != outW || (int)height != outH)
    {
        if (outTexture) glDeleteTextures(1, &outTexture);
        glGenTextures(1, &outTexture);
        glBindTexture(GL_TEXTURE_2D_ARRAY, outTexture);
        glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_RGBA8, (GLsizei)width, (GLsizei)height, 1);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        outW = (int)width;
        outH = (int)height;
    }
    GLuint dst = 0;
    glGenFramebuffers(1, &dst);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dst);
    glFramebufferTextureLayer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, outTexture, 0, 0);
    glDisable(GL_SCISSOR_TEST);
    glBlitFramebuffer(0, 0, (GLint)width, (GLint)height, 0, (GLint)height, (GLint)width, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    // The presenter blends by alpha; the core's alpha channel is not meaningful, so make it opaque.
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &dst);
}

bool ThreeDsCore::GetFrame(FrameInfo& out)
{
    if (!haveFrame) return false;
    out.screenCount = 1;
    if (softwareFrame)
    {
        out.hardware = false;
        out.screens[0] = softPixels.data();
        out.width = frameW;
        out.height = frameH;
        out.bgra = true;
        return true;
    }
    out.hardware = true;
    out.texture = outTexture;
    out.width = outW;
    out.height = outH;
    return true;
}

size_t ThreeDsCore::AudioBatch(const int16_t* data, size_t frames)
{
    // Linear resampler from the core's rate to 48 kHz; the skew keeps output locked to the display.
    double step = coreSampleRate * audioSkew / kOutputRate;
    std::lock_guard<std::mutex> l(audioLock);
    for (size_t i = 0; i < frames; i++)
    {
        int16_t curL = data[i * 2], curR = data[i * 2 + 1];
        while (resamplePos < 1.0)
        {
            if (ringLevel >= kRingFrames)
            {
                ringRead = (ringRead + 1) % kRingFrames;
                ringLevel--;
            }
            ring[ringWrite * 2] = (int16_t)(prevL + (curL - prevL) * resamplePos);
            ring[ringWrite * 2 + 1] = (int16_t)(prevR + (curR - prevR) * resamplePos);
            ringWrite = (ringWrite + 1) % kRingFrames;
            ringLevel++;
            resamplePos += step;
        }
        resamplePos -= 1.0;
        prevL = curL;
        prevR = curR;
    }
    return frames;
}

int ThreeDsCore::ReadAudio(int16_t* out, int frames)
{
    std::lock_guard<std::mutex> l(audioLock);
    int n = (int)std::min<size_t>((size_t)frames, ringLevel);
    for (int i = 0; i < n; i++)
    {
        out[i * 2] = ring[ringRead * 2];
        out[i * 2 + 1] = ring[ringRead * 2 + 1];
        ringRead = (ringRead + 1) % kRingFrames;
    }
    ringLevel -= (size_t)n;
    return n;
}

int ThreeDsCore::AudioFill()
{
    std::lock_guard<std::mutex> l(audioLock);
    return (int)ringLevel;
}

int16_t ThreeDsCore::InputState(unsigned port, unsigned device, unsigned index, unsigned id)
{
    if (port != 0) return 0;
    auto axis = [](float v) { return (int16_t)std::clamp(v * 32767.0f, -32767.0f, 32767.0f); };
    switch (device)
    {
    case RETRO_DEVICE_JOYPAD:
    {
        // Our key bits (DS layout plus ZL/ZR/HOME) -> libretro joypad ids.
        static const uint32_t kMap[16] = {
            1u << 1,  // B
            1u << 11, // Y
            1u << 2,  // SELECT
            1u << 3,  // START
            1u << 6,  // UP
            1u << 7,  // DOWN
            1u << 5,  // LEFT
            1u << 4,  // RIGHT
            1u << 0,  // A
            1u << 10, // X
            1u << 9,  // L
            1u << 8,  // R
            kKeyZL,   // L2
            kKeyZR,   // R2
            kKeyHome, // L3 (Home / swap screens)
            0,        // R3
        };
        if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
        {
            int16_t mask = 0;
            for (unsigned i = 0; i < 16; i++)
                if (input.keys & kMap[i]) mask |= (int16_t)(1 << i);
            return mask;
        }
        return id < 16 && (input.keys & kMap[id]) ? 1 : 0;
    }
    case RETRO_DEVICE_ANALOG:
        if (index == RETRO_DEVICE_INDEX_ANALOG_LEFT) return axis(id == RETRO_DEVICE_ID_ANALOG_X ? input.circleX : input.circleY);
        if (index == RETRO_DEVICE_INDEX_ANALOG_RIGHT) return axis(id == RETRO_DEVICE_ID_ANALOG_X ? input.cstickX : input.cstickY);
        return 0;
    case RETRO_DEVICE_POINTER:
        switch (id)
        {
        case RETRO_DEVICE_ID_POINTER_X: return (int16_t)std::clamp((input.pointerX * 2.0f - 1.0f) * 32767.0f, -32767.0f, 32767.0f);
        case RETRO_DEVICE_ID_POINTER_Y: return (int16_t)std::clamp((input.pointerY * 2.0f - 1.0f) * 32767.0f, -32767.0f, 32767.0f);
        case RETRO_DEVICE_ID_POINTER_PRESSED: return input.pointerDown ? 1 : 0;
        case RETRO_DEVICE_ID_POINTER_COUNT: return input.pointerDown ? 1 : 0;
        default: return 0;
        }
    default:
        return 0;
    }
}

bool ThreeDsCore::SaveState(std::vector<uint8_t>& out)
{
    if (!gameLoaded) return false;
    gActive = this;
    size_t size = api->retro_serialize_size();
    if (size == 0) return false;
    out.resize(size);
    return api->retro_serialize(out.data(), size);
}

bool ThreeDsCore::LoadState(const uint8_t* data, size_t len)
{
    if (!gameLoaded) return false;
    gActive = this;
    return api->retro_unserialize(data, len);
}

void ThreeDsCore::SetCheats(const std::vector<std::string>& codes)
{
    if (!gameLoaded) return;
    // Azahar's libretro core does not implement cheats yet (retro_cheat_set is a no-op).
    api->retro_cheat_reset();
    for (size_t i = 0; i < codes.size(); i++) api->retro_cheat_set((unsigned)i, true, codes[i].c_str());
}

void ThreeDsCore::Reset()
{
    if (!gameLoaded) return;
    gActive = this;
    api->retro_reset();
}

void ThreeDsCore::Shutdown()
{
    if (romFd >= 0) close(romFd);
    romFd = -1;
    if (!romLink.empty() && romLink.rfind("/proc/", 0) != 0) unlink(romLink.c_str());
    romLink.clear();
    if (!api->handle) return;
    gActive = this;
    if (gameLoaded)
    {
        if (hw->requested && hw->cb.context_destroy)
        {
            if (host.Presenter()) host.Presenter()->MakeCurrent();
            hw->cb.context_destroy();
        }
        api->retro_unload_game();
        api->retro_deinit();
        gameLoaded = false;
    }
    DestroyFramebuffer();
    // The library stays loaded: Azahar keeps global state that doesn't support dlclose/reload.
    gActive = nullptr;
    api->handle = nullptr;
}

}
