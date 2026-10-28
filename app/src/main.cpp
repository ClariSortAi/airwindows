// AirWindows: an AirPlay screen mirroring receiver for Windows. This
// file is the host: window, menu, settings and the glue between the protocol
// engine and the audio/video pipelines.

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <iphlpapi.h>
#include <objbase.h>
#include <shellapi.h>

#include <atomic>
#include <cmath>
#include <string>
#include <thread>
#include <vector>

#include <openssl/crypto.h>

#include "airplay_server.h"
#include "audio_pipeline.h"
#include "log.h"
#include "media_sink.h"
#include "resource.h"
#include "settings.h"
#include "version.h"
#include "video_pipeline.h"

namespace {

constexpr wchar_t kMainClass[] = L"AirWindowsMainWindow";
constexpr wchar_t kVideoClass[] = L"AirWindowsVideoWindow";
constexpr wchar_t kAppTitle[] = L"AirWindows";
constexpr UINT_PTR kTickTimer = 1;
constexpr UINT_PTR kExitTimer = 2;
constexpr COLORREF kBackground = RGB(0x2C, 0x2C, 0x2C);  // idle screen
constexpr DWORD kCursorHideMs = 3000;

// Routes protocol callbacks to the two pipelines.
class MediaRouter final : public IMediaSink {
public:
    MediaRouter(VideoPipeline& video, AudioPipeline& audio) : video_(video), audio_(audio) {}

    bool OnVideoCodec(bool h265) override { return video_.SetCodec(h265); }
    void OnVideoData(const uint8_t* data, int size, uint64_t ptsNs) override { video_.Push(data, size, ptsNs); }
    void OnVideoSize(int sourceWidth, int sourceHeight, int width, int height) override {
        LOGI("video stream %dx%d (source %dx%d)", width, height, sourceWidth, sourceHeight);
    }
    void OnVideoPause() override { LOGI("video paused by sender"); }
    void OnVideoResume() override { LOGI("video resumed by sender"); }
    void OnVideoFlush() override {}
    void OnVideoReset() override { video_.Reset(); }

    void OnAudioFormat(uint8_t compressionType) override { audio_.SetFormat(compressionType); }
    void OnAudioData(const uint8_t* data, int size, uint8_t compressionType, uint64_t) override {
        audio_.Push(data, size, compressionType);
    }
    void OnAudioFlush() override { audio_.Flush(); }
    void OnAudioStop() override { audio_.StopPlayback(); }
    void OnAudioVolume(double gain) override { audio_.SetGain(gain); }

private:
    VideoPipeline& video_;
    AudioPipeline& audio_;
};

struct App {
    HINSTANCE instance = nullptr;
    HWND window = nullptr;
    HWND videoWindow = nullptr;
    HMENU menu = nullptr;

    Settings settings;
    VideoPipeline video;
    AudioPipeline audio;
    MediaRouter router{video, audio};
    AirPlayServer server;

    bool serverOk = false;
    bool hevcHardware = false;
    bool debugLog = false;
    bool fullscreen = false;
    WINDOWPLACEMENT savedPlacement{sizeof(WINDOWPLACEMENT)};
    LONG_PTR savedStyle = 0;
    DWORD lastMouseMove = 0;
    bool cursorHidden = false;
    bool shutDown = false;

    bool hasPicture = false;
    int pictureWidth = 0;
    int pictureHeight = 0;
    bool mirrorRunning = false;
    int connections = 0;
    ClientInfo client;

    // --selftest options
    int decoderOverride = -1;
    std::wstring selfTestVideo;
    bool selfTestAudio = false;
    bool selfTestTone = false;
    int exitAfterSeconds = 0;
    std::thread selfTestThread;
    std::atomic<bool> selfTestStop{false};
    std::atomic<int> selfTestPushed{0};
};

App* g_app = nullptr;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

int Scale(HWND window, int value) {
    return MulDiv(value, static_cast<int>(GetDpiForWindow(window)), 96);
}

std::vector<std::wstring> LocalIPv4Addresses() {
    std::vector<std::wstring> result;
    ULONG size = 16 * 1024;
    std::vector<unsigned char> buffer(size);
    auto* addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG rc = GetAdaptersAddresses(AF_INET, flags, nullptr, addresses, &size);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(size);
        addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        rc = GetAdaptersAddresses(AF_INET, flags, nullptr, addresses, &size);
    }
    if (rc != NO_ERROR) {
        LOGW("GetAdaptersAddresses failed (error %lu)", rc);
        return result;
    }
    for (auto* adapter = addresses; adapter; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
            continue;
        }
        for (auto* unicast = adapter->FirstUnicastAddress; unicast; unicast = unicast->Next) {
            // Skip 169.254.x.x self-assigned addresses: no sender can reach the PC through them.
            const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(unicast->Address.lpSockaddr);
            if ((ntohl(ipv4->sin_addr.s_addr) & 0xFFFF0000u) == 0xA9FE0000u) {
                continue;
            }
            wchar_t text[64];
            DWORD length = ARRAYSIZE(text);
            if (WSAAddressToStringW(unicast->Address.lpSockaddr, unicast->Address.iSockaddrLength, nullptr, text,
                                    &length) == 0) {
                result.emplace_back(text);
            }
        }
    }
    return result;
}

