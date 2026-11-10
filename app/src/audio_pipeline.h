// Audio path: FFmpeg AAC-ELD / AAC-LC / ALAC decode into a short jitter buffer
// that a WASAPI shared-mode render thread drains.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

class Recorder;

class AudioPipeline {
public:
    AudioPipeline();
    ~AudioPipeline();
    AudioPipeline(const AudioPipeline&) = delete;
    AudioPipeline& operator=(const AudioPipeline&) = delete;

    // Starts the WASAPI render thread. Playback is silent until audio arrives.
    bool Start();
    void Stop();

    // Called from protocol threads. `compressionType` is AirPlay's ct value.
    void SetFormat(uint8_t compressionType);
    void Push(const uint8_t* data, int size, uint8_t compressionType);
    void Flush();
    void StopPlayback();
    void SetGain(double gain);

    double BufferedMs() const;
    std::string CodecName() const;

    // While `recorder` is recording it also receives the decoded audio, before
    // the sender's volume is applied. Null detaches.
    void SetRecorder(Recorder* recorder);

    // Diagnostics used by --selftest: opens each decoder with the codec
    // parameters AirPlay uses, and queues a short tone for the render thread.
    bool SelfTestDecoders();
    void PushTestTone(double seconds, double frequencyHz);
    // Self-test: behave as if Windows reported a new default playback device.
    void SimulateDefaultDeviceChange();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
