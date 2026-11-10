// Standalone check for Recorder: records synthetic pictures and sound in real time, then reads the
// files back with a Media Foundation source reader (compressed samples are counted, never decoded).
// Exit 0 on PASS or SKIP, 1 on any FAIL line. Files go to %TEMP%\airwindows-recorder-check and are
// deleted on success unless AIRWINDOWS_RECORDER_CHECK_KEEP is set.
#include "recorder.h"

#include <windows.h>
#include <objbase.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

const wchar_t kNoPicture[] = L"No picture arrived while recording.";
const wchar_t kBaseName[] = L"check";
constexpr double kTolerance = 0.4;       // seconds, for every duration comparison
constexpr int kChunkFrames = 441;        // 10 ms of sound

int failures = 0;

void Fail(const char* format, ...) {
    va_list arguments;
    va_start(arguments, format);
    std::printf("FAIL: ");
    std::vprintf(format, arguments);
    std::printf("\n");
    va_end(arguments);
    ++failures;
}

struct Stopwatch {
    LARGE_INTEGER frequency, origin;
    Stopwatch() {
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&origin);
    }
    double Seconds() const {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        return static_cast<double>(now.QuadPart - origin.QuadPart) / static_cast<double>(frequency.QuadPart);
    }
};

struct NoticeLog {
    std::mutex mutex;
    std::vector<RecorderNotice> notices;
    Recorder::Callback Callback() {
        return [this](const RecorderNotice& notice) {
            std::lock_guard<std::mutex> lock(mutex);
            notices.push_back(notice);
        };
    }
    std::vector<RecorderNotice> Take() {
        std::lock_guard<std::mutex> lock(mutex);
        return std::exchange(notices, {});
    }
};

// NV12 test picture with padded rows, so the recorder's stride handling is exercised too.
struct Picture {
    int width, height, stride;
    std::vector<uint8_t> luma, chroma;
    Picture(int w, int h)
        : width(w), height(h), stride(((w + 1) & ~1) + 24),
          luma(static_cast<size_t>(stride) * h), chroma(static_cast<size_t>(stride) * ((h + 1) / 2)) {}

    static uint8_t Wave(int value) {   // triangle wave: a gradient without hard edges
        value &= 511;
        return static_cast<uint8_t>(value > 255 ? 511 - value : value);
    }
    void Draw(int index) {
        for (int y = 0; y < height; ++y) {
            uint8_t* row = luma.data() + static_cast<size_t>(y) * stride;
            for (int x = 0; x < width; ++x) row[x] = Wave(x + y + index * 6);
        }
        for (int y = 0; y < (height + 1) / 2; ++y) {
            uint8_t* row = chroma.data() + static_cast<size_t>(y) * stride;
            for (int x = 0; x < ((width + 1) & ~1); x += 2) {
                row[x] = Wave(x + index * 4);
                row[x + 1] = Wave(2 * y - index * 4 + 4096);
            }
        }
    }
    RecorderFrame Frame() const {
        RecorderFrame frame;
        frame.width = width;
        frame.height = height;
        frame.luma = luma.data();
        frame.lumaStride = stride;
        frame.chroma = chroma.data();
        frame.chromaStride = stride;
        return frame;
    }
};

struct Segment {
    Picture* picture;
    double start;   // seconds from the beginning of the run
    double rate;    // pictures per second
    int pictures;
};

