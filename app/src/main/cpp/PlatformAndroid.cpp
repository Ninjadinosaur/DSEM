// Android implementation of the melonDS Platform interface.
// melonDS calls these for files, threads, logging, saves and host hardware (mic, rumble, ...).

#include "CpuTopology.h"
#include "DsCore.h"
#include "EmuSession.h"
#include "LogBuffer.h"

#include "Platform.h"
#include "SPI_Firmware.h"

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <mutex>
#include <pthread.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace melonDS::Platform
{

using ds13r::CoreHost;
using ds13r::DsCore;
using ds13r::EmuSession;

// melonDS's userdata is the DsCore that created the console.
static DsCore* Core(void* userdata)
{
    return static_cast<DsCore*>(userdata);
}

static CoreHost& Host(void* userdata)
{
    return userdata ? Core(userdata)->Host() : static_cast<CoreHost&>(EmuSession::Get());
}

void SignalStop(StopReason reason, void* userdata)
{
    Host(userdata).OnCoreStopped((int)reason);
}

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------

static std::string ModeString(FileMode mode, bool exists)
{
    std::string m;
    if (mode & FileMode::Append)
        m = (mode & FileMode::Read) ? "a+" : "a";
    else if (!(mode & FileMode::Write))
        m = "r";
    else if (mode & (FileMode::NoCreate | FileMode::Preserve))
        m = exists ? "r+" : ((mode & FileMode::Read) ? "w+" : "w");
    else
        m = (mode & FileMode::Read) ? "w+" : "w";
    if (!(mode & FileMode::Text)) m += "b";
    return m;
}

std::string GetLocalFilePath(const std::string& filename)
{
    if (!filename.empty() && filename[0] == '/') return filename;
    return EmuSession::Get().FilesDir() + "/" + filename;
}

FileHandle* OpenFile(const std::string& path, FileMode mode)
{
    if ((mode & (FileMode::ReadWrite | FileMode::Append)) == FileMode::None)
    {
        Log(LogLevel::Error, "Attempted to open \"%s\" in neither read nor write mode\n", path.c_str());
        return nullptr;
    }

    struct stat st;
    bool exists = stat(path.c_str(), &st) == 0;
    if ((mode & FileMode::NoCreate) && !exists) return nullptr;

    FILE* f = fopen(path.c_str(), ModeString(mode, exists).c_str());
    return reinterpret_cast<FileHandle*>(f);
}

FileHandle* OpenLocalFile(const std::string& path, FileMode mode)
{
    return OpenFile(GetLocalFilePath(path), mode);
}

bool FileExists(const std::string& name)
{
    struct stat st;
    return stat(name.c_str(), &st) == 0;
}

bool LocalFileExists(const std::string& name)
{
    return FileExists(GetLocalFilePath(name));
}

bool CheckFileWritable(const std::string& filepath)
{
    FileHandle* f = OpenFile(filepath, FileMode::Append);
    if (!f) return false;
    CloseFile(f);
    return true;
}

bool CheckLocalFileWritable(const std::string& filepath)
{
    return CheckFileWritable(GetLocalFilePath(filepath));
}

bool CloseFile(FileHandle* file)
{
    return fclose(reinterpret_cast<FILE*>(file)) == 0;
}

bool IsEndOfFile(FileHandle* file)
{
    return feof(reinterpret_cast<FILE*>(file)) != 0;
}

bool FileReadLine(char* str, int count, FileHandle* file)
{
    return fgets(str, count, reinterpret_cast<FILE*>(file)) != nullptr;
}

u64 FilePosition(FileHandle* file)
{
    return (u64)ftello(reinterpret_cast<FILE*>(file));
}

bool FileSeek(FileHandle* file, s64 offset, FileSeekOrigin origin)
{
    int whence = origin == FileSeekOrigin::Start ? SEEK_SET : origin == FileSeekOrigin::Current ? SEEK_CUR : SEEK_END;
    return fseeko(reinterpret_cast<FILE*>(file), (off_t)offset, whence) == 0;
}

void FileRewind(FileHandle* file)
{
    rewind(reinterpret_cast<FILE*>(file));
}

u64 FileRead(void* data, u64 size, u64 count, FileHandle* file)
{
    return fread(data, size, count, reinterpret_cast<FILE*>(file));
}

bool FileFlush(FileHandle* file)
{
    return fflush(reinterpret_cast<FILE*>(file)) == 0;
}

u64 FileWrite(const void* data, u64 size, u64 count, FileHandle* file)
{
    return fwrite(data, size, count, reinterpret_cast<FILE*>(file));
}

u64 FileWriteFormatted(FileHandle* file, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int n = vfprintf(reinterpret_cast<FILE*>(file), fmt, args);
    va_end(args);
    return n < 0 ? 0 : (u64)n;
}

u64 FileLength(FileHandle* file)
{
    FILE* f = reinterpret_cast<FILE*>(file);
    off_t pos = ftello(f);
    fseeko(f, 0, SEEK_END);
    off_t len = ftello(f);
    fseeko(f, pos, SEEK_SET);
    return len < 0 ? 0 : (u64)len;
}

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

void Log(LogLevel level, const char* fmt, ...)
{
    if (!fmt) return;
    ds13r::LogLevel l = ds13r::LogLevel::Info;
    switch (level)
    {
    case LogLevel::Debug: l = ds13r::LogLevel::Debug; break;
    case LogLevel::Info: l = ds13r::LogLevel::Info; break;
    case LogLevel::Warn: l = ds13r::LogLevel::Warn; break;
    case LogLevel::Error: l = ds13r::LogLevel::Error; break;
    }
    va_list args;
    va_start(args, fmt);
    ds13r::LogRaw(l, fmt, args);
    va_end(args);
}

// ---------------------------------------------------------------------------
// Threads and synchronisation
// ---------------------------------------------------------------------------

struct Thread
{
    std::thread thread;
};

Thread* Thread_Create(std::function<void()> func)
{
    // melonDS helper threads (threaded 3D rendering, etc.) go on the A720 cores,
    // leaving the Cortex-X4 to the main emulation thread.
    auto* t = new Thread;
    t->thread = std::thread([func = std::move(func)] {
        pthread_setname_np(pthread_self(), "ds13r-render");
        ds13r::PinCurrentThread(ds13r::CoreClass::Mid);
        func();
    });
    return t;
}

void Thread_Free(Thread* thread)
{
    if (thread->thread.joinable()) thread->thread.detach();
    delete thread;
}

void Thread_Wait(Thread* thread)
{
    if (thread->thread.joinable()) thread->thread.join();
}

struct Semaphore
{
    std::mutex lock;
    std::condition_variable cv;
    int count = 0;
};

Semaphore* Semaphore_Create()
{
    return new Semaphore;
}

void Semaphore_Free(Semaphore* sema)
{
    delete sema;
}

void Semaphore_Reset(Semaphore* sema)
{
    std::lock_guard<std::mutex> l(sema->lock);
    sema->count = 0;
}

void Semaphore_Wait(Semaphore* sema)
{
    std::unique_lock<std::mutex> l(sema->lock);
    sema->cv.wait(l, [sema] { return sema->count > 0; });
    sema->count--;
}

bool Semaphore_TryWait(Semaphore* sema, int timeout_ms)
{
    std::unique_lock<std::mutex> l(sema->lock);
    if (timeout_ms == 0)
    {
        if (sema->count <= 0) return false;
    }
    else if (!sema->cv.wait_for(l, std::chrono::milliseconds(timeout_ms), [sema] { return sema->count > 0; }))
    {
        return false;
    }
    sema->count--;
    return true;
}

void Semaphore_Post(Semaphore* sema, int count)
{
    {
        std::lock_guard<std::mutex> l(sema->lock);
        sema->count += count;
    }
    if (count == 1)
        sema->cv.notify_one();
    else
        sema->cv.notify_all();
}

struct Mutex
{
    std::mutex m;
};

Mutex* Mutex_Create()
{
    return new Mutex;
}

void Mutex_Free(Mutex* mutex)
{
    delete mutex;
}

void Mutex_Lock(Mutex* mutex)
{
    mutex->m.lock();
}

void Mutex_Unlock(Mutex* mutex)
{
    mutex->m.unlock();
}

bool Mutex_TryLock(Mutex* mutex)
{
    return mutex->m.try_lock();
}

void Sleep(u64 usecs)
{
    std::this_thread::sleep_for(std::chrono::microseconds(usecs));
}

u64 GetMSCount()
{
    return (u64)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

u64 GetUSCount()
{
    return (u64)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------------------
// Saves and clock
// ---------------------------------------------------------------------------

void WriteNDSSave(const u8* savedata, u32 savelen, u32 writeoffset, u32 writelen, void* userdata)
{
    if (userdata) Core(userdata)->OnNdsSaveWrite(savedata, savelen, writeoffset, writelen);
}

void WriteGBASave(const u8* savedata, u32 savelen, u32 writeoffset, u32 writelen, void* userdata)
{
    if (userdata) Core(userdata)->OnGbaSaveWrite(savedata, savelen, writeoffset, writelen);
}

void WriteFirmware(const Firmware& firmware, u32 writeoffset, u32 writelen, void* userdata)
{
    if (userdata) Core(userdata)->OnFirmwareWrite();
}

void WriteDateTime(int year, int month, int day, int hour, int minute, int second, void* userdata)
{
    if (userdata) Core(userdata)->OnDateTimeWrite(year, month, day, hour, minute, second);
}

// ---------------------------------------------------------------------------
// Local multiplayer and networking (not connected yet: behaves as "no other consoles")
// ---------------------------------------------------------------------------

void MP_Begin(void* userdata) {}
void MP_End(void* userdata) {}
int MP_SendPacket(u8* data, int len, u64 timestamp, void* userdata) { return len; }
int MP_RecvPacket(u8* data, u64* timestamp, void* userdata) { return 0; }
int MP_SendCmd(u8* data, int len, u64 timestamp, void* userdata) { return len; }
int MP_SendReply(u8* data, int len, u64 timestamp, u16 aid, void* userdata) { return len; }
int MP_SendAck(u8* data, int len, u64 timestamp, void* userdata) { return len; }
int MP_RecvHostPacket(u8* data, u64* timestamp, void* userdata) { return 0; }
u16 MP_RecvReplies(u8* data, u64 timestamp, u16 aidmask, void* userdata) { return 0; }

int Net_SendPacket(u8* data, int len, void* userdata) { return len; }
int Net_RecvPacket(u8* data, void* userdata) { return 0; }

// ---------------------------------------------------------------------------
// Cameras (DSi)
// ---------------------------------------------------------------------------

void Camera_Start(int num, void* userdata) {}
void Camera_Stop(int num, void* userdata) {}

void Camera_CaptureFrame(int num, u32* frame, int width, int height, bool yuv, void* userdata)
{
    // No camera feed: a neutral grey frame.
    u32 fill = yuv ? 0x80808080 : 0xFF808080;
    for (int i = 0; i < (yuv ? width * height / 2 : width * height); i++) frame[i] = fill;
}

// ---------------------------------------------------------------------------
// Microphone
// ---------------------------------------------------------------------------

void Mic_Start(void* userdata)
{
    Host(userdata).OnMicStart();
}

void Mic_Stop(void* userdata)
{
    Host(userdata).OnMicStop();
}

int Mic_ReadInput(s16* data, int maxlength, void* userdata)
{
    return Host(userdata).ReadMic(data, maxlength);
}

// ---------------------------------------------------------------------------
// AAC (DSi DSP HLE) - not available yet
// ---------------------------------------------------------------------------

struct AACDecoder
{
};

AACDecoder* AAC_Init()
{
    return nullptr;
}

void AAC_DeInit(AACDecoder* dec)
{
    delete dec;
}

bool AAC_Configure(AACDecoder* dec, int frequency, int channels)
{
    return false;
}

bool AAC_DecodeFrame(AACDecoder* dec, const void* input, int inputlen, void* output, int outputlen)
{
    return false;
}

// ---------------------------------------------------------------------------
// GBA slot accessories
// ---------------------------------------------------------------------------

bool Addon_KeyDown(KeyType type, void* userdata)
{
    return Host(userdata).GuitarKeyDown((int)type);
}

void Addon_RumbleStart(u32 len, void* userdata)
{
    Host(userdata).OnRumble(len);
}

void Addon_RumbleStop(void* userdata)
{
    Host(userdata).OnRumble(0);
}

float Addon_MotionQuery(MotionQueryType type, void* userdata)
{
    return Host(userdata).MotionQuery((int)type);
}

// ---------------------------------------------------------------------------
// Dynamic libraries (the JIT loads libandroid for ASharedMemory)
// ---------------------------------------------------------------------------

DynamicLibrary* DynamicLibrary_Load(const char* lib)
{
    return reinterpret_cast<DynamicLibrary*>(dlopen(lib, RTLD_NOW | RTLD_LOCAL));
}

void DynamicLibrary_Unload(DynamicLibrary* lib)
{
    if (lib) dlclose(lib);
}

void* DynamicLibrary_LoadFunction(DynamicLibrary* lib, const char* name)
{
    return lib ? dlsym(lib, name) : nullptr;
}

}
