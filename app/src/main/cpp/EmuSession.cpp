#include "EmuSession.h"
#include "CpuTopology.h"
#include "DsCore.h"
#include "GLPresenter.h"
#include "GbaCore.h"
#include "ThreeDsCore.h"
#include "VulkanContext.h"
#include "VulkanPresenter.h"
#include "IoWorker.h"
#include "LogBuffer.h"
#include "PerfManager.h"

#include "frontend/mic_blow.h"

#include <algorithm>
#include <android/native_window.h>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <ctime>
#include <future>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ds13r
{

namespace
{
constexpr int64_t kFrameNs = (int64_t)(1e9 / 60.0);

int64_t NowNs()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

void SleepNs(int64_t ns)
{
    if (ns <= 0) return;
    timespec ts {(time_t)(ns / 1000000000LL), (long)(ns % 1000000000LL)};
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
}

std::string SanitizeName(const std::string& s)
{
    std::string out;
    for (char c : s)
    {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
            out += '_';
        else
            out += c;
    }
    return out.empty() ? "game" : out;
}

bool ReadFd(int fd, std::unique_ptr<uint8_t[]>& data, uint32_t& len)
{
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0 || st.st_size > 0x40000000) return false;
    len = (uint32_t)st.st_size;
    data = std::make_unique<uint8_t[]>(len);
    size_t done = 0;
    while (done < len)
    {
        ssize_t n = pread(fd, data.get() + done, len - done, (off_t)done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        done += (size_t)n;
    }
    return true;
}

bool EndsWith(const std::string& s, const char* suffix)
{
    size_t n = strlen(suffix);
    if (s.size() < n) return false;
    return strcasecmp(s.c_str() + s.size() - n, suffix) == 0;
}

// The file's extension if it is a 3DS game (lower case), else "".
std::string ThreeDsExtension(const std::string& fileName)
{
    for (const char* ext : {".3ds", ".cci", ".cxi", ".3dsx", ".app", ".elf", ".axf"})
        if (EndsWith(fileName, ext)) return ext;
    return {};
}
}

EmuSession& EmuSession::Get()
{
    static EmuSession session;
    return session;
}

EmuSession::EmuSession()
{
    // Rewind snapshots are compressed off the emulation thread, on the A720 cores.
    rewindWorker = std::thread([this] {
        pthread_setname_np(pthread_self(), "ds13r-rewind");
        PinCurrentThread(CoreClass::Mid);
        std::vector<uint8_t> work;
        for (;;)
        {
            size_t len;
            {
                std::unique_lock<std::mutex> l(rewindLock);
                rewindCv.wait(l, [this] { return rewindPending; });
                work.swap(rewindCapture);
                len = rewindCaptureLen;
                rewindPending = false;
            }
            std::lock_guard<std::mutex> l(rewindLock);
            rewind.Push(work.data(), len);
        }
    });
    rewindWorker.detach();
}

void EmuSession::Init(JavaVM* javaVm, jobject act, const std::string& dir)
{
    vm = javaVm;
    filesDir = dir;

    if (activity)
    {
        JNIEnv* env = nullptr;
        vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
        if (env) env->DeleteGlobalRef(activity);
    }
    activity = act;

    for (const char* sub : {"/saves", "/states", "/backups", "/cheats", "/sd", "/bios", "/screenshots"})
        MakeDirs(filesDir + sub);

    if (!thread.joinable())
    {
        quit = false;
        thread = std::thread([this] { ThreadMain(); });
    }
}

void EmuSession::SetCallbacks(const HostCallbacks& cb)
{
    callbacks = cb;
}

JNIEnv* EmuSession::ThreadEnv()
{
    if (!vm) return nullptr;
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK) return env;
    JavaVMAttachArgs args {JNI_VERSION_1_6, "ds13r-native", nullptr};
    if (vm->AttachCurrentThread(&env, &args) != JNI_OK) return nullptr;
    return env;
}

