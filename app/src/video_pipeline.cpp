#include "video_pipeline.h"

#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <dxgi1_5.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/pixdesc.h>
}

#include "log.h"
#include "media_sink.h"

using Microsoft::WRL::ComPtr;

namespace {

// With more than this many access units waiting, frames are decoded but not
// drawn, so a slow display path cannot build up latency.
constexpr size_t kRenderBacklogLimit = 3;

// Safety valve for a stalled decoder.
constexpr size_t kMaxQueuedBytes = 64 * 1024 * 1024;

// Fullscreen triangle from the vertex id, no vertex buffer needed.
const char kVertexShader[] = R"(
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut main(uint id : SV_VertexID) {
    VSOut o;
    float2 t = float2((id << 1) & 2, id & 2);
    o.pos = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = t;
    return o;
}
)";

// NV12 -> RGB with BT.709 coefficients. AirPlay mirror streams are full range;
// lumaRange/chromaScale let a limited-range stream be expanded when the
// bitstream signals one.
const char kPixelShader[] = R"(
Texture2D<float>  texY  : register(t0);
Texture2D<float2> texUV : register(t1);
SamplerState samp : register(s0);
cbuffer Params : register(b0) { float2 uvScale; float2 lumaRange; float chromaScale; float3 pad; };
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float2 suv = uv * uvScale;
    float Y = (texY.Sample(samp, suv) - lumaRange.x) * lumaRange.y;
    float2 c = (texUV.Sample(samp, suv) - 0.5) * chromaScale;
    float3 rgb;
    rgb.r = Y + 1.5748 * c.y;
    rgb.g = Y - 0.1873 * c.x - 0.4681 * c.y;
    rgb.b = Y + 1.8556 * c.x;
    return float4(saturate(rgb), 1.0);
}
)";

struct ShaderParams {
    float uvScale[2];
    float lumaRange[2];
    float chromaScale;
    float pad[3];
};

struct Packet {
    std::vector<uint8_t> data;
    uint64_t pts = 0;
    FrameStamp stamp;
};

void FfmpegLog(void*, int level, const char* fmt, va_list args) {
    if (level > AV_LOG_WARNING) {
        return;
    }
    char text[1024];
    vsnprintf(text, sizeof(text), fmt, args);
    size_t len = strlen(text);
    while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == '\r')) {
        text[--len] = '\0';
    }
    if (len > 0) {
        LOGW("[ffmpeg] %s", text);
    }
}

HRESULT CreateDevice(ComPtr<ID3D11Device>& device, ComPtr<ID3D11DeviceContext>& context, bool* hardware) {
    static const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    const UINT base = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, base | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                                   levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &device, nullptr, &context);
    if (SUCCEEDED(hr)) {
        *hardware = true;
        return hr;
    }
    LOGW("[VIDEO] hardware D3D11 device with video support failed hr=0x%08lX, trying without", hr);
    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, base, levels, ARRAYSIZE(levels),
                           D3D11_SDK_VERSION, &device, nullptr, &context);
    if (SUCCEEDED(hr)) {
        *hardware = false;
        return hr;
    }
    LOGW("[VIDEO] hardware D3D11 device failed hr=0x%08lX, falling back to WARP", hr);
    *hardware = false;
    return D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, base, levels, ARRAYSIZE(levels),
                             D3D11_SDK_VERSION, &device, nullptr, &context);
}

}  // namespace

struct VideoPipeline::Impl {
    FrameServer frames;
    uint64_t controlFrameGeneration = 0;
    HWND hwnd = nullptr;
    HWND notify = nullptr;
    DecoderMode mode = DecoderMode::Auto;
    std::atomic<bool> vsync{true};

    // Shared with protocol and UI threads, guarded by `mutex`.
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::deque<Packet> queue;
    size_t queuedBytes = 0;
    bool stop = false;
    bool resetRequested = false;
    bool resizePending = false;
    bool redrawPending = false;
    int codecRequest = -1;  // 0 = H.264, 1 = H.265, -1 = none pending
    VideoStats stats;
    std::thread thread;

    // Everything below is touched only by the video thread once it is running.
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain1> swapChain;
    ComPtr<ID3D11RenderTargetView> backBufferView;
    ComPtr<ID3D11VertexShader> vertexShader;
    ComPtr<ID3D11PixelShader> pixelShader;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11Buffer> paramsBuffer;
    ComPtr<ID3D11Texture2D> frameTexture;
    ComPtr<ID3D11ShaderResourceView> lumaView;
    ComPtr<ID3D11ShaderResourceView> chromaView;
    DXGI_FORMAT frameFormat = DXGI_FORMAT_UNKNOWN;
    UINT frameTexWidth = 0;
    UINT frameTexHeight = 0;
    UINT backWidth = 0;
    UINT backHeight = 0;
    bool deviceHasVideoSupport = false;
    bool tearingSupported = false;