// Pushes pictures and 10 ms blocks of a 440 Hz tone when they fall due, sleeping in between.
void Play(Recorder& recorder, const std::vector<Segment>& segments, double soundSeconds, double totalSeconds) {
    const double kTwoPi = 6.283185307179586;
    const int chunkTotal = static_cast<int>(soundSeconds * 100.0 + 0.5);
    std::vector<float> chunk(static_cast<size_t>(kChunkFrames) * 2);
    std::vector<int> sent(segments.size(), 0);
    int chunks = 0;
    long long sample = 0;
    const Stopwatch watch;
    for (;;) {
        const double now = watch.Seconds();
        for (; chunks < chunkTotal && chunks * 0.010 <= now; ++chunks) {
            for (int i = 0; i < kChunkFrames; ++i, ++sample) {
                const float value = 0.25f * static_cast<float>(std::sin(kTwoPi * 440.0 * static_cast<double>(sample) / 44100.0));
                chunk[static_cast<size_t>(i) * 2] = chunk[static_cast<size_t>(i) * 2 + 1] = value;
            }
            recorder.PushAudio(chunk.data(), kChunkFrames);
        }
        for (size_t i = 0; i < segments.size(); ++i) {
            const Segment& segment = segments[i];
            for (; sent[i] < segment.pictures && segment.start + sent[i] / segment.rate <= now; ++sent[i]) {
                segment.picture->Draw(sent[i]);
                recorder.PushVideo(segment.picture->Frame());
            }
        }
        if (now >= totalSeconds) break;
        Sleep(1);
    }
}

bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

std::vector<std::wstring> ListRecordings(const std::wstring& directory) {
    std::vector<std::wstring> paths;
    WIN32_FIND_DATAW found;
    const HANDLE search = FindFirstFileW((directory + L"\\*.mp4").c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) return paths;
    do {
        paths.push_back(directory + L"\\" + found.cFileName);
    } while (FindNextFileW(search, &found));
    FindClose(search);
    return paths;
}

void DeleteRecordings(const std::wstring& directory) {
    for (const std::wstring& path : ListRecordings(directory)) {
        if (!DeleteFileW(path.c_str())) Fail("could not delete %ls (error %lu)", path.c_str(), GetLastError());
    }
}

// -1 when the list cannot be read, otherwise the number of registered H.264 encoders.
int CountH264Encoders() {
    MFT_REGISTER_TYPE_INFO h264 = {MFMediaType_Video, MFVideoFormat_H264};
    IMFActivate** found = nullptr;
    UINT32 count = 0;
    const HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_HARDWARE,
                                 nullptr, &h264, &found, &count);
    if (FAILED(hr)) return -1;
    for (UINT32 i = 0; i < count; ++i) found[i]->Release();
    CoTaskMemFree(found);
    return static_cast<int>(count);
}

struct Facts {
    int videoStreams = 0, audioStreams = 0;
    DWORD videoIndex = 0, audioIndex = 0;
    bool h264 = false, aac = false;
    UINT32 width = 0, height = 0, rate = 0, channels = 0;
    UINT32 frameRate = 0, frameRateBase = 0;   // what the file declares for its video
    double duration = 0, videoSeconds = 0, audioSeconds = 0;
    unsigned long long videoSamples = 0, audioSamples = 0;
    std::vector<LONGLONG> videoTimes;          // start of every video sample, 100 ns units
};

