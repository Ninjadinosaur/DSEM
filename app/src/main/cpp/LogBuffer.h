#pragma once

#include <cstdarg>
#include <string>
#include <vector>

// Logging for the native side. Every line goes to logcat and into an in-memory
// ring buffer that the in-app log viewer reads (features.md §14).
namespace ds13r
{

enum class LogLevel { Debug = 0, Info = 1, Warn = 2, Error = 3 };

// Logs one complete line (a trailing newline is implied).
void LogLine(LogLevel level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

// Logs raw text that may contain several lines or only part of one.
// melonDS prints messages in fragments, so text is buffered until a newline arrives.
void LogRaw(LogLevel level, const char* fmt, va_list args);

// Returns the buffered lines, oldest first.
std::vector<std::string> LogSnapshot();
void LogClear();

}

#define LOGD(...) ::ds13r::LogLine(::ds13r::LogLevel::Debug, __VA_ARGS__)
#define LOGI(...) ::ds13r::LogLine(::ds13r::LogLevel::Info, __VA_ARGS__)
#define LOGW(...) ::ds13r::LogLine(::ds13r::LogLevel::Warn, __VA_ARGS__)
#define LOGE(...) ::ds13r::LogLine(::ds13r::LogLevel::Error, __VA_ARGS__)