    bool hasPicture = false;
    int pictureWidth = 0;
    int pictureHeight = 0;
    ShaderParams params{};
    std::vector<uint8_t> uploadBuffer;

    AVBufferRef* hwDevice = nullptr;
    AVCodecContext* codec = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;
    bool codecIsH265 = false;
    bool usingHardware = false;
    unsigned decodeErrors = 0;

    ULONGLONG statsWindowStart = 0;
    unsigned statsFrames = 0;
    uint64_t statsBytes = 0;

    bool InitGraphics();
    bool CompileShaders();
    void InitHardwareDecoding();
    void Loop();
    void OpenDecoder(bool h265);
    void CloseDecoder();
    void Decode(const Packet& pkt);
    bool UploadHardwareFrame(const AVFrame* f);
    bool UploadSoftwareFrame(const AVFrame* f);
    bool EnsureFrameTexture(UINT width, UINT height, DXGI_FORMAT format);
    void SetPicture(const AVFrame* f, UINT texWidth, UINT texHeight);
    void ResizeSwapChain();
    void Render();
    void ClearPicture();
    void UpdateStats(size_t packetBytes, bool frameShown);
    static AVPixelFormat GetFormat(AVCodecContext* ctx, const AVPixelFormat* formats);
};

VideoPipeline::VideoPipeline() : impl_(std::make_unique<Impl>()) {}

VideoPipeline::~VideoPipeline() {
    Stop();
}

bool VideoPipeline::Start(HWND videoWindow, HWND notify, DecoderMode mode, bool vsync) {
    Impl& s = *impl_;
    if (s.thread.joinable()) {
        return true;
    }
    s.hwnd = videoWindow;
    s.notify = notify;
    s.mode = mode;
    s.vsync = vsync;
    s.stop = false;

    av_log_set_level(AV_LOG_WARNING);
    av_log_set_callback(FfmpegLog);

    if (!s.InitGraphics()) {
        return false;
    }
    s.InitHardwareDecoding();
    s.frame = av_frame_alloc();
    s.packet = av_packet_alloc();
    if (!s.frame || !s.packet) {
        LOGE("[VIDEO] out of memory allocating FFmpeg frame/packet");
        return false;
    }
    s.thread = std::thread([&s] { s.Loop(); });
    SetThreadDescription(s.thread.native_handle(), L"AirPlay_FFmpegVideoDecoder_Decoding_Loop");
    return true;
}

void VideoPipeline::Stop() {
    Impl& s = *impl_;
    s.frames.Stop();
    if (s.thread.joinable()) {
        {
            std::lock_guard<std::mutex> lock(s.mutex);
            s.stop = true;
        }
        s.wake.notify_all();
        s.thread.join();
    }
    s.CloseDecoder();
    av_frame_free(&s.frame);
    av_packet_free(&s.packet);
    av_buffer_unref(&s.hwDevice);
    s.lumaView.Reset();
    s.chromaView.Reset();
    s.frameTexture.Reset();
    s.backBufferView.Reset();
    s.swapChain.Reset();
    s.paramsBuffer.Reset();
    s.sampler.Reset();
    s.pixelShader.Reset();
    s.vertexShader.Reset();
    if (s.context) {
        // D3D11 defers destruction. A new flip-model swap chain cannot be created on
        // this window until the old one is really gone, so force it now.
        s.context->ClearState();
        s.context->Flush();
    }
    s.context.Reset();
    s.device.Reset();
    s.frameFormat = DXGI_FORMAT_UNKNOWN;
    s.frameTexWidth = s.frameTexHeight = 0;
    s.pictureWidth = s.pictureHeight = 0;
    if (s.hasPicture) {
        // The thread may have exited before handling a pending reset.
        s.hasPicture = false;
        PostMessageW(s.notify, WM_APP_VIDEO_PICTURE, 0, 0);
    }
    std::lock_guard<std::mutex> lock(s.mutex);
    s.queue.clear();
    s.queuedBytes = 0;
    s.resetRequested = s.resizePending = s.redrawPending = false;
    s.codecRequest = -1;
    s.stats = VideoStats{};
}

bool VideoPipeline::SetCodec(bool h265) {
    Impl& s = *impl_;
    s.frames.Invalidate();
    if (!avcodec_find_decoder(h265 ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264)) {
        LOGE("[VIDEO] no %s decoder in this FFmpeg build", h265 ? "H.265" : "H.264");
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.codecRequest = h265 ? 1 : 0;
    }
    s.wake.notify_all();
    return true;
}