std::wstring QualityLabel(const VideoQuality& quality) {
    wchar_t text[64];
    swprintf_s(text, L"%s (%d × %d)", quality.name, quality.width, quality.height);
    return text;
}

const wchar_t* CapabilitySummary(const App& app) {
    return app.hevcHardware
               ? L"The GPU in this PC can decode H.265. Audio: AAC-ELD and ALAC."
               : L"The GPU in this PC cannot decode H.265, so video uses H.264. Audio: AAC-ELD and ALAC.";
}

void UpdateTitle(App& app) {
    std::wstring title = std::wstring(kAppTitle) + L" — " + app.settings.serverName;
    if (app.connections > 0 && !app.client.name.empty()) {
        title += L" — " + Utf8ToWide(app.client.name);
    }
    if (app.settings.showStatus && app.hasPicture) {
        const VideoStats stats = app.video.Stats();
        wchar_t status[160];
        swprintf_s(status, L" | %d×%d %s (%s) | %.0f fps | %.1f Mbps | audio %hs %.0f ms", app.pictureWidth,
                   app.pictureHeight, stats.h265 ? L"H.265" : L"H.264", stats.hardware ? L"D3D11" : L"FFmpeg",
                   stats.fps, stats.bitrateMbps, app.audio.CodecName().c_str(), app.audio.BufferedMs());
        title += status;
    }
    SetWindowTextW(app.window, title.c_str());
}

void SetCursorHidden(App& app, bool hidden) {
    if (app.cursorHidden == hidden) {
        return;
    }
    app.cursorHidden = hidden;
    ShowCursor(hidden ? FALSE : TRUE);
}

// ---------------------------------------------------------------------------
// Server lifecycle
// ---------------------------------------------------------------------------

bool StartServer(App& app) {
    const VideoQuality& quality = FindVideoQuality(app.settings.videoQuality);
    app.hevcHardware = VideoPipeline::HardwareHevcAvailable();
    LOGI("%ls", CapabilitySummary(app));

    ServerConfig config;
    config.name = WideToUtf8(app.settings.serverName);
    config.macAddress = app.settings.macAddress;
    config.width = quality.width;
    config.height = quality.height;
    config.refreshRate = 60;
    config.maxFps = app.settings.maxFps;
    // H.265 is only offered when the GPU can decode it.
    config.supportsH265 = app.settings.allowH265 && app.hevcHardware && app.settings.decoderMode != DecoderMode::Software;
    config.debugLog = app.debugLog;

    LOGI("[CFG] Start() begin: serverName='%s' quality=%ls fps=%d decoderMode=%d vsync=%d", config.name.c_str(),
         quality.name, config.maxFps, static_cast<int>(app.settings.decoderMode), app.settings.vsync ? 1 : 0);
    app.serverOk = app.server.Start(config, &app.router, app.window);
    LOGI("[CFG] Start() %s", app.serverOk ? "complete" : "FAILED");
    return app.serverOk;
}

// Applies settings that only take effect on a fresh server (and, for the decoder mode, a fresh video pipeline).
void RestartService(App& app, bool restartVideo) {
    LOGI("[CFG] Stop() begin");
    app.server.Stop();
    app.audio.StopPlayback();
    app.video.Reset();
    LOGI("[CFG] Stop() complete");
    if (restartVideo) {
        app.video.Stop();
        if (!app.video.Start(app.videoWindow, app.window, app.settings.decoderMode, app.settings.vsync)) {
            LOGE("video pipeline failed to restart");
        }
    }
    app.connections = 0;
    app.client = ClientInfo{};
    app.mirrorRunning = false;
    StartServer(app);
    UpdateTitle(app);
    InvalidateRect(app.window, nullptr, TRUE);
}

// ---------------------------------------------------------------------------
// Window behavior
// ---------------------------------------------------------------------------

