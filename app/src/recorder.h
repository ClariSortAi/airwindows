// MP4 recorder: H.264 picture and AAC sound through the Media Foundation sink writer.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

// One NV12 picture. `width`/`height` are the visible size; odd sizes are cropped to even.
struct RecorderFrame {
    int width = 0, height = 0;
    const uint8_t* luma = nullptr;   int lumaStride = 0;    // height rows
    const uint8_t* chroma = nullptr; int chromaStride = 0;  // interleaved UV, height/2 rows
};

enum class RecorderEvent { Started, Finished, Failed };
struct RecorderNotice {
    RecorderEvent event;
    std::wstring path;      // file this notice is about
    std::wstring message;   // empty unless Failed (or a non-fatal note on Finished)
    double seconds = 0;     // duration written to this file
    uint64_t videoFrames = 0;   // frames in the file, repeats of a still picture included
};

class Recorder {
public:
    // Invoked on the recorder's worker thread. Recording waits while it runs, so it must return quickly
    // and must not wait for the thread that is inside Stop(). Calling Stop() from it only requests the stop.
    using Callback = std::function<void(const RecorderNotice&)>;
    Recorder();
    ~Recorder();                       // stops and finalizes
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    // Files: <directory>\<baseName>.mp4, then "<baseName> (2).mp4", "(3)"... each time the picture size changes.
    // Creates the directory if missing. Returns false if a session is already active.
    // Every file has a constant `framesPerSecond` (10 to 120): a picture goes into the frame nearest
    // its arrival, and the last picture is repeated while none arrive, so pushing pictures at an
    // uneven or lower rate still gives evenly spaced frames.
    bool Start(const std::wstring& directory, const std::wstring& baseName, int framesPerSecond, Callback callback);
    void Stop();                       // finalizes the open file; returns when it is written (bounded wait, 30 s)
    bool Active() const;               // cheap; callable from any thread
    double Seconds() const;            // seconds since Start, 0 when inactive
    void PushVideo(const RecorderFrame& frame);                       // any thread; copies the pixels
    void PushAudio(const float* interleavedStereo, size_t frames);    // any thread; 44100 Hz, 2 channels, [-1,1]

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
