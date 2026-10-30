// Optional local decoded-frame interface. No desktop/window capture or input.
#pragma once
#include <cstdint>
#include <memory>
struct AVFrame;

struct FrameStamp {
    uint64_t receivedMs = 0;
    uint64_t generation = 0;
};

class FrameServer {
public:
    FrameServer();
    ~FrameServer();
    bool Start(bool selfTest = false);
    void Stop();
    FrameStamp Stamp() const;
    void Publish(const AVFrame* frame, FrameStamp stamp);
    void SetActive(bool active);
    void SetPaused(bool paused);
    void Invalidate();
    void SetGeometry(int width, int height);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