void VideoPipeline::Push(const uint8_t* data, int size, uint64_t ptsNs) {
    Impl& s = *impl_;
    Packet pkt;
    pkt.stamp = s.frames.Stamp();
    pkt.data.assign(data, data + size);
    pkt.pts = ptsNs;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (s.queuedBytes > kMaxQueuedBytes) {
            LOGW("[VIDEO] decoder is not keeping up, dropping %zu queued packets", s.queue.size());
            s.queue.clear();
            s.queuedBytes = 0;
        }
        s.queuedBytes += pkt.data.size();
        s.queue.push_back(std::move(pkt));
    }
    s.wake.notify_all();
}

void VideoPipeline::Reset() {
    Impl& s = *impl_;
    s.frames.Invalidate();
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.queue.clear();
        s.queuedBytes = 0;
        s.codecRequest = -1;
        s.resetRequested = true;
    }
    s.wake.notify_all();
}

void VideoPipeline::NotifyResize() {
    Impl& s = *impl_;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.resizePending = true;
    }
    s.wake.notify_all();
}

void VideoPipeline::RequestRedraw() {
    Impl& s = *impl_;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.redrawPending = true;
    }
    s.wake.notify_all();
}

void VideoPipeline::SetVSync(bool vsync) {
    impl_->vsync = vsync;
    LOGI("[CFG] applying VSync to renderer = %d", vsync ? 1 : 0);
}

VideoStats VideoPipeline::Stats() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->stats;
}

FrameServer& VideoPipeline::Frames() { return impl_->frames; }

bool VideoPipeline::HardwareHevcAvailable() {
    static const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_VIDEO_SUPPORT, levels,
                                   ARRAYSIZE(levels), D3D11_SDK_VERSION, &device, nullptr, &context);
    if (FAILED(hr)) {
        LOGW("[VIDEO] capability check: no D3D11 video device (hr=0x%08lX)", hr);
        return false;
    }
    ComPtr<ID3D11VideoDevice> video;
    if (FAILED(device.As(&video))) {
        return false;
    }
    const UINT count = video->GetVideoDecoderProfileCount();
    for (UINT i = 0; i < count; ++i) {
        GUID profile{};
        if (SUCCEEDED(video->GetVideoDecoderProfile(i, &profile)) && profile == D3D11_DECODER_PROFILE_HEVC_VLD_MAIN) {
            return true;
        }
    }
    return false;
}

bool VideoPipeline::Impl::InitGraphics() {
    HRESULT hr = CreateDevice(device, context, &deviceHasVideoSupport);
    if (FAILED(hr)) {
        LOGE("[VIDEO] Init failed: no D3D device (hr=0x%08lX)", hr);
        return false;
    }
    // FFmpeg's hardware decoder shares this device, so let D3D serialize access.
    ComPtr<ID3D10Multithread> multithread;
    if (SUCCEEDED(context.As(&multithread))) {
        multithread->SetMultithreadProtected(TRUE);
    }

    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(hr = device.As(&dxgiDevice)) || FAILED(hr = dxgiDevice->GetAdapter(&adapter)) ||
        FAILED(hr = adapter->GetParent(IID_PPV_ARGS(&factory)))) {
        LOGE("[VIDEO] could not reach the DXGI factory (hr=0x%08lX)", hr);
        return false;
    }
    ComPtr<IDXGIFactory5> factory5;
    BOOL allowTearing = FALSE;
    if (SUCCEEDED(factory.As(&factory5)) &&
        SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing, sizeof(allowTearing)))) {
        tearingSupported = allowTearing != FALSE;
    }

    RECT client{};
    GetClientRect(hwnd, &client);
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = static_cast<UINT>(std::max<LONG>(client.right - client.left, 8));
    desc.Height = static_cast<UINT>(std::max<LONG>(client.bottom - client.top, 8));
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    desc.Flags = tearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    hr = factory->CreateSwapChainForHwnd(device.Get(), hwnd, &desc, nullptr, nullptr, &swapChain);
    if (FAILED(hr)) {
        LOGE("[VIDEO] CreateSwapChainForHwnd failed hr=0x%08lX", hr);
        return false;
    }
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    backWidth = desc.Width;
    backHeight = desc.Height;

    if (!CompileShaders()) {
        return false;
    }

    D3D11_SAMPLER_DESC samplerDesc{};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
    hr = device->CreateSamplerState(&samplerDesc, &sampler);
    if (FAILED(hr)) {
        LOGE("[VIDEO][shader] CreateSamplerState failed hr=0x%08lX", hr);
        return false;
    }

    D3D11_BUFFER_DESC bufferDesc{};
    bufferDesc.ByteWidth = sizeof(ShaderParams);
    bufferDesc.Usage = D3D11_USAGE_DEFAULT;
    bufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    hr = device->CreateBuffer(&bufferDesc, nullptr, &paramsBuffer);
    if (FAILED(hr)) {
        LOGE("[VIDEO][shader] CreateBuffer (cbuffer) failed hr=0x%08lX", hr);
        return false;
    }
    LOGI("[VIDEO][shader] pipeline ready (NV12->RGB, BT.709), tearing=%d videoDevice=%d", tearingSupported ? 1 : 0,
         deviceHasVideoSupport ? 1 : 0);
    return true;
}

