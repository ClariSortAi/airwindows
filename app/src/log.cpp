#include "log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace applog {
namespace {

// Start a fresh file once the old one passes this size, so the log cannot grow without bound.
constexpr long long kMaxLogBytes = 4 * 1024 * 1024;

std::mutex g_mutex;
FILE* g_file = nullptr;
std::wstring g_path;

}  // namespace

void Init(const std::wstring& path) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_path = path;

    WIN32_FILE_ATTRIBUTE_DATA info{};
    bool truncate = false;
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)) {
        const long long size = (static_cast<long long>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
        truncate = size > kMaxLogBytes;
    }
    g_file = _wfsopen(path.c_str(), truncate ? L"w" : L"a", _SH_DENYNO);
}

void Shutdown() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) {
        fclose(g_file);
        g_file = nullptr;
    }
}

const std::wstring& Path() {
    return g_path;
}

void Write(const char* level, const char* fmt, ...) {
    char message[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    SYSTEMTIME now;
    GetLocalTime(&now);
    char line[2200];
    snprintf(line, sizeof(line), "[%04d-%02d-%02d %02d:%02d:%02d.%03d] [%s] %s\n",
             now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds,
             level, message);

    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) {
        fputs(line, g_file);
        fflush(g_file);
    }
    OutputDebugStringA(line);
}

}  // namespace applog
