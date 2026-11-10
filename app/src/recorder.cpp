// MP4 recording through the Media Foundation sink writer: H.264 picture, AAC-LC sound.
// Callers only copy into bounded queues; one worker thread owns every Media Foundation object.
#include "recorder.h"

#include <windows.h>
#include <objbase.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <exception>
#include <filesystem>
#include <mutex>
#include <new>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr int64_t kAudioRate = 44100;
constexpr size_t kAudioChannels = 2;
constexpr int64_t kTicksPerSecond = 10'000'000;                    // Media Foundation counts time in 100 ns ticks
constexpr int64_t kStillMarginTicks = kTicksPerSecond * 4 / 1000;   // a frame slot stays open this long for a late picture
// Pictures that arrive together may run the file this far ahead of the clock. Replaying the arrival
// times of an iPhone on Wi-Fi (805 pictures in 20 s) through this rule, 35 ms left out 13 pictures
// and 100 ms none, with the file never more than 87 ms ahead.
constexpr int64_t kAheadTicks = kTicksPerSecond * 100 / 1000;
constexpr int kMinFps = 10, kMaxFps = 120;                          // frame rates a file may be given
constexpr int64_t kAudioLateTicks = kTicksPerSecond * 150 / 1000;  // sound later than this gets silence in front
constexpr int64_t kAudioLagTicks = kTicksPerSecond / 2;            // without sound, silence trails the picture by this
constexpr int64_t kPrerollTicks = kTicksPerSecond * 3;             // sound kept from before the first picture
constexpr size_t kPrerollFrames = static_cast<size_t>(kAudioRate) * 3;
constexpr size_t kAudioQueueFrames = static_cast<size_t>(kAudioRate) * 5;
constexpr size_t kSilenceBlockFrames = static_cast<size_t>(kAudioRate) / 2;
constexpr size_t kVideoQueueCap = 8;
// The encoder takes a few hundred milliseconds to open. Until it has opened and caught up, the queue may
// hold this much more, so the first moments of a recording are kept instead of dropped.
constexpr size_t kOpeningQueueBytes = 128u * 1024 * 1024;
constexpr size_t kOpeningQueueCap = 48;
constexpr size_t kSpareBuffers = kVideoQueueCap + 2;
constexpr int kMaxPictureSide = 16384;
constexpr int kMaxFileIndex = 9999;
constexpr auto kStopWait = std::chrono::seconds(30);

int64_t SamplesToTicks(int64_t samples) { return samples * kTicksPerSecond / kAudioRate; }
int64_t TicksToSamples(int64_t ticks) { return ticks * kAudioRate / kTicksPerSecond; }

std::wstring Format(const wchar_t* format, ...) {
    wchar_t text[512];
    va_list arguments;
    va_start(arguments, format);
    _vsnwprintf_s(text, _countof(text), _TRUNCATE, format, arguments);
    va_end(arguments);
    return text;
}

std::wstring Widen(const char* text) {
    std::wstring wide;
    for (; text && *text; ++text) wide += static_cast<wchar_t>(static_cast<unsigned char>(*text));
    return wide;
}

void AppendNote(std::wstring& notes, const std::wstring& note) {
    if (note.empty()) return;
    if (!notes.empty()) notes += L' ';
    notes += note;
}

// One fallible call and the name it is reported under.
struct Step {
    HRESULT hr = S_OK;
    const wchar_t* name = L"";
    bool smallerMayWork = false;   // the encoder refused the picture: half the size is worth one retry
};

std::wstring StepError(const Step& step) {
    return Format(L"%s failed with 0x%08X", step.name, static_cast<unsigned>(step.hr));
}

// Keeps the first failure of a run of attribute setters.
struct FirstFailure {
    HRESULT hr = S_OK;
    void operator()(HRESULT result) { if (SUCCEEDED(hr)) hr = result; }
};

struct VideoItem {
    int64_t ticks = 0;           // arrival since Start
    int width = 0, height = 0;   // even
    std::vector<uint8_t> nv12;   // width x height luma, then width x height/2 interleaved chroma, rows unpadded
};

struct AudioItem {
    int64_t ticks = 0;           // arrival since Start; taken as the time of the first sample
    std::vector<int16_t> pcm;    // interleaved stereo
    size_t Frames() const { return pcm.size() / kAudioChannels; }
};

// Everything the callers and the worker share. The worker holds its own reference, so a worker that
// outlives a timed-out Stop() never touches freed memory.
struct Session {
    std::wstring directory, baseName;
    Recorder::Callback callback;
    int fps = 60;                        // the file's constant frame rate
    int64_t qpcStart = 0, qpcFrequency = 1;

    std::atomic<bool> active{true};      // false once Stop() was requested or the session failed
    std::atomic<bool> muted{false};      // the owner stopped waiting for this worker: send no more notices
    std::atomic<bool> caughtUp{false};   // a file is open and the queue has drained once: the short cap applies

    // How many pictures of `bytes` each may wait for the worker.
    size_t VideoCap(size_t bytes) const {
        if (caughtUp.load() || bytes == 0) return kVideoQueueCap;
        return std::clamp(kOpeningQueueBytes / bytes, kVideoQueueCap, kOpeningQueueCap);
    }
    std::atomic<DWORD> workerThread{0};