bool VideoPipeline::Impl::CompileShaders() {
    ComPtr<ID3DBlob> code;
    ComPtr<ID3DBlob> errors;
    HRESULT hr = D3DCompile(kVertexShader, strlen(kVertexShader), "vs", nullptr, nullptr, "main", "vs_5_0", 0, 0, &code,
                            &errors);
    if (FAILED(hr)) {
        LOGE("[VIDEO][shader] vertex shader compile failed hr=0x%08lX %s", hr,
             errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
        return false;
    }
    hr = device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &vertexShader);
    if (FAILED(hr)) {
        LOGE("[VIDEO][shader] CreateVertexShader failed hr=0x%08lX", hr);
        return false;
    }
    code.Reset();
    errors.Reset();
    hr = D3DCompile(kPixelShader, strlen(kPixelShader), "ps", nullptr, nullptr, "main", "ps_5_0", 0, 0, &code, &errors);
    if (FAILED(hr)) {
        LOGE("[VIDEO][shader] pixel shader compile failed hr=0x%08lX %s", hr,
             errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
        return false;
    }
    hr = device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &pixelShader);
    if (FAILED(hr)) {
        LOGE("[VIDEO][shader] CreatePixelShader failed hr=0x%08lX", hr);
        return false;
    }
    return true;
}

// Wraps our own D3D11 device in an FFmpeg hardware context so decoded surfaces
// live on the device that draws them.
void VideoPipeline::Impl::InitHardwareDecoding() {
    if (mode == DecoderMode::Software) {
        LOGI("[VIDEO] decoder mode: software");
        return;
    }
    if (!deviceHasVideoSupport) {
        LOGW("[VIDEO] D3D11 device has no video support, using software decode");
        return;
    }
    AVBufferRef* ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (!ref) {
        LOGW("[VIDEO] av_hwdevice_ctx_alloc(D3D11VA) failed, using software decode");
        return;
    }
    auto* deviceCtx = reinterpret_cast<AVHWDeviceContext*>(ref->data);
    auto* d3d = static_cast<AVD3D11VADeviceContext*>(deviceCtx->hwctx);
    d3d->device = device.Get();
    d3d->device->AddRef();  // FFmpeg releases this reference when the context is freed
    const int result = av_hwdevice_ctx_init(ref);
    if (result < 0) {
        LOGW("[VIDEO] av_hwdevice_ctx_init failed (%d), using software decode", result);
        av_buffer_unref(&ref);
        return;
    }
    hwDevice = ref;
    LOGI("[VIDEO] decoder mode: %s, D3D11VA hardware decode available",
         mode == DecoderMode::Hardware ? "hardware" : "auto");
}

AVPixelFormat VideoPipeline::Impl::GetFormat(AVCodecContext* ctx, const AVPixelFormat* formats) {
    auto* self = static_cast<Impl*>(ctx->opaque);
    for (const AVPixelFormat* p = formats; *p != AV_PIX_FMT_NONE; ++p) {
        if (*p == AV_PIX_FMT_D3D11) {
            self->usingHardware = true;
            return *p;
        }
    }
    // No D3D11 surface format offered: take the first software format.
    self->usingHardware = false;
    for (const AVPixelFormat* p = formats; *p != AV_PIX_FMT_NONE; ++p) {
        const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(*p);
        if (desc && !(desc->flags & AV_PIX_FMT_FLAG_HWACCEL)) {
            LOGW("[VIDEO] hardware decode not offered for this stream, using software format %s", desc->name);
            return *p;
        }
    }
    return AV_PIX_FMT_NONE;
}

void VideoPipeline::Impl::OpenDecoder(bool h265) {
    CloseDecoder();
    const AVCodec* decoder = avcodec_find_decoder(h265 ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264);
    if (!decoder) {
        LOGE("[VIDEO] no %s decoder", h265 ? "H.265" : "H.264");
        return;
    }
    codec = avcodec_alloc_context3(decoder);
    if (!codec) {
        LOGE("[VIDEO] avcodec_alloc_context3 failed");
        return;
    }
    codec->opaque = this;
    codec->flags |= AV_CODEC_FLAG_LOW_DELAY | AV_CODEC_FLAG_COPY_OPAQUE;
    // Slice threads add no frame delay; frame threads would add one frame per thread.
    codec->thread_type = FF_THREAD_SLICE;
    codec->thread_count = 0;
    usingHardware = false;
    if (hwDevice) {
        codec->hw_device_ctx = av_buffer_ref(hwDevice);
        codec->get_format = GetFormat;
        codec->thread_count = 1;
    }
    const int result = avcodec_open2(codec, decoder, nullptr);
    if (result < 0) {
        LOGE("[VIDEO] avcodec_open2 failed (%d) for %s", result, h265 ? "H.265" : "H.264");
        avcodec_free_context(&codec);
        return;
    }
    codecIsH265 = h265;
    decodeErrors = 0;
    LOGI("[VIDEO] decoder opened: %s (%s)", h265 ? "H.265" : "H.264", hwDevice ? "D3D11VA preferred" : "software");
}

