#include "audio_pipeline.h"

#include <windows.h>
#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <wrl/client.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
#include <libswresample/swresample.h>
}

#include "log.h"

using Microsoft::WRL::ComPtr;

namespace {

// Everything AirPlay sends is 44.1 kHz stereo; the Windows audio engine
// converts from this to the device mix format.
constexpr int kSampleRate = 44100;
constexpr int kChannels = 2;

// Jitter buffer, in frames (one frame = one sample per channel).
constexpr size_t kPrimeFrames = kSampleRate * 60 / 1000;     // start playing at 60 ms
constexpr size_t kTargetFrames = kSampleRate * 80 / 1000;    // level kept after an overflow trim
constexpr size_t kMaxFrames = kSampleRate * 350 / 1000;      // trim when latency passes 350 ms
constexpr size_t kCapacityFrames = kSampleRate * 2;

constexpr REFERENCE_TIME kDeviceBufferHns = 30 * 10000;  // 30 ms

// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, spelled out to avoid pulling in ksmedia.h's GUID plumbing.
const GUID kSubtypeIeeeFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

// Codec configuration AirPlay implies for each compression type (values from UxPlay's renderer).
const uint8_t kAlacCookie[] = {0x00, 0x00, 0x00, 0x24, 0x61, 0x6c, 0x61, 0x63, 0x00, 0x00, 0x00, 0x00,
                               0x00, 0x00, 0x01, 0x60, 0x00, 0x10, 0x28, 0x0a, 0x0e, 0x02, 0x00, 0xff,
                               0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xac, 0x44};
const uint8_t kAacLcConfig[] = {0x12, 0x10};
const uint8_t kAacEldConfig[] = {0xf8, 0xe8, 0x50, 0x00};

const char* CompressionName(uint8_t ct) {
    switch (ct) {
    case 2: return "ALAC";
    case 4: return "AAC-LC";
    case 8: return "AAC-ELD";
    default: return "unknown";
    }
}

// The first byte tells a correctly decrypted frame from garbage (see UxPlay's audio renderer).
bool LooksValid(uint8_t ct, uint8_t firstByte) {
    switch (ct) {
    case 8: return firstByte == 0x8c || firstByte == 0x8d || firstByte == 0x8e || firstByte == 0x80 ||
                   firstByte == 0x81 || firstByte == 0x82;
    case 2: return firstByte == 0x20;
    case 4: return firstByte == 0xff;
    default: return true;
    }
}

AVCodecContext* OpenDecoderFor(uint8_t ct) {
    const AVCodecID id = ct == 2 ? AV_CODEC_ID_ALAC : AV_CODEC_ID_AAC;
    const uint8_t* extra = ct == 2 ? kAlacCookie : (ct == 4 ? kAacLcConfig : kAacEldConfig);
    const size_t extraSize = ct == 2 ? sizeof(kAlacCookie) : (ct == 4 ? sizeof(kAacLcConfig) : sizeof(kAacEldConfig));

    const AVCodec* decoder = avcodec_find_decoder(id);
    if (!decoder) {
        LOGE("[AUDIO] no %s decoder in this FFmpeg build", CompressionName(ct));
        return nullptr;
    }
    AVCodecContext* ctx = avcodec_alloc_context3(decoder);
    if (!ctx) {
        return nullptr;
    }
    ctx->sample_rate = kSampleRate;
    av_channel_layout_default(&ctx->ch_layout, kChannels);
    ctx->extradata = static_cast<uint8_t*>(av_mallocz(extraSize + AV_INPUT_BUFFER_PADDING_SIZE));
    if (!ctx->extradata) {
        avcodec_free_context(&ctx);
        return nullptr;
    }
    memcpy(ctx->extradata, extra, extraSize);
    ctx->extradata_size = static_cast<int>(extraSize);
    const int result = avcodec_open2(ctx, decoder, nullptr);
    if (result < 0) {
        LOGE("[AUDIO] avcodec_open2 failed (%d) for %s", result, CompressionName(ct));
        avcodec_free_context(&ctx);
        return nullptr;
    }
    return ctx;
}

}  // namespace

struct AudioPipeline::Impl {
    // Decoder state, used by protocol threads.
    std::mutex decodeMutex;
    AVCodecContext* codec = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;
    SwrContext* resampler = nullptr;
    int resamplerFormat = -1;
    int resamplerRate = 0;
    int resamplerChannels = 0;
    uint8_t codecCt = 0;
    unsigned badFrames = 0;
    std::vector<float> convertBuffer;

    // Ring buffer of interleaved float samples, shared with the render thread.
    mutable std::mutex bufferMutex;
    std::vector<float> ring = std::vector<float>(kCapacityFrames * kChannels);
    size_t readPos = 0;   // in frames
    size_t frames = 0;    // frames currently buffered
    bool primed = false;
    unsigned overflowTrims = 0;