    std::mutex mutex;                    // guards everything below
    std::condition_variable wake;        // worker: work queued or stop requested
    std::condition_variable finished;    // Stop(): the worker has returned
    std::deque<VideoItem> video;
    std::deque<AudioItem> audio;
    size_t audioFrames = 0;              // frames held in `audio`
    std::vector<std::vector<uint8_t>> spare;  // picture buffers to reuse instead of allocating per frame
    uint64_t droppedVideo = 0, rejectedVideo = 0, droppedAudioFrames = 0, allocationFailures = 0;
    bool stopRequested = false;
    int64_t stopTicks = 0;
    bool done = false;

    int64_t Ticks() const {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        const int64_t elapsed = now.QuadPart - qpcStart;
        // Split so a session of many hours cannot overflow the multiplication.
        return elapsed / qpcFrequency * kTicksPerSecond + elapsed % qpcFrequency * kTicksPerSecond / qpcFrequency;
    }

    void RequestStop() {
        const int64_t now = Ticks();
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!stopRequested) {
                stopRequested = true;
                stopTicks = now;
            }
            active.store(false);
        }
        wake.notify_all();
    }

    void CountAllocationFailure() {
        std::lock_guard<std::mutex> lock(mutex);
        ++allocationFailures;
    }
};

int16_t ToPcm16(float value) {
    if (std::isnan(value)) value = 0.0f;
    value = std::clamp(value, -1.0f, 1.0f);
    return static_cast<int16_t>(std::lrintf(value * 32767.0f));
}

void CopyPlane(uint8_t* to, const uint8_t* from, int fromStride, int rowBytes, int rows) {
    if (fromStride == rowBytes) {
        std::memcpy(to, from, static_cast<size_t>(rowBytes) * rows);
        return;
    }
    for (int row = 0; row < rows; ++row, to += rowBytes, from += fromStride) std::memcpy(to, from, static_cast<size_t>(rowBytes));
}

// 2x2 box average of an unpadded NV12 picture. toWidth/toHeight are even and at most half the source size.
void HalveNv12(const uint8_t* from, int fromWidth, int fromHeight, uint8_t* to, int toWidth, int toHeight) {
    for (int y = 0; y < toHeight; ++y) {
        const uint8_t* upper = from + static_cast<size_t>(2 * y) * fromWidth;
        const uint8_t* lower = upper + fromWidth;
        for (int x = 0; x < toWidth; ++x)
            *to++ = static_cast<uint8_t>((upper[2 * x] + upper[2 * x + 1] + lower[2 * x] + lower[2 * x + 1] + 2) >> 2);
    }
    // Chroma rows hold U,V pairs, so the horizontal neighbour of a value is two bytes away.
    const uint8_t* chroma = from + static_cast<size_t>(fromWidth) * fromHeight;
    for (int y = 0; y < toHeight / 2; ++y) {
        const uint8_t* upper = chroma + static_cast<size_t>(2 * y) * fromWidth;
        const uint8_t* lower = upper + fromWidth;
        for (int x = 0; x < toWidth; ++x) {
            const int left = 2 * (x & ~1) + (x & 1);
            *to++ = static_cast<uint8_t>((upper[left] + upper[left + 2] + lower[left] + lower[left + 2] + 2) >> 2);
        }
    }
}

HRESULT MakeVideoType(const GUID& subtype, int width, int height, int fps, bool encoded, ComPtr<IMFMediaType>& type) {
    HRESULT hr = MFCreateMediaType(type.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return hr;
    FirstFailure set;
    set(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
    set(type->SetGUID(MF_MT_SUBTYPE, subtype));
    set(type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
    set(MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, static_cast<UINT32>(width), static_cast<UINT32>(height)));
    // The real rate: the worker writes a frame into every slot, so editors see evenly spaced frames.
    set(MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, static_cast<UINT32>(fps), 1));
    set(MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1));
    // The pictures arrive as limited-range BT.709; this only labels them, no pixel is converted.
    set(type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235));
    set(type->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709));
    set(type->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709));
    set(type->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709));
    if (encoded) {
        const int64_t bitrate = std::clamp<int64_t>(static_cast<int64_t>(width) * height * 5, 3'000'000, 30'000'000);
        set(type->SetUINT32(MF_MT_AVG_BITRATE, static_cast<UINT32>(bitrate)));
    }
    return set.hr;
}

HRESULT MakeAudioType(bool encoded, ComPtr<IMFMediaType>& type) {
    HRESULT hr = MFCreateMediaType(type.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return hr;
    const UINT32 rate = static_cast<UINT32>(kAudioRate), channels = static_cast<UINT32>(kAudioChannels);
    FirstFailure set;
    set(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio));
    set(type->SetGUID(MF_MT_SUBTYPE, encoded ? MFAudioFormat_AAC : MFAudioFormat_PCM));
    set(type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16));
    set(type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate));
    set(type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels));
    if (encoded) {
        set(type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 20000));
    } else {
        set(type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, channels * 2));
        set(type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, rate * channels * 2));
        set(type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE));
    }
    return set.hr;
}

