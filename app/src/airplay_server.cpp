#include "airplay_server.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <BaseTsd.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

// UxPlay's raop_ntp.h uses ssize_t, which MSVC does not define.
typedef SSIZE_T ssize_t;

#include "dnssd.h"
#include "logger.h"
#include "raop.h"

#include "log.h"

namespace {

// Seconds without a client feedback request before the connection counts as lost (UxPlay's default).
constexpr unsigned kMissedFeedbackLimit = 15;

// A DNS label holds at most 63 bytes.
constexpr size_t kMaxServiceNameBytes = 63;

raop_t* Raop(void* p) { return static_cast<raop_t*>(p); }
dnssd_t* Dnssd(void* p) { return static_cast<dnssd_t*>(p); }

bool ParseMac(const std::string& text, std::vector<char>& bytes) {
    unsigned v[6];
    if (sscanf_s(text.c_str(), "%2x:%2x:%2x:%2x:%2x:%2x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) {
        return false;
    }
    bytes.clear();
    for (unsigned b : v) {
        bytes.push_back(static_cast<char>(b));
    }
    return true;
}

// Cuts a UTF-8 string to at most `limit` bytes without splitting a character.
std::string TruncateUtf8(const std::string& text, size_t limit) {
    if (text.size() <= limit) {
        return text;
    }
    size_t end = limit;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) {
        --end;
    }
    return text.substr(0, end);
}

// AirPlay feature bits, as UxPlay's own front end sets them (uxplay.cpp start_dnssd),
// with AirPlay video/HLS left off because this receiver only mirrors.
void SetFeatures(dnssd_t* dnssd, bool supportsH265) {
    static const struct { int bit; int value; } kBits[] = {
        {0, 0},   // AirPlay video (HLS)
        {1, 1},   // photo
        {2, 1},   // video protected with FairPlay DRM
        {3, 0},   // volume control for videos
        {4, 0},   // HTTP live streaming
        {5, 1},   // slideshow
        {6, 1},
        {7, 1},   // mirroring
        {8, 0},   // screen rotation
        {9, 1},   // audio
        {10, 1},
        {11, 1},  // audio packet redundancy
        {12, 1},  // FairPlay secure auth
        {13, 1},  // photo preloading
        {14, 1},  // authentication bit 4: FairPlay
        {15, 1},  // metadata: artwork
        {16, 1},  // metadata: progress
        {17, 1},  // metadata: text
        {18, 1},  // audio format 1
        {19, 1},  // audio format 2
        {20, 1},  // audio format 3
        {21, 1},  // audio format 4
        {22, 1},  // authentication type 4: FairPlay
        {23, 0},  // authentication type 1: RSA
        {24, 0},
        {25, 1},
        {26, 0},  // unified advertiser info
        {27, 0},  // legacy pairing
        {28, 1},
        {29, 0},
        {30, 1},  // RAOP support
        {31, 0},
    };
    for (const auto& bit : kBits) {
        dnssd_set_airplay_features(dnssd, bit.bit, bit.value);
    }
    dnssd_set_airplay_features(dnssd, 42, supportsH265 ? 1 : 0);  // screen multi-codec (H.265)
}

}  // namespace

// Static trampolines handed to the C library; `cls` is the AirPlayServer.
struct AirPlayServer::Callbacks {
    static AirPlayServer* Self(void* cls) { return static_cast<AirPlayServer*>(cls); }

    static void Log(void*, int level, const char* msg) {
        switch (level) {
        case LOGGER_EMERG:
        case LOGGER_ALERT:
        case LOGGER_CRIT:
        case LOGGER_ERR:
            LOGE("[airplay] %s", msg);
            break;
        case LOGGER_WARNING:
            LOGW("[airplay] %s", msg);
            break;
        case LOGGER_NOTICE:
        case LOGGER_INFO:
            LOGI("[airplay] %s", msg);
            break;
        default:
            LOGD("[airplay] %s", msg);
            break;
        }
    }

    static void ConnInit(void* cls) {
        auto* self = Self(cls);
        const int count = ++self->openConnections_;
        PostMessageW(self->notify_, WM_APP_CONNECTIONS, static_cast<WPARAM>(count), 0);
    }

