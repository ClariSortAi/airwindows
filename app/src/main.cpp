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
#include "native_control.h"

namespace {

constexpr wchar_t kMainClass[] = L"AirWindowsMainWindow";
constexpr wchar_t kVideoClass[] = L"AirWindowsVideoWindow";
constexpr wchar_t kAppTitle[] = L"AirWindows";
constexpr UINT_PTR kTickTimer = 1;
constexpr UINT_PTR kExitTimer = 2;
constexpr UINT_PTR kControlTimer = 3;
constexpr int kPairControl=40001, kPeerControl=40002, kBindControl=40003, kDisconnectControl=40004, kHomeControl=40005;
constexpr COLORREF kBackground = RGB(0x2C, 0x2C, 0x2C);  // idle screen
constexpr DWORD kCursorHideMs = 3000;

// Routes protocol callbacks to the two pipelines.
class MediaRouter final : public IMediaSink {
public:
    MediaRouter(VideoPipeline& video, AudioPipeline& audio, NativeControl& control) : video_(video), audio_(audio), control_(control) {}

    void AwaitPicture() { control_.AwaitPicture(video_.Frames().Stamp().generation); }
    void OnClientRequest(const std::string& identity) override { control_.Sender(identity);video_.Frames().Invalidate();AwaitPicture(); }
    bool OnVideoCodec(bool h265) override { control_.Invalidate();const bool result=video_.SetCodec(h265);AwaitPicture();return result; }
    void OnVideoData(const uint8_t* data, int size, uint64_t ptsNs) override { video_.Push(data, size, ptsNs); }
    void OnVideoSize(int sourceWidth, int sourceHeight, int width, int height) override {
        const bool widthChanged=geometryWidth_.exchange(width)!=width;
        const bool heightChanged=geometryHeight_.exchange(height)!=height;
        if(widthChanged || heightChanged) control_.Invalidate();
        video_.Frames().SetGeometry(width, height);
        if(widthChanged || heightChanged)AwaitPicture();
        LOGI("video stream %dx%d (source %dx%d)", width, height, sourceWidth, sourceHeight);
    }
    void OnVideoPause() override { control_.Pause(true); video_.Frames().SetPaused(true); AwaitPicture(); LOGI("video paused by sender"); }
    void OnVideoResume() override { video_.Frames().SetPaused(false); AwaitPicture(); control_.Pause(false); LOGI("video resumed by sender"); }
    void OnVideoFlush() override { control_.Invalidate(); video_.Frames().Invalidate(); AwaitPicture(); }
    void OnVideoReset() override { control_.Picture(false,0,0); video_.Reset(); }
    void OnMirrorState(bool running) override { control_.Mirror(running); video_.Frames().SetActive(running); AwaitPicture(); }

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
    NativeControl& control_;
    std::atomic<int> geometryWidth_{0},geometryHeight_{0};
};

struct App {
    HINSTANCE instance = nullptr;
    HWND window = nullptr;
    HWND videoWindow = nullptr;
    HMENU menu = nullptr;

    Settings settings;
    VideoPipeline video;
    AudioPipeline audio;
    NativeControl control;
    MediaRouter router{video, audio, control};
    AirPlayServer server;

    bool serverOk = false;
    bool hevcHardware = false;
    bool debugLog = false;
    bool exportFrames = false;
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
    HWND controlPair=nullptr,controlPeer=nullptr,controlBind=nullptr,controlDisconnect=nullptr,controlStatus=nullptr,controlHome=nullptr,controlHelp=nullptr;
    std::vector<ControlPeer> controlPeers;
    std::wstring lastControlStatus;
    std::string controlPhoneOverride;
    bool controlDragging=false;
    WheelSteps wheel;

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
    app.control.Invalidate();
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
        if (app.exportFrames) app.video.Frames().Start(!app.selfTestVideo.empty());
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

// Fit the picture and toolbar within this monitor; retain side bars if the
// toolbar's minimum width is wider than a portrait image at maximum height.
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