void EmuSession::Post(std::function<void()> fn, bool wait)
{
    if (std::this_thread::get_id() == thread.get_id())
    {
        fn();
        return;
    }

    if (!wait)
    {
        {
            std::lock_guard<std::mutex> l(cmdLock);
            commands.push_back(std::move(fn));
        }
        cmdCv.notify_all();
        return;
    }

    std::promise<void> done;
    auto fut = done.get_future();
    {
        std::lock_guard<std::mutex> l(cmdLock);
        commands.push_back([&] {
            fn();
            done.set_value();
        });
    }
    cmdCv.notify_all();
    fut.wait();
}

void EmuSession::RunCommands()
{
    std::deque<std::function<void()>> todo;
    {
        std::lock_guard<std::mutex> l(cmdLock);
        todo.swap(commands);
    }
    for (auto& fn : todo) fn();
}

// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------

// Emulation thread only. Detaches audio first so the audio thread never reads a dying core.
void EmuSession::SwapCore(std::unique_ptr<EmuCore> next)
{
    running = false;
    audio.SetSource(nullptr);
    audio.StopMic();
    if (core) core->Shutdown();
    core = std::move(next);
    dsCore = core && strcmp(core->SystemName(), "DS") == 0 ? static_cast<DsCore*>(core.get()) : nullptr;
    ClearRewind();
    undoLoadBackup.clear();
    rewindFrameCounter = 0;
    audioSkew = 1.0;
    fillAverage = -1.0;
    if (core) audio.SetSource(core.get());
    UpdateOutput();
}

void EmuSession::ClearRewind()
{
    std::lock_guard<std::mutex> l(rewindLock);
    rewind.Clear();
}

std::string EmuSession::LoadGame(int fd, const std::string& fileName, const std::string& key)
{
    std::string error;
    Post([&] {
        std::string gameKey = SanitizeName(key);
        // 3DS games run to gigabytes: the engine streams them from the file instead.
        std::string ext = ThreeDsExtension(fileName);
        if (!ext.empty())
        {
            SwapCore(nullptr);
            config.SetInt("video.scaleCap", 0);
            auto next = std::make_unique<ThreeDsCore>(*this);
            error = next->LoadGame(fd, ext, gameKey);
            if (error.empty()) SwapCore(std::move(next));
            running = error.empty();
            return;
        }

        std::unique_ptr<uint8_t[]> rom;
        uint32_t len = 0;
        if (!ReadFd(fd, rom, len))
        {
            error = "The game file could not be read.";
            return;
        }
        SwapCore(nullptr);
        config.SetInt("video.scaleCap", 0);

        // The extension decides. Only unknown extensions fall back to sniffing the header, and
        // DS files can't be sniffed as GBA: devkitARM homebrew embeds a GBA-style header so old
        // flash carts can boot it.
        bool dsExt = EndsWith(fileName, ".nds") || EndsWith(fileName, ".dsi") || EndsWith(fileName, ".srl") || EndsWith(fileName, ".ids");
        bool gbaExt = EndsWith(fileName, ".gba") || EndsWith(fileName, ".agb");
        bool gba = gbaExt || (!dsExt && GbaCore::LooksLikeGba(rom.get(), len));
        if (gba)
        {
            auto next = std::make_unique<GbaCore>(*this);
            error = next->LoadGame(std::move(rom), len, gameKey);
            if (error.empty()) SwapCore(std::move(next));
        }
        else
        {
            auto next = std::make_unique<DsCore>(*this);
            error = next->LoadGame(std::move(rom), len, fileName, gameKey);
            if (error.empty()) SwapCore(std::move(next));
        }
        running = error.empty();
    }, true);
    return error;
}

std::string EmuSession::BootFirmware()
{
    std::string error;
    Post([&] {
        SwapCore(nullptr);
        auto next = std::make_unique<DsCore>(*this);
        error = next->BootFirmware();
        if (error.empty()) SwapCore(std::move(next));
        running = error.empty();
    }, true);
    return error;
}

std::string EmuSession::LoadGbaSlotRom(int fd, const std::string& fileName)
{
    std::string error;
    Post([&] {
        if (!dsCore)
        {
            error = "Start a DS game first.";
            return;
        }
        std::unique_ptr<uint8_t[]> rom;
        uint32_t len = 0;
        if (!ReadFd(fd, rom, len))
        {
            error = "The GBA file could not be read.";
            return;
        }
        error = dsCore->InsertGbaCart(std::move(rom), len, SanitizeName(fileName));
    }, true);
    return error;
}

