// User settings, persisted to settings.ini in AppDataDir().
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
    std::string controlPhone;                // BLE peer fingerprint of the phone last controlled
    std::string controlSender;               // matching AirPlay sender fingerprint
    std::string controlPairs;                // every chosen phone: "sender=phone;sender=phone", newest first
    // Window position from the last run; all zero until one has been saved.
    int windowLeft = 0, windowTop = 0, windowRight = 0, windowBottom = 0;
    bool windowMaximized = false;
    // First-run setup guide: closed at least once, and the phone controlled at least once.
    bool setupGuideSeen = false;
    bool setupControlled = false;
    // The one-time line pointing to Help > Rate PhonePort… (rating.h): mirror sessions with
    // Bluetooth control used so far, and whether the line has been shown.
    int rateControlSessions = 0;
    bool rateNudgeShown = false;

    void Load();
    void Save() const;
};

// Where settings.ini, debug.txt and crashes\ live, created on demand. Unpackaged:
// %LOCALAPPDATA%\AirWindows. Packaged (MSIX): the package's local data folder
// (ApplicationData.Current.LocalFolder), which unpackaged tools can open too; the
// first packaged run copies settings.ini from %LOCALAPPDATA%\AirWindows if it exists.
std::wstring AppDataDir();
// %LOCALAPPDATA%\AirWindows, the folder an unpackaged run uses. Not created here.
std::wstring ClassicDataDir();
// Writes the data folder, and what the first packaged run carried over, to the log.
void LogDataFolder();

std::string WideToUtf8(const std::wstring& text);
std::wstring Utf8ToWide(const std::string& text);