    RECT frame{0, 0, 0, 0};
    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(app.window, GWL_STYLE));
    const DWORD exStyle = static_cast<DWORD>(GetWindowLongPtrW(app.window, GWL_EXSTYLE));
    AdjustWindowRectExForDpi(&frame, style, TRUE, exStyle, GetDpiForWindow(app.window));
    const int chromeWidth = frame.right - frame.left;
    const int chromeHeight = frame.bottom - frame.top;
    const auto fitted=FitMirrorClient(client.right,client.bottom,app.pictureWidth,app.pictureHeight,
        workWidth-chromeWidth,workHeight-chromeHeight,std::max(Scale(app.window,500),Scale(app.window,650)-chromeWidth),Scale(app.window,72));
    if(!fitted)return;
    const int width=fitted->first,height=fitted->second;
    RECT current{};GetWindowRect(app.window,&current);
    const int left=std::clamp(current.left,monitor.rcWork.left,monitor.rcWork.right-width-chromeWidth);
    const int top=std::clamp(current.top,monitor.rcWork.top,monitor.rcWork.bottom-height-chromeHeight);
    LOGI("[FIT] resize -> %dx%d client for %dx%d content", width, height, app.pictureWidth, app.pictureHeight);
    SetWindowPos(app.window, nullptr, left, top, width + chromeWidth, height + chromeHeight,
                 SWP_NOZORDER | SWP_NOACTIVATE);
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
    app.control.Shutdown();
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
    if(id==kHomeControl){
        app.controlDragging=false;if(GetCapture()==app.videoWindow)ReleaseCapture();
        app.control.Invalidate(); // the explicit global action replaces older queued input
        SetFocus(app.videoWindow);app.control.Focus(true);
        if(app.control.State().focused)app.control.Submit(ControlKind::Home);
        else app.control.RejectInput("Home is available while the selected iPhone is visibly mirroring.");
        return;
    }
    if (id==kPairControl) { app.control.Pair(); return; }
    if (id==kDisconnectControl) { app.control.Disconnect(); return; }
    if (id==kBindControl) {
        const auto selected=SendMessageW(app.controlPeer,CB_GETCURSEL,0,0);
        if(selected>=0 && size_t(selected)<app.controlPeers.size()) app.control.Bind(app.controlPeers[size_t(selected)].token);
        return;
    }
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

void UpdateControlUi(App& app) {
    const auto state=app.control.State();
    if(!state.focused)app.wheel.Reset();
    app.video.Frames().SetControlState(state.ready,state.busy,state.focused,state.bound,state.sender);
    if(!state.focused && app.controlDragging){app.controlDragging=false;if(GetCapture()==app.videoWindow)ReleaseCapture();}
    if(state.ready && (state.bound!=app.settings.controlPhone || state.sender!=app.settings.controlSender)){
        app.settings.controlPhone=state.bound;app.settings.controlSender=state.sender;app.settings.Save();
    }
    std::wstring text=state.focused ? L"Controlling iPhone - click elsewhere to return" : Utf8ToWide(state.message);
    if(text!=app.lastControlStatus){SetWindowTextW(app.controlStatus,text.c_str());app.lastControlStatus=text;}
    bool changed=state.peers.size()!=app.controlPeers.size();
    if(!changed)for(size_t i=0;i<state.peers.size();++i)if(state.peers[i].token!=app.controlPeers[i].token || state.peers[i].name!=app.controlPeers[i].name)changed=true;
    if(changed){
        const auto oldSelection=SendMessageW(app.controlPeer,CB_GETCURSEL,0,0);
        const std::string selectedToken=oldSelection>=0 && size_t(oldSelection)<app.controlPeers.size()
            ? app.controlPeers[size_t(oldSelection)].token : state.bound;
        app.controlPeers=state.peers;SendMessageW(app.controlPeer,CB_RESETCONTENT,0,0);
        for(const auto& peer:app.controlPeers){const auto label=Utf8ToWide(peer.name+" ["+peer.token+"]");SendMessageW(app.controlPeer,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.c_str()));}
        if(app.controlPeers.size()==1)SendMessageW(app.controlPeer,CB_SETCURSEL,0,0);
        else for(size_t i=0;i<app.controlPeers.size();++i)if(app.controlPeers[i].token==selectedToken)SendMessageW(app.controlPeer,CB_SETCURSEL,i,0);
    }
    EnableWindow(app.controlBind,!app.controlPeers.empty() && !state.ready);
    EnableWindow(app.controlPair,!state.ready);
    EnableWindow(app.controlDisconnect,state.published || state.ready);
    EnableWindow(app.controlHome,state.ready && app.hasPicture && app.mirrorRunning);
}