// Reads one stream to its end without a decoder and reports how many samples it has and where it ends.
// `times`, when given, receives the start time of every sample.
bool CountSamples(IMFSourceReader* reader, DWORD stream, const char* kind, const wchar_t* name,
                  unsigned long long& samples, double& endSeconds, std::vector<LONGLONG>* times = nullptr) {
    const GUID nativeTimeFormat{};   // GUID_NULL: 100 ns units
    PROPVARIANT start;
    PropVariantInit(&start);
    start.vt = VT_I8;
    start.hVal.QuadPart = 0;
    HRESULT hr = reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    if (SUCCEEDED(hr)) hr = reader->SetStreamSelection(stream, TRUE);
    if (SUCCEEDED(hr)) hr = reader->SetCurrentPosition(nativeTimeFormat, start);
    if (FAILED(hr)) {
        Fail("%ls: selecting the %s stream failed with 0x%08X", name, kind, static_cast<unsigned>(hr));
        return false;
    }
    LONGLONG end = 0;
    for (;;) {
        DWORD flags = 0;
        LONGLONG at = 0;
        ComPtr<IMFSample> sample;
        hr = reader->ReadSample(stream, 0, nullptr, &flags, &at, &sample);
        if (FAILED(hr)) {
            Fail("%ls: reading the %s stream failed with 0x%08X", name, kind, static_cast<unsigned>(hr));
            return false;
        }
        if (sample) {
            ++samples;
            if (times) times->push_back(at);
            LONGLONG duration = 0;
            if (FAILED(sample->GetSampleDuration(&duration))) duration = 0;   // the start time alone is close enough
            end = std::max(end, at + duration);
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
    }
    endSeconds = static_cast<double>(end) / 1e7;
    return true;
}

bool Inspect(const std::wstring& path, const wchar_t* name, Facts& facts) {
    ComPtr<IMFSourceReader> reader;
    HRESULT hr = MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader);
    if (FAILED(hr)) {
        Fail("%ls: MFCreateSourceReaderFromURL failed with 0x%08X", name, static_cast<unsigned>(hr));
        return false;
    }
    for (DWORD index = 0;; ++index) {
        ComPtr<IMFMediaType> type;
        hr = reader->GetNativeMediaType(index, 0, &type);
        if (hr == MF_E_INVALIDSTREAMNUMBER) break;
        GUID major{}, subtype{};
        if (SUCCEEDED(hr)) hr = type->GetGUID(MF_MT_MAJOR_TYPE, &major);
        if (SUCCEEDED(hr)) hr = type->GetGUID(MF_MT_SUBTYPE, &subtype);
        if (FAILED(hr)) {
            Fail("%ls: reading the type of stream %lu failed with 0x%08X", name, index, static_cast<unsigned>(hr));
            return false;
        }
        if (major == MFMediaType_Video) {
            ++facts.videoStreams;
            facts.videoIndex = index;
            facts.h264 = subtype == MFVideoFormat_H264;
            hr = MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &facts.width, &facts.height);
            if (FAILED(hr)) Fail("%ls: the video stream has no frame size (0x%08X)", name, static_cast<unsigned>(hr));
            hr = MFGetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, &facts.frameRate, &facts.frameRateBase);
            if (FAILED(hr)) Fail("%ls: the video stream declares no frame rate (0x%08X)", name, static_cast<unsigned>(hr));
        } else if (major == MFMediaType_Audio) {
            ++facts.audioStreams;
            facts.audioIndex = index;
            facts.aac = subtype == MFAudioFormat_AAC;
            facts.rate = MFGetAttributeUINT32(type.Get(), MF_MT_AUDIO_SAMPLES_PER_SECOND, 0);
            facts.channels = MFGetAttributeUINT32(type.Get(), MF_MT_AUDIO_NUM_CHANNELS, 0);
        }
    }
    PROPVARIANT duration;
    PropVariantInit(&duration);
    hr = reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &duration);
    if (FAILED(hr) || duration.vt != VT_UI8) {
        Fail("%ls: no duration (0x%08X)", name, static_cast<unsigned>(hr));
        return false;
    }
    facts.duration = static_cast<double>(duration.uhVal.QuadPart) / 1e7;
    if (facts.videoStreams != 1 || facts.audioStreams != 1) return true;   // reported by the caller
    return CountSamples(reader.Get(), facts.videoIndex, "video", name, facts.videoSamples, facts.videoSeconds, &facts.videoTimes) &&
           CountSamples(reader.Get(), facts.audioIndex, "audio", name, facts.audioSamples, facts.audioSeconds);
}

struct Expected {
    std::wstring path;
    const wchar_t* name;
    UINT32 width, height;
    double seconds;
    int pushed;
    int fps;          // the constant frame rate the session asked for
    bool tooFast;     // pictures were pushed faster than `fps`, so some must be left out
};

const wchar_t kLeftOut[] = L"Left out ";