    static void ConnDestroy(void* cls) {
        auto* self = Self(cls);
        const int count = --self->openConnections_;
        if (count <= 0) {
            self->remoteClockOffset_ = 0;
            self->sink_->OnAudioStop();
        }
        PostMessageW(self->notify_, WM_APP_CONNECTIONS, static_cast<WPARAM>(count < 0 ? 0 : count), 0);
    }

    static void ConnFeedback(void* cls) { Self(cls)->missedFeedback_ = 0; }

    // The listener cannot be restarted from inside a protocol callback, so hand it to the UI thread.
    static void ConnReset(void* cls, int reason) {
        LOGW("connection reset requested by protocol engine (reason %d)", reason);
        PostMessageW(Self(cls)->notify_, WM_APP_SERVER_RESET, static_cast<WPARAM>(reason), 0);
    }

    static void ReportClientRequest(void* cls, char* deviceId, char* model, char* name, bool* admit) {
        *admit = true;
        LOGI("connection request from %s (%s), device id %s", name ? name : "?", model ? model : "?", deviceId ? deviceId : "?");
        auto* info = new ClientInfo{name ? name : "", model ? model : "", deviceId ? deviceId : ""};
        if (!PostMessageW(Self(cls)->notify_, WM_APP_CLIENT_REQUEST, 0, reinterpret_cast<LPARAM>(info))) {
            delete info;
        }
    }

    // Maps the sender's timestamp onto the local clock the same way UxPlay does:
    // the first packet of a session fixes the offset for both audio and video.
    static uint64_t LocalPts(AirPlayServer* self, uint64_t ntpLocal, uint64_t ntpRemote) {
        uint64_t offset = self->remoteClockOffset_.load();
        if (!offset) {
            offset = (ntpLocal ? ntpLocal : get_local_time()) - ntpRemote;
            self->remoteClockOffset_ = offset;
        }
        return ntpRemote + offset;
    }

    static void AudioProcess(void* cls, raop_ntp_t*, audio_decode_struct* data) {
        auto* self = Self(cls);
        if (!data->data || data->data_len <= 0) {
            return;
        }
        self->sink_->OnAudioData(data->data, data->data_len, data->ct,
                                 LocalPts(self, data->ntp_time_local, data->ntp_time_remote));
    }

    static void VideoProcess(void* cls, raop_ntp_t*, video_decode_struct* data) {
        auto* self = Self(cls);
        if (!data->data || data->data_len <= 0) {
            return;
        }
        // A valid access unit starts with a 00 00 00 01 start code; anything else means decryption failed.
        if (data->data[0] != 0) {
            LOGE("video packet failed decryption, dropped (%d bytes)", data->data_len);
            return;
        }
        self->sink_->OnVideoData(data->data, data->data_len,
                                 LocalPts(self, data->ntp_time_local, data->ntp_time_remote));
    }

    static void VideoPause(void* cls) { Self(cls)->sink_->OnVideoPause(); }
    static void VideoResume(void* cls) { Self(cls)->sink_->OnVideoResume(); }
    static void AudioFlush(void* cls) { Self(cls)->sink_->OnAudioFlush(); }
    static void VideoFlush(void* cls) { Self(cls)->sink_->OnVideoFlush(); }

    static double AudioSetClientVolume(void* cls) { return Self(cls)->clientVolumeDb_.load(); }

    // AirPlay volume is in dB: 0 is full, -30 is the bottom of the slider, -144 is mute.
    static void AudioSetVolume(void* cls, float volume) {
        auto* self = Self(cls);
        double gain = 0.0;
        if (volume <= -30.0f) {
            gain = 0.0;
        } else if (volume >= 0.0f) {
            volume = 0.0f;
            gain = 1.0;
        } else {
            gain = std::pow(10.0, 0.05 * static_cast<double>(volume));
        }
        self->clientVolumeDb_ = static_cast<double>(volume < -30.0f ? -30.0f : volume);
        self->sink_->OnAudioVolume(gain);
    }