void LayoutControls(App& app,int width,int height){
    const int margin=Scale(app.window,8), row=Scale(app.window,26), top=Scale(app.window,72);
    const int pair=Scale(app.window,106), bind=Scale(app.window,86), disconnect=Scale(app.window,94),home=Scale(app.window,66);
    const int peer=std::max(Scale(app.window,100),width-pair-bind-disconnect-home-margin*6);
    if(app.controlPair){
        MoveWindow(app.controlPair,margin,margin,pair,row,TRUE);
        MoveWindow(app.controlPeer,margin*2+pair,margin,peer,row*8,TRUE);
        MoveWindow(app.controlBind,margin*3+pair+peer,margin,bind,row,TRUE);
        MoveWindow(app.controlDisconnect,margin*4+pair+peer+bind,margin,disconnect,row,TRUE);
        MoveWindow(app.controlHome,margin*5+pair+peer+bind+disconnect,margin,home,row,TRUE);
        MoveWindow(app.controlStatus,margin,margin*2+row,std::max(1,width-margin*2),row,TRUE);
    }
    if(app.videoWindow){MoveWindow(app.videoWindow,0,top,width,std::max(1,height-top),FALSE);app.video.NotifyResize();}
}

void PasteToPhone(HWND window,App& app){
    if(GetFocus()!=window || !app.control.State().focused)return;
    if(!OpenClipboard(window)){app.control.RejectInput("Paste unavailable: clipboard is busy. Click the mirror to retry.");return;}
    std::wstring text;
    bool valid=false;
    if(IsClipboardFormatAvailable(CF_UNICODETEXT)){
        const auto data=GetClipboardData(CF_UNICODETEXT);
        const auto bytes=data?GlobalSize(data):0;
        const auto chars=static_cast<const wchar_t*>(data?GlobalLock(data):nullptr);
        if(chars){
            const size_t limit=std::min<size_t>(bytes/sizeof(wchar_t),2049);
            size_t length=0;while(length<limit && chars[length])++length;
            if(length<limit){text.assign(chars,length);valid=true;}
            GlobalUnlock(data);
        }
    }
    CloseClipboard();
    const auto ascii=valid?PrintablePaste(text):std::optional<std::string>{};
    if(!ascii){app.control.RejectInput("Paste rejected: use up to 1,024 US ASCII characters, tabs or newlines.");return;}
    app.control.SubmitText(*ascii);
}