std::string EmuSession::SystemName()
{
    std::string name;
    Post([&] { name = core ? core->SystemName() : ""; }, true);
    return name;
}

bool EmuSession::ThreeDsFrameSize(int& width, int& height)
{
    bool ok = false;
    Post([&] {
        if (core && strcmp(core->SystemName(), "3DS") == 0)
        {
            static_cast<ThreeDsCore*>(core.get())->FrameSize(width, height);
            ok = true;
        }
    }, true);
    return ok;
}

// ---------------------------------------------------------------------------
// Run control
// ---------------------------------------------------------------------------

void EmuSession::Start()
{
    paused = false;
    cmdCv.notify_all();
}

void EmuSession::SetPaused(bool p)
{
    paused = p;
    if (p)
    {
        // OxygenOS may kill the app soon after it leaves the foreground; save right away.
        Post([this] { FlushSaves(); }, true);
    }
    cmdCv.notify_all();
}

void EmuSession::Reset()
{
    Post([this] {
        if (!core) return;
        core->Reset();
        ClearRewind();
    }, true);
}

void EmuSession::Stop()
{
    Post([this] { SwapCore(nullptr); }, true);
}

void EmuSession::FlushSaves()
{
    Post([this] {
        if (core) core->FlushSaves();
    }, true);
}

void EmuSession::SetSurface(ANativeWindow* newWindow)
{
    Post([this, newWindow] {
        if (!presenter) return;
        Output()->ReleaseWindow();
        if (window) ANativeWindow_release(window);
        window = newWindow;
        if (window)
        {
            ANativeWindow_acquire(window);
            Output()->SetWindow(window);
            needRedraw = true;
        }
    }, true);
}

VulkanContext* EmuSession::Vulkan()
{
    if (!vulkan)
    {
        vulkan = std::make_unique<VulkanContext>();
        if (!vulkan->Init(filesDir))
        {
            LOGE("Vulkan is unavailable on this device");
            vulkan.reset();
            vulkanFailed = true;
        }
    }
    return vulkan.get();
}

VulkanPresenter* EmuSession::VulkanOutput()
{
    if (!vkPresenter && !vulkanFailed && Vulkan())
    {
        vkPresenter = std::make_unique<VulkanPresenter>(*vulkan, vm, activity);
        if (vkPresenter->Init())
        {
            vkPresenter->SetLayout(lastLayout);
            vkPresenter->SetSettings(lastSettings);
        }
        else
        {
            LOGE("Vulkan presenter failed; staying on OpenGL ES output");
            vkPresenter.reset();
            vulkanFailed = true;
        }
    }
    return vkPresenter.get();
}

ds13r::Presenter* EmuSession::Output()
{
    if (vulkanOutput && vkPresenter) return vkPresenter.get();
    return presenter.get();
}

// Emulation thread. Chooses the presenter for the running game and hands it the window:
// Vulkan for DS games on the Vulkan renderer, OpenGL ES for everything else. A window can
// only belong to one graphics API at a time, so the other presenter lets go of it first.
void EmuSession::UpdateOutput()
{
    bool want = core && core->WantsVulkanOutput();
    if (want && !VulkanOutput()) want = false;
    if (want == vulkanOutput) return;

    Output()->ReleaseWindow();
    vulkanOutput = want;
    LOGI("Output: %s", vulkanOutput ? "Vulkan" : "OpenGL ES");
    if (window)
    {
        Output()->SetWindow(window);
        Output()->SetTargetRefreshRate(currentRefresh);
        needRedraw = true;
    }
}

void EmuSession::SetLayout(const PresentLayout& layout)
{
    Post([this, layout] {
        lastLayout = layout;
        if (presenter) presenter->SetLayout(layout);
        if (vkPresenter) vkPresenter->SetLayout(layout);
        needRedraw = true;
    }, false);
}

void EmuSession::SetPresentSettings(const PresentSettings& settings)
{
    Post([this, settings] {
        lastSettings = settings;
        if (presenter) presenter->SetSettings(settings);
        if (vkPresenter) vkPresenter->SetSettings(settings);
        needRedraw = true;
    }, false);
}

