#include "frame_server.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
extern "C" {
#include <libavutil/frame.h>
}

void Require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
std::string ReadFrame() {
    const std::wstring name = L"\\\\.\\pipe\\AirWindows.Frames." + std::to_wstring(GetCurrentProcessId());
    HANDLE pipe = INVALID_HANDLE_VALUE;
    for (int tries = 0; tries < 100 && pipe == INVALID_HANDLE_VALUE; ++tries) {
        pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) Sleep(10);
    }
    Require(pipe != INVALID_HANDLE_VALUE, "pipe unavailable");
    auto read = [pipe](void* data, DWORD size) {
        auto* cursor = static_cast<char*>(data);
        while (size) {
            DWORD count = 0;
            Require(ReadFile(pipe, cursor, size, &count, nullptr) && count, "short pipe read");
            cursor += count; size -= count;
        }
    };
    uint32_t size = 0; read(&size, sizeof(size));
    Require(size < 8192, "metadata length");
    std::string meta(size, '\0'); read(meta.data(), size);
    const auto pos = meta.find("\"image_bytes\":");
    Require(pos != std::string::npos, "image length missing");
    const DWORD bytes = static_cast<DWORD>(std::stoul(meta.substr(pos + 14)));
    std::vector<uint8_t> image(bytes);
    if (bytes) {
        read(image.data(), bytes);
        Require(image.size() == 8 * 8 * 3 + 11 && memcmp(image.data(), "P6\n8 8\n255\n", 11) == 0, "image geometry");
        Require(image[11] >= 126 && image[11] <= 130, "decoded pixel conversion");
    }
    DWORD count = 0; WriteFile(pipe, "K", 1, &count, nullptr); CloseHandle(pipe);
    return meta;
}
void Expected(bool fresh) {
    const std::string response = ReadFrame();
    Require(response.find(fresh ? "\"ok\":true" : "\"ok\":false") != std::string::npos, response.c_str());
}

int main() {
    try {
        FrameServer server;
        Require(server.Start(true), "server start");
        Expected(false);
        AVFrame* frame = av_frame_alloc();
        frame->format = AV_PIX_FMT_YUV420P; frame->width = frame->height = 8;
        Require(av_frame_get_buffer(frame, 32) == 0, "frame allocation");
        for (int plane = 0; plane < 3; ++plane)
            for (int y = 0; y < (plane ? 4 : 8); ++y) memset(frame->data[plane] + y * frame->linesize[plane], 128, plane ? 4 : 8);
        server.Publish(frame, server.Stamp()); Expected(true);
        server.SetControlState(true,false,true,"0000000000000001","0000000000000002");
        auto ready=ReadFrame();
        Require(ready.find("\"control_ready\":true")!=std::string::npos && ready.find("\"control_busy\":false")!=std::string::npos,"control readiness export");
        Require(ready.find("\"control_peer\":\"0000000000000001\"")!=std::string::npos && ready.find("\"control_epoch\":2")!=std::string::npos,"binding identity export");
        server.SetControlState(true,true,true,"0000000000000001","0000000000000002");
        Require(ReadFrame().find("\"control_busy\":true")!=std::string::npos,"busy action export");
        server.SetControlState(true,false,true,"invalid\"token","0000000000000002");
        auto invalid=ReadFrame();
        Require(invalid.find("\"control_ready\":false")!=std::string::npos && invalid.find("\"control_peer\":\"\"")!=std::string::npos,"invalid identity cannot be ready or enter JSON");
        auto old = server.Stamp();
        server.SetPaused(true); Expected(false);
        server.Publish(frame, old); Expected(false);
        server.SetPaused(false); Expected(false);
        server.Publish(frame, old); Expected(false);
        server.Publish(frame, server.Stamp()); Expected(true);
        Sleep(1100); Expected(false);
        server.Publish(frame, server.Stamp()); Expected(true);
        old = server.Stamp();
        server.SetActive(false); Expected(false);
        server.SetActive(true); server.Publish(frame, old); Expected(false);
        server.Publish(frame, server.Stamp()); Expected(true);
        old = server.Stamp();
        server.SetGeometry(9, 8); server.Publish(frame, old); Expected(false);
        server.Publish(frame, server.Stamp()); Expected(true);
        old = server.Stamp(); server.Invalidate(); server.Publish(frame, old); Expected(false);
        server.Stop();
        Require(server.Start(true), "server restart"); Expected(false);
        server.Publish(frame, server.Stamp()); Expected(true);
        server.Stop(); av_frame_free(&frame);
        puts("PASS: fresh pixels, idle, pause/resume, stale packets, timeout, disconnect/reconnect, geometry, invalidation, restart");
        return 0;
    } catch (const std::exception& error) { fprintf(stderr, "FAIL: %s\n", error.what()); return 1; }
}