// The point of a constant frame rate: frame i starts at i/fps, with no gaps and no bunching, however
// unevenly the pictures were pushed. Returns the largest distance of any frame from its place, in ms.
double CheckSpacing(const Expected& expected, const Facts& facts) {
    std::vector<LONGLONG> times = facts.videoTimes;
    std::sort(times.begin(), times.end());   // an encoder may reorder pictures; the times are what count
    const double slot = 1e7 / expected.fps;
    double worst = 0;
    size_t worstIndex = 0;
    for (size_t i = 0; i < times.size(); ++i) {
        const double off = std::fabs(static_cast<double>(times[i] - times[0]) - static_cast<double>(i) * slot);
        if (off > worst) {
            worst = off;
            worstIndex = i;
        }
    }
    // A container's own time base rounds each time a little; a missing or doubled frame is off by a whole slot.
    if (worst > slot / 4)
        Fail("%ls: frame %zu of %zu is %.2f ms from its place at %d fps", expected.name, worstIndex, times.size(), worst / 1e4, expected.fps);
    return worst / 1e4;
}

// Checks one file against what was pushed and appends its measurements to `summary`.
void Verify(const Expected& expected, const RecorderNotice& finished, std::string& summary) {
    Facts facts;
    if (!Inspect(expected.path, expected.name, facts)) return;
    if (facts.videoStreams != 1) Fail("%ls: %d video streams, expected 1", expected.name, facts.videoStreams);
    if (facts.audioStreams != 1) Fail("%ls: %d audio streams, expected 1", expected.name, facts.audioStreams);
    if (facts.videoStreams != 1 || facts.audioStreams != 1) return;
    if (!facts.h264) Fail("%ls: the video stream is not H.264", expected.name);
    if (facts.width != expected.width || facts.height != expected.height)
        Fail("%ls: picture is %ux%u, expected %ux%u", expected.name, facts.width, facts.height, expected.width, expected.height);
    if (!facts.aac) Fail("%ls: the audio stream is not AAC", expected.name);
    if (facts.rate != 44100 || facts.channels != 2)
        Fail("%ls: audio is %u Hz %u ch, expected 44100 Hz 2 ch", expected.name, facts.rate, facts.channels);
    if (std::fabs(facts.duration - expected.seconds) > kTolerance)
        Fail("%ls: duration %.3f s, expected about %.1f s", expected.name, facts.duration, expected.seconds);
    // Every frame slot of the file's length is filled, whatever number of pictures was pushed.
    const long long slots = std::llround(facts.videoSeconds * expected.fps);
    if (static_cast<long long>(facts.videoSamples) != slots)
        Fail("%ls: %llu frames in %.3f s of video, %lld make %d fps", expected.name, facts.videoSamples, facts.videoSeconds, slots, expected.fps);
    if (std::fabs(static_cast<double>(facts.videoSamples) - expected.seconds * expected.fps) > kTolerance * expected.fps)
        Fail("%ls: %llu frames, expected about %.0f", expected.name, facts.videoSamples, expected.seconds * expected.fps);
    if (facts.frameRateBase == 0 ||
        std::fabs(static_cast<double>(facts.frameRate) / facts.frameRateBase - expected.fps) > 0.005 * expected.fps)
        Fail("%ls: the file declares %u/%u fps, expected %d", expected.name, facts.frameRate, facts.frameRateBase, expected.fps);
    const double worstMs = CheckSpacing(expected, facts);
    const bool leftOut = finished.message.find(kLeftOut) != std::wstring::npos;
    if (expected.tooFast && !leftOut)
        Fail("%ls: %d pictures were pushed faster than %d fps and none was reported left out", expected.name, expected.pushed, expected.fps);
    if (std::fabs(facts.audioSeconds - facts.videoSeconds) > kTolerance)
        Fail("%ls: audio ends at %.3f s, video at %.3f s", expected.name, facts.audioSeconds, facts.videoSeconds);
    if (std::fabs(finished.seconds - expected.seconds) > kTolerance)
        Fail("%ls: the Finished notice says %.3f s, expected about %.1f s", expected.name, finished.seconds, expected.seconds);
    if (finished.videoFrames != facts.videoSamples)
        Fail("%ls: the Finished notice counts %llu frames, the file holds %llu", expected.name,
             static_cast<unsigned long long>(finished.videoFrames), facts.videoSamples);

    char text[200];
    std::snprintf(text, sizeof(text), "%s%ux%u %.2f s, %llu frames at %d fps from %d pictures (worst %.2f ms off), audio %.2f s",
                  summary.empty() ? "" : "; ", facts.width, facts.height, facts.duration, facts.videoSamples, expected.fps,
                  expected.pushed, worstMs, facts.audioSeconds);
    summary += text;
}