void VideoPipeline::Impl::CloseDecoder() {
    if (codec) {
        avcodec_free_context(&codec);
    }
    usingHardware = false;
}

void VideoPipeline::Impl::Loop() {
    statsWindowStart = GetTickCount64();
    for (;;) {
        Packet pkt;
        bool havePacket = false;
        bool doReset = false;
        bool doResize = false;
        bool doRedraw = false;
        bool skipRender = false;
        int codecChange = -1;
        {
            std::unique_lock<std::mutex> lock(mutex);
            wake.wait(lock, [this] {
                return stop || resetRequested || resizePending || redrawPending || codecRequest >= 0 || !queue.empty();
            });
            if (stop) {
                break;
            }
            doReset = resetRequested;
            doResize = resizePending;
            doRedraw = redrawPending;
            codecChange = codecRequest;
            resetRequested = resizePending = redrawPending = false;
            codecRequest = -1;
            if (!queue.empty()) {
                pkt = std::move(queue.front());
                queue.pop_front();
                queuedBytes -= pkt.data.size();
                havePacket = true;
                skipRender = queue.size() >= kRenderBacklogLimit;
            }
        }
        if (doReset) {
            CloseDecoder();
            ClearPicture();
        }
        if (codecChange >= 0) {
            OpenDecoder(codecChange == 1);
        }
        if (doResize) {
            ResizeSwapChain();
        }
        if (havePacket) {
            const bool hadPicture = hasPicture;
            const int oldWidth = pictureWidth;
            const int oldHeight = pictureHeight;
            bool shown = false;
            if (codec) {
                const size_t bytes = pkt.data.size();
                Decode(pkt);
                shown = hasPicture;
                if (shown && !skipRender) {
                    Render();
                }
                UpdateStats(bytes, shown);
            }
            if (hasPicture && (!hadPicture || oldWidth != pictureWidth || oldHeight != pictureHeight)) {
                LOGI("[VIDEO] picture %dx%d %s (%s)", pictureWidth, pictureHeight, codecIsH265 ? "H.265" : "H.264",
                     usingHardware ? "D3D11" : "FFmpeg");
                PostMessageW(notify, WM_APP_VIDEO_PICTURE, 1, MAKELPARAM(pictureWidth, pictureHeight));
            }
        } else if (doResize || doRedraw || doReset) {
            Render();
        }
    }
}

void VideoPipeline::Impl::Decode(const Packet& pkt) {
    if (av_new_packet(packet, static_cast<int>(pkt.data.size())) < 0) {
        LOGE("[VIDEO] av_new_packet failed for %zu bytes", pkt.data.size());
        return;
    }
    memcpy(packet->data, pkt.data.data(), pkt.data.size());
    packet->pts = static_cast<int64_t>(pkt.pts);
    packet->opaque_ref = av_buffer_alloc(sizeof(FrameStamp));
    if (packet->opaque_ref) memcpy(packet->opaque_ref->data, &pkt.stamp, sizeof(FrameStamp));
    int result = avcodec_send_packet(codec, packet);
    av_packet_unref(packet);
    if (result < 0 && result != AVERROR(EAGAIN)) {
        // Errors are expected until the first key frame; log the first few only.
        if (decodeErrors++ < 5) {
            LOGW("[VIDEO] avcodec_send_packet failed (%d)", result);
        }
        return;
    }
    for (;;) {
        result = avcodec_receive_frame(codec, frame);
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
            break;
        }
        if (result < 0) {
            if (decodeErrors++ < 5) {
                LOGW("[VIDEO] avcodec_receive_frame failed (%d)", result);
            }
            break;
        }
        const bool ok = frame->format == AV_PIX_FMT_D3D11 ? UploadHardwareFrame(frame) : UploadSoftwareFrame(frame);
        if (ok) {
            hasPicture = true;
            if (frame->opaque_ref && frame->opaque_ref->size == sizeof(FrameStamp)) {
                FrameStamp stamp;
                memcpy(&stamp, frame->opaque_ref->data, sizeof(stamp));
                frames.Publish(frame, stamp);
                if(stamp.generation!=controlFrameGeneration && stamp.generation==frames.Stamp().generation) {
                    controlFrameGeneration=stamp.generation;
                    PostMessageW(notify,WM_APP_CONTROL_FRAME,static_cast<WPARAM>(stamp.generation),MAKELPARAM(frame->width,frame->height));
                }
            }
        }
        av_frame_unref(frame);
    }
}