// Builds a one-buffer sample; `fill` writes exactly `bytes` bytes.
template <class Fill>
Step MakeSample(DWORD bytes, int64_t sampleTicks, int64_t duration, Fill fill, ComPtr<IMFSample>& sample) {
    ComPtr<IMFMediaBuffer> buffer;
    HRESULT hr = MFCreateMemoryBuffer(bytes, &buffer);
    if (FAILED(hr)) return {hr, L"MFCreateMemoryBuffer"};
    BYTE* data = nullptr;
    hr = buffer->Lock(&data, nullptr, nullptr);
    if (FAILED(hr)) return {hr, L"IMFMediaBuffer::Lock"};
    fill(data);
    hr = buffer->Unlock();
    if (FAILED(hr)) return {hr, L"IMFMediaBuffer::Unlock"};
    hr = buffer->SetCurrentLength(bytes);
    if (FAILED(hr)) return {hr, L"IMFMediaBuffer::SetCurrentLength"};
    hr = MFCreateSample(sample.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return {hr, L"MFCreateSample"};
    hr = sample->AddBuffer(buffer.Get());
    if (FAILED(hr)) return {hr, L"IMFSample::AddBuffer"};
    hr = sample->SetSampleTime(sampleTicks);
    if (FAILED(hr)) return {hr, L"IMFSample::SetSampleTime"};
    hr = sample->SetSampleDuration(duration);
    if (FAILED(hr)) return {hr, L"IMFSample::SetSampleDuration"};
    return {};
}

// Lives on the worker thread and owns the sink writer.
class Worker {
public:
    explicit Worker(Session& session) : s_(session) {}
    void Run();

private:
    void Loop();
    bool OnVideo(VideoItem&& picture);
    bool OnStill();
    bool OnAudio(AudioItem&& sound);
    void TrimPreroll();
    void Finish(int64_t stopTicks);

    std::wstring NextPath();
    bool OpenFile(const VideoItem& first);
    Step OpenWriter(const std::wstring& path, int width, int height);
    std::wstring DiscardFile(const std::wstring& path);
    bool CloseFile(int64_t endSessionTicks, RecorderNotice& finished);

    // The file has a constant frame rate: frame `index` starts at SlotTicks(index) and every slot
    // gets a frame. A picture belongs to the slot nearest its arrival; a slot nothing arrived for
    // repeats the picture before it.
    int64_t Frames() const { return static_cast<int64_t>(videoFrames_); }
    int64_t SlotTicks(int64_t index) const { return index * kTicksPerSecond / s_.fps; }
    int64_t SlotAt(int64_t fileTicks) const { return fileTicks <= 0 ? 0 : (fileTicks * s_.fps + kTicksPerSecond / 2) / kTicksPerSecond; }
    int64_t StillDueTicks() const;
    bool WriteFrame(const VideoItem& picture);
    bool FillStillSlots(int64_t upToSlot);
    bool KeepAudioNear();
    bool WriteAudio(const int16_t* pcm, size_t frames);
    bool PadSilenceTo(int64_t ticks);
    int64_t AudioClock() const { return SamplesToTicks(audioSamples_); }
    int64_t FileTicks() const;

    void Fail(const std::wstring& path, const std::wstring& message);
    void ReleaseWriter();
    void Recycle(std::vector<uint8_t>&& buffer);
    std::wstring TakeNotes();
    void Notify(const RecorderNotice& notice);

    Session& s_;
    RecorderNotice final_{};     // last notice of the session; sent after Media Foundation is shut down
    bool haveFinal_ = false;
    bool failed_ = false;

    std::deque<AudioItem> preroll_;  // sound that arrived before the first picture
    size_t prerollFrames_ = 0;
    int nextIndex_ = 1;              // 1 is the unnumbered name

    // Current file.
    ComPtr<IMFSinkWriter> writer_;
    bool open_ = false;
    DWORD videoStream_ = 0, audioStream_ = 0;
    std::wstring path_, sizeNote_;
    int sourceWidth_ = 0, sourceHeight_ = 0;   // pictures as pushed
    int encodeWidth_ = 0, encodeHeight_ = 0;   // half of that after the fallback
    int64_t origin_ = 0;                       // session time of this file's time zero
    int64_t lastVideoTicks_ = 0;               // file time of the newest written picture
    int64_t audioSamples_ = 0;                 // audio clock: frames written to this file
    uint64_t videoFrames_ = 0;                 // frames written, repeats included
    uint64_t skippedVideo_ = 0;                // pictures left out because the file was ahead of the clock
    VideoItem last_;                           // newest written picture, repeated while the screen is still
};

void Worker::Notify(const RecorderNotice& notice) {
    if (!s_.callback || s_.muted.load()) return;
    try {
        s_.callback(notice);
    } catch (const std::exception& error) {
        // The callback is the only reporting channel, so its own failure can only go to the debugger.
        OutputDebugStringA("Recorder: the notice callback threw: ");
        OutputDebugStringA(error.what());
        OutputDebugStringA("\n");
    } catch (...) {
        OutputDebugStringA("Recorder: the notice callback threw an unknown exception.\n");
    }
}

void Worker::ReleaseWriter() {
    writer_.Reset();
    open_ = false;
    s_.caughtUp.store(false);   // the next file opens as slowly as the first
    Recycle(std::move(last_.nv12));
    last_ = VideoItem{};
}

void Worker::Recycle(std::vector<uint8_t>&& buffer) {
    if (buffer.capacity() == 0) return;
    std::vector<uint8_t> released = std::move(buffer);
    std::lock_guard<std::mutex> lock(s_.mutex);
    if (s_.active.load() && s_.spare.size() < kSpareBuffers) s_.spare.push_back(std::move(released));
}

int64_t Worker::FileTicks() const {
    if (!open_) return 0;
    return std::max(SlotTicks(Frames()), AudioClock());
}

// Session time at which the next frame slot closes with nothing in it: the first moment OnStill()
// counts it as closed, so a wait that runs to this time always has a frame to write.
int64_t Worker::StillDueTicks() const {
    const int64_t halfSlots = 2 * Frames() + 1;
    return origin_ + kStillMarginTicks + (halfSlots * (kTicksPerSecond / 2) + s_.fps - 1) / s_.fps;
}

// Ends the session: the writer is released without Finalize, so a file being written stays incomplete.
void Worker::Fail(const std::wstring& path, const std::wstring& message) {
    if (failed_) return;
    failed_ = true;
    {
        std::lock_guard<std::mutex> lock(s_.mutex);
        s_.active.store(false);
        s_.video.clear();
        s_.audio.clear();
        s_.audioFrames = 0;
        s_.spare.clear();
    }
    final_ = RecorderNotice{};
    final_.event = RecorderEvent::Failed;
    final_.path = path;
    final_.message = message;
    final_.seconds = static_cast<double>(FileTicks()) / kTicksPerSecond;
    final_.videoFrames = open_ ? videoFrames_ : 0;
    haveFinal_ = true;
    ReleaseWriter();
}

std::wstring Worker::TakeNotes() {
    uint64_t dropped = 0, rejected = 0, lostAudio = 0, allocations = 0;
    {
        std::lock_guard<std::mutex> lock(s_.mutex);
        dropped = std::exchange(s_.droppedVideo, 0);
        rejected = std::exchange(s_.rejectedVideo, 0);
        lostAudio = std::exchange(s_.droppedAudioFrames, 0);
        allocations = std::exchange(s_.allocationFailures, 0);
    }
    std::wstring notes = std::move(sizeNote_);
    sizeNote_.clear();
    if (dropped) AppendNote(notes, Format(L"Dropped %llu pictures that arrived faster than they could be written.", dropped));
    if (const uint64_t skipped = std::exchange(skippedVideo_, 0))
        AppendNote(notes, Format(L"Left out %llu pictures that arrived faster than %d a second.", skipped, s_.fps));
    if (rejected) AppendNote(notes, Format(L"Ignored %llu pictures with an unusable size or stride.", rejected));
    if (lostAudio) AppendNote(notes, Format(L"Dropped %.1f s of sound that could not be written in time.", static_cast<double>(lostAudio) / kAudioRate));
    if (allocations) AppendNote(notes, Format(L"Ran out of memory %llu times while queueing pictures or sound.", allocations));
    return notes;
}

void Worker::Run() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        Fail(L"", L"Could not start recording: " + StepError({hr, L"CoInitializeEx"}) + L".");
    } else {
        hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
        if (FAILED(hr)) {
            Fail(L"", L"Could not start recording: " + StepError({hr, L"MFStartup"}) + L".");
        } else {
            try {
                Loop();
            } catch (const std::exception& error) {
                Fail(open_ ? path_ : std::wstring(), L"Recording stopped: " + Widen(error.what()) + L".");
            }
            // Every Media Foundation object has to be gone before MFShutdown.
            ReleaseWriter();
            hr = MFShutdown();
            if (FAILED(hr)) {
                // The last notice is still unsent, so this can ride along with it.
                const std::wstring shutdown = StepError({hr, L"MFShutdown"}) + L".";
                if (haveFinal_) AppendNote(final_.message, shutdown);
                else Fail(L"", shutdown);
            }
        }
        CoUninitialize();
    }
    if (haveFinal_) Notify(final_);
}

