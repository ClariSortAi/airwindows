#include "frame_server.h"
#include <windows.h>
#include <sddl.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
}
#include "log.h"

namespace {
constexpr uint64_t kMaxAgeMs = 1000;
struct FrameOwner {
    AVFrame* value = nullptr;
    ~FrameOwner() { av_frame_free(&value); }
};

// Match the renderer's BT.709 conversion. Export visible pixels, without
// decoder padding, scaling, window chrome or letterboxing.
bool ToPpm(const AVFrame* source, std::vector<uint8_t>& output) {
    FrameOwner downloaded;
    const AVFrame* f = source;
    if (source->format == AV_PIX_FMT_D3D11) {
        downloaded.value = av_frame_alloc();
        if (!downloaded.value || av_hwframe_transfer_data(downloaded.value, source, 0) < 0) return false;
        av_frame_copy_props(downloaded.value, source);
        f = downloaded.value;
    }
    const bool nv12 = f->format == AV_PIX_FMT_NV12;
    const bool p010 = f->format == AV_PIX_FMT_P010LE;
    const bool planar10 = f->format == AV_PIX_FMT_YUV420P10LE;
    const bool planar8 = f->format == AV_PIX_FMT_YUV420P || f->format == AV_PIX_FMT_YUVJ420P;
    if ((!nv12 && !p010 && !planar10 && !planar8) || f->width <= 0 || f->height <= 0 ||
        f->width > 4096 || f->height > 4096) return false;
    const std::string header = "P6\n" + std::to_string(f->width) + " " + std::to_string(f->height) + "\n255\n";
    output.assign(header.begin(), header.end());
    output.resize(header.size() + static_cast<size_t>(f->width) * f->height * 3);
    uint8_t* dst = output.data() + header.size();
    const bool limited = f->color_range == AVCOL_RANGE_MPEG && f->format != AV_PIX_FMT_YUVJ420P;
    auto byte = [](float value) { return static_cast<uint8_t>(std::clamp(value, 0.0f, 255.0f) + 0.5f); };
    for (int y = 0; y < f->height; ++y) {
        const uint8_t* rowY = f->data[0] + static_cast<ptrdiff_t>(y) * f->linesize[0];
        const uint8_t* rowU = f->data[1] + static_cast<ptrdiff_t>(y / 2) * f->linesize[1];
        const uint8_t* rowV = (nv12 || p010) ? nullptr : f->data[2] + static_cast<ptrdiff_t>(y / 2) * f->linesize[2];
        for (int x = 0; x < f->width; ++x) {
            float yy, u, v;
            if (p010 || planar10) {
                const float scale = p010 ? 255.0f / 65535.0f : 255.0f / 1023.0f;
                yy = reinterpret_cast<const uint16_t*>(rowY)[x] * scale;
                u = reinterpret_cast<const uint16_t*>(rowU)[p010 ? (x / 2) * 2 : x / 2] * scale;
                v = reinterpret_cast<const uint16_t*>(p010 ? rowU : rowV)[p010 ? (x / 2) * 2 + 1 : x / 2] * scale;
            } else {
                yy = rowY[x];
                u = rowU[nv12 ? (x / 2) * 2 : x / 2];
                v = nv12 ? rowU[(x / 2) * 2 + 1] : rowV[x / 2];
            }
            yy = limited ? (yy - 16.0f) * (255.0f / 219.0f) : yy;
            u = (u - 127.5f) * (limited ? 255.0f / 224.0f : 1.0f);
            v = (v - 127.5f) * (limited ? 255.0f / 224.0f : 1.0f);
            *dst++ = byte(yy + 1.5748f * v);
            *dst++ = byte(yy - 0.1873f * u - 0.4681f * v);
            *dst++ = byte(yy + 1.8556f * u);
        }
    }
    return true;
}

PSECURITY_DESCRIPTOR UserOnlySecurity() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return nullptr;
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<uint8_t> bytes(size);
    const bool ok = GetTokenInformation(token, TokenUser, bytes.data(), size, &size) != FALSE;
    CloseHandle(token);
    if (!ok) return nullptr;
    LPWSTR sid = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid, &sid)) return nullptr;
    const std::wstring sddl = L"D:P(A;;GA;;;" + std::wstring(sid) + L")";
    LocalFree(sid);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) return nullptr;
    return descriptor;
}
}