bool VideoPipeline::Impl::EnsureFrameTexture(UINT width, UINT height, DXGI_FORMAT format) {
    if (frameTexture && frameTexWidth == width && frameTexHeight == height && frameFormat == format) {
        return true;
    }
    DXGI_FORMAT lumaFormat;
    DXGI_FORMAT chromaFormat;
    if (format == DXGI_FORMAT_NV12) {
        lumaFormat = DXGI_FORMAT_R8_UNORM;
        chromaFormat = DXGI_FORMAT_R8G8_UNORM;
    } else if (format == DXGI_FORMAT_P010) {
        lumaFormat = DXGI_FORMAT_R16_UNORM;
        chromaFormat = DXGI_FORMAT_R16G16_UNORM;
    } else {
        LOGE("[VIDEO] unsupported GPU frame format %d", static_cast<int>(format));
        return false;
    }
    lumaView.Reset();
    chromaView.Reset();
    frameTexture.Reset();

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    HRESULT hr = device->CreateTexture2D(&desc, nullptr, &frameTexture);
    if (FAILED(hr)) {
        LOGE("[VIDEO][shader] CreateTexture2D (%ux%u format %d) failed hr=0x%08lX", width, height,
             static_cast<int>(format), hr);
        return false;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
    viewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    viewDesc.Texture2D.MipLevels = 1;
    viewDesc.Format = lumaFormat;
    hr = device->CreateShaderResourceView(frameTexture.Get(), &viewDesc, &lumaView);
    if (FAILED(hr)) {
        LOGE("[VIDEO][shader] CreateShaderResourceView (Y) failed hr=0x%08lX", hr);
        frameTexture.Reset();
        return false;
    }
    viewDesc.Format = chromaFormat;
    hr = device->CreateShaderResourceView(frameTexture.Get(), &viewDesc, &chromaView);
    if (FAILED(hr)) {
        LOGE("[VIDEO][shader] CreateShaderResourceView (UV) failed hr=0x%08lX", hr);
        lumaView.Reset();
        frameTexture.Reset();
        return false;
    }
    frameTexWidth = width;
    frameTexHeight = height;
    frameFormat = format;
    LOGI("[VIDEO][shader] frame texture (re)created %ux%u format %d", width, height, static_cast<int>(format));
    return true;
}

void VideoPipeline::Impl::SetPicture(const AVFrame* f, UINT texWidth, UINT texHeight) {
    pictureWidth = f->width;
    pictureHeight = f->height;
    // The decoder surface is padded to macroblock size; sample only the visible part.
    params.uvScale[0] = static_cast<float>(f->width) / static_cast<float>(texWidth);
    params.uvScale[1] = static_cast<float>(f->height) / static_cast<float>(texHeight);
    // Unspecified range is treated as full range, which is what AirPlay mirroring sends.
    const bool limited = f->color_range == AVCOL_RANGE_MPEG;
    params.lumaRange[0] = limited ? 16.0f / 255.0f : 0.0f;
    params.lumaRange[1] = limited ? 255.0f / 219.0f : 1.0f;
    params.chromaScale = limited ? 255.0f / 224.0f : 1.0f;
}

// A D3D11VA frame is one slice of the decoder's texture array. Copy it into a
// texture we can bind as a shader resource.
bool VideoPipeline::Impl::UploadHardwareFrame(const AVFrame* f) {
    auto* source = reinterpret_cast<ID3D11Texture2D*>(f->data[0]);
    const UINT slice = static_cast<UINT>(reinterpret_cast<intptr_t>(f->data[1]));
    if (!source) {
        return false;
    }
    D3D11_TEXTURE2D_DESC desc{};
    source->GetDesc(&desc);
    if (!EnsureFrameTexture(desc.Width, desc.Height, desc.Format)) {
        return false;
    }
    context->CopySubresourceRegion(frameTexture.Get(), 0, 0, 0, 0, source, slice, nullptr);
    SetPicture(f, desc.Width, desc.Height);
    usingHardware = true;
    return true;
}

// Software frames are planar; repack them as NV12 and upload.
bool VideoPipeline::Impl::UploadSoftwareFrame(const AVFrame* f) {
    const auto format = static_cast<AVPixelFormat>(f->format);
    const bool planar8 = format == AV_PIX_FMT_YUV420P || format == AV_PIX_FMT_YUVJ420P;
    const bool planar10 = format == AV_PIX_FMT_YUV420P10LE;
    const bool nv12 = format == AV_PIX_FMT_NV12;
    if (!planar8 && !planar10 && !nv12) {
        if (decodeErrors++ < 5) {
            LOGE("[VIDEO] unsupported software pixel format %s", av_get_pix_fmt_name(format));
        }
        return false;
    }
    const UINT width = (static_cast<UINT>(f->width) + 1) & ~1u;
    const UINT height = (static_cast<UINT>(f->height) + 1) & ~1u;
    if (!EnsureFrameTexture(width, height, DXGI_FORMAT_NV12)) {
        return false;
    }
    const UINT chromaWidth = width / 2;
    const UINT chromaHeight = height / 2;
    const UINT srcChromaWidth = (static_cast<UINT>(f->width) + 1) / 2;
    const UINT srcChromaHeight = (static_cast<UINT>(f->height) + 1) / 2;
    uploadBuffer.resize(static_cast<size_t>(width) * height * 3 / 2);
    uint8_t* dstY = uploadBuffer.data();
    uint8_t* dstUV = dstY + static_cast<size_t>(width) * height;

    for (UINT y = 0; y < height; ++y) {
        const UINT srcRow = std::min<UINT>(y, static_cast<UINT>(f->height) - 1);
        uint8_t* dst = dstY + static_cast<size_t>(y) * width;
        if (planar10) {
            const auto* src = reinterpret_cast<const uint16_t*>(f->data[0] + static_cast<ptrdiff_t>(srcRow) * f->linesize[0]);
            for (UINT x = 0; x < static_cast<UINT>(f->width); ++x) {
                dst[x] = static_cast<uint8_t>(src[x] >> 2);
            }
        } else {
            memcpy(dst, f->data[0] + static_cast<ptrdiff_t>(srcRow) * f->linesize[0], static_cast<size_t>(f->width));
        }
        if (static_cast<UINT>(f->width) < width) {
            dst[width - 1] = dst[width - 2];
        }
    }
    for (UINT y = 0; y < chromaHeight; ++y) {
        const UINT srcRow = std::min<UINT>(y, srcChromaHeight - 1);
        uint8_t* dst = dstUV + static_cast<size_t>(y) * width;
        if (nv12) {
            memcpy(dst, f->data[1] + static_cast<ptrdiff_t>(srcRow) * f->linesize[1], static_cast<size_t>(srcChromaWidth) * 2);
        } else if (planar10) {
            const auto* u = reinterpret_cast<const uint16_t*>(f->data[1] + static_cast<ptrdiff_t>(srcRow) * f->linesize[1]);
            const auto* v = reinterpret_cast<const uint16_t*>(f->data[2] + static_cast<ptrdiff_t>(srcRow) * f->linesize[2]);
            for (UINT x = 0; x < srcChromaWidth && x < chromaWidth; ++x) {
                dst[2 * x] = static_cast<uint8_t>(u[x] >> 2);
                dst[2 * x + 1] = static_cast<uint8_t>(v[x] >> 2);
            }
        } else {
            const uint8_t* u = f->data[1] + static_cast<ptrdiff_t>(srcRow) * f->linesize[1];
            const uint8_t* v = f->data[2] + static_cast<ptrdiff_t>(srcRow) * f->linesize[2];
            for (UINT x = 0; x < srcChromaWidth && x < chromaWidth; ++x) {
                dst[2 * x] = u[x];
                dst[2 * x + 1] = v[x];
            }
        }
    }
    // For NV12 the chroma plane follows the luma plane at the same row pitch.
    context->UpdateSubresource(frameTexture.Get(), 0, nullptr, uploadBuffer.data(), width, 0);
    SetPicture(f, width, height);
    // yuvj420p is FFmpeg's legacy way of saying "full range".
    if (format == AV_PIX_FMT_YUVJ420P) {
        params.lumaRange[0] = 0.0f;
        params.lumaRange[1] = 1.0f;
        params.chromaScale = 1.0f;
    }
    usingHardware = false;
    return true;
}

void VideoPipeline::Impl::ResizeSwapChain() {
    if (!swapChain) {
        return;
    }
    RECT client{};
    GetClientRect(hwnd, &client);
    const UINT width = static_cast<UINT>(std::max<LONG>(client.right - client.left, 8));
    const UINT height = static_cast<UINT>(std::max<LONG>(client.bottom - client.top, 8));
    if (width == backWidth && height == backHeight && backBufferView) {
        return;
    }
    context->OMSetRenderTargets(0, nullptr, nullptr);
    backBufferView.Reset();
    const HRESULT hr = swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN,
                                                tearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    if (FAILED(hr)) {
        LOGE("[VIDEO] ResizeBuffers(%ux%u) failed hr=0x%08lX", width, height, hr);
        return;
    }
    backWidth = width;
    backHeight = height;
}

void VideoPipeline::Impl::Render() {
    if (!swapChain) {
        return;
    }
    if (!backBufferView) {
        ComPtr<ID3D11Texture2D> backBuffer;
        HRESULT hr = swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
        if (FAILED(hr)) {
            LOGE("[VIDEO][shader] swapChain GetBuffer failed hr=0x%08lX", hr);
            return;
        }
        hr = device->CreateRenderTargetView(backBuffer.Get(), nullptr, &backBufferView);
        if (FAILED(hr)) {
            LOGE("[VIDEO][shader] CreateRenderTargetView failed hr=0x%08lX", hr);
            return;
        }
    }
    const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    ID3D11RenderTargetView* target = backBufferView.Get();
    context->OMSetRenderTargets(1, &target, nullptr);
    context->ClearRenderTargetView(target, black);

    if (hasPicture && lumaView && chromaView && pictureWidth > 0 && pictureHeight > 0) {
        // Letterbox: largest rectangle with the picture's aspect ratio that fits the window.
        const float windowAspect = static_cast<float>(backWidth) / static_cast<float>(backHeight);
        const float pictureAspect = static_cast<float>(pictureWidth) / static_cast<float>(pictureHeight);
        D3D11_VIEWPORT viewport{};
        if (pictureAspect > windowAspect) {
            viewport.Width = static_cast<float>(backWidth);
            viewport.Height = viewport.Width / pictureAspect;
        } else {
            viewport.Height = static_cast<float>(backHeight);
            viewport.Width = viewport.Height * pictureAspect;
        }
        viewport.TopLeftX = (static_cast<float>(backWidth) - viewport.Width) * 0.5f;
        viewport.TopLeftY = (static_cast<float>(backHeight) - viewport.Height) * 0.5f;
        viewport.MaxDepth = 1.0f;
        context->RSSetViewports(1, &viewport);

        context->UpdateSubresource(paramsBuffer.Get(), 0, nullptr, &params, 0, 0);
        ID3D11ShaderResourceView* views[2] = {lumaView.Get(), chromaView.Get()};
        ID3D11SamplerState* samplers[1] = {sampler.Get()};
        ID3D11Buffer* buffers[1] = {paramsBuffer.Get()};
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vertexShader.Get(), nullptr, 0);
        context->PSSetShader(pixelShader.Get(), nullptr, 0);
        context->PSSetShaderResources(0, 2, views);
        context->PSSetSamplers(0, 1, samplers);
        context->PSSetConstantBuffers(0, 1, buffers);
        context->Draw(3, 0);
        // Unbind so the next frame can be copied into the texture.
        ID3D11ShaderResourceView* none[2] = {nullptr, nullptr};
        context->PSSetShaderResources(0, 2, none);
    }

    const bool sync = vsync.load();
    const HRESULT hr = swapChain->Present(sync ? 1 : 0, (!sync && tearingSupported) ? DXGI_PRESENT_ALLOW_TEARING : 0);
    if (FAILED(hr) && hr != DXGI_ERROR_WAS_STILL_DRAWING) {
        if (decodeErrors++ < 5) {
            LOGE("[VIDEO] Present failed hr=0x%08lX", hr);
        }
    }
}

