// Video path: FFmpeg H.264/H.265 decode (D3D11VA or software) and a D3D11
// NV12 -> RGB pixel-shader renderer.
#pragma once

#include <windows.h>

#include <cstdint>
#include <memory>

#include "settings.h"
#include "frame_server.h"

struct VideoStats {
    double fps = 0.0;
    double bitrateMbps = 0.0;
    int width = 0;
    int height = 0;
    bool h265 = false;
    bool hardware = false;
    bool hasPicture = false;
};

class VideoPipeline {
public:
    VideoPipeline();
    ~VideoPipeline();
    VideoPipeline(const VideoPipeline&) = delete;
    VideoPipeline& operator=(const VideoPipeline&) = delete;

    // Creates the D3D11 device and swap chain on `videoWindow` and starts the
    // decode/render thread. WM_APP_VIDEO_PICTURE is posted to `notify`.
    bool Start(HWND videoWindow, HWND notify, DecoderMode mode, bool vsync);
    void Stop();

    // Called from protocol threads.
    bool SetCodec(bool h265);
    void Push(const uint8_t* data, int size, uint64_t ptsNs);
    void Reset();  // drop queued packets, close the decoder and blank the picture

    // Called from the UI thread.
    void NotifyResize();
    void RequestRedraw();
    void SetVSync(bool vsync);
    VideoStats Stats() const;
    FrameServer& Frames();

    // True when the default adapter exposes an HEVC Main hardware decoder.
    static bool HardwareHevcAvailable();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