void EmuSession::SetKeys(uint32_t pressedMask)
{
    keys.store(pressedMask & 0x7FFF);
}

void EmuSession::SetAnalog(int stick, float x, float y)
{
    auto pack = [](float v) { return (uint32_t)(uint16_t)(int16_t)(std::clamp(v, -1.0f, 1.0f) * 32767.0f); };
    (stick == 0 ? circlePad : cStick).store(pack(x) | (pack(y) << 16));
}

void EmuSession::SetPointer(bool down, float x, float y)
{
    auto pack = [](float v) { return (uint32_t)(std::clamp(v, 0.0f, 1.0f) * 32767.0f); };
    pointerState.store((down ? 0x80000000u : 0u) | pack(x) | (pack(y) << 15));
}

void EmuSession::SetTouch(bool down, int x, int y)
{
    x = std::max(0, std::min(255, x));
    y = std::max(0, std::min(191, y));
    touchState.store((down ? 0x80000000u : 0u) | (uint32_t)x | ((uint32_t)y << 8));
}

void EmuSession::SetLidClosed(bool closed)
{
    lidRequest.store(closed ? 1 : 0);
}

void EmuSession::SetMotion(const float accel[3], const float gyro[3])
{
    for (int i = 0; i < 3; i++)
    {
        motion[i].store(accel[i]);
        motion[3 + i].store(gyro[i]);
    }
}

void EmuSession::SetSolarLevel(int delta)
{
    Post([this, delta] {
        if (core) core->SetSolarLevel(delta);
    }, false);
}

void EmuSession::SetSpeedMode(SpeedMode mode)
{
    speedMode.store((int)mode);
    cmdCv.notify_all();
}

void EmuSession::FrameAdvance()
{
    frameStep = true;
    cmdCv.notify_all();
}

// ---------------------------------------------------------------------------
// Save states
// ---------------------------------------------------------------------------

bool EmuSession::SaveState(const std::string& path, const std::string& thumbPath)
{
    bool ok = false;
    Post([&] {
        if (!core) return;
        std::vector<uint8_t> state;
        if (!core->SaveState(state)) return;

        // Keep the file being replaced so "undo save state" can restore it.
        if (FileExistsAt(path)) rename(path.c_str(), (path + ".undo").c_str());
        ok = WriteFileAtomic(path, state.data(), state.size());

        if (ok && !thumbPath.empty())
        {
            // Thumbnail: the screens stacked, raw RGBA; Kotlin turns it into a PNG.
            std::vector<uint32_t> pixels;
            int w = 0, h = 0, screens = 0;
            if (Screenshot(pixels, w, h, screens))
            {
                std::vector<uint8_t> blob(8 + pixels.size() * 4);
                int32_t dims[2] = {w, h * screens};
                memcpy(blob.data(), dims, 8);
                memcpy(blob.data() + 8, pixels.data(), pixels.size() * 4);
                WriteFileAtomic(thumbPath, blob.data(), blob.size());
            }
        }
        LOGI("Save state %s: %s (%zu bytes)", ok ? "written" : "FAILED", path.c_str(), state.size());
    }, true);
    return ok;
}

bool EmuSession::LoadState(const std::string& path)
{
    bool ok = false;
    Post([&] {
        if (!core) return;
        std::vector<unsigned char> data;
        if (!ReadWholeFile(path, data) || data.empty())
        {
            LOGE("Load state: cannot read %s", path.c_str());
            return;
        }

        std::vector<uint8_t> backup;
        if (!core->SaveState(backup)) return;

        if (!core->LoadState(data.data(), data.size()))
        {
            LOGE("Load state: %s is not compatible", path.c_str());
            core->LoadState(backup.data(), backup.size()); // put the console back as it was
            return;
        }

        undoLoadBackup = std::move(backup);
        ClearRewind();
        needRedraw = true;
        ok = true;
        LOGI("Loaded state %s", path.c_str());
    }, true);
    return ok;
}

