// The newest decoded picture, kept for screenshots and recordings, and the
// optional local decoded-frame pipe. No desktop/window capture or input.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
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
    void SetControlState(bool ready, bool busy, bool focused, const std::string& peer, const std::string& sender);
    // The picture on screen now, whether or not the pipe was started. CopyLatest
    // gives width*height*3 RGB bytes, top row first. CloneLatest gives a new
    // reference the caller releases with av_frame_free. Empty/null while there
    // is no mirror or it is paused.
    bool CopyLatest(std::vector<uint8_t>& rgb, int& width, int& height);
    AVFrame* CloneLatest();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
