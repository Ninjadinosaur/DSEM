#include "LogBuffer.h"

#include <android/log.h>
#include <cstdio>
#include <deque>
#include <mutex>

namespace ds13r
{

namespace
{
constexpr size_t kMaxLines = 2000;
constexpr const char* kTag = "DS13R";

std::mutex gLock;
std::deque<std::string> gLines;
std::string gPartial;

const char* LevelPrefix(LogLevel level)
{
    switch (level)
    {
    case LogLevel::Debug: return "D ";
    case LogLevel::Info: return "I ";
    case LogLevel::Warn: return "W ";
    case LogLevel::Error: return "E ";
    }
    return "";
}

int AndroidPriority(LogLevel level)
{
    switch (level)
    {
    case LogLevel::Debug: return ANDROID_LOG_DEBUG;
    case LogLevel::Info: return ANDROID_LOG_INFO;
    case LogLevel::Warn: return ANDROID_LOG_WARN;
    case LogLevel::Error: return ANDROID_LOG_ERROR;
    }
    return ANDROID_LOG_INFO;
}

// Caller holds gLock.
void EmitLine(LogLevel level, const std::string& line)
{
    __android_log_write(AndroidPriority(level), kTag, line.c_str());
    gLines.push_back(LevelPrefix(level) + line);
    if (gLines.size() > kMaxLines) gLines.pop_front();
}
}

void LogLine(LogLevel level, const char* fmt, ...)
{
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    std::lock_guard<std::mutex> lock(gLock);
    EmitLine(level, buf);
}

void LogRaw(LogLevel level, const char* fmt, va_list args)
{
    char buf[1024];
    vsnprintf(buf, sizeof(buf), fmt, args);

    std::lock_guard<std::mutex> lock(gLock);
    gPartial += buf;

    size_t pos;
    while ((pos = gPartial.find('\n')) != std::string::npos)
    {
        std::string line = gPartial.substr(0, pos);
        gPartial.erase(0, pos + 1);
        if (!line.empty()) EmitLine(level, line);
    }
}

std::vector<std::string> LogSnapshot()
{
    std::lock_guard<std::mutex> lock(gLock);
    return {gLines.begin(), gLines.end()};
}

void LogClear()
{
    std::lock_guard<std::mutex> lock(gLock);
    gLines.clear();
}

}