void Worker::Loop() {
    std::error_code error;
    std::filesystem::create_directories(std::filesystem::path(s_.directory), error);
    if (error) {
        const Step step{HRESULT_FROM_WIN32(static_cast<DWORD>(error.value())), L"create_directories"};
        Fail(s_.directory, L"Could not create the recording folder: " + StepError(step) + L".");
        return;
    }
    for (;;) {
        VideoItem picture;
        AudioItem sound;
        enum class Next { Picture, Sound, Still, Stop } next = Next::Stop;
        int64_t stopTicks = 0;
        {
            std::unique_lock<std::mutex> lock(s_.mutex);
            const auto pending = [this] { return s_.stopRequested || !s_.video.empty() || !s_.audio.empty(); };
            if (open_) {
                // With a file open the wait also ends when the next frame slot closes, so a still
                // screen gets its repeated frame on time instead of in a burst when it next changes.
                const int64_t wait = StillDueTicks() - s_.Ticks();
                if (wait > 0) (void)s_.wake.wait_for(lock, std::chrono::microseconds(wait / 10 + 1), pending);
            } else {
                s_.wake.wait(lock, pending);
            }
            // One item at a time keeps the queue caps honest; oldest first, so sound from before a
            // picture-size change still lands in the earlier file.
            if (!s_.audio.empty() && (s_.video.empty() || s_.audio.front().ticks <= s_.video.front().ticks)) {
                sound = std::move(s_.audio.front());
                s_.audio.pop_front();
                s_.audioFrames -= sound.Frames();
                next = Next::Sound;
            } else if (!s_.video.empty()) {
                picture = std::move(s_.video.front());
                s_.video.pop_front();
                next = Next::Picture;
                // The backlog from opening the file is gone; from here the encoder keeps pace.
                if (open_ && s_.video.size() <= 1) s_.caughtUp.store(true);
            } else if (s_.stopRequested) {
                stopTicks = s_.stopTicks;
            } else {
                next = Next::Still;
            }
        }
        if (next == Next::Sound) {
            if (!OnAudio(std::move(sound))) return;
        } else if (next == Next::Picture) {
            if (!OnVideo(std::move(picture))) return;
        } else if (next == Next::Still) {
            if (!OnStill()) return;
        } else {
            Finish(stopTicks);
            return;
        }
    }
}