bool EmuSession::UndoLoadState()
{
    bool ok = false;
    Post([&] {
        if (!core || undoLoadBackup.empty()) return;
        ok = core->LoadState(undoLoadBackup.data(), undoLoadBackup.size());
        undoLoadBackup.clear();
        ClearRewind();
        needRedraw = true;
    }, true);
    return ok;
}

bool EmuSession::UndoSaveState(const std::string& path)
{
    std::string undo = path + ".undo";
    if (!FileExistsAt(undo)) return false;
    return rename(undo.c_str(), path.c_str()) == 0;
}

// ---------------------------------------------------------------------------
// Rewind
// ---------------------------------------------------------------------------

void EmuSession::SetRewinding(bool active)
{
    rewinding.store(active);
    cmdCv.notify_all();
}

void EmuSession::CaptureRewind()
{
    if (!config.GetBool("rewind.enabled", true)) return;
    int interval = (int)config.GetInt("rewind.interval", 6);
    if (++rewindFrameCounter < interval) return;
    rewindFrameCounter = 0;

    std::unique_lock<std::mutex> l(rewindLock, std::try_to_lock);
    if (!l.owns_lock() || rewindPending) return; // worker still busy; skip this snapshot

    // The capture buffer keeps its size, so capturing never reallocates or zeroes memory.
    size_t cap = core->MaxStateSize();
    if (rewindCapture.size() < cap) rewindCapture.resize(cap);
    size_t len = core->CaptureState(rewindCapture.data(), rewindCapture.size());
    if (len == 0) return;
    rewindCaptureLen = len;
    rewind.SetBudget((size_t)config.GetInt("rewind.budgetMB", 1024) * 1024 * 1024);
    rewindPending = true;
    rewindCv.notify_one();
}

void EmuSession::DoRewindStep()
{
    std::vector<uint8_t> state;
    {
        std::unique_lock<std::mutex> l(rewindLock);
        if (rewindPending)
        {
            // Let the worker finish the snapshot it is packing first.
            l.unlock();
            SleepNs(1000000);
            return;
        }
        if (!rewind.Pop(state)) return;
    }
    core->LoadState(state.data(), state.size());
    // Run one frame so the restored moment is on screen.
    CoreInput input;
    input.keys = keys.load();
    core->RunFrame(input);
}

// ---------------------------------------------------------------------------
// Misc
// ---------------------------------------------------------------------------

bool EmuSession::Screenshot(std::vector<uint32_t>& pixels, int& width, int& height, int& screens)
{
    bool ok = false;
    Post([&] {
        if (!presenter || !core) return;
        FrameInfo frame;
        if (!core->GetFrame(frame)) return;
        screens = frame.screenCount;
        pixels.clear();
        for (int i = 0; i < screens; i++)
        {
            std::vector<uint32_t> one;
            if (!Output()->ReadScreen(i, one, width, height)) return;
            pixels.insert(pixels.end(), one.begin(), one.end());
        }
        ok = true;
    }, true);
    return ok;
}

void EmuSession::SetCheats(const std::vector<std::string>& codes)
{
    Post([this, codes] {
        if (core) core->SetCheats(codes);
    }, true);
}

void EmuSession::ApplyLiveSettings()
{
    Post([this] {
        audio.SetVolume((float)config.GetFloat("audio.volume", 1.0));
        audio.SetExtraLatencyMs((int)config.GetInt("audio.extraLatencyMs", 0));
        if (core) core->ApplySettings();
        UpdateOutput();
        needRedraw = true;
    }, true);
}

EmuStats EmuSession::GetStats()
{
    std::lock_guard<std::mutex> l(statsLock);
    return stats;
}

// ---------------------------------------------------------------------------
// Emulation thread
// ---------------------------------------------------------------------------

