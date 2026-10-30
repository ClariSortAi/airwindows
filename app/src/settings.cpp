#include "settings.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>

#include "log.h"

namespace {

constexpr wchar_t kSection[] = L"AirWindows";

std::wstring IniPath() {
    return AppDataDir() + L"\\settings.ini";
}

// The profile APIs only write Unicode when the file already starts with a
// UTF-16 byte-order mark, so create it that way before the first write.
void EnsureUnicodeIni(const std::wstring& path) {
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return;
    }
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        LOGW("settings: could not create %ls (error %lu)", path.c_str(), GetLastError());
        return;
    }
    const wchar_t bom = 0xFEFF;
    DWORD written = 0;
    WriteFile(file, &bom, sizeof(bom), &written, nullptr);
    CloseHandle(file);
}

std::wstring ReadString(const std::wstring& path, const wchar_t* key, const std::wstring& fallback) {
    wchar_t buffer[512];
    GetPrivateProfileStringW(kSection, key, fallback.c_str(), buffer, ARRAYSIZE(buffer), path.c_str());
    return buffer;
}

int ReadInt(const std::wstring& path, const wchar_t* key, int fallback) {
    return static_cast<int>(GetPrivateProfileIntW(kSection, key, fallback, path.c_str()));
}

void WriteString(const std::wstring& path, const wchar_t* key, const std::wstring& value) {
    if (!WritePrivateProfileStringW(kSection, key, value.c_str(), path.c_str())) {
        LOGW("settings: failed to write %ls (error %lu)", key, GetLastError());
    }
}

std::wstring DefaultServerName() {
    wchar_t name[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD size = ARRAYSIZE(name);
    if (GetComputerNameW(name, &size) && size > 0) {
        return name;
    }
    return L"AirWindows";
}

}  // namespace

const std::vector<VideoQuality>& VideoQualities() {
    static const std::vector<VideoQuality> qualities = {
        {L"HD", 1280, 720},
        {L"HD+", 1280, 1280},
        {L"FHD", 1920, 1080},
        {L"FHD+", 1920, 1920},
        {L"QHD", 2560, 1440},
        {L"QHD+", 2560, 2560},
        {L"UHD", 3840, 2160},
    };
    return qualities;
}

const VideoQuality& FindVideoQuality(const std::wstring& name) {
    const auto& all = VideoQualities();
    const auto it = std::find_if(all.begin(), all.end(), [&](const VideoQuality& q) { return name == q.name; });
    return it != all.end() ? *it : all[2];  // FHD
}

std::wstring AppDataDir() {
    static std::wstring dir;
    if (!dir.empty()) {
        return dir;
    }
    PWSTR base = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &base))) {
        dir = std::wstring(base) + L"\\AirWindows";
        CoTaskMemFree(base);
    } else {
        wchar_t temp[MAX_PATH];
        GetTempPathW(ARRAYSIZE(temp), temp);
        dir = std::wstring(temp) + L"AirWindows";
    }
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

void Settings::Load() {
    const std::wstring path = IniPath();
    serverName = ReadString(path, L"AirPlayServerName", DefaultServerName());
    if (serverName.empty()) {
        serverName = DefaultServerName();
    }
    videoQuality = FindVideoQuality(ReadString(path, L"AirPlayVideoQuality", L"FHD")).name;
    maxFps = ReadInt(path, L"AirPlay_FPS", 60) >= 60 ? 60 : 30;
    const int mode = ReadInt(path, L"AirPlay_DecoderMode", 0);
    decoderMode = (mode >= 0 && mode <= 2) ? static_cast<DecoderMode>(mode) : DecoderMode::Auto;
    vsync = ReadInt(path, L"AirPlay_VSync", 1) != 0;
    showStatus = ReadInt(path, L"AirPlay_ShowStatus", 0) != 0;
    alwaysOnTop = ReadInt(path, L"AlwaysOnTop", 0) != 0;
    allowH265 = ReadInt(path, L"AllowH265", 1) != 0;
    macAddress = WideToUtf8(ReadString(path, L"MacAddress", L""));
    controlPhone = WideToUtf8(ReadString(path, L"NativeControlPhone", L""));
    if(controlPhone.size()!=16 || controlPhone.find_first_not_of("0123456789abcdef")!=std::string::npos)controlPhone.clear();
    controlSender = WideToUtf8(ReadString(path, L"NativeControlSender", L""));
    if(controlSender.size()!=16 || controlSender.find_first_not_of("0123456789abcdef")!=std::string::npos)controlSender.clear();
}

void Settings::Save() const {
    const std::wstring path = IniPath();
    EnsureUnicodeIni(path);
    WriteString(path, L"AirPlayServerName", serverName);
    WriteString(path, L"AirPlayVideoQuality", videoQuality);
    WriteString(path, L"AirPlay_FPS", std::to_wstring(maxFps));
    WriteString(path, L"AirPlay_DecoderMode", std::to_wstring(static_cast<int>(decoderMode)));
    WriteString(path, L"AirPlay_VSync", vsync ? L"1" : L"0");
    WriteString(path, L"AirPlay_ShowStatus", showStatus ? L"1" : L"0");
    WriteString(path, L"AlwaysOnTop", alwaysOnTop ? L"1" : L"0");
    WriteString(path, L"AllowH265", allowH265 ? L"1" : L"0");
    WriteString(path, L"MacAddress", Utf8ToWide(macAddress));
    WriteString(path, L"NativeControlPhone", Utf8ToWide(controlPhone));
    WriteString(path, L"NativeControlSender", Utf8ToWide(controlSender));
}

std::string WideToUtf8(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}
