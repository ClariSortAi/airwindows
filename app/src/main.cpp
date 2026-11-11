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

#include <algorithm>
#include <atomic>
#include <cmath>
#include <string>
#include <thread>
#include <vector>

#include <openssl/crypto.h>

#include "airplay_server.h"
#include "audio_pipeline.h"
#include "capture.h"
#include "crash.h"
#include "guide.h"
#include "log.h"
#include "media_sink.h"
#include "recorder.h"
#include "resource.h"
#include "settings.h"
#include "ui.h"
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
constexpr UINT_PTR kDeviceLossTimer = 4;  // --selftest-device-loss
constexpr UINT_PTR kCrashTimer = 5;       // --selftest-crash
// One receiver per desktop session: two would advertise the same AirPlay identity
// and publish the same Bluetooth service. The window property marks the receiver
// that holds the mutex, so a second launch can find it among self-test windows.
constexpr wchar_t kInstanceMutex[] = L"Local\\AirWindows.Receiver";
constexpr wchar_t kPrimaryProperty[] = L"AirWindows.Primary";
constexpr UINT_PTR kSelfTestCaptureTimer = 6;  // --selftest-screenshot, --selftest-record
// Toolbar IDs share WM_COMMAND with the menu; keep them clear of the IDM_* range in resource.h.
// Pair, Connect and Disconnect have no button of their own any more (the phone
// button's menu sends them) but keep their ids for scripts that post them.
constexpr int kPairControl=41001, kPeerControl=41002, kBindControl=41003, kDisconnectControl=41004, kHomeControl=41005;
constexpr int kShotControl=41006, kRecordControl=41007, kFullControl=41008, kMenuControl=41009;
constexpr int kStatusControl=41010, kGuideOpenControl=41011, kGuideCloseControl=41012;
constexpr int kPeerFirst=41100, kPeerLast=41131;  // phones in the phone button's menu and the setup guide
constexpr int kDumpMenuCommand=41200;             // --selftest-menu: write the menu, as it would open now, to the log
// Heights at 96 DPI: the button row above the picture and the status bar below it.
constexpr int kTopBar = 52;
constexpr int kStatusBar = 30;
constexpr int kMinClientWidth = 560;
constexpr DWORD kCursorHideMs = 3000;
constexpr DWORD kToastMs = 5000;
constexpr UINT WM_APP_RECORDER = WM_APP + 20;  // lParam: RecorderNotice* (receiver deletes)

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
    Recorder recorder;  // declared before the pipelines that feed it, so it outlives them
    VideoPipeline video;
    AudioPipeline audio;
    NativeControl control;
    MediaRouter router{video, audio, control};
    AirPlayServer server;

    bool serverOk = false;
    bool videoOk = false;
    ULONGLONG videoRecoverAt = 0;  // next attempt to rebuild video after the graphics device was lost
    DWORD videoRecoverDelay = 0;
    std::wstring notice;           // shown on the idle screen until the next connection
    std::wstring addresses;        // local addresses on the idle screen, refreshed by the tick timer
    HFONT uiFont = nullptr, uiBold = nullptr, uiTitle = nullptr, iconFont = nullptr;
    HICON idleIcon = nullptr;         // the application icon on the idle screen, at idleIconSize pixels
    int idleIconSize = 0;
    HACCEL accelerators = nullptr;
    Guide guide;
    bool guideOpen = false;
    NativeControlState controlState;  // as of the last control tick
    std::wstring phoneLabel;
    COLORREF phoneDot = CLR_INVALID;
    bool iconOnly = false;            // the window is too narrow for labels on the right-hand buttons
    std::wstring toast;               // short confirmation shown in the status bar
    std::string lastControlMessage;   // Bluetooth status as of the last tick, to notice when it changes
    ULONGLONG toastUntil = 0;
    std::wstring statusDetail;        // right side of the status bar: stream details
    std::wstring statusClock;         // and the recording clock
    std::wstring captureDir;          // --capture-dir
    ULONGLONG recordStarted = 0;
    bool recording = false;
    bool recordLabelStop = false;     // what the Record button currently says
    bool statusReady = false, statusFocused = false;  // state the status bar was last drawn for
    ULONGLONG lastFocusedTick = 0;    // when the phone last had the keyboard
    int selfTestRecordSeconds = 0;
    bool selfTestMenu = false;
    bool selfTestScreenshot = false;
    bool selfTestCaptureArmed = false;
    bool selfTestCaptureUsed = false;  // the capture self-test runs once, not again after a picture returns
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
    // The phone button stands for pairing, choosing a phone and disconnecting.
    HWND controlPeer=nullptr,controlHome=nullptr,controlShot=nullptr,controlRecord=nullptr,controlFull=nullptr,controlMenu=nullptr;
    HWND controlStatus=nullptr,controlGuideOpen=nullptr,controlHelp=nullptr;
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
    bool selfTestAudioSwitch = false;
    int selfTestDeviceLossSeconds = 0;
    std::wstring selfTestCrash;
    int exitAfterSeconds = 0;
    std::thread selfTestThread;
    std::atomic<bool> selfTestStop{false};
    std::atomic<int> selfTestPushed{0};
};

// --decoder applies to this run only; settings keep the saved mode.
DecoderMode Decoder(const App& app) {
    return app.decoderOverride >= 0 ? static_cast<DecoderMode>(app.decoderOverride) : app.settings.decoderMode;
}

// Self-test runs feed the pipelines directly. They do not advertise a receiver,
// take the single-instance slot or change saved settings.
bool SelfTesting(const App& app) {
    return !app.selfTestVideo.empty() || app.selfTestAudio || app.selfTestTone || app.selfTestAudioSwitch ||
           !app.selfTestCrash.empty();
}

// Posted by a second launch to the receiver already running. wParam bit 0: the
// new launch asked for --frames.
UINT SecondLaunchMessage() {
    static const UINT id = RegisterWindowMessageW(L"AirWindows.SecondLaunch");
    return id;
}

App* g_app = nullptr;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

int Scale(HWND window, int value) {
    return MulDiv(value, static_cast<int>(GetDpiForWindow(window)), 96);
}

// Fonts are sized for one DPI, so they are rebuilt when the window moves to a
// display with another scale.
void RebuildFonts(App& app) {
    HFONT old[] = {app.uiFont, app.uiBold, app.uiTitle, app.iconFont};
    app.uiFont = ui::CreateTextFont(app.window, 9, false);
    app.uiBold = ui::CreateTextFont(app.window, 10, true);
    app.uiTitle = ui::CreateTextFont(app.window, 14, true);
    app.iconFont = ui::CreateIconFont(app.window, 11);
    for (HWND control : {app.controlPeer, app.controlHome, app.controlShot, app.controlRecord, app.controlFull,
                         app.controlMenu, app.controlStatus, app.controlGuideOpen}) {
        if (control) {
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(app.uiFont), TRUE);
        }
    }
    if (app.guide.Window()) {
        app.guide.SetFonts(app.uiFont, app.uiBold, app.uiTitle, app.iconFont);
    }
    for (HFONT font : old) {
        if (font) {
            DeleteObject(font);
        }
    }
}

// Shows a short confirmation in the status bar, in place of the Bluetooth
// status, for a few seconds.
void Toast(App& app, const std::wstring& text) {
    app.toast = text;
    app.toastUntil = GetTickCount64() + kToastMs;
    if (app.controlStatus) {
        InvalidateRect(app.controlStatus, nullptr, FALSE);
    }
}

// "Pictures\AirWindows\name.png" rather than the whole path: enough to find it.
std::wstring ShortPath(const std::wstring& path) {
    size_t cut = path.find_last_of(L'\\');
    for (int parts = 0; parts < 2 && cut != std::wstring::npos && cut > 0; ++parts) {
        cut = path.find_last_of(L'\\', cut - 1);
    }
    return cut == std::wstring::npos ? path : path.substr(cut + 1);
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

std::wstring NetworkSummary() {
    std::wstring network;
    for (const auto& address : LocalIPv4Addresses()) {
        network += network.empty() ? address : L", " + address;
    }
    return network;
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
    SetWindowTextW(app.window, title.c_str());
}

// Right side of the status bar: the recording clock, which always shows while
// recording, and what the stream is (when View > Stream details is on and
// there is room).
std::wstring RecordClock(const App& app) {
    if (!app.recording) {
        return {};
    }
    const ULONGLONG seconds = (GetTickCount64() - app.recordStarted) / 1000;
    wchar_t clock[32];
    swprintf_s(clock, L"REC %llu:%02llu", seconds / 60, seconds % 60);
    return clock;
}

// Between the parts of the stream details. DrawStatus drops whole parts at this mark when the
// status bar is too narrow for all of them.
constexpr wchar_t kDetailSeparator[] = L"  ·  ";

std::wstring StatusDetail(App& app) {
    std::wstring detail;
    if (app.settings.showStatus && app.hasPicture) {
        const VideoStats stats = app.video.Stats();
        const std::string audio = app.audio.CodecName();
        wchar_t text[200];
        // The separator before the sound codec is passed as wide text: as a narrow argument its
        // two UTF-8 bytes were printed as two characters, a stray letter and then the dot.
        swprintf_s(text, L"%d × %d%ls%.0f fps%ls%.1f Mbps%ls%ls, %ls%ls%hs", app.pictureWidth, app.pictureHeight,
                   kDetailSeparator, stats.fps, kDetailSeparator, stats.bitrateMbps, kDetailSeparator,
                   stats.h265 ? L"H.265" : L"H.264", stats.hardware ? L"GPU decode" : L"software decode",
                   audio.empty() ? L"" : kDetailSeparator, audio.c_str());
        detail = text;
    }
    return detail;
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
    config.supportsH265 = app.settings.allowH265 && app.hevcHardware && Decoder(app) != DecoderMode::Software;
    config.debugLog = app.debugLog;

    LOGI("[CFG] Start() begin: serverName='%s' quality=%ls fps=%d decoderMode=%d vsync=%d", config.name.c_str(),
         quality.name, config.maxFps, static_cast<int>(Decoder(app)), app.settings.vsync ? 1 : 0);
    app.serverOk = app.server.Start(config, &app.router, app.window);
    LOGI("[CFG] Start() %s", app.serverOk ? "complete" : "FAILED");
    return app.serverOk;
}

// Creates the graphics device and the decode/render thread. False while no
// graphics device can be created, for example while its driver is being replaced.
bool StartVideo(App& app) {
    app.videoOk = app.video.Start(app.videoWindow, app.window, Decoder(app), app.settings.vsync);
    if (app.exportFrames && !app.video.Frames().Start(!app.selfTestVideo.empty())) {
        LOGE("[FRAMES] could not start local frame interface");
    }
    if (!app.selfTestVideo.empty()) {
        // No sender announces a mirror in a self-test, so mark it active here:
        // screenshots and recordings need the newest picture kept.
        app.video.Frames().SetActive(true);
    }
    return app.videoOk;
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
        if (!StartVideo(app)) {
            LOGE("video pipeline failed to restart");
        }
    }
    app.connections = 0;
    app.client = ClientInfo{};
    app.mirrorRunning = false;
    if (!SelfTesting(app)) {
        StartServer(app);
    }
    UpdateTitle(app);
    InvalidateRect(app.window, nullptr, TRUE);
}

