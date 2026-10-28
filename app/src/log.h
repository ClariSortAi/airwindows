// Process-wide log: appends to a text file and mirrors to the debugger.
#pragma once

#include <string>

namespace applog {

// Opens (or creates) the log file. Safe to call once at startup.
void Init(const std::wstring& path);
void Shutdown();
const std::wstring& Path();

// printf-style, thread-safe. `level` is a short tag such as "INFO".
void Write(const char* level, const char* fmt, ...);

}  // namespace applog

#define LOGD(...) ::applog::Write("DEBUG", __VA_ARGS__)
#define LOGI(...) ::applog::Write("INFO ", __VA_ARGS__)
#define LOGW(...) ::applog::Write("WARN ", __VA_ARGS__)
#define LOGE(...) ::applog::Write("ERROR", __VA_ARGS__)
