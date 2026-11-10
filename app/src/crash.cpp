#include "crash.h"

#include <windows.h>
#include <dbghelp.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "log.h"
#include "settings.h"
#include "version.h"

namespace crash {
namespace {

constexpr size_t kReportsKept = 5;

std::atomic<bool> g_handling{false};
bool g_interactive = true;
// Fixed at install time: a failing process should allocate as little as it can.
wchar_t g_directory[MAX_PATH] = L"";

struct Report {
    EXCEPTION_POINTERS* exception;
    DWORD threadId;
    const char* reason;
};

// Describes where the failure happened as module+offset, which stays meaningful
// across runs even though the load address changes.
void DescribeAddress(const void* address, char* text, size_t size) {
    HMODULE module = nullptr;
    wchar_t path[MAX_PATH] = L"";
    if (address &&
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCWSTR>(address), &module) &&
        GetModuleFileNameW(module, path, ARRAYSIZE(path))) {
        const wchar_t* name = wcsrchr(path, L'\\');
        snprintf(text, size, "%ls+0x%llX", name ? name + 1 : path,
                 static_cast<unsigned long long>(static_cast<const char*>(address) - reinterpret_cast<const char*>(module)));
    } else {
        snprintf(text, size, "%p", address);
    }
}

// Runs on its own thread: the failing thread may have no stack left, and a
// dialog must not pump messages for the windows of a thread that has failed.
DWORD WINAPI WriteReport(LPVOID parameter) {
    const Report& report = *static_cast<const Report*>(parameter);
    SYSTEMTIME now;
    GetLocalTime(&now);
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%ls\\AirWindows-%hs-%04d%02d%02d-%02d%02d%02d.dmp", g_directory, APP_VERSION_STRING,
               now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);

    bool written = false;
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION info{report.threadId, report.exception, FALSE};
        // Thread stacks and the module list, without the heap: small enough to
        // send, and it leaves out most of what the program had in memory.
        const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
        written = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, type,
                                    report.exception ? &info : nullptr, nullptr, nullptr) != FALSE;
        CloseHandle(file);
        if (!written) DeleteFileW(path);
    }

    char where[320] = "";
    char line[640];
    if (report.exception && report.exception->ExceptionRecord) {
        DescribeAddress(report.exception->ExceptionRecord->ExceptionAddress, where, sizeof(where));
        snprintf(line, sizeof(line), "[CRASH] %s: exception 0x%08lX at %s on thread %lu; report %s: %ls", report.reason,
                 report.exception->ExceptionRecord->ExceptionCode, where, report.threadId,
                 written ? "saved" : "could not be saved", path);
    } else {
        snprintf(line, sizeof(line), "[CRASH] %s on thread %lu; report %s: %ls", report.reason, report.threadId,
                 written ? "saved" : "could not be saved", path);
    }
    applog::Emergency(line);

    if (g_interactive) {
        wchar_t text[900];
        if (written) {
            swprintf_s(text,
                       L"AirWindows has stopped working.\n\n"
                       L"A crash report was saved in:\n%ls\n\n"
                       L"Start AirWindows again to continue. If this keeps happening, send the newest .dmp file "
                       L"from that folder, together with debug.txt from the folder above it, when you report the "
                       L"problem.\n\nOpen the folder now?",
                       g_directory);
        } else {
            swprintf_s(text,
                       L"AirWindows has stopped working, and a crash report could not be saved.\n\n"
                       L"Start AirWindows again to continue. The log is in:\n%ls\n\nOpen that folder now?",
                       AppDataDir().c_str());
        }
        if (MessageBoxW(nullptr, text, L"AirWindows", MB_YESNO | MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST) == IDYES) {
            ShellExecuteW(nullptr, L"open", written ? g_directory : AppDataDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
    }
    return 0;
}

void Handle(EXCEPTION_POINTERS* exception, const char* reason) {
    if (g_handling.exchange(true)) {
        // Another thread is already reporting; a second failure adds nothing.
        Sleep(INFINITE);
    }
    Report report{exception, GetCurrentThreadId(), reason};
    HANDLE thread = CreateThread(nullptr, 0, WriteReport, &report, 0, nullptr);
    if (thread) {
        WaitForSingleObject(thread, g_interactive ? INFINITE : 30000);
        CloseHandle(thread);
    }
}

LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS* exception) {
    Handle(exception, "unhandled exception");
    // Let Windows record the failure in the Event Log and end the process.
    return EXCEPTION_CONTINUE_SEARCH;
}

[[noreturn]] void Fail(const char* reason) {
    Handle(nullptr, reason);
    TerminateProcess(GetCurrentProcess(), 3);
    for (;;) Sleep(INFINITE);
}

// std::terminate and abort() end here on every thread: an exception that
// escaped a thread, a failed noexcept, a broken invariant in the C runtime.
void OnAbort(int) { Fail("abnormal termination (abort or an uncaught C++ exception)"); }
void OnPureCall() { Fail("pure virtual function call"); }
void OnInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t) {
    Fail("invalid parameter passed to a C runtime function");
}

void RemoveOldReports() {
    struct Entry {
        std::wstring name;
        ULONGLONG written;
    };
    std::vector<Entry> entries;
    WIN32_FIND_DATAW found;
    HANDLE search = FindFirstFileW((std::wstring(g_directory) + L"\\*.dmp").c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) return;
    do {
        entries.push_back({found.cFileName, (static_cast<ULONGLONG>(found.ftLastWriteTime.dwHighDateTime) << 32) |
                                                found.ftLastWriteTime.dwLowDateTime});
    } while (FindNextFileW(search, &found));
    FindClose(search);
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.written > b.written; });
    // Leave room for the next report.
    for (size_t i = kReportsKept - 1; i < entries.size(); ++i) {
        const std::wstring path = std::wstring(g_directory) + L"\\" + entries[i].name;
        if (!DeleteFileW(path.c_str())) LOGW("[CRASH] could not remove old report %ls (error %lu)", path.c_str(), GetLastError());
    }
}

}  // namespace

void Install(bool interactive) {
    g_interactive = interactive;
    const std::wstring directory = AppDataDir() + L"\\crashes";
    if (directory.size() >= ARRAYSIZE(g_directory) - 64) {
        LOGW("[CRASH] report folder path is too long; crash reports are off");
        return;
    }
    wcscpy_s(g_directory, directory.c_str());
    if (!CreateDirectoryW(g_directory, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        LOGW("[CRASH] could not create %ls (error %lu); crash reports are off", g_directory, GetLastError());
        return;
    }
    RemoveOldReports();

    SetUnhandledExceptionFilter(OnUnhandledException);
    // abort() would otherwise go straight to Windows without calling the filter above.
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    signal(SIGABRT, OnAbort);
    _set_purecall_handler(OnPureCall);
    _set_invalid_parameter_handler(OnInvalidParameter);
    // Keep enough stack on the UI thread to start the report after a stack overflow.
    ULONG reserve = 64 * 1024;
    SetThreadStackGuarantee(&reserve);
}

void TestAccessViolation() {
    LOGI("[SELFTEST] deliberate access violation");
    volatile int* nowhere = nullptr;
    *nowhere = 1;
    for (;;) Sleep(INFINITE);
}

void TestTerminate() {
    LOGI("[SELFTEST] deliberate uncaught C++ exception on a worker thread");
    std::thread([] { throw std::runtime_error("deliberate self-test failure"); }).join();
    for (;;) Sleep(INFINITE);
}

}  // namespace crash