struct FrameServer::Impl {
    mutable std::mutex mutex;
    AVFrame* latest = nullptr;
    uint64_t generation = 1, sequence = 0, receivedMs = 0, decodedMs = 0, instance = 0;
    int width = 0, height = 0;
    bool active = false, paused = false, selfTest = false;
    std::atomic<bool> enabled{false};
    HANDLE stopEvent = nullptr;
    std::thread worker;

    void Clear() { av_frame_free(&latest); receivedMs = decodedMs = 0; ++generation; }
    bool Fresh() const {
        const uint64_t now = GetTickCount64();
        return active && !paused && latest && receivedMs && now >= receivedMs && now >= decodedMs &&
            now - receivedMs <= kMaxAgeMs && now - decodedMs <= kMaxAgeMs;
    }

    std::string Snapshot(std::vector<uint8_t>& image) {
        FrameOwner copy;
        uint64_t gen, seq, received, decoded;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!Fresh()) return "{\"ok\":false,\"error\":\"no_fresh_active_frame\",\"image_bytes\":0}";
            copy.value = av_frame_clone(latest);
            gen = generation; seq = sequence; received = receivedMs; decoded = decodedMs;
        }
        if (!copy.value || !ToPpm(copy.value, image)) {
            image.clear();
            return "{\"ok\":false,\"error\":\"frame_conversion_failed\",\"image_bytes\":0}";
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!Fresh() || gen != generation || GetTickCount64() - received > kMaxAgeMs) {
                image.clear();
                return "{\"ok\":false,\"error\":\"frame_invalidated\",\"image_bytes\":0}";
            }
        }
        return "{\"ok\":true,\"protocol\":1,\"source\":\"" + std::string(selfTest ? "selftest" : "airplay") +
            "\",\"receiver_pid\":" + std::to_string(GetCurrentProcessId()) +
            ",\"instance\":\"" + std::to_string(instance) + "\",\"generation\":" + std::to_string(gen) +
            ",\"sequence\":" + std::to_string(seq) + ",\"received_tick_ms\":" + std::to_string(received) +
            ",\"decoded_tick_ms\":" + std::to_string(decoded) + ",\"source_pts_ns\":" + std::to_string(copy.value->pts) +
            ",\"width\":" + std::to_string(copy.value->width) + ",\"height\":" + std::to_string(copy.value->height) +
            ",\"format\":\"ppm\",\"image_bytes\":" + std::to_string(image.size()) + "}";
    }

    bool Complete(HANDLE pipe, OVERLAPPED& ov, BOOL immediate, DWORD timeout, DWORD* count) {
        if (immediate) return true;
        if (GetLastError() != ERROR_IO_PENDING) return false;
        HANDLE events[] = {stopEvent, ov.hEvent};
        if (WaitForMultipleObjects(2, events, FALSE, timeout) == WAIT_OBJECT_0 + 1)
            return GetOverlappedResult(pipe, &ov, count, FALSE) != FALSE;
        CancelIoEx(pipe, &ov);
        GetOverlappedResult(pipe, &ov, count, TRUE); // keep OVERLAPPED alive until cancellation completes
        return false;
    }

    void Loop(PSECURITY_DESCRIPTOR security) {
        SECURITY_ATTRIBUTES sa{sizeof(sa), security, FALSE};
        const std::wstring name = L"\\\\.\\pipe\\AirWindows.Frames." + std::to_wstring(GetCurrentProcessId());
        while (WaitForSingleObject(stopEvent, 0) != WAIT_OBJECT_0) {
            HANDLE pipe = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 4096, 1000, &sa);
            if (pipe == INVALID_HANDLE_VALUE) { LOGE("[FRAMES] pipe creation failed: %lu", GetLastError()); break; }
            OVERLAPPED ov{};
            ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!ov.hEvent) { CloseHandle(pipe); LOGE("[FRAMES] event creation failed"); break; }
            DWORD transferred = 0;
            BOOL connected = ConnectNamedPipe(pipe, &ov);
            const DWORD connectError = connected ? ERROR_SUCCESS : GetLastError();
            const bool ready = connected || connectError == ERROR_PIPE_CONNECTED ||
                (SetLastError(connectError), Complete(pipe, ov, FALSE, INFINITE, &transferred));
            if (ready && WaitForSingleObject(stopEvent, 0) != WAIT_OBJECT_0) {
                std::vector<uint8_t> picture;
                const std::string meta = Snapshot(picture);
                const uint32_t length = static_cast<uint32_t>(meta.size());
                std::vector<uint8_t> response(sizeof(length) + meta.size() + picture.size());
                memcpy(response.data(), &length, sizeof(length));
                memcpy(response.data() + sizeof(length), meta.data(), meta.size());
                if (!picture.empty()) memcpy(response.data() + sizeof(length) + meta.size(), picture.data(), picture.size());
                size_t offset = 0;
                const uint64_t deadline = GetTickCount64() + 1500;
                while (offset < response.size() && GetTickCount64() < deadline) {
                    ResetEvent(ov.hEvent);
                    const DWORD count = static_cast<DWORD>(std::min<size_t>(65536, response.size() - offset));
                    if (!Complete(pipe, ov, WriteFile(pipe, response.data() + offset, count, &transferred, &ov), 500, &transferred) || !transferred) break;
                    offset += transferred;
                }
                // Client acknowledges full receipt. Avoid unbounded FlushFileBuffers.
                if (offset == response.size()) {
                    char ack = 0;
                    ResetEvent(ov.hEvent);
                    Complete(pipe, ov, ReadFile(pipe, &ack, 1, &transferred, &ov), 500, &transferred);
                }
            }
            DisconnectNamedPipe(pipe);
            CloseHandle(ov.hEvent);
            CloseHandle(pipe);
        }
        LocalFree(security);
    }
};