void EmuSession::RunOneFrame()
{
    CoreInput input;
    input.keys = keys.load();
    uint32_t touch = touchState.load();
    input.touching = (touch & 0x80000000u) != 0;
    input.touchX = (int)(touch & 0xFF);
    input.touchY = (int)((touch >> 8) & 0xFF);
    input.lidRequest = lidRequest.exchange(-1);
    auto unpackAxis = [](uint32_t v) { return (int16_t)(uint16_t)v / 32767.0f; };
    uint32_t circle = circlePad.load(), cs = cStick.load(), pointer = pointerState.load();
    input.circleX = unpackAxis(circle & 0xFFFF);
    input.circleY = unpackAxis(circle >> 16);
    input.cstickX = unpackAxis(cs & 0xFFFF);
    input.cstickY = unpackAxis(cs >> 16);
    input.pointerDown = (pointer & 0x80000000u) != 0;
    input.pointerX = (pointer & 0x7FFF) / 32767.0f;
    input.pointerY = ((pointer >> 15) & 0x7FFF) / 32767.0f;

    core->SyncClock();
    core->RunFrame(input);
    core->CheckFlush();
    CaptureRewind();
    statFrames++;
}

void EmuSession::PresentFrame()
{
    FrameInfo frame;
    if (core && core->GetFrame(frame))
    {
        if (frame.vkTexture && vulkanOutput)
            vkPresenter->SetExternalFrame(frame.vkTexture, frame.width, frame.height, frame.screenCount);
        else if (frame.vkImage && vulkanOutput)
            vkPresenter->SetExternalImage((VkImage)frame.vkImage, (VkFormat)frame.vkFormat, frame.width, frame.height);
        else if (frame.hardware)
            presenter->SetHardwareFrame(frame.texture, frame.width, frame.height);
        else
            Output()->UploadSoftwareFrame(frame.screens, frame.screenCount, frame.width, frame.height, frame.bgra);
    }
    else if (vulkanOutput)
    {
        // No frame (e.g. the 3DS engine just rebuilt its renderer for a state load): the image
        // we were showing may be gone, so never sample it again.
        vkPresenter->DropExternalImage();
    }
    if (Output()->Present()) statPresents++;
}

void EmuSession::UpdateAudioSync(double emulatedFps)
{
    // Emulation is locked to the display (60 Hz) rather than the console's own rate, so tell the
    // resampler how fast we really run, then nudge it to keep the output buffer half full.
    double nominal = emulatedFps / core->NativeFps();
    int fill = core->AudioFill();
    fillAverage = fillAverage < 0 ? fill : fillAverage * 0.95 + fill * 0.05;
    constexpr double kTarget = 1024.0;
    double correction = std::clamp((fillAverage - kTarget) / kTarget * 0.005, -0.005, 0.005);
    double skew = std::max(0.25, nominal * (1.0 + correction));
    if (std::fabs(skew - audioSkew) > 1e-5)
    {
        audioSkew = skew;
        core->SetAudioSkew(skew);
    }
}

// Lowers upscaling *before* the phone throttles (features.md §13), using Android's thermal
// headroom forecast, and restores it once the phone has stayed cool for a minute.
void EmuSession::UpdateThermalScale()
{
    if (!core || !core->UsesHardwareRenderer() || !config.GetBool("perf.thermal", true))
    {
        if (config.GetInt("video.scaleCap", 0) != 0)
        {
            config.SetInt("video.scaleCap", 0);
            if (core) core->ApplySettings();
        }
        return;
    }

    int64_t now = NowNs();
    if (now - lastThermalChange < 20000000000LL) return; // at most one change every 20 s

    float headroom = perf->Headroom();
    int status = perf->Status();
    int configured = (int)config.GetInt("video.scale", 4);
    int scale = core->RenderScale();
    int cap = (int)config.GetInt("video.scaleCap", 0);

    bool hot = headroom >= 0.9f || status >= 2; // 2 = ATHERMAL_STATUS_MODERATE
    bool cool = headroom >= 0.0f && headroom < 0.7f && status <= 1;

    if (hot && scale > 2)
    {
        cap = scale - 1;
        coolSince = 0;
    }
    else if (cool && cap > 0)
    {
        if (coolSince == 0) coolSince = now;
        if (now - coolSince < 60000000000LL) return;
        cap = scale + 1 >= configured ? 0 : scale + 1;
        coolSince = 0;
    }
    else
    {
        if (!cool) coolSince = 0;
        return;
    }

    lastThermalChange = now;
    config.SetInt("video.scaleCap", cap);
    core->ApplySettings();
    LOGI("Thermal: headroom %.2f status %d -> upscaling %dx", headroom, status, core->RenderScale());
    JNIEnv* env = ThreadEnv();
    if (env && callbacks.bridge && callbacks.onThermal)
        env->CallVoidMethod(callbacks.bridge, callbacks.onThermal, (jint)status, (jint)core->RenderScale());
}