// The graphics device went away: a driver update, a GPU reset, an adapter that
// was unplugged. Nothing decoded or drawn on it survives and the sender will not
// resend a key frame on its own, so the session ends and the pipeline is rebuilt
// on a new device. The sender reconnects from Screen Mirroring.
void RecoverVideo(App& app, bool lostNow) {
    if (lostNow) {
        LOGW("[VIDEO] graphics device lost; ending the session and rebuilding the video pipeline");
        app.notice = L"The graphics device was reset, so mirroring stopped. Start Screen Mirroring again on your device.";
        RestartService(app, true);
    } else {
        app.video.Stop();
        // A sender that connected while there was no device has no decoder to feed; start it clean.
        if (StartVideo(app) && app.connections > 0) {
            RestartService(app, false);
        }
    }
    if (app.videoOk) {
        LOGI("[VIDEO] video pipeline rebuilt on a new graphics device");
        app.videoRecoverAt = 0;
        app.videoRecoverDelay = 0;
    } else {
        // The device can take a while to return during a driver install. Back off to 30 s.
        app.videoRecoverDelay = std::clamp<DWORD>(app.videoRecoverDelay * 2, 2000, 30000);
        app.videoRecoverAt = GetTickCount64() + app.videoRecoverDelay;
        LOGW("[VIDEO] no graphics device yet; trying again in %lu ms", app.videoRecoverDelay);
    }
    InvalidateRect(app.window, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
// Window behavior
// ---------------------------------------------------------------------------

void LayoutControls(App& app, int width, int height);

// Full screen is the picture alone: the button row, the status bar and the
// setup guide step aside. Esc, F11 or a right-click in the black bars leave it.
void ToggleFullscreen(App& app) {
    HWND window = app.window;
    if (!app.fullscreen) {
        app.savedStyle = GetWindowLongPtrW(window, GWL_STYLE);
        GetWindowPlacement(window, &app.savedPlacement);
        MONITORINFO monitor{sizeof(MONITORINFO)};
        GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor);
        app.fullscreen = true;
        SetWindowLongPtrW(window, GWL_STYLE, app.savedStyle & ~static_cast<LONG_PTR>(WS_OVERLAPPEDWINDOW));
        SetWindowPos(window, HWND_TOP, monitor.rcMonitor.left, monitor.rcMonitor.top,
                     monitor.rcMonitor.right - monitor.rcMonitor.left, monitor.rcMonitor.bottom - monitor.rcMonitor.top,
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        app.lastMouseMove = GetTickCount();
    } else {
        app.fullscreen = false;
        SetWindowLongPtrW(window, GWL_STYLE, app.savedStyle);
        SetWindowPlacement(window, &app.savedPlacement);
        SetWindowPos(window, app.settings.alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        SetCursorHidden(app, false);
    }
    // The size may not have changed (a maximized window on the same display), so lay out explicitly.
    RECT client{};
    GetClientRect(window, &client);
    LayoutControls(app, client.right, client.bottom);
    InvalidateRect(window, nullptr, FALSE);
}

// Width the window gives up to the setup guide while it is open.
int GuideWidth(const App& app) {
    return app.guideOpen && !app.fullscreen ? Guide::Width(app.window) : 0;
}

// Fit the picture and the bars around it within this monitor; retain side bars
// if the button row's minimum width is wider than a portrait image at maximum height.
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
    AdjustWindowRectExForDpi(&frame, style, FALSE, exStyle, GetDpiForWindow(app.window));
    const int chromeWidth = frame.right - frame.left;
    const int chromeHeight = frame.bottom - frame.top;
    // The guide sits beside the picture, so it is taken out before fitting and added back after.
    const int guide = GuideWidth(app);
    const auto fitted=FitMirrorClient(client.right-guide,client.bottom,app.pictureWidth,app.pictureHeight,
        workWidth-chromeWidth-guide,workHeight-chromeHeight,Scale(app.window,kMinClientWidth),Scale(app.window,kTopBar+kStatusBar));
    if(!fitted)return;
    const int width=fitted->first+guide,height=fitted->second;
    RECT current{};GetWindowRect(app.window,&current);
    const int left=std::clamp(current.left,monitor.rcWork.left,monitor.rcWork.right-width-chromeWidth);
    const int top=std::clamp(current.top,monitor.rcWork.top,monitor.rcWork.bottom-height-chromeHeight);
    LOGI("[FIT] resize -> %dx%d client for %dx%d content", width, height, app.pictureWidth, app.pictureHeight);
    SetWindowPos(app.window, nullptr, left, top, width + chromeWidth, height + chromeHeight,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

// Remembers where the window was, so it reopens on the same display at the same size.
void SavePlacement(App& app) {
    WINDOWPLACEMENT placement{sizeof(WINDOWPLACEMENT)};
    if (app.fullscreen) {
        placement = app.savedPlacement;  // full screen is not restored; keep the window it came from
    } else if (!GetWindowPlacement(app.window, &placement)) {
        return;
    }
    Settings& settings = app.settings;
    settings.windowLeft = placement.rcNormalPosition.left;
    settings.windowTop = placement.rcNormalPosition.top;
    settings.windowRight = placement.rcNormalPosition.right;
    settings.windowBottom = placement.rcNormalPosition.bottom;
    settings.windowMaximized = placement.showCmd == SW_SHOWMAXIMIZED ||
                               (placement.showCmd == SW_SHOWMINIMIZED && (placement.flags & WPF_RESTORETOMAXIMIZED));
    settings.Save();
}

// Applies the saved position unless that spot is no longer on any display, which
// happens when a monitor was unplugged. Returns true when the window should open maximized.
bool RestorePlacement(App& app) {
    const Settings& settings = app.settings;
    RECT saved{settings.windowLeft, settings.windowTop, settings.windowRight, settings.windowBottom};
    if (saved.right - saved.left < 200 || saved.bottom - saved.top < 150 ||
        !MonitorFromRect(&saved, MONITOR_DEFAULTTONULL)) {
        return false;
    }
    WINDOWPLACEMENT placement{sizeof(WINDOWPLACEMENT)};
    placement.showCmd = SW_HIDE;  // position only; wWinMain shows the window
    placement.rcNormalPosition = saved;
    SetWindowPlacement(app.window, &placement);
    return settings.windowMaximized;
}

// The menu is not attached to the window as a bar. The Menu button, a
// right-click outside the picture and the Alt key all show it as a popup.
HMENU BuildMenu() {
    HMENU bar = CreateMenu();

    HMENU receiver = CreatePopupMenu();
    AppendMenuW(receiver, MF_STRING, IDM_RENAME, L"&Rename…");
    AppendMenuW(receiver, MF_STRING, IDM_RESTART, L"Restart &AirPlay service");
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
    AppendMenuW(decoder, MF_STRING, IDM_DECODER_HARDWARE, L"Hardware (GPU)");
    AppendMenuW(decoder, MF_STRING, IDM_DECODER_SOFTWARE, L"Software");
    AppendMenuW(video, MF_POPUP, reinterpret_cast<UINT_PTR>(decoder), L"&Decoder");
    AppendMenuW(video, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(video, MF_STRING, IDM_VSYNC, L"&VSync");
    AppendMenuW(video, MF_STRING, IDM_ALLOW_H265, L"Allow &H.265 when the GPU can decode it");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(video), L"&Video");

    HMENU capture = CreatePopupMenu();
    AppendMenuW(capture, MF_STRING, IDM_SCREENSHOT, L"Take &screenshot\tF8");
    AppendMenuW(capture, MF_STRING, IDM_RECORD, L"Start &recording\tF9");
    AppendMenuW(capture, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(capture, MF_STRING, IDM_OPEN_SCREENSHOTS, L"Open screenshots &folder");
    AppendMenuW(capture, MF_STRING, IDM_OPEN_RECORDINGS, L"Open recordings f&older");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(capture), L"&Capture");

    HMENU view = CreatePopupMenu();
    AppendMenuW(view, MF_STRING, IDM_FULLSCREEN, L"&Full screen\tF11");
    AppendMenuW(view, MF_STRING, IDM_FIT_WINDOW, L"Fit &window to video");
    AppendMenuW(view, MF_STRING, IDM_ALWAYS_ON_TOP, L"Always on &top");
    AppendMenuW(view, MF_STRING, IDM_SHOW_STATUS, L"Stream &details in status bar");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(view), L"Vie&w");

    HMENU help = CreatePopupMenu();
    AppendMenuW(help, MF_STRING, IDM_SETUP_GUIDE, L"Setup &guide");
    AppendMenuW(help, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(help, MF_STRING, IDM_ABOUT, L"&About AirWindows");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(help), L"&Help");
    return bar;
}

// Every check mark, label and grey-out is set from the current state each time
// the menu opens, so the menu cannot disagree with what the receiver is doing.
void SyncMenu(App& app) {
    HMENU menu = app.menu;
    const auto& qualities = VideoQualities();
    for (size_t i = 0; i < qualities.size(); ++i) {
        const bool current = app.settings.videoQuality == qualities[i].name;
        CheckMenuItem(menu, IDM_QUALITY_FIRST + static_cast<UINT>(i), current ? MF_CHECKED : MF_UNCHECKED);
    }
    CheckMenuItem(menu, IDM_FPS_30, app.settings.maxFps == 30 ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem(menu, IDM_FPS_60, app.settings.maxFps == 60 ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem(menu, IDM_DECODER_AUTO, Decoder(app) == DecoderMode::Auto ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem(menu, IDM_DECODER_HARDWARE,
                  Decoder(app) == DecoderMode::Hardware ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem(menu, IDM_DECODER_SOFTWARE,
                  Decoder(app) == DecoderMode::Software ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem(menu, IDM_VSYNC, app.settings.vsync ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem(menu, IDM_ALLOW_H265, app.settings.allowH265 ? MF_CHECKED : MF_UNCHECKED);
    // The GPU decides whether the H.265 setting can do anything.
    EnableMenuItem(menu, IDM_ALLOW_H265, app.hevcHardware ? MF_ENABLED : MF_GRAYED);
    CheckMenuItem(menu, IDM_FULLSCREEN, app.fullscreen ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem(menu, IDM_ALWAYS_ON_TOP, app.settings.alwaysOnTop ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem(menu, IDM_SHOW_STATUS, app.settings.showStatus ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem(menu, IDM_SETUP_GUIDE, app.guideOpen ? MF_CHECKED : MF_UNCHECKED);
    EnableMenuItem(menu, IDM_FIT_WINDOW, app.hasPicture && !app.fullscreen && !IsZoomed(app.window) ? MF_ENABLED : MF_GRAYED);
    EnableMenuItem(menu, IDM_SCREENSHOT, app.hasPicture ? MF_ENABLED : MF_GRAYED);
    EnableMenuItem(menu, IDM_RECORD, app.hasPicture || app.recording ? MF_ENABLED : MF_GRAYED);
    ModifyMenuW(menu, IDM_RECORD, MF_BYCOMMAND | MF_STRING, IDM_RECORD,
                app.recording ? L"Stop &recording\tF9" : L"Start &recording\tF9");
    // ModifyMenu clears the grey state it replaces.
    EnableMenuItem(menu, IDM_RECORD, app.hasPicture || app.recording ? MF_ENABLED : MF_GRAYED);
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

HRESULT CALLBACK OpenLinkCallback(HWND, UINT message, WPARAM, LPARAM lParam, LONG_PTR) {
    if (message == TDN_HYPERLINK_CLICKED) {
        ShellExecuteW(nullptr, L"open", reinterpret_cast<LPCWSTR>(lParam), nullptr, nullptr, SW_SHOWNORMAL);
    }
    return S_OK;
}

void ShowAbout(App& app) {
    static const wchar_t kSummary[] =
        L"Mirrors an iPhone, iPad or Mac on this PC over AirPlay, and controls an iPhone with this PC's mouse and "
        L"keyboard over Bluetooth.\n\n"
        L"<a href=\"https://graniteai.co/tools/airwindows\">graniteai.co/tools/airwindows</a>";
    static const wchar_t kNotices[] =
        L"AirPlay protocol engine: UxPlay (GPL-3.0)\n"
        L"Decoding: FFmpeg (LGPL-2.1-or-later)\n"
        L"Also uses OpenSSL and libplist.\n\n"
        L"This program is free software under the GNU General Public License, version 3 or later, "
        L"and comes with no warranty. Its source code comes with the download.\n\n"
        L"AirPlay, iPhone, iPad and Mac are trademarks of Apple Inc. This program is not affiliated "
        L"with or endorsed by Apple.";
    TASKDIALOGCONFIG config{};
    config.cbSize = sizeof(config);
    config.hwndParent = app.window;
    config.hInstance = app.instance;
    config.dwFlags = TDF_ENABLE_HYPERLINKS | TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
    config.dwCommonButtons = TDCBF_CLOSE_BUTTON;
    config.pszWindowTitle = L"About AirWindows";
    config.pszMainIcon = MAKEINTRESOURCEW(IDI_APP);
    config.pszMainInstruction = L"AirWindows " APP_VERSION_STRING;
    config.pszContent = kSummary;
    config.pszFooter = kNotices;
    config.pfCallback = OpenLinkCallback;
    const HRESULT hr = TaskDialogIndirect(&config, nullptr, nullptr, nullptr);
    if (FAILED(hr)) {
        LOGW("[UI] About dialog could not be shown (0x%08lX); using a message box", hr);
        const std::wstring text = std::wstring(L"AirWindows " APP_VERSION_STRING L"\ngraniteai.co/tools/airwindows\n\n") + kNotices;
        MessageBoxW(app.window, text.c_str(), L"About AirWindows", MB_OK | MB_ICONINFORMATION);
    }
}

// ---------------------------------------------------------------------------
// Idle screen and window background
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

// Paints everything the child windows do not cover: the strips behind the
// button row and the status bar, and the idle screen where the picture goes.
void PaintIdle(App& app, HDC target, const RECT& client) {
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width <= 0 || height <= 0) {
        return;
    }
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bitmap = CreateCompatibleBitmap(target, width, height);
    HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
    HBRUSH background = CreateSolidBrush(ui::kSurface);
    FillRect(dc, &client, background);
    DeleteObject(background);
    RECT idle = client;
    if (!app.fullscreen) {
        HBRUSH chrome = CreateSolidBrush(ui::kChrome);
        HBRUSH divider = CreateSolidBrush(ui::kDivider);
        const RECT top{client.left, client.top, client.right, client.top + Scale(app.window, kTopBar)};
        const RECT bottom{client.left, client.bottom - Scale(app.window, kStatusBar), client.right, client.bottom};
        FillRect(dc, &top, chrome);
        FillRect(dc, &bottom, chrome);
        const RECT underTop{client.left, top.bottom - 1, client.right, top.bottom};
        const RECT overBottom{client.left, bottom.top, client.right, bottom.top + 1};
        FillRect(dc, &underTop, divider);
        FillRect(dc, &overBottom, divider);
        DeleteObject(chrome);
        DeleteObject(divider);
        idle = RECT{client.left, top.bottom, client.right - GuideWidth(app), bottom.top};
    }
    SetBkMode(dc, TRANSPARENT);

    HFONT nameFont = ui::CreateTextFont(app.window, 26, true);
    HFONT statusFont = ui::CreateTextFont(app.window, 13, false);
    HFONT bodyFont = ui::CreateTextFont(app.window, 11, false);
    HFONT smallFont = ui::CreateTextFont(app.window, 9, false);

    const int margin = Scale(app.window, 36);
    RECT area{idle.left + margin, idle.top + std::max<LONG>(Scale(app.window, 28), (idle.bottom - idle.top) / 5),
              idle.right - margin, idle.bottom};
    if (area.right - area.left > Scale(app.window, 120)) {
        // The application icon above the name, when the window is tall enough to spare the room.
        const int iconSize = Scale(app.window, 72);
        if (idle.bottom - idle.top > Scale(app.window, 420)) {
            if (app.idleIconSize != iconSize) {
                if (app.idleIcon) {
                    DestroyIcon(app.idleIcon);
                }
                app.idleIcon = static_cast<HICON>(LoadImageW(app.instance, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, iconSize,
                                                             iconSize, LR_DEFAULTCOLOR));
                app.idleIconSize = iconSize;
            }
            if (app.idleIcon) {
                area.top -= Scale(app.window, 20);
                DrawIconEx(dc, (area.left + area.right - iconSize) / 2, area.top, app.idleIcon, iconSize, iconSize, 0,
                           nullptr, DI_NORMAL);
                area.top += iconSize + Scale(app.window, 14);
            }
        }
        DrawLine(dc, nameFont, ui::kText, app.settings.serverName, area, Scale(app.window, 6));

        std::wstring status;
        COLORREF statusColor = ui::kGood;
        if (!app.serverOk && !SelfTesting(app)) {
            status = L"The AirPlay service could not start. See the log (Menu › Receiver › Open log folder).";
            statusColor = ui::kBad;
        } else if (!app.videoOk) {
            status = L"Video cannot be shown: no graphics device is available. See the log (Menu › Receiver › Open log folder).";
            statusColor = ui::kBad;
        } else if (app.connections > 0) {
            const std::wstring who = app.client.name.empty() ? L"a device" : Utf8ToWide(app.client.name);
            status = app.mirrorRunning ? L"Connected to " + who + L", waiting for video…"
                                       : L"Connected to " + who + L". Ready to play.";
        } else if (app.addresses.empty() && !SelfTesting(app)) {
            status = L"Waiting for a network connection";
            statusColor = ui::kWarn;
        } else {
            status = L"Ready to mirror";
        }
        DrawLine(dc, statusFont, statusColor, status, area, Scale(app.window, app.notice.empty() ? 26 : 10));
        if (!app.notice.empty()) {
            DrawLine(dc, bodyFont, ui::kWarn, app.notice, area, Scale(app.window, 22));
        }

        DrawLine(dc, bodyFont, ui::kTextDim,
                 L"On your iPhone, iPad or Mac, open Control Center, tap Screen Mirroring and choose “" +
                     app.settings.serverName + L"”.",
                 area, Scale(app.window, 22));

        // With no network the receiver keeps running and joins one when it appears.
        const VideoQuality& quality = FindVideoQuality(app.settings.videoQuality);
        const std::wstring details =
            (app.addresses.empty() ? std::wstring(L"No network yet") : app.addresses) + L"  ·  port " +
            std::to_wstring(app.server.Port()) + L"  ·  up to " + std::to_wstring(quality.width) + L" × " +
            std::to_wstring(quality.height) + L" at " + std::to_wstring(app.settings.maxFps) + L" fps";
        DrawLine(dc, smallFont, ui::kTextFaint, details, area, 0);
    }

    BitBlt(target, client.left, client.top, width, height, dc, 0, 0, SRCCOPY);
    DeleteObject(nameFont);
    DeleteObject(statusFont);
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
    if (app.selfTestAudioSwitch) {
        // Muted, so the check makes no sound: it only watches the stream move to the "new" device.
        app.audio.SetGain(0.0);
        app.audio.PushTestTone(0.5, 440.0);
        LOGI("[SELFTEST] simulating a new default playback device");
        app.audio.SimulateDefaultDeviceChange();
        app.audio.PushTestTone(0.8, 440.0);
        Sleep(400);
        LOGI("[SELFTEST] audio after the device change: %.0f ms still queued", app.audio.BufferedMs());
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
    // A recording in progress is finished and saved, not thrown away.
    app.recorder.Stop();
    app.video.SetRecorder(nullptr);
    app.audio.SetRecorder(nullptr);
    app.video.Stop();
    app.audio.Stop();
    SetThreadExecutionState(ES_CONTINUOUS);
}

// ---------------------------------------------------------------------------
// Screenshots and recordings
// ---------------------------------------------------------------------------

// Saves the phone's own picture, at the size the phone sent it, not the window.
void TakeScreenshot(App& app) {
    std::vector<uint8_t> rgb;
    int width = 0, height = 0;
    if (!app.hasPicture || !app.video.Frames().CopyLatest(rgb, width, height)) {
        Toast(app, L"Nothing to capture yet. Start Screen Mirroring first.");
        return;
    }
    const std::wstring folder = capture::PicturesFolder(app.captureDir);
    if (folder.empty()) {
        Toast(app, L"The screenshots folder could not be created. See the log.");
        return;
    }
    const std::wstring path = capture::FreePath(folder, capture::TimestampName(), L".png");
    std::wstring error;
    if (!capture::SavePng(path, rgb, width, height, error)) {
        LOGE("[CAPTURE] screenshot not saved to %ls: %ls", path.c_str(), error.c_str());
        Toast(app, L"The screenshot could not be saved. " + error);
        return;
    }
    // An unattended self-test must not replace what the user has on the clipboard.
    const bool copied = !app.selfTestScreenshot && capture::CopyToClipboard(app.window, rgb, width, height);
    LOGI("[CAPTURE] screenshot %dx%d saved to %ls%s", width, height, path.c_str(),
         copied ? " and copied to the clipboard" : "");
    Toast(app, L"Screenshot saved to " + ShortPath(path) + (copied ? L" and copied to the clipboard" : L""));
}

// Windows N editions ship without Media Foundation until the Media Feature Pack
// is installed. The recorder's libraries are loaded on first use, so check for
// them before that use rather than fail inside it.
bool MediaFoundationPresent() {
    static const bool present = [] {
        HMODULE platform = LoadLibraryExW(L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        HMODULE writer = LoadLibraryExW(L"mfreadwrite.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        const bool both = platform && writer;
        if (platform) FreeLibrary(platform);
        if (writer) FreeLibrary(writer);
        return both;
    }();
    return present;
}

void StartRecording(App& app) {
    if (app.recording || app.recorder.Active()) {
        return;
    }
    if (!app.hasPicture) {
        Toast(app, L"Nothing to record yet. Start Screen Mirroring first.");
        return;
    }
    if (!MediaFoundationPresent()) {
        LOGW("[RECORD] Media Foundation is not installed (Windows N edition without the Media Feature Pack)");
        Toast(app, L"Recording needs the Windows Media Feature Pack, which is not installed on this PC.");
        return;
    }
    const std::wstring folder = capture::VideosFolder(app.captureDir);
    if (folder.empty()) {
        Toast(app, L"The recordings folder could not be created. See the log.");
        return;
    }
    const HWND window = app.window;
    // The file gets the frame rate offered to the phone, so each picture it sends has a frame of its own.
    const int fps = app.settings.maxFps;
    const bool started = app.recorder.Start(folder, capture::TimestampName(), fps, [window](const RecorderNotice& notice) {
        // Runs on the recorder's thread; the window thread owns the interface.
        auto* copy = new RecorderNotice(notice);
        if (!PostMessageW(window, WM_APP_RECORDER, 0, reinterpret_cast<LPARAM>(copy))) {
            delete copy;
        }
    });
    if (!started) {
        LOGE("[RECORD] the recorder refused to start");
        Toast(app, L"Recording could not start. See the log.");
        return;
    }
    app.recording = true;
    app.recordStarted = GetTickCount64();
    // A still screen sends no new frames, so begin with the picture that is showing.
    app.video.RecordCurrentPicture();
    LOGI("[RECORD] started in %ls at a constant %d fps", folder.c_str(), fps);
}

void StopRecording(App& app) {
    if (!app.recording) {
        return;
    }
    app.recording = false;
    app.recorder.Stop();  // writes the end of the file; its name arrives as WM_APP_RECORDER
}

void OnRecorderNotice(App& app, const RecorderNotice& notice) {
    switch (notice.event) {
    case RecorderEvent::Started:
        LOGI("[RECORD] writing %ls", notice.path.c_str());
        break;
    case RecorderEvent::Finished:
        LOGI("[RECORD] saved %ls (%.1f s, %llu frames)%s%ls", notice.path.c_str(), notice.seconds,
             static_cast<unsigned long long>(notice.videoFrames), notice.message.empty() ? "" : ": ",
             notice.message.c_str());
        Toast(app, L"Recording saved to " + ShortPath(notice.path));
        break;
    case RecorderEvent::Failed:
        LOGE("[RECORD] failed: %ls", notice.message.c_str());
        app.recording = false;
        Toast(app, L"Recording stopped. " + notice.message);
        break;
    }
}

// ---------------------------------------------------------------------------
// Toolbar, status bar and setup guide
// ---------------------------------------------------------------------------

// The name a phone reports. The end of its id is added only when two phones
// share a name, which is the one case where it helps to tell them apart.
std::wstring PhoneName(const App& app, const ControlPeer& peer) {
    std::wstring name = Utf8ToWide(peer.name);
    if (name.empty()) {
        name = L"iPhone";
    }
    for (const auto& other : app.controlPeers) {
        if (other.token != peer.token && other.name == peer.name) {
            const size_t tail = peer.token.size() > 4 ? peer.token.size() - 4 : 0;
            return name + L" (" + Utf8ToWide(peer.token.substr(tail)) + L")";
        }
    }
    return name;
}

// True while the phone button has nothing to offer but pairing.
bool PhoneButtonPairs(const App& app) {
    const NativeControlState& state = app.controlState;
    return !state.published && !state.ready && state.bound.empty() && app.controlPeers.empty();
}

ui::ButtonLook LookFor(const App& app, HWND control) {
    ui::ButtonLook look;
    look.kind = ui::ButtonKind::Subtle;
    if (control == app.controlPeer) {
        look.kind = ui::ButtonKind::Plain;
        look.glyph = PhoneButtonPairs(app) ? ui::kGlyphBluetooth : ui::kGlyphPhone;
        look.dot = app.phoneDot;
        look.chevron = !PhoneButtonPairs(app);
        look.alignLeft = true;
    } else if (control == app.controlHome) {
        look.glyph = ui::kGlyphHome;
        look.showLabel = !app.iconOnly;
    } else if (control == app.controlShot) {
        look.glyph = ui::kGlyphCamera;
        look.showLabel = !app.iconOnly;
    } else if (control == app.controlRecord) {
        look.kind = app.recording ? ui::ButtonKind::Record : ui::ButtonKind::Subtle;
        look.glyph = app.recording ? ui::kGlyphStop : ui::kGlyphRecord;
        look.showLabel = !app.iconOnly;
    } else if (control == app.controlFull) {
        look.glyph = app.fullscreen ? ui::kGlyphBackToWindow : ui::kGlyphFullScreen;
        look.showLabel = false;
    } else if (control == app.controlMenu) {
        look.glyph = ui::kGlyphMenu;
        look.showLabel = false;
    } else if (control == app.controlGuideOpen) {
        look.kind = ui::ButtonKind::Plain;
    }
    return look;
}

// The status bar: what Bluetooth control is doing on the left, the recording
// clock and stream details on the right. Its window text is the left part, so
// screen readers and scripts can read it.
void DrawStatus(const App& app, const DRAWITEMSTRUCT& item) {
    const RECT bounds = item.rcItem;
    const int width = bounds.right - bounds.left;
    const int height = bounds.bottom - bounds.top;
    if (width <= 0 || height <= 0) {
        return;
    }
    HDC dc = CreateCompatibleDC(item.hDC);
    HBITMAP bitmap = CreateCompatibleBitmap(item.hDC, width, height);
    HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
    const RECT local{0, 0, width, height};
    HBRUSH chrome = CreateSolidBrush(ui::kChrome);
    FillRect(dc, &local, chrome);
    DeleteObject(chrome);
    const RECT line{0, 0, width, 1};
    HBRUSH divider = CreateSolidBrush(ui::kDivider);
    FillRect(dc, &line, divider);
    DeleteObject(divider);
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ oldFont = SelectObject(dc, app.uiFont);

    const int margin = Scale(app.window, 12);
    int right = width - margin;
    const auto measure = [dc](const std::wstring& text) {
        SIZE size{};
        GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size);
        return static_cast<int>(size.cx);
    };
    // The details give way before the status does: they lose parts from the right until the
    // status sentence fits beside them, and go altogether in a window too narrow for that.
    const int statusLeft = margin + Scale(app.window, 16);
    const int clockWidth = app.statusClock.empty() ? 0 : measure(app.statusClock);
    const int detailRoom = right - Scale(app.window, 14) - (clockWidth ? clockWidth + Scale(app.window, 24) : 0) -
                           statusLeft - measure(app.lastControlStatus);
    std::wstring details = app.statusDetail;
    while (!details.empty() && measure(details) > detailRoom) {
        const size_t cut = details.rfind(kDetailSeparator);
        details = cut == std::wstring::npos ? std::wstring() : details.substr(0, cut);
    }
    if (!details.empty()) {
        RECT detail{right - measure(details), 0, right, height};
        SetTextColor(dc, ui::kTextDim);
        DrawTextW(dc, details.c_str(), -1, &detail, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        right = detail.left - Scale(app.window, 14);
    }
    if (clockWidth) {
        // Never dropped for lack of room: it is the only sign that a recording is running.
        SIZE size{clockWidth, 0};
        RECT clock{right - size.cx, 0, right, height};
        SetTextColor(dc, ui::kText);
        DrawTextW(dc, app.statusClock.c_str(), -1, &clock, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        ui::FillCircle(dc, clock.left - Scale(app.window, 10), height / 2, Scale(app.window, 4), ui::kRecord);
        right = clock.left - Scale(app.window, 24);
    }

    const bool toast = GetTickCount64() < app.toastUntil;
    const NativeControlState& state = app.controlState;
    const COLORREF dot = toast ? ui::kAccent
                         : state.focused ? ui::kAccent
                         : (state.ready && app.hasPicture) ? ui::kGood  // green only while that phone is on screen
                         : (!state.bound.empty() || state.published || !app.controlPeers.empty()) ? ui::kWarn
                         : ui::kTextOff;
    int left = margin;
    ui::FillCircle(dc, left + Scale(app.window, 4), height / 2, Scale(app.window, 4), dot);
    left += Scale(app.window, 16);
    RECT text{left, 0, std::max(left, right), height};
    SetTextColor(dc, toast ? ui::kText : ui::kTextDim);
    DrawTextW(dc, app.lastControlStatus.c_str(), -1, &text,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

    BitBlt(item.hDC, bounds.left, bounds.top, width, height, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldFont);
    SelectObject(dc, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(dc);
}

GuideState MakeGuideState(const App& app) {
    const NativeControlState& state = app.controlState;
    GuideState guide;
    guide.receiverName = app.settings.serverName;
    guide.addresses = app.addresses;
    guide.mirroring = app.hasPicture;
    guide.mirrorDevice = Utf8ToWide(app.client.name);
    guide.pairingOpen = state.published && app.controlPeers.empty();
    for (const auto& peer : app.controlPeers) {
        guide.phones.push_back(PhoneName(app, peer));
    }
    guide.chosen = !state.bound.empty();
    guide.ready = state.ready;
    guide.controlled = app.settings.setupControlled;
    guide.bluetoothStatus = state.ready ? std::wstring() : Utf8ToWide(state.message);
    return guide;
}

void LayoutControls(App& app, int width, int height) {
    if (!app.controlPeer) {
        return;
    }
    const HWND window = app.window;
    const bool chrome = !app.fullscreen;
    const int top = chrome ? Scale(window, kTopBar) : 0;
    const int status = chrome ? Scale(window, kStatusBar) : 0;
    const int guide = GuideWidth(app);
    const int videoWidth = std::max(1, width - guide);
    const int videoHeight = std::max(1, height - top - status);
    for (HWND control : {app.controlPeer, app.controlHome, app.controlShot, app.controlRecord, app.controlFull,
                         app.controlMenu, app.controlStatus}) {
        ShowWindow(control, chrome ? SW_SHOWNA : SW_HIDE);
    }
    if (chrome) {
        const int margin = Scale(window, 10), gap = Scale(window, 6), button = Scale(window, 32), icon = Scale(window, 36);
        const int y = (top - button) / 2;
        // The right-hand buttons carry labels while there is room; a narrow
        // window keeps their icons, and the labels move to the tooltips.
        app.iconOnly = false;
        const int homeWide = ui::ButtonWidth(app.controlHome, LookFor(app, app.controlHome), app.uiFont);
        const int shotWide = ui::ButtonWidth(app.controlShot, LookFor(app, app.controlShot), app.uiFont);
        const int recordWide = std::max(ui::ButtonWidth(app.controlRecord, LookFor(app, app.controlRecord), app.uiFont),
                                        Scale(window, 84));
        const int fixed = margin * 2 + icon * 2 + gap * 5;
        app.iconOnly = width < fixed + homeWide + shotWide + recordWide + Scale(window, 170);
        const int home = app.iconOnly ? icon : homeWide;
        const int shot = app.iconOnly ? icon : shotWide;
        const int record = app.iconOnly ? icon : recordWide;
        int x = width - margin - icon;
        MoveWindow(app.controlMenu, x, y, icon, button, TRUE);
        x -= gap + icon;
        MoveWindow(app.controlFull, x, y, icon, button, TRUE);
        x -= gap + record;
        MoveWindow(app.controlRecord, x, y, record, button, TRUE);
        x -= gap + shot;
        MoveWindow(app.controlShot, x, y, shot, button, TRUE);
        x -= gap + home;
        MoveWindow(app.controlHome, x, y, home, button, TRUE);
        const int room = std::max(Scale(window, 90), x - gap - margin);
        const int wanted = ui::ButtonWidth(app.controlPeer, LookFor(app, app.controlPeer), app.uiFont) + Scale(window, 14);
        MoveWindow(app.controlPeer, margin, y, std::min(room, std::clamp(wanted, Scale(window, 150), Scale(window, 300))),
                   button, TRUE);
        MoveWindow(app.controlStatus, 0, height - status, width, status, TRUE);
    }
    if (app.videoWindow) {
        MoveWindow(app.videoWindow, 0, top, videoWidth, videoHeight, FALSE);
        app.video.NotifyResize();
    }
    if (HWND panel = app.guide.Window()) {
        if (guide) {
            MoveWindow(panel, width - guide, top, guide, videoHeight, TRUE);
        }
        ShowWindow(panel, guide ? SW_SHOWNA : SW_HIDE);
    }
    // On the idle screen, with the guide closed, offer it.
    const bool offerGuide = chrome && !app.hasPicture && !app.guideOpen && videoHeight > Scale(window, 260);
    if (offerGuide) {
        const int wide = ui::ButtonWidth(app.controlGuideOpen, LookFor(app, app.controlGuideOpen), app.uiFont) + Scale(window, 16);
        MoveWindow(app.controlGuideOpen, (videoWidth - wide) / 2, top + videoHeight - Scale(window, 60), wide,
                   Scale(window, 32), TRUE);
    }
    ShowWindow(app.controlGuideOpen, offerGuide ? SW_SHOWNA : SW_HIDE);
}

void RelayoutWindow(App& app) {
    RECT client{};
    GetClientRect(app.window, &client);
    LayoutControls(app, client.right, client.bottom);
    InvalidateRect(app.window, nullptr, FALSE);
}

// Opens or closes the setup guide beside the picture. The window grows to make
// room rather than squeezing the picture, and gives the room back on close.
void SetGuideOpen(App& app, bool open) {
    if (app.guideOpen == open) {
        return;
    }
    app.guideOpen = open;
    if (!open && !app.settings.setupGuideSeen) {
        app.settings.setupGuideSeen = true;
        app.settings.Save();
    }
    if (!app.fullscreen && !IsZoomed(app.window) && !IsIconic(app.window)) {
        RECT bounds{};
        GetWindowRect(app.window, &bounds);
        MONITORINFO monitor{sizeof(MONITORINFO)};
        GetMonitorInfoW(MonitorFromWindow(app.window, MONITOR_DEFAULTTONEAREST), &monitor);
        const int workWidth = monitor.rcWork.right - monitor.rcWork.left;
        const int workHeight = monitor.rcWork.bottom - monitor.rcWork.top;
        const int panel = Guide::Width(app.window);
        int width = bounds.right - bounds.left + (open ? panel : -panel);
        int height = bounds.bottom - bounds.top;
        if (open) {
            height = std::max(height, std::min(workHeight, Scale(app.window, 680)));
        }
        width = std::clamp(width, Scale(app.window, kMinClientWidth), workWidth);
        const int left = std::clamp<int>(bounds.left, monitor.rcWork.left, monitor.rcWork.right - width);
        const int top = std::clamp<int>(bounds.top, monitor.rcWork.top, monitor.rcWork.bottom - height);
        SetWindowPos(app.window, nullptr, left, top, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (open) {
        app.guide.Update(MakeGuideState(app));
    }
    RelayoutWindow(app);
}

// The application menu as a popup. `anchor` is the button it drops from, or
// null to open at `point` (a right-click).
void ShowAppMenu(App& app, HWND anchor, POINT point) {
    // Reuse the menu's submenus so there is one definition of every command.
    HMENU popup = CreatePopupMenu();
    wchar_t label[64];
    const int count = GetMenuItemCount(app.menu);
    for (int i = 0; i < count; ++i) {
        GetMenuStringW(app.menu, static_cast<UINT>(i), label, ARRAYSIZE(label), MF_BYPOSITION);
        AppendMenuW(popup, MF_POPUP, reinterpret_cast<UINT_PTR>(GetSubMenu(app.menu, i)), label);
    }
    SyncMenu(app);
    SetCursorHidden(app, false);
    UINT flags = TPM_RIGHTBUTTON;
    if (anchor) {
        RECT rect{};
        GetWindowRect(anchor, &rect);
        point = POINT{rect.right, rect.bottom + Scale(app.window, 2)};
        flags |= TPM_RIGHTALIGN | TPM_TOPALIGN;
    }
    TrackPopupMenu(popup, flags, point.x, point.y, 0, app.window, nullptr);
    // Detach the shared submenus before destroying the wrapper.
    for (int i = count - 1; i >= 0; --i) {
        RemoveMenu(popup, static_cast<UINT>(i), MF_BYPOSITION);
    }
    DestroyMenu(popup);
}

// Writes one line per menu item, as the menu would show it now.
void LogMenu(HMENU menu, const std::wstring& path) {
    const int count = GetMenuItemCount(menu);
    for (int i = 0; i < count; ++i) {
        wchar_t label[128] = L"";
        MENUITEMINFOW info{sizeof(MENUITEMINFOW)};
        info.fMask = MIIM_STRING | MIIM_STATE | MIIM_SUBMENU | MIIM_FTYPE | MIIM_ID;
        info.dwTypeData = label;
        info.cch = ARRAYSIZE(label);
        if (!GetMenuItemInfoW(menu, static_cast<UINT>(i), TRUE, &info) || (info.fType & MFT_SEPARATOR)) {
            continue;
        }
        std::wstring name = label;
        name.erase(std::remove(name.begin(), name.end(), L'&'), name.end());
        std::replace(name.begin(), name.end(), L'\t', L' ');
        const std::wstring full = path.empty() ? name : path + L" > " + name;
        if (info.hSubMenu) {
            LogMenu(info.hSubMenu, full);
        } else {
            LOGI("[SELFTEST] menu: %ls | id=%u%s%s", full.c_str(), info.wID, (info.fState & MFS_CHECKED) ? " | checked" : "",
                 (info.fState & MFS_GRAYED) ? " | grey" : "");
        }
    }
}

// Phones to choose from, then pairing and disconnecting.
void ShowPhoneMenu(App& app) {
    const NativeControlState& state = app.controlState;
    HMENU menu = CreatePopupMenu();
    for (size_t i = 0; i < app.controlPeers.size() && kPeerFirst + static_cast<int>(i) <= kPeerLast; ++i) {
        const UINT checked = app.controlPeers[i].token == state.bound ? MF_CHECKED : MF_UNCHECKED;
        // A phone chosen for another mirror stays selectable: an iPhone's AirPlay identity
        // can change, and the user may be correcting an earlier choice.
        const std::wstring name = PhoneName(app, app.controlPeers[i]) +
                                  (app.controlPeers[i].elsewhere ? L"  (a different iPhone)" : L"");
        AppendMenuW(menu, MF_STRING | checked, static_cast<UINT_PTR>(kPeerFirst) + i, name.c_str());
    }
    if (app.controlPeers.empty()) {
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0,
                    state.published ? L"Waiting for an iPhone to pair…" : L"No iPhone is paired");
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (state.ready ? MF_GRAYED : MF_ENABLED), kPairControl, L"Pair Bluetooth");
    AppendMenuW(menu, MF_STRING | (state.published || state.ready ? MF_ENABLED : MF_GRAYED), kDisconnectControl,
                L"Disconnect");
    RECT rect{};
    GetWindowRect(app.controlPeer, &rect);
    TrackPopupMenu(menu, TPM_LEFTALIGN | TPM_TOPALIGN, rect.left, rect.bottom + Scale(app.window, 2), 0, app.window, nullptr);
    DestroyMenu(menu);
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
    if (id==kPeerControl) {
        // With nothing paired and nothing pairing there is one thing to do, so do it.
        if (PhoneButtonPairs(app)) app.control.Pair();
        else ShowPhoneMenu(app);
        return;
    }
    if (id>=static_cast<UINT>(kPeerFirst) && id<=static_cast<UINT>(kPeerLast)) {
        const size_t index=id-kPeerFirst;
        if(index<app.controlPeers.size()) app.control.Bind(app.controlPeers[index].token);
        return;
    }
    if (id==kPairControl) { app.control.Pair(); return; }
    if (id==kDisconnectControl) { app.control.Disconnect(); return; }
    if (id==kBindControl) {
        // Kept for scripts that post it: connect the only phone, or reconnect the chosen one.
        if(app.controlPeers.size()==1) app.control.Bind(app.controlPeers[0].token);
        else for(const auto& peer:app.controlPeers) if(peer.token==app.controlState.bound) app.control.Bind(peer.token);
        return;
    }
    // A click on one of these buttons moved the keyboard off the phone. If the
    // user was controlling it a moment ago, hand it back when the command is done.
    const bool resume = app.hasPicture && GetTickCount64() - app.lastFocusedTick < 1500;
    const auto resumeControl = [&] { if (resume && !app.fullscreen) SetFocus(app.videoWindow); };
    if (id==kShotControl || id==IDM_SCREENSHOT) { TakeScreenshot(app); resumeControl(); return; }
    if (id==kRecordControl || id==IDM_RECORD) {
        if (app.recording) StopRecording(app); else StartRecording(app);
        resumeControl();
        return;
    }
    if (id==kFullControl) { ToggleFullscreen(app); if (resume) SetFocus(app.videoWindow); return; }
    if (id==kMenuControl) { ShowAppMenu(app, app.controlMenu, POINT{}); return; }
    if (id==kGuideOpenControl) { SetGuideOpen(app, true); return; }
    if (id==kGuideCloseControl) { SetGuideOpen(app, false); return; }
    if (id==kDumpMenuCommand) {
        if (app.selfTestMenu) { SyncMenu(app); LogMenu(app.menu, L""); }
        return;
    }
    Settings& settings = app.settings;
    // These settings take effect on a fresh service, which ends a session in progress.
    const auto restart = [&](bool video, const wchar_t* what = L"The setting was applied") {
        if (app.connections > 0) {
            app.notice = std::wstring(what) + L", which stopped mirroring. Start Screen Mirroring again on your device.";
        }
        RestartService(app, video);
    };
    const auto& qualities = VideoQualities();
    if (id >= IDM_QUALITY_FIRST && id < IDM_QUALITY_FIRST + qualities.size()) {
        settings.videoQuality = qualities[id - IDM_QUALITY_FIRST].name;
        LOGI("[CFG] SetVideoQualityName('%ls')", settings.videoQuality.c_str());
        settings.Save();
        restart(false);
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
            restart(false, L"The receiver was renamed");
        }
        break;
    }
    case IDM_RESTART:
        restart(false, L"The AirPlay service was restarted");
        break;
    case IDM_OPEN_LOG_FOLDER:
        ShellExecuteW(app.window, L"open", AppDataDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        break;
    case IDM_OPEN_SCREENSHOTS:
    case IDM_OPEN_RECORDINGS: {
        const std::wstring folder = id == IDM_OPEN_SCREENSHOTS ? capture::PicturesFolder(app.captureDir)
                                                               : capture::VideosFolder(app.captureDir);
        if (folder.empty()) {
            Toast(app, L"That folder could not be created. See the log.");
        } else {
            ShellExecuteW(app.window, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        break;
    }
    case IDM_EXIT:
        PostMessageW(app.window, WM_CLOSE, 0, 0);
        break;
    case IDM_FPS_30:
    case IDM_FPS_60:
        settings.maxFps = id == IDM_FPS_60 ? 60 : 30;
        settings.Save();
        restart(false);
        break;
    case IDM_DECODER_AUTO:
    case IDM_DECODER_HARDWARE:
    case IDM_DECODER_SOFTWARE:
        settings.decoderMode = id == IDM_DECODER_AUTO ? DecoderMode::Auto
                               : id == IDM_DECODER_HARDWARE ? DecoderMode::Hardware : DecoderMode::Software;
        settings.Save();
        app.decoderOverride = -1;
        restart(true);
        break;
    case IDM_VSYNC:
        settings.vsync = !settings.vsync;
        settings.Save();
        app.video.SetVSync(settings.vsync);
        break;
    case IDM_ALLOW_H265:
        settings.allowH265 = !settings.allowH265;
        settings.Save();
        restart(false);
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
        break;
    case IDM_SETUP_GUIDE:
        SetGuideOpen(app, !app.guideOpen);
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
    // Every phone chosen so far, so a mirror from any of them finds its phone again.
    if(state.pairs!=app.settings.controlPairs){app.settings.controlPairs=state.pairs;app.settings.Save();}
    if(state.focused){
        app.lastFocusedTick=GetTickCount64();
        if(!app.settings.setupControlled){app.settings.setupControlled=true;app.settings.Save();}
    }
    bool peersChanged=state.peers.size()!=app.controlPeers.size();
    if(!peersChanged)for(size_t i=0;i<state.peers.size();++i)if(state.peers[i].token!=app.controlPeers[i].token || state.peers[i].name!=app.controlPeers[i].name ||
        state.peers[i].elsewhere!=app.controlPeers[i].elsewhere)peersChanged=true;
    if(peersChanged)app.controlPeers=state.peers;
    app.controlState=state;

    // Status bar: a confirmation for a few seconds, otherwise what Bluetooth control is doing.
    // News from Bluetooth is newer than the confirmation, so it takes the line back at once.
    if(state.message!=app.lastControlMessage){
        app.lastControlMessage=state.message;
        app.toastUntil=0;
    }
    const bool toast=GetTickCount64()<app.toastUntil;
    const std::wstring text=toast ? app.toast : state.focused ? L"Controlling iPhone - click elsewhere to return" : Utf8ToWide(state.message);
    const std::wstring detail=StatusDetail(app);
    const std::wstring clock=RecordClock(app);
    const bool usable=state.ready && app.hasPicture;  // decides the dot's colour, so a change repaints
    if(text!=app.lastControlStatus || detail!=app.statusDetail || clock!=app.statusClock || usable!=app.statusReady || state.focused!=app.statusFocused){
        app.lastControlStatus=text;app.statusDetail=detail;app.statusClock=clock;app.statusReady=usable;app.statusFocused=state.focused;
        SetWindowTextW(app.controlStatus,text.c_str());
        InvalidateRect(app.controlStatus,nullptr,FALSE);
    }

    // Phone button: one control for "pair", "which phone" and "is it connected".
    std::wstring label;
    COLORREF dot=CLR_INVALID;
    const auto boundPeer=std::find_if(app.controlPeers.begin(),app.controlPeers.end(),[&](const ControlPeer& peer){return peer.token==state.bound;});
    if(state.ready || !state.bound.empty()){
        label=boundPeer!=app.controlPeers.end() ? PhoneName(app,*boundPeer) : L"iPhone";
        // Green means usable now: connected, and that phone is the one on screen.
        dot=state.ready && app.hasPicture ? ui::kGood : ui::kWarn;
    }else if(state.waiting){
        // The phone for this mirror is known; the phones listed meanwhile are other phones.
        label=L"Waiting for iPhone…";dot=ui::kAccent;
    }else if(!app.controlPeers.empty()){
        // Every listed phone belongs to another mirror: the one on screen still has to pair.
        const bool candidate=std::any_of(app.controlPeers.begin(),app.controlPeers.end(),[](const ControlPeer& peer){return !peer.elsewhere;});
        label=candidate ? L"Choose phone" : L"Pair this iPhone";dot=ui::kWarn;
    }else if(state.published){
        label=L"Waiting for iPhone…";dot=ui::kAccent;
    }else{
        label=L"Pair Bluetooth";
    }
    if(label!=app.phoneLabel || dot!=app.phoneDot){
        app.phoneLabel=label;app.phoneDot=dot;
        SetWindowTextW(app.controlPeer,label.c_str());
        RelayoutWindow(app);
        InvalidateRect(app.controlPeer,nullptr,FALSE);  // the dot can change while the name stays the same
    }

    const bool wasRecordLabel=app.recordLabelStop;
    app.recordLabelStop=app.recording;
    if(wasRecordLabel!=app.recordLabelStop){
        SetWindowTextW(app.controlRecord,app.recording ? L"Stop" : L"Record");
        InvalidateRect(app.controlRecord,nullptr,FALSE);
    }
    EnableWindow(app.controlHome,state.ready && app.hasPicture && app.mirrorRunning);
    EnableWindow(app.controlShot,app.hasPicture);
    EnableWindow(app.controlRecord,app.hasPicture || app.recording);
    if(app.guideOpen)app.guide.Update(MakeGuideState(app));
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
                // The bars next to the picture act as the phone's bezel: a press there snaps to
                // the edge, and a drag stays pinned to the edge however far it goes.
                const int bezel=g_app->controlDragging?bounds.right+bounds.bottom:Scale(window,16);
                const auto point=MirrorPoint(GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam),bounds.right,bounds.bottom,g_app->pictureWidth,g_app->pictureHeight,bezel);
                if(down){
                    g_app->wheel.Reset();
                    if(point){SetFocus(window);g_app->control.Focus(true);
                        if(g_app->control.State().focused){g_app->controlDragging=true;SetCapture(window);g_app->control.Submit(ControlKind::Down,point->first,point->second);}}
                    // Far out in a bar: not a target. Control stays on; clicking another app returns to Windows.
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
            if(wParam==VK_F11 || (wParam==VK_ESCAPE && g_app->fullscreen))return SendMessageW(GetParent(window),message,wParam,lParam);
            unsigned char usage=0;
            switch(wParam){case VK_RETURN:usage=40;break;case VK_BACK:usage=42;break;case VK_TAB:usage=43;break;
                case VK_RIGHT:usage=79;break;case VK_LEFT:usage=80;break;case VK_DOWN:usage=81;break;case VK_UP:usage=82;break;}
            if(usage){
                // Auto-repeat outruns BLE (~30 keys/s vs ~10/s) and would expire the queue;
                // skip repeats while earlier input is still being sent.
                if(!((lParam&(1LL<<30)) && g_app->control.State().busy))g_app->control.Submit(ControlKind::Key,0,0,usage);
                return 0;
            }
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
    if (message == SecondLaunchMessage()) {
        // AirWindows was started again. This receiver stays the only one; the new
        // process has already brought this window forward and exited.
        const bool wantsFrames = (wParam & 1) != 0;
        LOGI("second launch%s handed over to this receiver", wantsFrames ? " (with --frames)" : "");
        if (wantsFrames && !app->exportFrames) {
            // Started without the launcher, then started again with it: give desktop
            // agents the frame access the launcher asks for instead of a dead end.
            app->exportFrames = app->video.Frames().Start(false);
            if (app->exportFrames) {
                // Start() marks a new interface idle; restore the running mirror's state.
                app->video.Frames().SetActive(app->mirrorRunning);
            } else {
                LOGE("[FRAMES] could not start local frame interface");
            }
        }
        return 0;
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
        // Room for the button row, plus the setup guide while it is open.
        info->ptMinTrackSize.x = Scale(window, kMinClientWidth + 16) + GuideWidth(*app);
        info->ptMinTrackSize.y = Scale(window, 380);
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
        // Fonts are sized for one DPI; rebuild them and lay everything out again.
        RebuildFonts(*app);
        RelayoutWindow(*app);
        return 0;
    }
    case WM_DRAWITEM: {
        const auto& item = *reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
        if (item.hwndItem == app->controlStatus) {
            DrawStatus(*app, item);
        } else {
            ui::DrawButton(item, LookFor(*app, item.hwndItem), app->uiFont, app->iconFont);
        }
        return TRUE;
    }
    case WM_SYSCOMMAND:
        // Alt or F10 on its own: there is no menu bar to activate, so open the menu.
        if ((wParam & 0xFFF0) == SC_KEYMENU && lParam == 0 && !app->fullscreen) {
            ShowAppMenu(*app, app->controlMenu, POINT{});
            return 0;
        }
        break;
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
        ShowAppMenu(*app, nullptr, point);
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
            if (app->videoRecoverAt && GetTickCount64() >= app->videoRecoverAt) {
                RecoverVideo(*app, false);
            }
            // Discovery follows network changes by itself; keep the idle screen and the log in step.
            std::wstring addresses = NetworkSummary();
            if (addresses != app->addresses) {
                LOGI("[NET] local addresses now: %ls", addresses.empty() ? L"none" : addresses.c_str());
                app->addresses = std::move(addresses);
                InvalidateRect(window, nullptr, FALSE);
            }
            if (app->fullscreen && app->hasPicture && GetTickCount() - app->lastMouseMove > kCursorHideMs) {
                SetCursorHidden(*app, true);
            }
        } else if (wParam == kExitTimer) {
            PostMessageW(window, WM_CLOSE, 0, 0);
        // The two self-test timers act only when their option was given: a timer
        // message can be posted by any program running as this user.
        } else if (wParam == kDeviceLossTimer && app->selfTestDeviceLossSeconds > 0) {
            KillTimer(window, kDeviceLossTimer);
            LOGI("[SELFTEST] simulating a lost graphics device");
            app->video.SimulateDeviceLoss();
        } else if (wParam == kCrashTimer && !app->selfTestCrash.empty()) {
            KillTimer(window, kCrashTimer);
            if (app->selfTestCrash == L"terminate") {
                crash::TestTerminate();
            }
            crash::TestAccessViolation();
        } else if (wParam == kSelfTestCaptureTimer && app->selfTestCaptureArmed) {
            // First firing: capture. Second firing: stop the recording it started.
            KillTimer(window, kSelfTestCaptureTimer);
            if (app->recording) {
                app->selfTestCaptureArmed = false;
                LOGI("[SELFTEST] stopping the recording");
                StopRecording(*app);
            } else {
                if (app->selfTestScreenshot) {
                    LOGI("[SELFTEST] taking a screenshot");
                    TakeScreenshot(*app);
                }
                if (app->selfTestRecordSeconds > 0) {
                    LOGI("[SELFTEST] recording for %d seconds", app->selfTestRecordSeconds);
                    StartRecording(*app);
                    SetTimer(window, kSelfTestCaptureTimer, static_cast<UINT>(app->selfTestRecordSeconds) * 1000, nullptr);
                } else {
                    app->selfTestCaptureArmed = false;
                }
            }
        }
        return 0;

    case WM_APP_CLIENT_REQUEST: {
        auto* info = reinterpret_cast<ClientInfo*>(lParam);
        app->client = *info;
        delete info;
        app->notice.clear();
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
            if ((app->selfTestScreenshot || app->selfTestRecordSeconds > 0) && !app->selfTestCaptureUsed) {
                app->selfTestCaptureUsed = true;
                app->selfTestCaptureArmed = true;
                SetTimer(window, kSelfTestCaptureTimer, 700, nullptr);
            }
        } else {
            SetCursorHidden(*app, false);
            // The picture is gone, so there is nothing more to record; keep what was captured.
            StopRecording(*app);
        }
        // The idle screen's guide button comes and goes with the picture.
        RelayoutWindow(*app);
        UpdateTitle(*app);
        return 0;

    case WM_APP_RECORDER: {
        auto* notice = reinterpret_cast<RecorderNotice*>(lParam);
        OnRecorderNotice(*app, *notice);
        delete notice;
        return 0;
    }

    case WM_APP_VIDEO_DEVICE_LOST:
        RecoverVideo(*app, true);
        return 0;

    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        KillTimer(window, kTickTimer);
        KillTimer(window,kControlTimer);
        if (!SelfTesting(*app)) {
            SavePlacement(*app);
        }
        RemovePropW(window, kPrimaryProperty);
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
        } else if (arg == L"--selftest-audio-switch") {
            // Silent: behave as if the default playback device changed and watch the stream reopen.
            app.selfTestAudioSwitch = true;
        } else if (arg == L"--selftest-video" && i + 1 < argc) {
            app.selfTestVideo = argv[++i];
        } else if (arg == L"--selftest-device-loss" && i + 1 < argc) {
            // With --selftest-video: report the graphics device lost after this many seconds.
            app.selfTestDeviceLossSeconds = _wtoi(argv[++i]);
        } else if (arg == L"--selftest-crash" && i + 1 < argc) {
            // Fail on purpose to exercise the crash report: "access" or "terminate".
            app.selfTestCrash = argv[++i];
        } else if (arg == L"--selftest-menu") {
            // Lets a test ask for the menu's current labels, check marks and grey-outs in the log.
            app.selfTestMenu = true;
        } else if (arg == L"--selftest-screenshot") {
            // With --selftest-video: save one screenshot once the picture is up.
            app.selfTestScreenshot = true;
        } else if (arg == L"--selftest-record" && i + 1 < argc) {
            // With --selftest-video: record this many seconds once the picture is up.
            app.selfTestRecordSeconds = _wtoi(argv[++i]);
        } else if (arg == L"--capture-dir" && i + 1 < argc) {
            // Screenshots and recordings go here instead of Pictures and Videos.
            app.captureDir = argv[++i];
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

BOOL CALLBACK FindPrimaryWindow(HWND window, LPARAM result) {
    wchar_t name[64];
    if (GetClassNameW(window, name, ARRAYSIZE(name)) && wcscmp(name, kMainClass) == 0 &&
        GetPropW(window, kPrimaryProperty)) {
        *reinterpret_cast<HWND*>(result) = window;
        return FALSE;
    }
    return TRUE;
}

// A receiver is already running: bring its window forward instead of starting a
// second one beside it.
void HandOverToRunningReceiver(bool wantsFrames) {
    HWND primary = nullptr;
    // The first receiver may still be creating its window.
    for (int attempt = 0; attempt < 50 && !primary; ++attempt) {
        EnumWindows(FindPrimaryWindow, reinterpret_cast<LPARAM>(&primary));
        if (!primary) {
            Sleep(100);
        }
    }
    if (!primary) {
        LOGW("another receiver holds the single-instance slot but its window was not found");
        return;
    }
    if (IsIconic(primary)) {
        ShowWindowAsync(primary, SW_RESTORE);
    }
    SetForegroundWindow(primary);
    PostMessageW(primary, SecondLaunchMessage(), wantsFrames ? 1 : 0, 0);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    App app;
    g_app = &app;
    app.instance = instance;

    applog::Init(AppDataDir() + L"\\debug.txt");
    LOGI("--- AirWindows " APP_VERSION_STRING " starting ---");
    ParseCommandLine(app);
    // A run with --exit-after is unattended and must end on its own, so it saves
    // the report without showing the dialog.
    crash::Install(app.exitAfterSeconds == 0);

    HANDLE instanceMutex = nullptr;
    if (!SelfTesting(app)) {
        instanceMutex = CreateMutexW(nullptr, FALSE, kInstanceMutex);
        if (instanceMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
            LOGI("AirWindows is already running; bringing that receiver forward and exiting");
            HandOverToRunningReceiver(app.exportFrames);
            CloseHandle(instanceMutex);
            applog::Shutdown();
            g_app = nullptr;
            return 0;
        }
        if (!instanceMutex) {
            LOGW("could not create the single-instance mutex (error %lu)", GetLastError());
        }
    }

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

    ui::Startup();

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

    // The menu is shown as a popup (Menu button, right-click, Alt), not as a bar.
    app.menu = BuildMenu();
    app.window = CreateWindowExW(0, kMainClass, kAppTitle, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT,
                                 CW_USEDEFAULT, 1120, 760, nullptr, nullptr, instance, nullptr);
    if (!app.window) {
        LOGE("CreateWindowEx failed (error %lu)", GetLastError());
        return 1;
    }
    ui::StyleWindow(app.window);
    if (instanceMutex) {
        SetPropW(app.window, kPrimaryProperty, reinterpret_cast<HANDLE>(static_cast<INT_PTR>(1)));
    }
    const bool openMaximized = RestorePlacement(app);
    RECT client{};
    GetClientRect(app.window, &client);
    app.videoWindow = CreateWindowExW(0, kVideoClass, L"", WS_CHILD, 0, 0, client.right, client.bottom, app.window,
                                      nullptr, instance, nullptr);
    // Real button windows, drawn by ui::DrawButton: their text names them for
    // screen readers and automation even where only an icon is shown.
    const auto makeButton=[&](const wchar_t* text,int id){
        HWND handle=CreateWindowExW(0,L"BUTTON",text,WS_CHILD|WS_VISIBLE|BS_OWNERDRAW,0,0,1,1,app.window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),instance,nullptr);
        ui::TrackHover(handle,ui::kChrome);
        return handle;
    };
    app.controlPeer=makeButton(L"Pair Bluetooth",kPeerControl);
    app.controlHome=makeButton(L"Home",kHomeControl);
    app.controlShot=makeButton(L"Screenshot",kShotControl);
    app.controlRecord=makeButton(L"Record",kRecordControl);
    app.controlFull=makeButton(L"Full screen",kFullControl);
    app.controlMenu=makeButton(L"Menu",kMenuControl);
    app.controlGuideOpen=CreateWindowExW(0,L"BUTTON",L"Open the setup guide",WS_CHILD|BS_OWNERDRAW,0,0,1,1,app.window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kGuideOpenControl)),instance,nullptr);
    ui::TrackHover(app.controlGuideOpen,ui::kSurface);
    app.controlStatus=CreateWindowExW(0,L"STATIC",L"Bluetooth control disconnected",WS_CHILD|WS_VISIBLE|SS_OWNERDRAW,0,0,1,1,
        app.window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(kStatusControl)),instance,nullptr);
    app.lastControlStatus=L"Bluetooth control disconnected";
    app.phoneLabel=L"Pair Bluetooth";
    Guide::Commands guideCommands;
    guideCommands.pair=kPairControl;
    guideCommands.firstPhone=kPeerFirst;
    guideCommands.close=kGuideCloseControl;
    if(!app.guide.Create(app.window,instance,guideCommands)){
        LOGW("[UI] the setup guide could not be created (error %lu)",GetLastError());
    }
    app.controlHelp=CreateWindowExW(WS_EX_TOPMOST,TOOLTIPS_CLASSW,nullptr,WS_POPUP|TTS_ALWAYSTIP|TTS_NOPREFIX,
        CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,app.window,nullptr,instance,nullptr);
    if(app.controlHelp){
        const auto tip=[&](HWND control,const wchar_t* text){
            TOOLINFOW info{sizeof(TOOLINFOW)};info.uFlags=TTF_IDISHWND|TTF_SUBCLASS;info.hwnd=app.window;
            info.uId=reinterpret_cast<UINT_PTR>(control);info.lpszText=const_cast<wchar_t*>(text);
            SendMessageW(app.controlHelp,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&info));
        };
        tip(app.controlPeer,L"Bluetooth mouse and keyboard for an iPhone: pair, choose the phone, or disconnect.");
        tip(app.controlHome,L"Home. One-time setup on the iPhone: Settings > Accessibility > Touch > AssistiveTouch > Devices > this PC > Customize Additional Buttons, press this button, then assign Button 2 to Home. Right-clicking the picture does the same.");
        tip(app.controlShot,L"Screenshot (F8). Saves the phone's picture to Pictures\\AirWindows and copies it.");
        tip(app.controlRecord,L"Record (F9). Saves video and the phone's sound to Videos\\AirWindows. Press again to stop.");
        tip(app.controlFull,L"Full screen (F11). Esc returns.");
        tip(app.controlMenu,L"Menu (Alt). Also opens with a right-click outside the picture.");
        SendMessageW(app.controlHelp,TTM_SETMAXTIPWIDTH,0,Scale(app.window,380));
        SendMessageW(app.controlHelp,TTM_SETDELAYTIME,TTDT_AUTOPOP,15000);
    }
    ACCEL keys[]={{FVIRTKEY,VK_F8,IDM_SCREENSHOT},{FVIRTKEY,VK_F9,IDM_RECORD},{FVIRTKEY,VK_F11,IDM_FULLSCREEN}};
    app.accelerators=CreateAcceleratorTableW(keys,ARRAYSIZE(keys));
    app.video.SetRecorder(&app.recorder);
    app.audio.SetRecorder(&app.recorder);
    RebuildFonts(app);
    // A new user sees the guide first. Someone who already paired a phone has done all of this.
    if(!app.settings.setupGuideSeen && app.settings.controlPhone.empty() && !SelfTesting(app)){
        SetGuideOpen(app,true);
    }
    GetClientRect(app.window,&client);
    LayoutControls(app,client.right,client.bottom);
    app.control.Start(app.settings.controlPairs,app.settings.controlPhone,app.settings.controlSender,!app.controlPhoneOverride.empty());

    if (!StartVideo(app)) {
        MessageBoxW(app.window, L"Direct3D 11 could not be initialized, so video cannot be shown.", kAppTitle,
                    MB_OK | MB_ICONERROR);
    }
    if (!app.audio.Start()) {
        LOGE("audio pipeline failed to start");
    }
    // Known before the menu is first opened, including in a self-test that never starts the service.
    app.hevcHardware = VideoPipeline::HardwareHevcAvailable();
    app.addresses = NetworkSummary();
    LOGI("[NET] local addresses: %ls", app.addresses.empty() ? L"none" : app.addresses.c_str());
    if (!SelfTesting(app)) StartServer(app);
    UpdateTitle(app);

    if (app.settings.alwaysOnTop) {
        SetWindowPos(app.window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    // A shortcut set to open minimized or maximized still wins over the saved state.
    const bool defaultShow = showCommand == SW_SHOWNORMAL || showCommand == SW_SHOWDEFAULT;
    ShowWindow(app.window, openMaximized && defaultShow ? SW_SHOWMAXIMIZED : showCommand);
    UpdateWindow(app.window);
    SetTimer(app.window, kTickTimer, 1000, nullptr);
    SetTimer(app.window,kControlTimer,100,nullptr);
    if (app.exitAfterSeconds > 0) {
        SetTimer(app.window, kExitTimer, static_cast<UINT>(app.exitAfterSeconds) * 1000, nullptr);
    }
    if (app.selfTestDeviceLossSeconds > 0) {
        SetTimer(app.window, kDeviceLossTimer, static_cast<UINT>(app.selfTestDeviceLossSeconds) * 1000, nullptr);
    }
    if (!app.selfTestCrash.empty()) {
        SetTimer(app.window, kCrashTimer, 1000, nullptr);
    }
    if (app.selfTestAudio || app.selfTestTone || app.selfTestAudioSwitch || !app.selfTestVideo.empty()) {
        app.selfTestThread = std::thread([&app] { RunSelfTest(app); });
    }

    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        // F8, F9 and F11 work wherever the keyboard is, including on the phone picture.
        if (app.accelerators && TranslateAcceleratorW(app.window, app.accelerators, &message)) {
            continue;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    // Normally already done in WM_DESTROY; repeated here in case the window was never created.
    Shutdown(app);
    for (HFONT font : {app.uiFont, app.uiBold, app.uiTitle, app.iconFont}) {
        if (font) {
            DeleteObject(font);
        }
    }
    if (app.idleIcon) {
        DestroyIcon(app.idleIcon);
    }
    if (app.accelerators) {
        DestroyAcceleratorTable(app.accelerators);
    }
    if (app.menu) {
        DestroyMenu(app.menu);  // never attached to the window, so it is not destroyed with it
    }
    ui::Shutdown();
    if (instanceMutex) {
        CloseHandle(instanceMutex);
    }
    WSACleanup();
    CoUninitialize();
    LOGI("--- AirWindows exiting ---");
    applog::Shutdown();
    g_app = nullptr;
    return static_cast<int>(message.wParam);
}