    static void AudioGetFormat(void* cls, unsigned char* ct, unsigned short* spf, bool* usingScreen, bool* isMedia,
                               uint64_t* audioFormat) {
        LOGI("audio format: ct=%d spf=%d usingScreen=%d isMedia=%d audioFormat=0x%llx", *ct, *spf, *usingScreen,
             *isMedia, static_cast<unsigned long long>(*audioFormat));
        Self(cls)->sink_->OnAudioFormat(*ct);
    }

    static void VideoReportSize(void* cls, float* widthSource, float* heightSource, float* width, float* height) {
        Self(cls)->sink_->OnVideoSize(static_cast<int>(*widthSource), static_cast<int>(*heightSource),
                                      static_cast<int>(*width), static_cast<int>(*height));
    }

    static void MirrorVideoRunning(void* cls, bool running) {
        PostMessageW(Self(cls)->notify_, WM_APP_MIRROR_RUNNING, running ? 1 : 0, 0);
    }

    static void VideoReset(void* cls, reset_type_t type) {
        auto* self = Self(cls);
        LOGI("video reset (type %d)", static_cast<int>(type));
        if (type == RESET_TYPE_ON_VIDEO_PLAY) {
            return;
        }
        self->remoteClockOffset_ = 0;
        self->sink_->OnVideoReset();
    }

    static int VideoSetCodec(void* cls, video_codec_t codec) {
        return Self(cls)->sink_->OnVideoCodec(codec == VIDEO_CODEC_H265) ? 0 : -1;
    }

    // Access control is off (no PIN, no password), so these only need to exist.
    static void DisplayPin(void*, char* pin) { LOGI("client asked for PIN display: %s", pin ? pin : ""); }
    static void RegisterClient(void*, const char*, const char*, const char*) {}
    static bool CheckRegister(void*, const char*) { return true; }
    static const char* Passwd(void*, int* len) {
        *len = 0;
        return nullptr;
    }

    // Audio-only extras (cover art, track text, progress, remote control) are not rendered.
    static void AudioSetMetadata(void*, const void*, int) {}
    static void AudioSetCoverart(void*, const void*, int) {}
    static void AudioStopCoverartRendering(void*) {}
    static void AudioRemoteControlId(void*, const char*, const char*) {}
    static void AudioSetProgress(void*, uint32_t*, uint32_t*, uint32_t*) {}
    static void ExportDacp(void*, const char*, const char*) {}

    // HLS ("AirPlay video") is not advertised, so these are never expected to run.
    static void OnVideoPlay(void*, const char* location, const float) {
        LOGW("ignoring AirPlay video (HLS) request for %s", location ? location : "?");
    }
    static void OnVideoScrub(void*, const float) {}
    static void OnVideoRate(void*, const float) {}
    static void OnVideoStop(void*) {}
    static void OnVideoAcquirePlaybackInfo(void*, playback_info_t*) {}
    static float OnVideoPlaylistRemove(void*) { return 0.0f; }
    static void GetCustomProfile(void*, const char** profile) { *profile = nullptr; }
};

AirPlayServer::~AirPlayServer() {
    Stop();
}