void EmuSession::ThreadMain()
{
    pthread_setname_np(pthread_self(), "ds13r-emu");
    PinCurrentThread(CoreClass::Prime);
    ThreadEnv();

    presenter = std::make_unique<GLPresenter>(vm, activity);
    if (!presenter->Init()) LOGE("Presenter failed to initialise");

    perf = std::make_unique<PerfManager>();
    perf->StartSession({(int32_t)CurrentTid()}, kFrameNs);

    audio.StartOutput();
    statStart = NowNs();

    bool wasActive = false;
    while (!quit)
    {
        RunCommands();

        bool step = frameStep.load();
        bool active = running && core && (!paused || step);

        if (active != wasActive)
        {
            wasActive = active;
            if (active)
            {
                audio.StartOutput();
                Output()->SetTargetRefreshRate(currentRefresh);
            }
            else
            {
                if (paused) audio.StopOutput(); // release the audio path while paused
                // No frame-rate preference while paused, so menus can run at 120 Hz.
                Output()->SetTargetRefreshRate(0.0f);
            }
        }

        if (!active)
        {
            if (needRedraw.exchange(false) && Output()->HasWindow()) PresentFrame();
            std::unique_lock<std::mutex> l(cmdLock);
            cmdCv.wait_for(l, std::chrono::milliseconds(100), [this] {
                return !commands.empty() || quit || (running && !paused) || frameStep || needRedraw;
            });
            continue;
        }

        int64_t t0 = NowNs();
        SpeedMode mode = static_cast<SpeedMode>(speedMode.load());
        float wantRefresh = (mode == SpeedMode::FastForward && config.GetBool("video.ff120", true)) ? 120.0f : 60.0f;
        if (wantRefresh != currentRefresh)
        {
            currentRefresh = wantRefresh;
            Output()->SetTargetRefreshRate(currentRefresh);
        }

        int frames = 0;
        if (rewinding.load())
        {
            DoRewindStep();
        }
        else if (step)
        {
            frameStep = false;
            RunOneFrame();
            frames = 1;
        }
        else if (mode == SpeedMode::Normal)
        {
            // One console frame per 60 Hz refresh: perfectly even motion.
            RunOneFrame();
            frames = 1;
        }
        else
        {
            double speed = mode == SpeedMode::FastForward ? config.GetFloat("speed.fastForward", 3.0) : config.GetFloat("speed.slowMotion", 0.5);
            if (mode == SpeedMode::FastForward && speed <= 0.0)
            {
                // Unlimited: emulate for most of the refresh interval.
                int64_t budget = (int64_t)(1e9 / currentRefresh * 0.85);
                do
                {
                    RunOneFrame();
                    frames++;
                } while (NowNs() - t0 < budget);
            }
            else
            {
                frameAccumulator += speed * core->NativeFps() / currentRefresh;
                while (frameAccumulator >= 1.0)
                {
                    RunOneFrame();
                    frames++;
                    frameAccumulator -= 1.0;
                }
            }
        }

        int64_t work = NowNs() - t0;
        perf->ReportWork(work);
        // Past the 60 Hz frame budget: this refresh will be missed. Logged for tuning.
        if (work > kFrameNs) LOGD("Slow frame: %.2f ms of emulation (%d frames)", work / 1e6, frames);
        statWorkMs += work / 1e6;

        bool muteFf = mode == SpeedMode::FastForward && config.GetBool("audio.muteFastForward", true);
        audio.SetMuted(muteFf || rewinding.load());

        if (Output()->HasWindow())
        {
            PresentFrame(); // Swappy blocks here until the next vsync slot
        }
        else
        {
            // No surface (e.g. mid-rotation): keep time with a plain sleep.
            SleepNs((int64_t)(1e9 / currentRefresh) - (NowNs() - t0));
        }

        double emulatedFps = mode == SpeedMode::Normal ? currentRefresh : frames * currentRefresh;
        if (frames > 0) UpdateAudioSync(emulatedFps);

        perf->PollThermal();
        UpdateThermalScale();

        int64_t now = NowNs();
        if (now - statStart >= 1000000000LL)
        {
            double secs = (now - statStart) / 1e9;
            std::lock_guard<std::mutex> l(statsLock);
            stats.fps = (float)(statFrames / secs);
            stats.speed = (float)(stats.fps / core->NativeFps() * 100.0);
            stats.presentFps = (float)(statPresents / secs);
            stats.frameTimeMs = statFrames ? (float)(statWorkMs / statFrames) : 0.0f;
            stats.thermalHeadroom = perf->Headroom();
            stats.thermalStatus = perf->Status();
            stats.renderScale = core->RenderScale();
            // Never wait on the rewind worker here; keep last second's figures if it is busy.
            if (std::unique_lock<std::mutex> rl(rewindLock, std::try_to_lock); rl.owns_lock())
            {
                stats.rewindSeconds = (float)(rewind.Count() * config.GetInt("rewind.interval", 6) / core->NativeFps());
                stats.rewindMemoryMb = rewind.MemoryUsed() / (1024.0f * 1024.0f);
            }
            statFrames = statPresents = 0;
            statWorkMs = 0;
            statStart = now;
        }
    }

    SwapCore(nullptr);
    vkPresenter.reset();
    vulkan.reset(); // after the core: the DS Vulkan renderer uses it
    presenter->Shutdown();
    presenter.reset();
    if (window) ANativeWindow_release(window);
    window = nullptr;
    if (vm) vm->DetachCurrentThread();
}