bool Worker::OnAudio(AudioItem&& sound) {
    if (!open_) {
        prerollFrames_ += sound.Frames();
        preroll_.push_back(std::move(sound));
        TrimPreroll();
        return true;
    }
    const int64_t arrival = sound.ticks - origin_;
    if (arrival > AudioClock() + kAudioLateTicks && !PadSilenceTo(arrival)) return false;
    return WriteAudio(sound.pcm.data(), sound.Frames());
}

void Worker::TrimPreroll() {
    while (prerollFrames_ > kPrerollFrames) {
        AudioItem& oldest = preroll_.front();
        const size_t excess = prerollFrames_ - kPrerollFrames;
        if (oldest.Frames() <= excess) {
            prerollFrames_ -= oldest.Frames();
            preroll_.pop_front();
            continue;
        }
        oldest.pcm.erase(oldest.pcm.begin(), oldest.pcm.begin() + static_cast<std::ptrdiff_t>(excess * kAudioChannels));
        oldest.ticks += SamplesToTicks(static_cast<int64_t>(excess));
        prerollFrames_ -= excess;
    }
}

bool Worker::OnVideo(VideoItem&& picture) {
    if (open_ && (picture.width != sourceWidth_ || picture.height != sourceHeight_)) {
        RecorderNotice finished{};
        if (!CloseFile(picture.ticks, finished)) return false;
        Notify(finished);
    }
    if (!open_ && !OpenFile(picture)) return false;
    // A file's first picture goes into slot zero even when kept sound starts the file earlier, so
    // both tracks begin together; later pictures go into the slot nearest their arrival.
    int64_t slot = videoFrames_ ? SlotAt(picture.ticks - origin_) : 0;
    if (slot < Frames()) {
        // Its slot is taken: pictures that the network delivered together. They go into the next
        // slots, which keeps motion smooth, as long as the file stays within kAheadTicks of the
        // clock. While it is ahead no slot closes empty, so the next pause brings it back.
        // Beyond that the phone is sending faster than the file's rate and one is skipped.
        const int64_t ahead = std::max<int64_t>(1, kAheadTicks * s_.fps / kTicksPerSecond);
        if (Frames() - slot > ahead) {
            ++skippedVideo_;
            Recycle(std::move(picture.nv12));
            return true;
        }
        slot = Frames();
    }
    if (!FillStillSlots(slot) || !WriteFrame(picture)) return false;
    Recycle(std::move(last_.nv12));
    last_ = std::move(picture);
    return KeepAudioNear();
}

// The slots that have closed with nothing in them belong to a still screen: repeat its picture.
bool Worker::OnStill() {
    if (!open_ || !videoFrames_) return true;
    const int64_t closedBefore = SlotAt(s_.Ticks() - origin_ - kStillMarginTicks);
    return FillStillSlots(closedBefore) && KeepAudioNear();
}

bool Worker::FillStillSlots(int64_t upToSlot) {
    while (Frames() < upToSlot) {
        if (!WriteFrame(last_)) return false;
    }
    return true;
}

// The muxer interleaves the two tracks, so the audio track may not fall far behind while nothing is heard.
bool Worker::KeepAudioNear() {
    return AudioClock() >= lastVideoTicks_ - kAudioLagTicks || PadSilenceTo(lastVideoTicks_ - kAudioLagTicks);
}

void Worker::Finish(int64_t stopTicks) {
    if (failed_) return;
    if (!open_) {
        Fail(L"", L"No picture arrived while recording.");
        return;
    }
    RecorderNotice finished{};
    if (!CloseFile(stopTicks, finished)) return;
    final_ = std::move(finished);
    haveFinal_ = true;
}

std::wstring Worker::NextPath() {
    std::wstring stem = s_.directory;
    if (!stem.empty() && stem.back() != L'\\' && stem.back() != L'/') stem += L'\\';
    stem += s_.baseName;
    while (nextIndex_ <= kMaxFileIndex) {
        const int index = nextIndex_++;
        std::wstring path = index == 1 ? stem + L".mp4" : stem + L" (" + std::to_wstring(index) + L").mp4";
        // A name whose attributes cannot be read is tried anyway: creating the file then reports the real error.
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return path;
    }
    return {};
}

// Removes what a failed open left on disk. Returns a sentence only when a file stays behind.
std::wstring Worker::DiscardFile(const std::wstring& path) {
    writer_.Reset();
    if (DeleteFileW(path.c_str())) return {};
    const DWORD error = GetLastError();
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return {};   // the open never created it
    return L" " + StepError({HRESULT_FROM_WIN32(error), L"DeleteFile"}) + L".";
}