    std::atomic<float> gain{1.0f};
    std::atomic<bool> stop{false};
    std::thread renderThread;

    void CloseDecoder();
    bool EnsureDecoder(uint8_t ct);
    void Enqueue(const float* samples, size_t count);
    size_t Dequeue(float* out, size_t count);
    void ClearBuffer();
    void RenderLoop();
    bool RenderSession();
};

AudioPipeline::AudioPipeline() : impl_(std::make_unique<Impl>()) {}

AudioPipeline::~AudioPipeline() {
    Stop();
}

bool AudioPipeline::Start() {
    Impl& s = *impl_;
    if (s.renderThread.joinable()) {
        return true;
    }
    s.frame = av_frame_alloc();
    s.packet = av_packet_alloc();
    if (!s.frame || !s.packet) {
        LOGE("[AUDIO] out of memory allocating FFmpeg frame/packet");
        return false;
    }
    s.stop = false;
    s.renderThread = std::thread([&s] { s.RenderLoop(); });
    SetThreadDescription(s.renderThread.native_handle(), L"AirPlay_WASAPI_Renderer_Loop");
    return true;
}

void AudioPipeline::Stop() {
    Impl& s = *impl_;
    s.stop = true;
    if (s.renderThread.joinable()) {
        s.renderThread.join();
    }
    std::lock_guard<std::mutex> lock(s.decodeMutex);
    s.CloseDecoder();
    av_frame_free(&s.frame);
    av_packet_free(&s.packet);
}

void AudioPipeline::SetFormat(uint8_t compressionType) {
    Impl& s = *impl_;
    std::lock_guard<std::mutex> lock(s.decodeMutex);
    if (s.EnsureDecoder(compressionType)) {
        LOGI("[AUDIO] decoder ready: %s 44100 Hz stereo", CompressionName(compressionType));
    }
}