// ---------------------------------------------------------------------------
// CoreHost
// ---------------------------------------------------------------------------

void EmuSession::OnRtcOffset(int64_t offset)
{
    JNIEnv* env = ThreadEnv();
    if (env && callbacks.bridge && callbacks.onRtcOffset) env->CallVoidMethod(callbacks.bridge, callbacks.onRtcOffset, (jlong)offset);
}

void EmuSession::OnCoreStopped(int reason)
{
    LOGI("Emulator stopped by the console (reason %d)", reason);
    running = false;
    JNIEnv* env = ThreadEnv();
    if (env && callbacks.bridge && callbacks.onEmuStopped) env->CallVoidMethod(callbacks.bridge, callbacks.onEmuStopped, reason);
}

void EmuSession::OnMicStart()
{
    JNIEnv* env = ThreadEnv();
    if (env && callbacks.bridge && callbacks.onMicRequest) env->CallVoidMethod(callbacks.bridge, callbacks.onMicRequest, JNI_TRUE);
    audio.StartMic();
}

void EmuSession::OnMicStop()
{
    audio.StopMic();
    JNIEnv* env = ThreadEnv();
    if (env && callbacks.bridge && callbacks.onMicRequest) env->CallVoidMethod(callbacks.bridge, callbacks.onMicRequest, JNI_FALSE);
}

int EmuSession::ReadMic(int16_t* data, int maxlen)
{
    if (blowActive.load())
    {
        // "Blow" button: replay a recorded breath sample (from melonDS).
        constexpr size_t kBlowLen = sizeof(mic_blow) / sizeof(mic_blow[0]);
        for (int i = 0; i < maxlen; i++)
        {
            data[i] = mic_blow[blowPos];
            blowPos = (blowPos + 1) % kBlowLen;
        }
        return maxlen;
    }
    blowPos = 0;

    if (audio.MicActive()) return audio.ReadMic(data, maxlen);

    memset(data, 0, (size_t)maxlen * sizeof(int16_t));
    return maxlen;
}

void EmuSession::OnRumble(uint32_t ms)
{
    JNIEnv* env = ThreadEnv();
    if (env && callbacks.bridge && callbacks.onRumble) env->CallVoidMethod(callbacks.bridge, callbacks.onRumble, (jint)ms);
}

float EmuSession::MotionQuery(int type)
{
    if (type < 0 || type > 5) return 0.0f;
    return motion[type].load();
}

bool EmuSession::GuitarKeyDown(int key)
{
    return (guitarKeys.load() >> key) & 1;
}

}