Step Worker::OpenWriter(const std::wstring& path, int width, int height) {
    ComPtr<IMFAttributes> attributes;
    HRESULT hr = MFCreateAttributes(&attributes, 2);
    if (FAILED(hr)) return {hr, L"MFCreateAttributes"};
    hr = attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    if (FAILED(hr)) return {hr, L"SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS)"};
    // Throttling would block WriteSample to pace the tracks against each other; a still screen stalls one of them.
    hr = attributes->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
    if (FAILED(hr)) return {hr, L"SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING)"};
    hr = MFCreateSinkWriterFromURL(path.c_str(), nullptr, attributes.Get(), writer_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return {hr, L"MFCreateSinkWriterFromURL"};

    ComPtr<IMFMediaType> type;
    hr = MakeVideoType(MFVideoFormat_H264, width, height, s_.fps, true, type);
    if (FAILED(hr)) return {hr, L"H.264 media type"};
    hr = writer_->AddStream(type.Get(), &videoStream_);
    if (FAILED(hr)) return {hr, L"AddStream(video)", true};
    hr = MakeAudioType(true, type);
    if (FAILED(hr)) return {hr, L"AAC media type"};
    hr = writer_->AddStream(type.Get(), &audioStream_);
    if (FAILED(hr)) return {hr, L"AddStream(audio)"};
    hr = MakeVideoType(MFVideoFormat_NV12, width, height, s_.fps, false, type);
    if (FAILED(hr)) return {hr, L"NV12 media type"};
    hr = writer_->SetInputMediaType(videoStream_, type.Get(), nullptr);
    if (FAILED(hr)) return {hr, L"SetInputMediaType(video)", true};
    hr = MakeAudioType(false, type);
    if (FAILED(hr)) return {hr, L"PCM media type"};
    hr = writer_->SetInputMediaType(audioStream_, type.Get(), nullptr);
    if (FAILED(hr)) return {hr, L"SetInputMediaType(audio)"};
    hr = writer_->BeginWriting();
    if (FAILED(hr)) return {hr, L"BeginWriting", true};
    return {};
}

bool Worker::OpenFile(const VideoItem& first) {
    while (!preroll_.empty() && preroll_.front().ticks < first.ticks - kPrerollTicks) {
        prerollFrames_ -= preroll_.front().Frames();
        preroll_.pop_front();
    }
    const std::wstring path = NextPath();
    if (path.empty()) {
        Fail(s_.directory, L"Could not find a free file name for the recording.");
        return false;
    }

    int width = first.width, height = first.height;
    std::wstring note;
    Step step = OpenWriter(path, width, height);
    if (FAILED(step.hr)) {
        // Encoders have size limits; half the picture is better than no recording.
        const std::wstring nativeError = StepError(step) + Format(L" at %dx%d", width, height);
        std::wstring cleanup = DiscardFile(path);
        std::wstring halfError;
        const int halfWidth = (width / 2) & ~1, halfHeight = (height / 2) & ~1;
        if (step.smallerMayWork && halfWidth >= 2 && halfHeight >= 2) {
            width = halfWidth;
            height = halfHeight;
            step = OpenWriter(path, width, height);
            if (FAILED(step.hr)) {
                halfError = L"; " + StepError(step) + Format(L" at %dx%d", width, height);
                cleanup = DiscardFile(path);
            }
        }
        if (FAILED(step.hr)) {
            Fail(path, L"Could not open the recording: " + nativeError + halfError + L"." + cleanup);
            return false;
        }
        note = Format(L"Recorded at half size, %dx%d: ", width, height) + nativeError + L".";
    }

    open_ = true;
    path_ = path;
    sizeNote_ = std::move(note);
    sourceWidth_ = first.width;
    sourceHeight_ = first.height;
    encodeWidth_ = width;
    encodeHeight_ = height;
    origin_ = preroll_.empty() ? first.ticks : std::min(first.ticks, preroll_.front().ticks);
    lastVideoTicks_ = 0;
    audioSamples_ = 0;
    videoFrames_ = 0;

    RecorderNotice started{};
    started.event = RecorderEvent::Started;
    started.path = path_;
    Notify(started);

    while (!preroll_.empty()) {
        AudioItem sound = std::move(preroll_.front());
        preroll_.pop_front();
        if (!OnAudio(std::move(sound))) return false;
    }
    prerollFrames_ = 0;
    return true;
}

bool Worker::CloseFile(int64_t endSessionTicks, RecorderNotice& finished) {
    // The file ends on the frame boundary nearest the moment it was closed. A still tail is filled
    // like any other still stretch, which gives it its real length.
    if (!FillStillSlots(SlotAt(endSessionTicks - origin_))) return false;
    if (!PadSilenceTo(SlotTicks(Frames()))) return false;
    const HRESULT hr = writer_->Finalize();
    if (FAILED(hr)) {
        Fail(path_, L"Could not finish the recording: " + StepError({hr, L"IMFSinkWriter::Finalize"}) + L".");
        return false;
    }
    finished.event = RecorderEvent::Finished;
    finished.path = path_;
    finished.seconds = static_cast<double>(FileTicks()) / kTicksPerSecond;
    finished.videoFrames = videoFrames_;
    finished.message = TakeNotes();
    ReleaseWriter();
    return true;
}

// Writes `picture` into the next frame slot.
bool Worker::WriteFrame(const VideoItem& picture) {
    const DWORD bytes = static_cast<DWORD>(static_cast<size_t>(encodeWidth_) * encodeHeight_ * 3 / 2);
    const bool halve = encodeWidth_ != picture.width || encodeHeight_ != picture.height;
    const int64_t fileTicks = SlotTicks(Frames());
    ComPtr<IMFSample> sample;
    Step step = MakeSample(bytes, fileTicks, SlotTicks(Frames() + 1) - fileTicks, [&](BYTE* data) {
        if (halve) HalveNv12(picture.nv12.data(), picture.width, picture.height, data, encodeWidth_, encodeHeight_);
        else std::memcpy(data, picture.nv12.data(), bytes);
    }, sample);
    if (SUCCEEDED(step.hr)) step = {writer_->WriteSample(videoStream_, sample.Get()), L"WriteSample(video)"};
    if (FAILED(step.hr)) {
        Fail(path_, L"Recording stopped: " + StepError(step) + L".");
        return false;
    }
    lastVideoTicks_ = fileTicks;
    ++videoFrames_;
    return true;
}

// `pcm` null writes silence. Sample times come from the audio clock, so the track has no gaps or overlaps.
bool Worker::WriteAudio(const int16_t* pcm, size_t frames) {
    if (frames == 0) return true;
    const DWORD bytes = static_cast<DWORD>(frames * kAudioChannels * sizeof(int16_t));
    const int64_t fileTicks = AudioClock();
    const int64_t duration = SamplesToTicks(audioSamples_ + static_cast<int64_t>(frames)) - fileTicks;
    ComPtr<IMFSample> sample;
    Step step = MakeSample(bytes, fileTicks, duration, [&](BYTE* data) {
        if (pcm) std::memcpy(data, pcm, bytes);
        else std::memset(data, 0, bytes);
    }, sample);
    if (SUCCEEDED(step.hr)) step = {writer_->WriteSample(audioStream_, sample.Get()), L"WriteSample(audio)"};
    if (FAILED(step.hr)) {
        Fail(path_, L"Recording stopped: " + StepError(step) + L".");
        return false;
    }
    audioSamples_ += static_cast<int64_t>(frames);
    return true;
}

bool Worker::PadSilenceTo(int64_t ticks) {
    const int64_t target = TicksToSamples(ticks);
    while (audioSamples_ < target) {
        const size_t frames = static_cast<size_t>(std::min<int64_t>(target - audioSamples_, static_cast<int64_t>(kSilenceBlockFrames)));
        if (!WriteAudio(nullptr, frames)) return false;
    }
    return true;
}

void WorkerMain(std::shared_ptr<Session> session) {
    session->workerThread.store(GetCurrentThreadId());
    try {
        Worker(*session).Run();
    } catch (const std::exception& error) {
        // Only reachable when reporting a failure itself ran out of memory; Stop() must still be released.
        OutputDebugStringA("Recorder: the worker stopped: ");
        OutputDebugStringA(error.what());
        OutputDebugStringA("\n");
    }
    {
        std::lock_guard<std::mutex> lock(session->mutex);
        session->active.store(false);
        session->video.clear();
        session->audio.clear();
        session->audioFrames = 0;
        session->spare.clear();
        session->done = true;
    }
    session->finished.notify_all();
}

}  // namespace