bool AirPlayServer::Start(const ServerConfig& config, IMediaSink* sink, HWND notify) {
    if (raop_) {
        LOGE("AirPlayServer::Start called while already running");
        return false;
    }
    sink_ = sink;
    notify_ = notify;
    openConnections_ = 0;
    missedFeedback_ = 0;
    remoteClockOffset_ = 0;
    clientVolumeDb_ = 0.0;

    std::vector<char> hwAddr;
    if (!ParseMac(config.macAddress, hwAddr)) {
        LOGE("invalid MAC address '%s'", config.macAddress.c_str());
        return false;
    }
    const std::string name = TruncateUtf8(config.name, kMaxServiceNameBytes);

    int dnssdError = 0;
    LOGD("server start: dnssd_init");
    dnssd_t* dnssd = dnssd_init(name.c_str(), static_cast<int>(name.size()), hwAddr.data(),
                                static_cast<int>(hwAddr.size()), 0 /* no PIN or password */, &dnssdError);
    if (dnssdError || !dnssd) {
        LOGE("dnssd_init failed (error %d)", dnssdError);
        return false;
    }
    SetFeatures(dnssd, config.supportsH265);
    dnssd_ = dnssd;

    raop_callbacks_t cbs;
    memset(&cbs, 0, sizeof(cbs));
    cbs.cls = this;
    cbs.audio_process = Callbacks::AudioProcess;
    cbs.video_process = Callbacks::VideoProcess;
    cbs.video_pause = Callbacks::VideoPause;
    cbs.video_resume = Callbacks::VideoResume;
    cbs.conn_feedback = Callbacks::ConnFeedback;
    cbs.conn_reset = Callbacks::ConnReset;
    cbs.video_reset = Callbacks::VideoReset;
    cbs.conn_init = Callbacks::ConnInit;
    cbs.conn_destroy = Callbacks::ConnDestroy;
    cbs.audio_flush = Callbacks::AudioFlush;
    cbs.video_flush = Callbacks::VideoFlush;
    cbs.audio_set_client_volume = Callbacks::AudioSetClientVolume;
    cbs.audio_set_volume = Callbacks::AudioSetVolume;
    cbs.audio_set_metadata = Callbacks::AudioSetMetadata;
    cbs.audio_set_coverart = Callbacks::AudioSetCoverart;
    cbs.audio_stop_coverart_rendering = Callbacks::AudioStopCoverartRendering;
    cbs.audio_remote_control_id = Callbacks::AudioRemoteControlId;
    cbs.audio_set_progress = Callbacks::AudioSetProgress;
    cbs.audio_get_format = Callbacks::AudioGetFormat;
    cbs.video_report_size = Callbacks::VideoReportSize;
    cbs.mirror_video_running = Callbacks::MirrorVideoRunning;
    cbs.report_client_request = Callbacks::ReportClientRequest;
    cbs.display_pin = Callbacks::DisplayPin;
    cbs.register_client = Callbacks::RegisterClient;
    cbs.check_register = Callbacks::CheckRegister;
    cbs.passwd = Callbacks::Passwd;
    cbs.export_dacp = Callbacks::ExportDacp;
    cbs.video_set_codec = Callbacks::VideoSetCodec;
    cbs.on_video_play = Callbacks::OnVideoPlay;
    cbs.on_video_scrub = Callbacks::OnVideoScrub;
    cbs.on_video_rate = Callbacks::OnVideoRate;
    cbs.on_video_stop = Callbacks::OnVideoStop;
    cbs.on_video_acquire_playback_info = Callbacks::OnVideoAcquirePlaybackInfo;
    cbs.on_video_playlist_remove = Callbacks::OnVideoPlaylistRemove;
    cbs.get_custom_profile = Callbacks::GetCustomProfile;

    LOGD("server start: raop_init");
    raop_t* raop = raop_init(&cbs);
    if (!raop) {
        LOGE("raop_init failed");
        Stop();
        return false;
    }
    raop_ = raop;
    raop_set_log_callback(raop, Callbacks::Log, nullptr);
    raop_set_log_level(raop, config.debugLog ? LOGGER_DEBUG : LOGGER_INFO);

    // An empty key file makes the library derive its Ed25519 key from the device
    // id, so the receiver keeps one identity for as long as the MAC stays the same.
    LOGD("server start: raop_init2");
    if (raop_init2(raop, 0 /* nohold */, config.macAddress.c_str(), "")) {
        LOGE("raop_init2 failed");
        Stop();
        return false;
    }

    // Display parameters advertised to the sender.
    raop_set_plist(raop, "width", config.width);
    raop_set_plist(raop, "height", config.height);
    raop_set_plist(raop, "refreshRate", config.refreshRate);
    raop_set_plist(raop, "maxFPS", config.maxFps);

    // Zero means "let the OS pick a free port".
    unsigned short tcp[3] = {0, 0, 0};
    unsigned short udp[3] = {0, 0, 0};
    raop_set_tcp_ports(raop, tcp);
    raop_set_udp_ports(raop, udp);

    unsigned short port = raop_get_port(raop);
    LOGD("server start: raop_start_httpd");
    if (raop_start_httpd(raop, &port) < 0) {
        LOGE("raop_start_httpd failed");
        Stop();
        return false;
    }
    raop_set_port(raop, port);
    port_ = port;
    raop_set_dnssd(raop, dnssd);

    LOGD("server start: dnssd_register (port %u)", port);
    int error = dnssd_register_raop(dnssd, port);
    if (error) {
        LOGE("dnssd_register_raop failed (error %d)", error);
        Stop();
        return false;
    }
    error = dnssd_register_airplay(dnssd, port);
    if (error) {
        LOGE("dnssd_register_airplay failed (error %d)", error);
        Stop();
        return false;
    }
    features_ = dnssd_get_airplay_features(dnssd);

    LOGI("AirPlay server listening: name='%s' port=%u deviceId=%s features=0x%llX res=%dx%d fps=%d supportsH265=%d",
         name.c_str(), port, config.macAddress.c_str(), static_cast<unsigned long long>(features_), config.width,
         config.height, config.maxFps, config.supportsH265 ? 1 : 0);
    return true;
}