// True when `notices` is exactly Started then Finished for each expected file, in order.
bool CheckNotices(const char* session, const std::vector<RecorderNotice>& notices, const std::vector<Expected>& files) {
    bool ok = notices.size() == files.size() * 2;
    for (size_t i = 0; ok && i < files.size(); ++i) {
        const RecorderNotice& started = notices[i * 2];
        const RecorderNotice& finished = notices[i * 2 + 1];
        ok = started.event == RecorderEvent::Started && finished.event == RecorderEvent::Finished &&
             started.path == files[i].path && finished.path == files[i].path;
    }
    for (const RecorderNotice& notice : notices) {
        if (notice.event == RecorderEvent::Failed)
            Fail("%s: recorder failed for \"%ls\": %ls", session, notice.path.c_str(), notice.message.c_str());
        else if (!notice.message.empty())
            std::printf("note: %s: %ls\n", session, notice.message.c_str());
    }
    if (!ok) {
        Fail("%s: expected Started and Finished for %zu files, got %zu notices", session, files.size(), notices.size());
        for (const RecorderNotice& notice : notices)
            std::printf("      %s \"%ls\"\n", notice.event == RecorderEvent::Started ? "Started" :
                        notice.event == RecorderEvent::Finished ? "Finished" : "Failed", notice.path.c_str());
    }
    return ok;
}

// Returns false when the machine cannot encode H.264 at all (the caller prints SKIP).
bool Run(const std::wstring& directory, std::string& summary) {
    const std::wstring first = directory + L"\\check.mp4";
    const std::wstring second = directory + L"\\check (2).mp4";
    const std::wstring third = directory + L"\\check (3).mp4";
    const std::wstring fourth = directory + L"\\check (4).mp4";

    {   // A session that never sees a picture reports that and leaves no file behind.
        Recorder recorder;
        NoticeLog seen;
        if (!recorder.Start(directory, kBaseName, 60, seen.Callback())) Fail("no-picture session: Start returned false");
        if (!recorder.Active()) Fail("no-picture session: not active after Start");
        Play(recorder, {}, 0.2, 0.2);
        recorder.Stop();
        const std::vector<RecorderNotice> notices = seen.Take();
        if (notices.size() != 1 || notices[0].event != RecorderEvent::Failed || notices[0].message != kNoPicture)
            Fail("no-picture session: expected one Failed notice \"%ls\", got %zu notices%ls%ls", kNoPicture, notices.size(),
                 notices.empty() ? L"" : L", first message: ", notices.empty() ? L"" : notices[0].message.c_str());
        if (!ListRecordings(directory).empty()) Fail("no-picture session: a file was created");
        if (recorder.Active() || recorder.Seconds() != 0) Fail("no-picture session: still active after Stop");
    }

    Picture portrait(640, 1136), landscape(1136, 640), odd(641, 479);
    {   // A 60 fps file: 3 s of pictures at the file's own rate with sound (Play pushes them in uneven
        // groups, as a network does), 1 s still with sound, then 2 s at another size and half the rate
        // without sound. The still second and the half rate are filled with repeats.
        Recorder recorder;
        NoticeLog seen;
        if (!recorder.Start(directory, kBaseName, 60, seen.Callback())) Fail("main session: Start returned false");
        if (recorder.Start(directory, kBaseName, 60, seen.Callback())) Fail("main session: a second Start was accepted");
        Play(recorder, {{&portrait, 0.0, 60.0, 180}, {&landscape, 4.0, 30.0, 60}}, 4.0, 6.0);
        const double seconds = recorder.Seconds();
        if (!recorder.Active() || std::fabs(seconds - 6.0) > kTolerance) Fail("main session: Seconds() is %.3f before Stop", seconds);
        recorder.Stop();
        if (recorder.Active() || recorder.Seconds() != 0) Fail("main session: still active after Stop");
        const std::vector<RecorderNotice> notices = seen.Take();

        if (notices.size() == 1 && notices[0].event == RecorderEvent::Failed && notices[0].message != kNoPicture) {
            const int encoders = CountH264Encoders();
            if (encoders == 0 || notices[0].message.find(L"0xC00D5212") != std::wstring::npos) {   // MF_E_TOPO_CODEC_NOT_FOUND
                std::printf("note: %ls\n", notices[0].message.c_str());
                return false;
            }
        }
        const std::vector<Expected> files = {{first, L"check.mp4", 640, 1136, 4.0, 180, 60, false},
                                             {second, L"check (2).mp4", 1136, 640, 2.0, 60, 60, false}};
        if (CheckNotices("main session", notices, files)) {
            Verify(files[0], notices[1], summary);
            Verify(files[1], notices[3], summary);
        }
    }
    {   // Odd sizes are cropped to even; the name continues after the files that already exist.
        // A 30 fps file fed 15 pictures a second repeats each one.
        Recorder recorder;
        NoticeLog seen;
        if (!recorder.Start(directory, kBaseName, 30, seen.Callback())) Fail("odd-size session: Start returned false");
        Play(recorder, {{&odd, 0.0, 15.0, 8}}, 0.0, 0.5);
        recorder.Stop();
        const std::vector<RecorderNotice> notices = seen.Take();
        const std::vector<Expected> files = {{third, L"check (3).mp4", 640, 478, 0.5, 8, 30, false}};
        if (CheckNotices("odd-size session", notices, files)) Verify(files[0], notices[1], summary);
    }
    {   // A 30 fps file fed 60 pictures a second keeps its rate: half the pictures are left out and
        // the Finished notice says so.
        Recorder recorder;
        NoticeLog seen;
        if (!recorder.Start(directory, kBaseName, 30, seen.Callback())) Fail("fast session: Start returned false");
        Play(recorder, {{&landscape, 0.0, 60.0, 60}}, 0.0, 1.0);
        recorder.Stop();
        const std::vector<RecorderNotice> notices = seen.Take();
        const std::vector<Expected> files = {{fourth, L"check (4).mp4", 1136, 640, 1.0, 60, 30, true}};
        if (CheckNotices("fast session", notices, files)) Verify(files[0], notices[1], summary);
    }
    summary += "; no-picture session reported";
    return true;
}

}  // namespace