void ToggleFullscreen(App& app) {
    HWND window = app.window;
    if (!app.fullscreen) {
        app.savedStyle = GetWindowLongPtrW(window, GWL_STYLE);
        GetWindowPlacement(window, &app.savedPlacement);
        MONITORINFO monitor{sizeof(MONITORINFO)};
        GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor);
        SetMenu(window, nullptr);
        SetWindowLongPtrW(window, GWL_STYLE, app.savedStyle & ~static_cast<LONG_PTR>(WS_OVERLAPPEDWINDOW));
        SetWindowPos(window, HWND_TOP, monitor.rcMonitor.left, monitor.rcMonitor.top,
                     monitor.rcMonitor.right - monitor.rcMonitor.left, monitor.rcMonitor.bottom - monitor.rcMonitor.top,
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        app.fullscreen = true;
        app.lastMouseMove = GetTickCount();
    } else {
        SetWindowLongPtrW(window, GWL_STYLE, app.savedStyle);
        SetMenu(window, app.menu);
        SetWindowPlacement(window, &app.savedPlacement);
        SetWindowPos(window, app.settings.alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        app.fullscreen = false;
        SetCursorHidden(app, false);
    }
}

// Resizes the window so the client area has exactly the picture's aspect ratio (no black bars).
void FitWindowToVideo(App& app) {
    if (!app.hasPicture || app.pictureWidth <= 0 || app.pictureHeight <= 0 || app.fullscreen || IsZoomed(app.window)) {
        return;
    }
    RECT client{};
    GetClientRect(app.window, &client);
    MONITORINFO monitor{sizeof(MONITORINFO)};
    GetMonitorInfoW(MonitorFromWindow(app.window, MONITOR_DEFAULTTONEAREST), &monitor);
    const int workWidth = monitor.rcWork.right - monitor.rcWork.left;
    const int workHeight = monitor.rcWork.bottom - monitor.rcWork.top;

    // Keep the current area roughly constant while changing the shape.
    const double area = static_cast<double>(client.right) * client.bottom;
    const double aspect = static_cast<double>(app.pictureWidth) / app.pictureHeight;
    int width = static_cast<int>(sqrt(area * aspect));
    int height = static_cast<int>(width / aspect);

    RECT frame{0, 0, width, height};
    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(app.window, GWL_STYLE));
    const DWORD exStyle = static_cast<DWORD>(GetWindowLongPtrW(app.window, GWL_EXSTYLE));
    AdjustWindowRectExForDpi(&frame, style, TRUE, exStyle, GetDpiForWindow(app.window));
    const int chromeWidth = (frame.right - frame.left) - width;
    const int chromeHeight = (frame.bottom - frame.top) - height;
    if (width + chromeWidth > workWidth) {
        width = workWidth - chromeWidth;
        height = static_cast<int>(width / aspect);
    }
    if (height + chromeHeight > workHeight) {
        height = workHeight - chromeHeight;
        width = static_cast<int>(height * aspect);
    }
    LOGI("[FIT] resize -> %dx%d client for %dx%d content", width, height, app.pictureWidth, app.pictureHeight);
    SetWindowPos(app.window, nullptr, 0, 0, width + chromeWidth, height + chromeHeight,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

HMENU BuildMenu() {
    HMENU bar = CreateMenu();

    HMENU receiver = CreatePopupMenu();
    AppendMenuW(receiver, MF_STRING, IDM_RENAME, L"&Rename…");
    AppendMenuW(receiver, MF_STRING, IDM_RESTART, L"Restart &service");
    AppendMenuW(receiver, MF_STRING, IDM_OPEN_LOG_FOLDER, L"Open &log folder");
    AppendMenuW(receiver, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(receiver, MF_STRING, IDM_EXIT, L"E&xit");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(receiver), L"&Receiver");

    HMENU video = CreatePopupMenu();
    HMENU quality = CreatePopupMenu();
    const auto& qualities = VideoQualities();
    for (size_t i = 0; i < qualities.size(); ++i) {
        AppendMenuW(quality, MF_STRING, IDM_QUALITY_FIRST + static_cast<UINT>(i), QualityLabel(qualities[i]).c_str());
    }
    AppendMenuW(video, MF_POPUP, reinterpret_cast<UINT_PTR>(quality), L"&Resolution");
    HMENU fps = CreatePopupMenu();
    AppendMenuW(fps, MF_STRING, IDM_FPS_30, L"30 fps");
    AppendMenuW(fps, MF_STRING, IDM_FPS_60, L"60 fps");
    AppendMenuW(video, MF_POPUP, reinterpret_cast<UINT_PTR>(fps), L"&Frame rate");
    HMENU decoder = CreatePopupMenu();
    AppendMenuW(decoder, MF_STRING, IDM_DECODER_AUTO, L"Auto");
    AppendMenuW(decoder, MF_STRING, IDM_DECODER_HARDWARE, L"Hardware");
    AppendMenuW(decoder, MF_STRING, IDM_DECODER_SOFTWARE, L"Software");
    AppendMenuW(video, MF_POPUP, reinterpret_cast<UINT_PTR>(decoder), L"&Decoder");
    AppendMenuW(video, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(video, MF_STRING, IDM_VSYNC, L"&VSync");
    AppendMenuW(video, MF_STRING, IDM_ALLOW_H265, L"Allow &H.265 when the GPU can decode it");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(video), L"&Video");

    HMENU view = CreatePopupMenu();
    AppendMenuW(view, MF_STRING, IDM_FULLSCREEN, L"&Full screen\tF11");
    AppendMenuW(view, MF_STRING, IDM_FIT_WINDOW, L"Fit &window to video");
    AppendMenuW(view, MF_STRING, IDM_ALWAYS_ON_TOP, L"Always on &top");
    AppendMenuW(view, MF_STRING, IDM_SHOW_STATUS, L"Show &status in title bar");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(view), L"Vie&w");

    HMENU help = CreatePopupMenu();
    AppendMenuW(help, MF_STRING, IDM_HOW_TO_CONNECT, L"&How to connect");
    AppendMenuW(help, MF_STRING, IDM_ABOUT, L"&About");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(help), L"&Help");
    return bar;
}

