#include "log.h"

#include <windows.h>

#include <clocale>
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

// Wide strings (%ls) are written as UTF-8. With the default "C" locale a
// receiver, phone or folder name outside ASCII would cut the line short.
// Null on a Windows version without UTF-8 locales, where the default is used.
_locale_t Utf8Locale() {
    static const _locale_t locale = _create_locale(LC_CTYPE, ".UTF8");
    return locale;
}

// debug.txt -> debug.old.txt
std::wstring PreviousPath(const std::wstring& path) {
    const size_t dot = path.find_last_of(L'.');
    const size_t slash = path.find_last_of(L"\\/");
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) {
        return path + L".old";
    }
    return path.substr(0, dot) + L".old" + path.substr(dot);
}

// Opens a fresh log and keeps the full one as the previous file, so the events
// that led up to a problem survive the rotation. If the full file cannot be
// moved (another program holds it open) it is emptied instead. Mutex held,
// file closed.
void OpenFresh() {
    const bool kept = MoveFileExW(g_path.c_str(), PreviousPath(g_path).c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
    g_file = _wfsopen(g_path.c_str(), kept ? L"a" : L"w", _SH_DENYNO);
}

void Format(char* line, size_t size, const char* level, const char* message) {
    SYSTEMTIME now;
    GetLocalTime(&now);
    snprintf(line, size, "[%04d-%02d-%02d %02d:%02d:%02d.%03d] [%s] %s\n",
             now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds,
             level, message);
}

}  // namespace

void Init(const std::wstring& path) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_path = path;

    WIN32_FILE_ATTRIBUTE_DATA info{};
    bool full = false;
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)) {
        const long long size = (static_cast<long long>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
        full = size > kMaxLogBytes;
    }
    if (full) {
        OpenFresh();
    } else {
        g_file = _wfsopen(path.c_str(), L"a", _SH_DENYNO);
    }
    if (!g_file) OutputDebugStringW((L"[applog] could not open " + path + L"\n").c_str());
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
    if (const _locale_t utf8 = Utf8Locale()) {
        // Not the _s form: that one ends the program on a line it cannot format.
        _vsnprintf_l(message, sizeof(message) - 1, fmt, utf8, args);
        message[sizeof(message) - 1] = '\0';
    } else {
        vsnprintf(message, sizeof(message), fmt, args);
    }
    va_end(args);

    char line[2200];
    Format(line, sizeof(line), level, message);

    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file && _ftelli64(g_file) > kMaxLogBytes) {
        fclose(g_file);
        OpenFresh();
        if (!g_file) OutputDebugStringA("[applog] could not reopen the log after rotation\n");
    }
    if (g_file) {
        fputs(line, g_file);
        fflush(g_file);
    }
    OutputDebugStringA(line);
}

void Emergency(const char* message) {
    char line[700];
    Format(line, sizeof(line), "FATAL", message);
    // The failing thread may be the one holding the lock, so never wait for it.
    const bool locked = g_mutex.try_lock();
    if (g_file) {
        fputs(line, g_file);
        fflush(g_file);
    }
    if (locked) g_mutex.unlock();
    OutputDebugStringA(line);
}

}  // namespace applog