struct Recorder::Impl {
    std::mutex control;           // serializes Start() and Stop()
    mutable std::mutex guard;     // protects the `session` pointer only; never held while waiting
    std::shared_ptr<Session> session;
    std::thread thread;           // worker of `session`; touched only under `control`

    std::shared_ptr<Session> Current() const {
        std::lock_guard<std::mutex> lock(guard);
        return session;
    }

    // Waits (bounded) for the worker of `session` and forgets the session. Needs `control`.
    void Reap(bool ownerGone) {
        const std::shared_ptr<Session> ended = Current();
        if (thread.joinable()) {
            // A worker cannot wait for itself: this happens when the notice callback calls back in.
            const bool self = ended && ended->workerThread.load() == GetCurrentThreadId();
            bool exited = false;
            if (ended && !self) {
                std::unique_lock<std::mutex> lock(ended->mutex);
                exited = ended->finished.wait_for(lock, kStopWait, [&ended] { return ended->done; });
            }
            if (exited) {
                thread.join();
            } else {
                // The worker keeps the session alive by itself. One that is stuck, or whose owner is
                // being destroyed, must not call back into code that has moved on.
                if (ended && (!self || ownerGone)) ended->muted.store(true);
                thread.detach();
            }
        }
        std::lock_guard<std::mutex> lock(guard);
        session.reset();
    }
};

Recorder::Recorder() : impl_(std::make_unique<Impl>()) {}

Recorder::~Recorder() {
    Stop();
    // Stop() returns early when called from the notice callback, which leaves the worker to detach here.
    std::lock_guard<std::mutex> control(impl_->control);
    impl_->Reap(true);
}

