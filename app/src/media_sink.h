// The boundary between the AirPlay protocol engine and the renderers.
//
// Every method here is called on a protocol thread and must return quickly
// without blocking on the UI thread.
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>

struct ClientInfo {
    std::string name;
    std::string model;
    std::string deviceId;
};

class IMediaSink {
public:
    virtual ~IMediaSink() = default;
    virtual void OnClientRequest(const std::string&) {} // invalidate controls before a new sender is admitted

    // Video. `data` is an Annex-B access unit (start codes, SPS/PPS prepended to key frames).
    virtual bool OnVideoCodec(bool h265) = 0;
    virtual void OnVideoData(const uint8_t* data, int size, uint64_t ptsNs) = 0;
    virtual void OnVideoSize(int sourceWidth, int sourceHeight, int width, int height) = 0;
    virtual void OnVideoPause() = 0;
    virtual void OnVideoResume() = 0;
    virtual void OnVideoFlush() = 0;
    virtual void OnVideoReset() = 0;
    virtual void OnMirrorState(bool running) = 0;

    // Audio. `compressionType` is AirPlay's ct: 2 = ALAC, 4 = AAC-LC, 8 = AAC-ELD.
    virtual void OnAudioFormat(uint8_t compressionType) = 0;
    virtual void OnAudioData(const uint8_t* data, int size, uint8_t compressionType, uint64_t ptsNs) = 0;
    virtual void OnAudioFlush() = 0;
    virtual void OnAudioStop() = 0;
    // Linear gain in [0, 1] derived from the sender's volume slider.
    virtual void OnAudioVolume(double gain) = 0;
};

// Messages the server and the pipelines post to the main window.
constexpr UINT WM_APP_CLIENT_REQUEST = WM_APP + 1;  // lParam: ClientInfo* (receiver deletes)
constexpr UINT WM_APP_CONNECTIONS = WM_APP + 2;     // wParam: open connection count
constexpr UINT WM_APP_SERVER_RESET = WM_APP + 3;    // wParam: reason from the protocol engine
constexpr UINT WM_APP_MIRROR_RUNNING = WM_APP + 4;  // wParam: nonzero while a mirror stream runs
constexpr UINT WM_APP_VIDEO_PICTURE = WM_APP + 5;   // wParam: nonzero once a frame is on screen
constexpr UINT WM_APP_CONTROL_FRAME = WM_APP + 6;   // wParam: decoded generation; lParam: geometry