int main() {
    wchar_t temp[MAX_PATH];
    const DWORD length = GetEnvironmentVariableW(L"TEMP", temp, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        Fail("TEMP is not set");
        return 1;
    }
    std::wstring directory = temp;
    while (!directory.empty() && directory.back() == L'\\') directory.pop_back();
    directory += L"\\airwindows-recorder-check";

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        Fail("CoInitializeEx failed with 0x%08X", static_cast<unsigned>(hr));
        return 1;
    }
    hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    if (FAILED(hr)) {
        Fail("MFStartup failed with 0x%08X", static_cast<unsigned>(hr));
        CoUninitialize();
        return 1;
    }

    DeleteRecordings(directory);   // leftovers of a failed run would shift the numbered names
    std::string summary;
    const bool encoded = failures == 0 && Run(directory, summary);
    const bool keep = GetEnvironmentVariableW(L"AIRWINDOWS_RECORDER_CHECK_KEEP", nullptr, 0) != 0;
    if (failures == 0 && !keep) {
        DeleteRecordings(directory);
        // Only the recordings are ours to delete; the folder goes too unless something else is in it.
        if (!RemoveDirectoryW(directory.c_str())) {
            const DWORD error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_DIR_NOT_EMPTY) Fail("could not remove %ls (error %lu)", directory.c_str(), error);
        }
    }

    hr = MFShutdown();
    if (FAILED(hr)) Fail("MFShutdown failed with 0x%08X", static_cast<unsigned>(hr));
    CoUninitialize();

    if (failures != 0) return 1;
    if (!encoded) {
        std::printf("SKIP: no H.264 encoder\n");
        return 0;
    }
    std::printf("PASS: %s\n", summary.c_str());
    return 0;
}