void AudioPipeline::Push(const uint8_t* data, int size, uint8_t compressionType) {
    Impl& s = *impl_;
    if (!data || size <= 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(s.decodeMutex);
    if (!s.packet || !s.EnsureDecoder(compressionType)) {
        return;
    }
    if (!LooksValid(compressionType, data[0])) {
        if (s.badFrames++ < 5) {
            LOGE("[AUDIO] invalid %s frame skipped (first byte 0x%02x)", CompressionName(compressionType), data[0]);
        }
        return;
    }
    if (av_new_packet(s.packet, size) < 0) {
        LOGE("[AUDIO] av_new_packet failed for %d bytes", size);
        return;
    }
    memcpy(s.packet->data, data, static_cast<size_t>(size));
    int result = avcodec_send_packet(s.codec, s.packet);
    av_packet_unref(s.packet);
    if (result < 0 && result != AVERROR(EAGAIN)) {
        if (s.badFrames++ < 5) {
            LOGW("[AUDIO] avcodec_send_packet failed (%d)", result);
        }
        return;
    }
    for (;;) {
        result = avcodec_receive_frame(s.codec, s.frame);
        if (result < 0) {
            break;
        }
        AVFrame* f = s.frame;
        if (!s.resampler || s.resamplerFormat != f->format || s.resamplerRate != f->sample_rate ||
            s.resamplerChannels != f->ch_layout.nb_channels) {
            swr_free(&s.resampler);
            AVChannelLayout out;
            av_channel_layout_default(&out, kChannels);
            const int rc = swr_alloc_set_opts2(&s.resampler, &out, AV_SAMPLE_FMT_FLT, kSampleRate, &f->ch_layout,
                                               static_cast<AVSampleFormat>(f->format), f->sample_rate, 0, nullptr);
            if (rc < 0 || swr_init(s.resampler) < 0) {
                LOGE("[AUDIO] could not set up sample conversion (format %d, %d Hz, %d ch)", f->format, f->sample_rate,
                     f->ch_layout.nb_channels);
                swr_free(&s.resampler);
                av_frame_unref(f);
                return;
            }
            s.resamplerFormat = f->format;
            s.resamplerRate = f->sample_rate;
            s.resamplerChannels = f->ch_layout.nb_channels;
        }
        const int capacity = swr_get_out_samples(s.resampler, f->nb_samples);
        if (capacity > 0) {
            s.convertBuffer.resize(static_cast<size_t>(capacity) * kChannels);
            uint8_t* outPlanes[1] = {reinterpret_cast<uint8_t*>(s.convertBuffer.data())};
            const int produced = swr_convert(s.resampler, outPlanes, capacity,
                                             const_cast<const uint8_t**>(f->extended_data), f->nb_samples);
            if (produced > 0) {
                s.Enqueue(s.convertBuffer.data(), static_cast<size_t>(produced));
            }
        }
        av_frame_unref(f);
    }
}

void AudioPipeline::Flush() {
    Impl& s = *impl_;
    {
        std::lock_guard<std::mutex> lock(s.decodeMutex);
        if (s.codec) {
            avcodec_flush_buffers(s.codec);
        }
    }
    s.ClearBuffer();
}

void AudioPipeline::StopPlayback() {
    Flush();
}

void AudioPipeline::SetGain(double gain) {
    if (gain < 0.0) gain = 0.0;
    if (gain > 1.0) gain = 1.0;
    impl_->gain = static_cast<float>(gain);
}

double AudioPipeline::BufferedMs() const {
    std::lock_guard<std::mutex> lock(impl_->bufferMutex);
    return static_cast<double>(impl_->frames) * 1000.0 / kSampleRate;
}

std::string AudioPipeline::CodecName() const {
    std::lock_guard<std::mutex> lock(impl_->decodeMutex);
    return impl_->codec ? CompressionName(impl_->codecCt) : "";
}

bool AudioPipeline::SelfTestDecoders() {
    bool allOk = true;
    for (uint8_t ct : {uint8_t{8}, uint8_t{2}, uint8_t{4}}) {
        AVCodecContext* ctx = OpenDecoderFor(ct);
        LOGI("[SELFTEST] audio decoder %s: %s", CompressionName(ct), ctx ? "opened" : "FAILED");
        if (ctx) {
            if (ct == 8) {
                LOGI("[SELFTEST] AAC-ELD frame size reported by decoder: %d samples", ctx->frame_size);
            }
            avcodec_free_context(&ctx);
        } else {
            allOk = false;
        }
    }
    return allOk;
}

void AudioPipeline::PushTestTone(double seconds, double frequencyHz) {
    const size_t total = static_cast<size_t>(seconds * kSampleRate);
    std::vector<float> tone(total * kChannels);
    const double twoPi = 6.283185307179586;
    for (size_t i = 0; i < total; ++i) {
        const float v = static_cast<float>(0.2 * std::sin(twoPi * frequencyHz * static_cast<double>(i) / kSampleRate));
        tone[i * 2] = v;
        tone[i * 2 + 1] = v;
    }
    // Feed in packet-sized pieces so the overflow trim does not discard the tone.
    const size_t chunk = 480;
    for (size_t offset = 0; offset < total && !impl_->stop; offset += chunk) {
        const size_t count = (total - offset) < chunk ? (total - offset) : chunk;
        impl_->Enqueue(tone.data() + offset * kChannels, count);
        Sleep(static_cast<DWORD>(chunk * 1000 / kSampleRate));
    }
}

void AudioPipeline::Impl::CloseDecoder() {
    if (codec) {
        avcodec_free_context(&codec);
    }
    swr_free(&resampler);
    resamplerFormat = -1;
    codecCt = 0;
}

bool AudioPipeline::Impl::EnsureDecoder(uint8_t ct) {
    if (codec && codecCt == ct) {
        return true;
    }
    CloseDecoder();
    if (ct != 2 && ct != 4 && ct != 8) {
        if (badFrames++ < 5) {
            LOGE("[AUDIO] unsupported AirPlay compression type %d", ct);
        }
        return false;
    }
    codec = OpenDecoderFor(ct);
    if (!codec) {
        return false;
    }
    codecCt = ct;
    badFrames = 0;
    return true;
}

void AudioPipeline::Impl::Enqueue(const float* samples, size_t count) {
    std::lock_guard<std::mutex> lock(bufferMutex);
    if (frames + count > kMaxFrames) {
        // Latency is building up (clock drift or a burst): drop the oldest audio.
        const size_t keep = kTargetFrames < frames ? kTargetFrames : frames;
        readPos = (readPos + (frames - keep)) % kCapacityFrames;
        frames = keep;
        if (overflowTrims++ < 5) {
            LOGW("[AUDIO] jitter buffer over %zu ms, trimmed to %zu ms", kMaxFrames * 1000 / kSampleRate,
                 keep * 1000 / kSampleRate);
        }
    }
    size_t writePos = (readPos + frames) % kCapacityFrames;
    for (size_t i = 0; i < count; ++i) {
        ring[writePos * kChannels] = samples[i * kChannels];
        ring[writePos * kChannels + 1] = samples[i * kChannels + 1];
        writePos = (writePos + 1) % kCapacityFrames;
    }
    frames += count;
}

// Fills `out` with `count` frames, padding with silence on underrun. Returns the frames of real audio.
size_t AudioPipeline::Impl::Dequeue(float* out, size_t count) {
    std::lock_guard<std::mutex> lock(bufferMutex);
    if (!primed && frames >= kPrimeFrames) {
        primed = true;
    }
    size_t take = 0;
    if (primed) {
        take = frames < count ? frames : count;
        const float g = gain.load();
        for (size_t i = 0; i < take; ++i) {
            out[i * kChannels] = ring[readPos * kChannels] * g;
            out[i * kChannels + 1] = ring[readPos * kChannels + 1] * g;
            readPos = (readPos + 1) % kCapacityFrames;
        }
        frames -= take;
        if (take < count) {
            primed = false;  // ran dry: rebuild the cushion before playing again
        }
    }
    if (take < count) {
        memset(out + take * kChannels, 0, (count - take) * kChannels * sizeof(float));
    }
    return take;
}

void AudioPipeline::Impl::ClearBuffer() {
    std::lock_guard<std::mutex> lock(bufferMutex);
    readPos = 0;
    frames = 0;
    primed = false;
}

void AudioPipeline::Impl::RenderLoop() {
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        LOGE("[AUDIO] CoInitializeEx failed hr=0x%08lX", hr);
        return;
    }
    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    if (mmcss) {
        LOGI("[AUDIO] render thread joined MMCSS 'Pro Audio'");
    } else {
        LOGW("[AUDIO] MMCSS unavailable, falling back to TIME_CRITICAL");
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    }
    // A session ends when the device goes away (unplugged, default changed); then open the new default.
    while (!stop) {
        if (!RenderSession() && !stop) {
            Sleep(500);
        }
    }
    if (mmcss) {
        AvRevertMmThreadCharacteristics(mmcss);
    }
    CoUninitialize();
}