void SyncMenu(App& app) {
    HMENU menu = app.menu;
    const auto& qualities = VideoQualities();
    for (size_t i = 0; i < qualities.size(); ++i) {
        const bool current = app.settings.videoQuality == qualities[i].name;
        CheckMenuItem(menu, IDM_QUALITY_FIRST + static_cast<UINT>(i), current ? MF_CHECKED : MF_UNCHECKED);
    }
    CheckMenuItem(menu, IDM_FPS_30, app.settings.maxFps == 30 ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem(menu, IDM_FPS_60, app.settings.maxFps == 60 ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem(menu, IDM_DECODER_AUTO, app.settings.decoderMode == DecoderMode::Auto ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem(menu, IDM_DECODER_HARDWARE,
                  app.settings.decoderMode == DecoderMode::Hardware ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem(menu, IDM_DECODER_SOFTWARE,
                  app.settings.decoderMode == DecoderMode::Software ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem(menu, IDM_VSYNC, app.settings.vsync ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem(menu, IDM_ALLOW_H265, app.settings.allowH265 ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem(menu, IDM_FULLSCREEN, app.fullscreen ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem(menu, IDM_ALWAYS_ON_TOP, app.settings.alwaysOnTop ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem(menu, IDM_SHOW_STATUS, app.settings.showStatus ? MF_CHECKED : MF_UNCHECKED);
    EnableMenuItem(menu, IDM_FIT_WINDOW, app.hasPicture && !app.fullscreen ? MF_ENABLED : MF_GRAYED);
}

INT_PTR CALLBACK RenameDialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_INITDIALOG: {
        SetWindowLongPtrW(dialog, DWLP_USER, lParam);
        auto* name = reinterpret_cast<std::wstring*>(lParam);
        SetDlgItemTextW(dialog, IDC_NAME_EDIT, name->c_str());
        SendDlgItemMessageW(dialog, IDC_NAME_EDIT, EM_SETLIMITTEXT, 40, 0);
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK) {
            auto* name = reinterpret_cast<std::wstring*>(GetWindowLongPtrW(dialog, DWLP_USER));
            wchar_t text[64];
            GetDlgItemTextW(dialog, IDC_NAME_EDIT, text, ARRAYSIZE(text));
            std::wstring trimmed = text;
            const size_t first = trimmed.find_first_not_of(L" \t");
            const size_t last = trimmed.find_last_not_of(L" \t");
            trimmed = first == std::wstring::npos ? L"" : trimmed.substr(first, last - first + 1);
            if (trimmed.empty()) {
                MessageBeep(MB_ICONWARNING);
                return TRUE;
            }
            *name = trimmed;
            EndDialog(dialog, IDOK);
            return TRUE;
        }
        if (LOWORD(wParam) == IDCANCEL) {
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

void ShowHowToConnect(App& app) {
    std::wstring text =
        L"1. Connect your iPhone, iPad or Mac to the same network as this PC.\n"
        L"2. Open Control Center and choose Screen Mirroring.\n"
        L"3. Pick “" + app.settings.serverName + L"” from the list.\n\n"
        L"If the name does not appear, allow AirWindows through Windows Firewall on private "
        L"networks and make sure the network is not set to isolate devices from each other.";
    MessageBoxW(app.window, text.c_str(), L"How to connect", MB_OK | MB_ICONINFORMATION);
}

void ShowAbout(App& app) {
    MessageBoxW(app.window,
                L"AirWindows " APP_VERSION_STRING L"\n"
                L"graniteai.co/tools/airwindows\n\n"
                L"Shows the screen of an iPhone, iPad or Mac on this PC over AirPlay.\n\n"
                L"AirPlay protocol engine: UxPlay (GPL-3.0)\n"
                L"Decoding: FFmpeg (LGPL-2.1-or-later)\n"
                L"Also uses OpenSSL and libplist.\n\n"
                L"This program is free software under the GNU General Public License, version 3 or later, "
                L"and comes with no warranty. Its source code comes with the download.\n\n"
                L"AirPlay, iPhone, iPad and Mac are trademarks of Apple Inc. This program is not affiliated "
                L"with or endorsed by Apple.",
                L"About AirWindows", MB_OK | MB_ICONINFORMATION);
}

// ---------------------------------------------------------------------------
// Idle screen
// ---------------------------------------------------------------------------

void DrawLine(HDC dc, HFONT font, COLORREF color, const std::wstring& text, RECT& area, int gapAfter) {
    HGDIOBJ old = SelectObject(dc, font);
    SetTextColor(dc, color);
    RECT measure = area;
    DrawTextW(dc, text.c_str(), -1, &measure, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
    RECT draw = area;
    draw.bottom = draw.top + (measure.bottom - measure.top);
    DrawTextW(dc, text.c_str(), -1, &draw, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
    area.top = draw.bottom + gapAfter;
    SelectObject(dc, old);
}

HFONT MakeFont(HWND window, int points, int weight) {
    return CreateFontW(-MulDiv(points, static_cast<int>(GetDpiForWindow(window)), 72), 0, 0, 0, weight, FALSE, FALSE,
                       FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
}

void PaintIdle(App& app, HDC target, const RECT& client) {
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width <= 0 || height <= 0) {
        return;
    }
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bitmap = CreateCompatibleBitmap(target, width, height);
    HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
    HBRUSH background = CreateSolidBrush(kBackground);
    FillRect(dc, &client, background);
    DeleteObject(background);
    SetBkMode(dc, TRANSPARENT);

    HFONT titleFont = MakeFont(app.window, 30, FW_SEMIBOLD);
    HFONT headingFont = MakeFont(app.window, 15, FW_NORMAL);
    HFONT bodyFont = MakeFont(app.window, 12, FW_NORMAL);
    HFONT smallFont = MakeFont(app.window, 9, FW_NORMAL);

    const int margin = Scale(app.window, 32);
    RECT area{client.left + margin, client.top + height / 6, client.right - margin, client.bottom};

    DrawLine(dc, titleFont, RGB(0xFF, 0xFF, 0xFF), app.settings.serverName, area, Scale(app.window, 8));

    std::wstring status;
    COLORREF statusColor = RGB(0x8F, 0xD1, 0x8F);
    if (!app.serverOk) {
        status = L"The AirPlay service could not start. See the log (Receiver ▸ Open log folder).";
        statusColor = RGB(0xFF, 0x8A, 0x80);
    } else if (app.connections > 0) {
        const std::wstring who = app.client.name.empty() ? L"a device" : Utf8ToWide(app.client.name);
        status = app.mirrorRunning ? L"Connected to " + who + L" — waiting for video…"
                                   : L"Connected to " + who + L". Ready to play.";
    } else {
        status = L"Ready to mirror";
    }
    DrawLine(dc, headingFont, statusColor, status, area, Scale(app.window, 28));

    const COLORREF bodyColor = RGB(0xD8, 0xD8, 0xD8);
    DrawLine(dc, bodyFont, bodyColor, L"1. Connect your iPhone, iPad or Mac to the same network as this PC.", area,
             Scale(app.window, 6));
    DrawLine(dc, bodyFont, bodyColor, L"2. Open Control Center and choose Screen Mirroring.", area,
             Scale(app.window, 6));
    DrawLine(dc, bodyFont, bodyColor, L"3. Pick “" + app.settings.serverName + L"” from the list.", area,
             Scale(app.window, 28));

    std::wstring network;
    for (const auto& address : LocalIPv4Addresses()) {
        network += network.empty() ? address : L", " + address;
    }
    if (network.empty()) {
        network = L"no network connection";
    }
    const VideoQuality& quality = FindVideoQuality(app.settings.videoQuality);
    const std::wstring details = L"Listening on " + network + L", port " + std::to_wstring(app.server.Port()) +
                                 L" · " + QualityLabel(quality) + L", up to " +
                                 std::to_wstring(app.settings.maxFps) + L" fps";
    const COLORREF dimColor = RGB(0x9A, 0x9A, 0x9A);
    DrawLine(dc, smallFont, dimColor, details, area, Scale(app.window, 4));
    DrawLine(dc, smallFont, dimColor, CapabilitySummary(app), area, 0);

    BitBlt(target, client.left, client.top, width, height, dc, 0, 0, SRCCOPY);
    DeleteObject(titleFont);
    DeleteObject(headingFont);
    DeleteObject(bodyFont);
    DeleteObject(smallFont);
    SelectObject(dc, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(dc);
}

// ---------------------------------------------------------------------------
// Self test (no Apple device needed): feeds an Annex-B elementary stream
// through the same sink the protocol engine uses.
// ---------------------------------------------------------------------------

bool ReadWholeFile(const std::wstring& path, std::vector<uint8_t>& bytes) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        LOGE("[SELFTEST] cannot open %ls (error %lu)", path.c_str(), GetLastError());
        return false;
    }
    LARGE_INTEGER size{};
    GetFileSizeEx(file, &size);
    bytes.resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    const BOOL ok = ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr);
    CloseHandle(file);
    if (!ok || read != bytes.size()) {
        LOGE("[SELFTEST] short read on %ls", path.c_str());
        return false;
    }
    return true;
}

// Splits an Annex-B stream into access units: parameter sets and SEI stay with the picture that follows.
std::vector<std::vector<uint8_t>> SplitAccessUnits(const std::vector<uint8_t>& bytes, bool h265) {
    std::vector<size_t> starts;
    for (size_t i = 0; i + 3 < bytes.size(); ++i) {
        if (bytes[i] == 0 && bytes[i + 1] == 0 && bytes[i + 2] == 1) {
            starts.push_back(i > 0 && bytes[i - 1] == 0 ? i - 1 : i);
            i += 2;
        }
    }
    std::vector<std::vector<uint8_t>> units;
    std::vector<uint8_t> current;
    bool currentHasPicture = false;
    for (size_t n = 0; n < starts.size(); ++n) {
        const size_t begin = starts[n];
        const size_t end = n + 1 < starts.size() ? starts[n + 1] : bytes.size();
        const size_t header = begin + (bytes[begin + 2] == 1 ? 3 : 4);
        if (header + 2 >= end) {
            continue;
        }
        bool isPicture;
        bool firstSlice;
        if (h265) {
            const int type = (bytes[header] >> 1) & 0x3F;
            isPicture = type <= 31;
            firstSlice = (bytes[header + 2] & 0x80) != 0;  // first_slice_segment_in_pic_flag
        } else {
            const int type = bytes[header] & 0x1F;
            isPicture = type >= 1 && type <= 5;
            firstSlice = (bytes[header + 1] & 0x80) != 0;  // first_mb_in_slice == 0
        }
        if (currentHasPicture && (!isPicture || firstSlice)) {
            units.push_back(std::move(current));
            current.clear();
            currentHasPicture = false;
        }
        // The decoder expects four-byte start codes at the head of a unit.
        if (current.empty() && bytes[begin + 2] == 1) {
            current.push_back(0);
        }
        current.insert(current.end(), bytes.begin() + static_cast<ptrdiff_t>(begin), bytes.begin() + static_cast<ptrdiff_t>(end));
        currentHasPicture = currentHasPicture || isPicture;
    }
    if (currentHasPicture) {
        units.push_back(std::move(current));
    }
    return units;
}

void RunSelfTest(App& app) {
    if (app.selfTestAudio) {
        const bool ok = app.audio.SelfTestDecoders();
        LOGI("[SELFTEST] audio decoders: %s", ok ? "all opened" : "FAILURE");
    }
    if (app.selfTestTone) {
        app.audio.PushTestTone(1.0, 440.0);
        const double queued = app.audio.BufferedMs();
        Sleep(400);
        // The render thread is the only consumer, so an empty buffer means the device took the samples.
        LOGI("[SELFTEST] audio tone: %.0f ms still queued after the last packet, %.0f ms after a further 400 ms",
             queued, app.audio.BufferedMs());
    }
    if (app.selfTestVideo.empty()) {
        return;
    }
    std::vector<uint8_t> bytes;
    if (!ReadWholeFile(app.selfTestVideo, bytes)) {
        return;
    }
    const std::wstring& path = app.selfTestVideo;
    const bool h265 = path.size() > 4 && (path.rfind(L".h265") == path.size() - 5 || path.rfind(L".hevc") == path.size() - 5 ||
                                          path.rfind(L".265") == path.size() - 4);
    const auto units = SplitAccessUnits(bytes, h265);
    LOGI("[SELFTEST] %ls: %zu bytes, %zu access units, codec %s", path.c_str(), bytes.size(), units.size(),
         h265 ? "H.265" : "H.264");
    if (units.empty()) {
        return;
    }
    IMediaSink& sink = app.router;
    uint64_t pts = 0;
    while (!app.selfTestStop) {
        // Announce the codec on every pass, as a reconnecting sender would, so the
        // stream recovers after the video pipeline is restarted from the menu.
        sink.OnVideoCodec(h265);
        for (const auto& unit : units) {
            if (app.selfTestStop) {
                break;
            }
            sink.OnVideoData(unit.data(), static_cast<int>(unit.size()), pts);
            ++app.selfTestPushed;
            pts += 33333333ull;
            Sleep(33);
        }
    }
}

void Shutdown(App& app) {
    if (app.shutDown) {
        return;
    }
    app.shutDown = true;
    app.selfTestStop = true;
    if (app.selfTestThread.joinable()) {
        app.selfTestThread.join();
    }
    if (!app.selfTestVideo.empty()) {
        const VideoStats stats = app.video.Stats();
        LOGI("[SELFTEST] video summary: pushed=%d picture=%dx%d codec=%s decoder=%s fps=%.1f bitrate=%.2fMbps",
             app.selfTestPushed.load(), stats.width, stats.height, stats.h265 ? "H.265" : "H.264",
             stats.hardware ? "D3D11" : "FFmpeg", stats.fps, stats.bitrateMbps);
    }
    // The server goes first so no callback reaches a pipeline that is shutting down.
    app.server.Stop();
    app.video.Stop();
    app.audio.Stop();
    SetThreadExecutionState(ES_CONTINUOUS);
}

// ---------------------------------------------------------------------------
// Window procedures
// ---------------------------------------------------------------------------

void OnCommand(App& app, UINT id) {
    Settings& settings = app.settings;
    const auto& qualities = VideoQualities();
    if (id >= IDM_QUALITY_FIRST && id < IDM_QUALITY_FIRST + qualities.size()) {
        settings.videoQuality = qualities[id - IDM_QUALITY_FIRST].name;
        LOGI("[CFG] SetVideoQualityName('%ls')", settings.videoQuality.c_str());
        settings.Save();
        RestartService(app, false);
        return;
    }
    switch (id) {
    case IDM_RENAME: {
        std::wstring name = settings.serverName;
        if (DialogBoxParamW(app.instance, MAKEINTRESOURCEW(IDD_RENAME), app.window, RenameDialogProc,
                            reinterpret_cast<LPARAM>(&name)) == IDOK && name != settings.serverName) {
            settings.serverName = name;
            LOGI("[CFG] SetAirPlayServerName('%s')", WideToUtf8(name).c_str());
            settings.Save();
            RestartService(app, false);
        }
        break;
    }
    case IDM_RESTART:
        RestartService(app, false);
        break;
    case IDM_OPEN_LOG_FOLDER:
        ShellExecuteW(app.window, L"open", AppDataDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        break;
    case IDM_EXIT:
        PostMessageW(app.window, WM_CLOSE, 0, 0);
        break;
    case IDM_FPS_30:
    case IDM_FPS_60:
        settings.maxFps = id == IDM_FPS_60 ? 60 : 30;
        settings.Save();
        RestartService(app, false);
        break;
    case IDM_DECODER_AUTO:
    case IDM_DECODER_HARDWARE:
    case IDM_DECODER_SOFTWARE:
        settings.decoderMode = id == IDM_DECODER_AUTO ? DecoderMode::Auto
                               : id == IDM_DECODER_HARDWARE ? DecoderMode::Hardware : DecoderMode::Software;
        settings.Save();
        RestartService(app, true);
        break;
    case IDM_VSYNC:
        settings.vsync = !settings.vsync;
        settings.Save();
        app.video.SetVSync(settings.vsync);
        break;
    case IDM_ALLOW_H265:
        settings.allowH265 = !settings.allowH265;
        settings.Save();
        RestartService(app, false);
        break;
    case IDM_FULLSCREEN:
        ToggleFullscreen(app);
        break;
    case IDM_FIT_WINDOW:
        FitWindowToVideo(app);
        break;
    case IDM_ALWAYS_ON_TOP:
        settings.alwaysOnTop = !settings.alwaysOnTop;
        settings.Save();
        if (!app.fullscreen) {
            SetWindowPos(app.window, settings.alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        break;
    case IDM_SHOW_STATUS:
        settings.showStatus = !settings.showStatus;
        settings.Save();
        UpdateTitle(app);
        break;
    case IDM_HOW_TO_CONNECT:
        ShowHowToConnect(app);
        break;
    case IDM_ABOUT:
        ShowAbout(app);
        break;
    }
}

LRESULT CALLBACK VideoWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        ValidateRect(window, nullptr);
        if (g_app) {
            g_app->video.RequestRedraw();
        }
        return 0;
    // The video surface has no behavior of its own; the main window handles input.
    case WM_LBUTTONDBLCLK:
    case WM_MOUSEMOVE:
        return SendMessageW(GetParent(window), message, wParam, lParam);
    case WM_RBUTTONUP: {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ClientToScreen(window, &point);
        return SendMessageW(GetParent(window), WM_CONTEXTMENU, reinterpret_cast<WPARAM>(window),
                            MAKELPARAM(point.x, point.y));
    }
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK MainWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    App* app = g_app;
    if (!app) {
        return DefWindowProcW(window, message, wParam, lParam);
    }
    switch (message) {
    case WM_SIZE:
        if (app->videoWindow && wParam != SIZE_MINIMIZED) {
            MoveWindow(app->videoWindow, 0, 0, LOWORD(lParam), HIWORD(lParam), FALSE);
            app->video.NotifyResize();
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize.x = Scale(window, 420);
        info->ptMinTrackSize.y = Scale(window, 300);
        return 0;
    }
    case WM_DPICHANGED: {
        const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(window, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                     suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint;
        HDC dc = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        PaintIdle(*app, dc, client);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_INITMENUPOPUP:
        SyncMenu(*app);
        return 0;
    case WM_COMMAND:
        OnCommand(*app, LOWORD(wParam));
        return 0;
    case WM_CONTEXTMENU: {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (point.x == -1 && point.y == -1) {
            GetCursorPos(&point);
        }
        // Reuse the menu bar's submenus so the context menu works in full screen too.
        HMENU popup = CreatePopupMenu();
        wchar_t label[64];
        const int count = GetMenuItemCount(app->menu);
        for (int i = 0; i < count; ++i) {
            GetMenuStringW(app->menu, static_cast<UINT>(i), label, ARRAYSIZE(label), MF_BYPOSITION);
            AppendMenuW(popup, MF_POPUP, reinterpret_cast<UINT_PTR>(GetSubMenu(app->menu, i)), label);
        }
        SyncMenu(*app);
        SetCursorHidden(*app, false);
        TrackPopupMenu(popup, TPM_RIGHTBUTTON, point.x, point.y, 0, window, nullptr);
        // Detach the shared submenus before destroying the wrapper.
        for (int i = count - 1; i >= 0; --i) {
            RemoveMenu(popup, static_cast<UINT>(i), MF_BYPOSITION);
        }
        DestroyMenu(popup);
        return 0;
    }
    case WM_LBUTTONDBLCLK:
        ToggleFullscreen(*app);
        return 0;
    case WM_MOUSEMOVE:
        app->lastMouseMove = GetTickCount();
        SetCursorHidden(*app, false);
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_F11 || (wParam == VK_ESCAPE && app->fullscreen)) {
            ToggleFullscreen(*app);
            return 0;
        }
        break;
    case WM_SYSKEYDOWN:
        if (wParam == VK_RETURN) {  // Alt+Enter
            ToggleFullscreen(*app);
            return 0;
        }
        break;
    case WM_TIMER:
        if (wParam == kTickTimer) {
            if (app->server.WatchdogTick()) {
                app->server.RestartListener();
            }
            if (app->settings.showStatus && app->hasPicture) {
                UpdateTitle(*app);
            }
            if (app->fullscreen && app->hasPicture && GetTickCount() - app->lastMouseMove > kCursorHideMs) {
                SetCursorHidden(*app, true);
            }
        } else if (wParam == kExitTimer) {
            PostMessageW(window, WM_CLOSE, 0, 0);
        }
        return 0;

    case WM_APP_CLIENT_REQUEST: {
        auto* info = reinterpret_cast<ClientInfo*>(lParam);
        app->client = *info;
        delete info;
        UpdateTitle(*app);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    }
    case WM_APP_CONNECTIONS:
        app->connections = static_cast<int>(wParam);
        if (app->connections == 0) {
            app->client = ClientInfo{};
            app->mirrorRunning = false;
        }
        UpdateTitle(*app);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_APP_SERVER_RESET:
        app->server.RestartListener();
        return 0;
    case WM_APP_MIRROR_RUNNING:
        app->mirrorRunning = wParam != 0;
        // Keep the display awake while a device is mirroring.
        SetThreadExecutionState(app->mirrorRunning ? (ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED)
                                                   : ES_CONTINUOUS);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_APP_VIDEO_PICTURE:
        app->hasPicture = wParam != 0;
        app->pictureWidth = LOWORD(lParam);
        app->pictureHeight = HIWORD(lParam);
        ShowWindow(app->videoWindow, app->hasPicture ? SW_SHOWNA : SW_HIDE);
        if (app->hasPicture) {
            app->video.RequestRedraw();
        } else {
            SetCursorHidden(*app, false);
            InvalidateRect(window, nullptr, FALSE);
        }
        UpdateTitle(*app);
        return 0;

    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        KillTimer(window, kTickTimer);
        // Stop everything while the windows still exist; the swap chain needs its window.
        Shutdown(*app);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void ParseCommandLine(App& app) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) {
        return;
    }
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--debug") {
            app.debugLog = true;
        } else if (arg == L"--selftest-audio") {
            app.selfTestAudio = true;
        } else if (arg == L"--selftest-tone") {
            app.selfTestTone = true;
        } else if (arg == L"--selftest-video" && i + 1 < argc) {
            app.selfTestVideo = argv[++i];
        } else if (arg == L"--exit-after" && i + 1 < argc) {
            app.exitAfterSeconds = _wtoi(argv[++i]);
        } else if (arg == L"--decoder" && i + 1 < argc) {
            // One-run override for testing; not saved.
            const std::wstring mode = argv[++i];
            app.decoderOverride = mode == L"software" ? 2 : mode == L"hardware" ? 1 : 0;
        }
    }
    LocalFree(argv);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    App app;
    g_app = &app;
    app.instance = instance;

    applog::Init(AppDataDir() + L"\\debug.txt");
    LOGI("--- AirWindows " APP_VERSION_STRING " starting ---");
    ParseCommandLine(app);

    // Use OpenSSL's built-in defaults: a system-wide openssl.cnf on the user's
    // PC must not change how pairing and decryption behave here.
    OPENSSL_init_crypto(OPENSSL_INIT_NO_LOAD_CONFIG, nullptr);

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        LOGE("CoInitializeEx failed hr=0x%08lX", hr);
    }
    WSADATA wsa{};
    const int wsaResult = WSAStartup(MAKEWORD(2, 2), &wsa);
    if (wsaResult != 0) {
        LOGE("WSAStartup failed (%d)", wsaResult);
        MessageBoxW(nullptr, L"Windows Sockets could not be initialized.", kAppTitle, MB_OK | MB_ICONERROR);
        return 1;
    }

    app.settings.Load();
    if (app.decoderOverride >= 0) {
        app.settings.decoderMode = static_cast<DecoderMode>(app.decoderOverride);
    }
    if (app.settings.macAddress.empty()) {
        // The device id must stay stable between runs, so it is stored once chosen.
        app.settings.macAddress = AirPlayServer::FindMacAddress();
        if (app.settings.macAddress.empty()) {
            app.settings.macAddress = AirPlayServer::RandomMacAddress();
        }
        app.settings.Save();
    }
    LOGI("[CFG] identity: serverName='%s' deviceId=%s", WideToUtf8(app.settings.serverName).c_str(),
         app.settings.macAddress.c_str());

    WNDCLASSEXW mainClass{sizeof(WNDCLASSEXW)};
    mainClass.style = CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW;
    mainClass.lpfnWndProc = MainWindowProc;
    mainClass.hInstance = instance;
    mainClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    mainClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP));
    mainClass.lpszClassName = kMainClass;
    RegisterClassExW(&mainClass);

    WNDCLASSEXW videoClass{sizeof(WNDCLASSEXW)};
    videoClass.style = CS_DBLCLKS;
    videoClass.lpfnWndProc = VideoWindowProc;
    videoClass.hInstance = instance;
    videoClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    videoClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    videoClass.lpszClassName = kVideoClass;
    RegisterClassExW(&videoClass);

    app.menu = BuildMenu();
    app.window = CreateWindowExW(0, kMainClass, kAppTitle, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT,
                                 CW_USEDEFAULT, 1120, 720, nullptr, app.menu, instance, nullptr);
    if (!app.window) {
        LOGE("CreateWindowEx failed (error %lu)", GetLastError());
        return 1;
    }
    RECT client{};
    GetClientRect(app.window, &client);
    app.videoWindow = CreateWindowExW(0, kVideoClass, L"", WS_CHILD, 0, 0, client.right, client.bottom, app.window,
                                      nullptr, instance, nullptr);

    if (!app.video.Start(app.videoWindow, app.window, app.settings.decoderMode, app.settings.vsync)) {
        MessageBoxW(app.window, L"Direct3D 11 could not be initialized, so video cannot be shown.", kAppTitle,
                    MB_OK | MB_ICONERROR);
    }
    if (!app.audio.Start()) {
        LOGE("audio pipeline failed to start");
    }
    StartServer(app);
    UpdateTitle(app);

    if (app.settings.alwaysOnTop) {
        SetWindowPos(app.window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    ShowWindow(app.window, showCommand);
    UpdateWindow(app.window);
    SetTimer(app.window, kTickTimer, 1000, nullptr);
    if (app.exitAfterSeconds > 0) {
        SetTimer(app.window, kExitTimer, static_cast<UINT>(app.exitAfterSeconds) * 1000, nullptr);
    }
    if (app.selfTestAudio || app.selfTestTone || !app.selfTestVideo.empty()) {
        app.selfTestThread = std::thread([&app] { RunSelfTest(app); });
    }

    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    // Normally already done in WM_DESTROY; repeated here in case the window was never created.
    Shutdown(app);
    WSACleanup();
    CoUninitialize();
    LOGI("--- AirWindows exiting ---");
    applog::Shutdown();
    g_app = nullptr;
    return static_cast<int>(message.wParam);
}