void AirPlayServer::Stop() {
    if (dnssd_) {
        dnssd_unregister_raop(Dnssd(dnssd_));
        dnssd_unregister_airplay(Dnssd(dnssd_));
    }
    if (raop_) {
        raop_destroy(Raop(raop_));
        raop_ = nullptr;
    }
    if (dnssd_) {
        dnssd_destroy(Dnssd(dnssd_));
        dnssd_ = nullptr;
    }
    port_ = 0;
    openConnections_ = 0;
}

void AirPlayServer::RestartListener() {
    if (!raop_) {
        return;
    }
    LOGI("restarting AirPlay listener");
    raop_t* raop = Raop(raop_);
    raop_stop_httpd(raop);
    sink_->OnAudioStop();
    sink_->OnVideoReset();
    raop_remove_known_connections(raop);
    unsigned short port = raop_get_port(raop);
    if (raop_start_httpd(raop, &port) < 0) {
        LOGE("raop_start_httpd failed during restart");
        return;
    }
    raop_set_port(raop, port);
    port_ = port;
    missedFeedback_ = 0;
    remoteClockOffset_ = 0;
}

bool AirPlayServer::WatchdogTick() {
    if (!raop_ || openConnections_.load() <= 0) {
        missedFeedback_ = 0;
        return false;
    }
    const unsigned missed = missedFeedback_++;
    if (missed > kMissedFeedbackLimit) {
        LOGW("no client feedback for %u seconds, treating the connection as lost", missed);
        missedFeedback_ = 0;
        return true;
    }
    return false;
}

std::string AirPlayServer::FindMacAddress() {
    ULONG size = 16 * 1024;
    std::vector<unsigned char> buffer(size);
    auto* addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    ULONG result = GetAdaptersAddresses(AF_UNSPEC, 0, nullptr, addresses, &size);
    if (result == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(size);
        addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        result = GetAdaptersAddresses(AF_UNSPEC, 0, nullptr, addresses, &size);
    }
    if (result != NO_ERROR) {
        LOGW("GetAdaptersAddresses failed (error %lu)", result);
        return {};
    }
    for (auto* adapter = addresses; adapter; adapter = adapter->Next) {
        const bool wiredOrWifi = adapter->IfType == IF_TYPE_ETHERNET_CSMACD || adapter->IfType == IF_TYPE_IEEE80211;
        if (adapter->PhysicalAddressLength != 6 || !wiredOrWifi || adapter->OperStatus != IfOperStatusUp) {
            continue;
        }
        char text[18];
        snprintf(text, sizeof(text), "%02x:%02x:%02x:%02x:%02x:%02x", adapter->PhysicalAddress[0],
                 adapter->PhysicalAddress[1], adapter->PhysicalAddress[2], adapter->PhysicalAddress[3],
                 adapter->PhysicalAddress[4], adapter->PhysicalAddress[5]);
        return text;
    }
    return {};
}

std::string AirPlayServer::RandomMacAddress() {
    std::random_device device;
    unsigned char octets[6];
    for (auto& octet : octets) {
        octet = static_cast<unsigned char>(device() & 0xFF);
    }
    octets[0] = static_cast<unsigned char>((octets[0] & 0xFC) | 0x02);  // locally administered, unicast
    char text[18];
    snprintf(text, sizeof(text), "%02x:%02x:%02x:%02x:%02x:%02x", octets[0], octets[1], octets[2], octets[3],
             octets[4], octets[5]);
    return text;
}