bool AudioPipeline::Impl::RenderSession() {
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> device;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioRenderClient> render;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) {
        LOGE("[AUDIO] MMDeviceEnumerator failed hr=0x%08lX", hr);
        return false;
    }
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (FAILED(hr)) {
        static bool logged = false;
        if (!logged) {
            LOGW("[AUDIO] no default playback device (hr=0x%08lX); audio is muted until one appears", hr);
            logged = true;
        }
        return false;
    }
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client);
    if (FAILED(hr)) {
        LOGE("[AUDIO] IAudioClient activation failed hr=0x%08lX", hr);
        return false;
    }

    WAVEFORMATEXTENSIBLE format{};
    format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    format.Format.nChannels = kChannels;
    format.Format.nSamplesPerSec = kSampleRate;
    format.Format.wBitsPerSample = 32;
    format.Format.nBlockAlign = kChannels * sizeof(float);
    format.Format.nAvgBytesPerSec = kSampleRate * kChannels * sizeof(float);
    format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    format.Samples.wValidBitsPerSample = 32;
    format.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    format.SubFormat = kSubtypeIeeeFloat;

    const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                        AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, kDeviceBufferHns, 0, &format.Format, nullptr);
    if (FAILED(hr)) {
        LOGE("[AUDIO] IAudioClient::Initialize failed hr=0x%08lX", hr);
        return false;
    }
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) {
        LOGE("[AUDIO] CreateEvent failed (error %lu)", GetLastError());
        return false;
    }
    UINT32 bufferFrames = 0;
    if (FAILED(hr = client->SetEventHandle(event)) || FAILED(hr = client->GetBufferSize(&bufferFrames)) ||
        FAILED(hr = client->GetService(IID_PPV_ARGS(&render))) || FAILED(hr = client->Start())) {
        LOGE("[AUDIO] WASAPI stream setup failed hr=0x%08lX", hr);
        CloseHandle(event);
        return false;
    }
    LOGI("[AUDIO] WASAPI stream started: 44100 Hz stereo float, device buffer %u frames", bufferFrames);

    bool healthy = true;
    while (!stop) {
        if (WaitForSingleObject(event, 200) != WAIT_OBJECT_0) {
            continue;
        }
        UINT32 padding = 0;
        hr = client->GetCurrentPadding(&padding);
        if (FAILED(hr)) {
            LOGW("[AUDIO] GetCurrentPadding failed hr=0x%08lX, reopening the device", hr);
            healthy = false;
            break;
        }
        const UINT32 writable = bufferFrames - padding;
        if (writable == 0) {
            continue;
        }
        BYTE* data = nullptr;
        hr = render->GetBuffer(writable, &data);
        if (FAILED(hr)) {
            LOGW("[AUDIO] GetBuffer failed hr=0x%08lX, reopening the device", hr);
            healthy = false;
            break;
        }
        const size_t real = Dequeue(reinterpret_cast<float*>(data), writable);
        render->ReleaseBuffer(writable, real == 0 ? AUDCLNT_BUFFERFLAGS_SILENT : 0);
    }
    client->Stop();
    CloseHandle(event);
    return healthy;
}