void VideoPipeline::Impl::ClearPicture() {
    const bool had = hasPicture;
    hasPicture = false;
    pictureWidth = 0;
    pictureHeight = 0;
    {
        std::lock_guard<std::mutex> lock(mutex);
        stats = VideoStats{};
    }
    statsFrames = 0;
    statsBytes = 0;
    statsWindowStart = GetTickCount64();
    if (had) {
        PostMessageW(notify, WM_APP_VIDEO_PICTURE, 0, 0);
    }
}

void VideoPipeline::Impl::UpdateStats(size_t packetBytes, bool frameShown) {
    statsBytes += packetBytes;
    if (frameShown) {
        ++statsFrames;
    }
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG elapsed = now - statsWindowStart;
    if (elapsed < 1000) {
        return;
    }
    VideoStats next;
    next.fps = statsFrames * 1000.0 / static_cast<double>(elapsed);
    next.bitrateMbps = static_cast<double>(statsBytes) * 8.0 / 1e6 * 1000.0 / static_cast<double>(elapsed);
    next.width = pictureWidth;
    next.height = pictureHeight;
    next.h265 = codecIsH265;
    next.hardware = usingHardware;
    next.hasPicture = hasPicture;
    {
        std::lock_guard<std::mutex> lock(mutex);
        stats = next;
    }
    statsFrames = 0;
    statsBytes = 0;
    statsWindowStart = now;
}