LRESULT CALLBACK VideoWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if(g_app){
        if(message==WM_SETFOCUS)g_app->control.Focus(true);
        if(message==WM_KILLFOCUS || message==WM_CANCELMODE || (message==WM_CAPTURECHANGED && g_app->controlDragging)){
            g_app->wheel.Reset();
            g_app->controlDragging=false;if(GetCapture()==window)ReleaseCapture();g_app->control.Focus(false);
        }
        if(message==WM_RBUTTONDOWN || message==WM_RBUTTONDBLCLK || message==WM_RBUTTONUP){
            RECT bounds{};GetClientRect(window,&bounds);
            const auto point=MirrorPoint(GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam),bounds.right,bounds.bottom,g_app->pictureWidth,g_app->pictureHeight);
            if(point){
                if(message==WM_RBUTTONUP){
                    g_app->wheel.Reset();
                    g_app->controlDragging=false;if(GetCapture()==window)ReleaseCapture();
                    g_app->control.Invalidate(); // secondary/Home replaces earlier pending input
                    SetFocus(window);g_app->control.Focus(true);
                    g_app->control.Submit(ControlKind::Secondary,point->first,point->second);
                }
                return 0; // one pulse on release; never invoke the PC context menu here
            }
            if(message==WM_RBUTTONUP){
                SetFocus(GetParent(window));g_app->control.Focus(false);
                POINT screen{GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)};ClientToScreen(window,&screen);
                return SendMessageW(GetParent(window),WM_CONTEXTMENU,reinterpret_cast<WPARAM>(window),MAKELPARAM(screen.x,screen.y));
            }
            return 0;
        }
        if(message==WM_CONTEXTMENU){
            POINT point{GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)};
            if(point.x!=-1 || point.y!=-1){
                ScreenToClient(window,&point);RECT bounds{};GetClientRect(window,&bounds);
                if(MirrorPoint(point.x,point.y,bounds.right,bounds.bottom,g_app->pictureWidth,g_app->pictureHeight))return 0;
            }
            return SendMessageW(GetParent(window),message,wParam,lParam);
        }
        if(message==WM_LBUTTONDOWN || message==WM_LBUTTONDBLCLK || message==WM_MOUSEMOVE || message==WM_LBUTTONUP){
            const bool down=message==WM_LBUTTONDOWN || message==WM_LBUTTONDBLCLK;
            if(GetFocus()==window || down){
                RECT bounds{};GetClientRect(window,&bounds);
                const auto point=MirrorPoint(GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam),bounds.right,bounds.bottom,g_app->pictureWidth,g_app->pictureHeight);
                if(down){
                    g_app->wheel.Reset();
                    if(point){SetFocus(window);g_app->control.Focus(true);
                        if(g_app->control.State().focused){g_app->controlDragging=true;SetCapture(window);g_app->control.Submit(ControlKind::Down,point->first,point->second);}}
                    else {SetFocus(GetParent(window));g_app->control.Focus(false);}
                } else if(message==WM_LBUTTONUP){
                    if(g_app->controlDragging)g_app->control.Submit(ControlKind::Up,point?point->first:-1,point?point->second:-1);
                    g_app->controlDragging=false;if(GetCapture()==window)ReleaseCapture();
                } else if(point)g_app->control.Submit(ControlKind::Point,point->first,point->second);
            }
            if(message!=WM_MOUSEMOVE)return 0;
        }
        if(message==WM_MOUSEWHEEL && GetFocus()==window){
            POINT mouse{GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)};ScreenToClient(window,&mouse);
            RECT bounds{};GetClientRect(window,&bounds);
            auto point=MirrorPoint(mouse.x,mouse.y,bounds.right,bounds.bottom,g_app->pictureWidth,g_app->pictureHeight);
            if(point && g_app->control.State().focused){
                const int steps=g_app->wheel.Add(GET_WHEEL_DELTA_WPARAM(wParam),g_app->video.Frames().Stamp().generation,GetTickCount64());
                for(int i=0;i<std::abs(steps);++i)g_app->control.Submit(steps>0?ControlKind::ScrollUp:ControlKind::ScrollDown,point->first,point->second);
            }else g_app->wheel.Reset();
            return 0;
        }
        if(message==WM_CHAR){
            g_app->wheel.Reset();
            if(wParam>=32 && wParam<=126)g_app->control.Submit(ControlKind::Text,0,0,static_cast<unsigned char>(wParam));
            else if(wParam>=128)g_app->control.RejectInput("Text rejected: this keyboard currently supports US ASCII only.");
            return 0;
        }
        if(message==WM_PASTE){PasteToPhone(window,*g_app);return 0;}
        if(message==WM_KEYDOWN){
            if(wParam=='V' && (GetKeyState(VK_CONTROL)&0x8000)){
                if(!(lParam&(1LL<<30)))PasteToPhone(window,*g_app);
                return 0;
            }
            if(wParam==VK_F11)return SendMessageW(GetParent(window),message,wParam,lParam);
            unsigned char usage=0;
            switch(wParam){case VK_RETURN:usage=40;break;case VK_BACK:usage=42;break;case VK_TAB:usage=43;break;
                case VK_RIGHT:usage=79;break;case VK_LEFT:usage=80;break;case VK_DOWN:usage=81;break;case VK_UP:usage=82;break;}
            if(usage){g_app->control.Submit(ControlKind::Key,0,0,usage);return 0;}
        }
        if(message==WM_SYSKEYDOWN && wParam==VK_RETURN)return SendMessageW(GetParent(window),message,wParam,lParam);
    }
    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        ValidateRect(window, nullptr);
        if (g_app) {
            g_app->video.RequestRedraw();
        }
        return 0;
    // Keep parent cursor visibility bookkeeping while the child handles phone input.
    case WM_MOUSEMOVE:
        return SendMessageW(GetParent(window), message, wParam, lParam);
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
        app->controlDragging=false;if(GetCapture()==app->videoWindow)ReleaseCapture();
        app->control.Invalidate();
        if (app->videoWindow && wParam != SIZE_MINIMIZED) {
            LayoutControls(*app,LOWORD(lParam),HIWORD(lParam));
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize.x = Scale(window, 650);
        info->ptMinTrackSize.y = Scale(window, 300);
        MONITORINFO monitor{sizeof(MONITORINFO)};
        if(GetMonitorInfoW(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST),&monitor)){
            info->ptMinTrackSize.x=std::min(info->ptMinTrackSize.x,monitor.rcWork.right-monitor.rcWork.left);
            info->ptMinTrackSize.y=std::min(info->ptMinTrackSize.y,monitor.rcWork.bottom-monitor.rcWork.top);
        }
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
    case WM_ACTIVATEAPP:
        if(!wParam){app->controlDragging=false;if(GetCapture()==app->videoWindow)ReleaseCapture();app->control.Focus(false);}
        return 0;
    case WM_ENTERMENULOOP:
        app->controlDragging=false;if(GetCapture()==app->videoWindow)ReleaseCapture();
        app->control.Focus(false);
        return 0;
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
        if(wParam==kControlTimer){UpdateControlUi(*app);return 0;}
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
            app->control.Mirror(false);
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
    case WM_APP_CONTROL_FRAME:
        if(static_cast<uint64_t>(wParam)==app->video.Frames().Stamp().generation){
            app->pictureWidth=LOWORD(lParam);app->pictureHeight=HIWORD(lParam);
            app->control.Picture(true,app->pictureWidth,app->pictureHeight,static_cast<uint64_t>(wParam));
        }
        return 0;
    case WM_APP_VIDEO_PICTURE:
        app->hasPicture = wParam != 0;
        app->pictureWidth = LOWORD(lParam);
        app->pictureHeight = HIWORD(lParam);
        if(!app->hasPicture)app->control.Picture(false,0,0);
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
        KillTimer(window,kControlTimer);
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
        } else if (arg == L"--frames") {
            app.exportFrames = true;
        } else if (arg == L"--control-phone" && i + 1 < argc) {
            const auto token=WideToUtf8(argv[++i]);
            if(token.size()==16 && token.find_first_not_of("0123456789abcdef")==std::string::npos)app.controlPhoneOverride=token;
            else LOGW("[CONTROL] --control-phone requires 16 lowercase hexadecimal characters");
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
    if(!app.controlPhoneOverride.empty()){app.settings.controlPhone=app.controlPhoneOverride;app.settings.controlSender.clear();app.settings.Save();}
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
    const auto makeControl=[&](const wchar_t* kind,const wchar_t* text,DWORD style,int id){
        auto handle=CreateWindowExW(0,kind,text,WS_CHILD|WS_VISIBLE|style,0,0,1,1,app.window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),instance,nullptr);
        SendMessageW(handle,WM_SETFONT,reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),TRUE);return handle;
    };
    app.controlPair=makeControl(L"BUTTON",L"Pair Bluetooth",BS_PUSHBUTTON|WS_TABSTOP,kPairControl);
    app.controlPeer=makeControl(L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_VSCROLL|WS_TABSTOP,kPeerControl);
    app.controlBind=makeControl(L"BUTTON",L"Connect",BS_PUSHBUTTON|WS_TABSTOP,kBindControl);
    app.controlDisconnect=makeControl(L"BUTTON",L"Disconnect",BS_PUSHBUTTON|WS_TABSTOP,kDisconnectControl);
    app.controlHome=makeControl(L"BUTTON",L"Home",BS_PUSHBUTTON|WS_TABSTOP,kHomeControl);
    app.controlHelp=CreateWindowExW(WS_EX_TOPMOST,TOOLTIPS_CLASSW,nullptr,WS_POPUP|TTS_ALWAYSTIP,
        CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,app.window,nullptr,instance,nullptr);
    if(app.controlHelp){
        TOOLINFOW tip{sizeof(TOOLINFOW)};tip.uFlags=TTF_IDISHWND|TTF_SUBCLASS;tip.hwnd=app.window;
        tip.uId=reinterpret_cast<UINT_PTR>(app.controlHome);
        tip.lpszText=const_cast<wchar_t*>(L"One-time setup on iPhone: Settings > Accessibility > Touch > AssistiveTouch > Devices > this PC's Bluetooth name. Assign Button 2 to Home. This button sends that configured action.");
        SendMessageW(app.controlHelp,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&tip));
        SendMessageW(app.controlHelp,TTM_SETMAXTIPWIDTH,0,Scale(app.window,420));
        SendMessageW(app.controlHelp,TTM_SETDELAYTIME,TTDT_AUTOPOP,15000);
    }
    app.controlStatus=makeControl(L"STATIC",L"Bluetooth control disconnected",SS_LEFT,0);
    LayoutControls(app,client.right,client.bottom);
    app.control.Start(app.settings.controlPhone,app.settings.controlSender,!app.controlPhoneOverride.empty());

    if (!app.video.Start(app.videoWindow, app.window, app.settings.decoderMode, app.settings.vsync)) {
        MessageBoxW(app.window, L"Direct3D 11 could not be initialized, so video cannot be shown.", kAppTitle,
                    MB_OK | MB_ICONERROR);
    }
    if (!app.audio.Start()) {
        LOGE("audio pipeline failed to start");
    }
    if (app.exportFrames && !app.video.Frames().Start(!app.selfTestVideo.empty())) {
        LOGE("[FRAMES] could not start local frame interface");
    }
    if (app.selfTestVideo.empty() && !app.selfTestAudio && !app.selfTestTone) StartServer(app);
    UpdateTitle(app);

    if (app.settings.alwaysOnTop) {
        SetWindowPos(app.window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    ShowWindow(app.window, showCommand);
    UpdateWindow(app.window);
    SetTimer(app.window, kTickTimer, 1000, nullptr);
    SetTimer(app.window,kControlTimer,100,nullptr);
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