bool Recorder::Start(const std::wstring& directory, const std::wstring& baseName, int framesPerSecond, Callback callback) {
    std::lock_guard<std::mutex> control(impl_->control);
    if (const std::shared_ptr<Session> current = impl_->Current()) {
        if (current->active.load()) return false;
    }
    impl_->Reap(false);   // a session that ended by itself may still own the thread

    auto session = std::make_shared<Session>();
    session->directory = directory;
    session->baseName = baseName;
    session->fps = std::clamp(framesPerSecond, kMinFps, kMaxFps);
    session->callback = std::move(callback);
    LARGE_INTEGER value;
    QueryPerformanceFrequency(&value);
    session->qpcFrequency = value.QuadPart;
    QueryPerformanceCounter(&value);
    session->qpcStart = value.QuadPart;
    {
        std::lock_guard<std::mutex> lock(impl_->guard);
        impl_->session = session;
    }
    try {
        impl_->thread = std::thread(WorkerMain, session);
    } catch (const std::system_error& error) {
        {
            std::lock_guard<std::mutex> lock(impl_->guard);
            impl_->session.reset();
        }
        // No worker exists to deliver this, so it is the one notice sent on the caller's thread.
        if (session->callback) {
            RecorderNotice notice{};
            notice.event = RecorderEvent::Failed;
            notice.message = L"Could not start the recorder thread: " + Widen(error.what()) + L".";
            session->callback(notice);
        }
        return false;
    }
    return true;
}

void Recorder::Stop() {
    const std::shared_ptr<Session> session = impl_->Current();
    if (!session) return;
    session->RequestStop();
    // From the notice callback the worker finishes the file after the callback returns.
    if (session->workerThread.load() == GetCurrentThreadId()) return;
    std::lock_guard<std::mutex> control(impl_->control);
    if (impl_->Current() == session) impl_->Reap(false);
}

bool Recorder::Active() const {
    const std::shared_ptr<Session> session = impl_->Current();
    return session && session->active.load();
}

double Recorder::Seconds() const {
    const std::shared_ptr<Session> session = impl_->Current();
    if (!session || !session->active.load()) return 0;
    return static_cast<double>(session->Ticks()) / kTicksPerSecond;
}

void Recorder::PushVideo(const RecorderFrame& frame) {
    const std::shared_ptr<Session> session = impl_->Current();
    if (!session || !session->active.load()) return;
    const int64_t ticks = session->Ticks();
    const int width = frame.width & ~1, height = frame.height & ~1;
    try {
        std::vector<uint8_t> pixels;
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            if (!session->active.load()) return;
            if (width < 2 || height < 2 || width > kMaxPictureSide || height > kMaxPictureSide || !frame.luma || !frame.chroma ||
                frame.lumaStride < width || frame.chromaStride < width) {
                ++session->rejectedVideo;
                return;
            }
            // Checked before copying so a busy encoder costs the caller no pixel copy.
            if (session->video.size() >= session->VideoCap(static_cast<size_t>(width) * height * 3 / 2)) {
                ++session->droppedVideo;
                return;
            }
            if (!session->spare.empty()) {
                pixels = std::move(session->spare.back());
                session->spare.pop_back();
            }
        }
        const size_t lumaBytes = static_cast<size_t>(width) * height;
        pixels.resize(lumaBytes + lumaBytes / 2);
        CopyPlane(pixels.data(), frame.luma, frame.lumaStride, width, height);
        CopyPlane(pixels.data() + lumaBytes, frame.chroma, frame.chromaStride, width, height / 2);
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            if (!session->active.load()) return;
            if (session->video.size() >= session->VideoCap(pixels.size())) {   // another thread filled it during the copy
                ++session->droppedVideo;
                if (session->spare.size() < kSpareBuffers) session->spare.push_back(std::move(pixels));
                return;
            }
            VideoItem item;
            item.ticks = ticks;
            item.width = width;
            item.height = height;
            item.nv12 = std::move(pixels);
            session->video.push_back(std::move(item));
        }
        session->wake.notify_one();
    } catch (const std::bad_alloc&) {
        session->CountAllocationFailure();   // reported in the next Finished notice
    }
}

void Recorder::PushAudio(const float* interleavedStereo, size_t frames) {
    const std::shared_ptr<Session> session = impl_->Current();
    if (!session || !session->active.load() || !interleavedStereo || frames == 0) return;
    int64_t ticks = session->Ticks();
    size_t skipped = 0;
    if (frames > kAudioQueueFrames) {   // more than the queue holds: only the newest part can be kept
        skipped = frames - kAudioQueueFrames;
        interleavedStereo += skipped * kAudioChannels;
        frames = kAudioQueueFrames;
        ticks += SamplesToTicks(static_cast<int64_t>(skipped));
    }
    try {
        AudioItem item;
        item.ticks = ticks;
        item.pcm.resize(frames * kAudioChannels);
        for (size_t i = 0; i < item.pcm.size(); ++i) item.pcm[i] = ToPcm16(interleavedStereo[i]);
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            if (!session->active.load()) return;
            session->droppedAudioFrames += skipped;
            session->audioFrames += frames;
            session->audio.push_back(std::move(item));
            while (session->audioFrames > kAudioQueueFrames) {
                const size_t oldest = session->audio.front().Frames();
                session->audioFrames -= oldest;
                session->droppedAudioFrames += oldest;
                session->audio.pop_front();
            }
        }
        session->wake.notify_one();
    } catch (const std::bad_alloc&) {
        session->CountAllocationFailure();   // reported in the next Finished notice
    }
}