FrameServer::FrameServer() : impl_(std::make_unique<Impl>()) {}
FrameServer::~FrameServer() { Stop(); }
bool FrameServer::Start(bool selfTest) {
    auto& s = *impl_;
    if (s.worker.joinable()) return true;
    auto security = UserOnlySecurity();
    if (!security) return false;
    s.stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!s.stopEvent) { LocalFree(security); return false; }
    FILETIME time; GetSystemTimeAsFileTime(&time);
    s.instance = (static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    s.selfTest = selfTest;
    s.enabled = true;
    SetActive(selfTest);
    s.worker = std::thread([&s, security] { s.Loop(security); });
    LOGI("[FRAMES] local user-only pipe: AirWindows.Frames.%lu", GetCurrentProcessId());
    return true;
}
void FrameServer::Stop() {
    auto& s = *impl_;
    s.enabled = false;
    if (s.worker.joinable()) { SetEvent(s.stopEvent); s.worker.join(); CloseHandle(s.stopEvent); s.stopEvent = nullptr; }
    std::lock_guard<std::mutex> lock(s.mutex);
    s.active = false; s.Clear();
}
FrameStamp FrameServer::Stamp() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return {GetTickCount64(), impl_->generation};
}
void FrameServer::Publish(const AVFrame* frame, FrameStamp stamp) {
    auto& s = *impl_;
    if (!s.enabled) return;
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.active || s.paused || stamp.generation != s.generation) return;
    if (s.latest && (s.latest->width != frame->width || s.latest->height != frame->height)) { s.Clear(); return; }
    AVFrame* copy = av_frame_clone(frame);
    if (!copy) return;
    av_frame_free(&s.latest); s.latest = copy;
    s.receivedMs = stamp.receivedMs; s.decodedMs = GetTickCount64(); ++s.sequence;
}
void FrameServer::SetActive(bool active) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->active != active) { impl_->Clear(); impl_->active = active; impl_->paused = false; }
}
void FrameServer::SetPaused(bool paused) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->paused = paused; impl_->Clear();
}
void FrameServer::Invalidate() {
    std::lock_guard<std::mutex> lock(impl_->mutex); impl_->Clear();
}
void FrameServer::SetGeometry(int width, int height) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (width != impl_->width || height != impl_->height) { impl_->Clear(); impl_->width = width; impl_->height = height; }
}
