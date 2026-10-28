// User settings, persisted to %LOCALAPPDATA%\AirWindows\settings.ini.
#pragma once

#include <string>
#include <vector>

enum class DecoderMode { Auto = 0, Hardware = 1, Software = 2 };

struct VideoQuality {
    const wchar_t* name;   // stored value, also the menu label
    int width;
    int height;
};

// The resolutions the receiver can advertise to the sender.
const std::vector<VideoQuality>& VideoQualities();
const VideoQuality& FindVideoQuality(const std::wstring& name);

struct Settings {
    std::wstring serverName;                 // AirPlay name shown on the sender
    std::wstring videoQuality = L"FHD";      // key into VideoQualities()
    int maxFps = 60;
    DecoderMode decoderMode = DecoderMode::Auto;
    bool vsync = true;
    bool showStatus = false;
    bool alwaysOnTop = false;
    bool allowH265 = true;
    std::string macAddress;                  // "aa:bb:cc:dd:ee:ff"; AirPlay device id

    void Load();
    void Save() const;
};

// %LOCALAPPDATA%\AirWindows, created on demand.
std::wstring AppDataDir();

std::string WideToUtf8(const std::wstring& text);
std::wstring Utf8ToWide(const std::string& text);
