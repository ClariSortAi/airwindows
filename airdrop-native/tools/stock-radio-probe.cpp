// Bounded diagnostics for the exact reviewed MT7922 stock driver.
// No arbitrary OIDs, writes to registers, key queries, or packet transmission.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>
#include <wlanapi.h>
#include <bcrypt.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <thread>
#include <stdexcept>

static const GUID rxProvider = {0xef7656c2,0xfaf6,0x4a18,{0x95,0xe0,0xc7,0x9a,0x91,0x6d,0xdd,0xe8}};
static const char expectedHash[] = "20112897f39752f1877ccbab9ca9b1fe6e5d554fe662b6d50973aa19f3c3d34a";

static void status(const char* operation, ULONG code) {
    std::printf("{\"operation\":\"%s\",\"status\":%lu}\n", operation, code);
    std::fflush(stdout);
}
static void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
static void verifyDriver() {
    WCHAR path[2048]{}; DWORD size = sizeof(path);
    require(RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\mtkwlex",
        L"ImagePath", RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, path, &size) == ERROR_SUCCESS,
        "Cannot resolve installed driver");
    std::wstring file(path);
    if (file.rfind(L"\\SystemRoot\\", 0) == 0) {
        WCHAR win[MAX_PATH]{}; require(GetWindowsDirectoryW(win, MAX_PATH) != 0, "Windows path unavailable");
        file = std::wstring(win) + file.substr(11);
    }
    HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    require(h != INVALID_HANDLE_VALUE, "Cannot read installed driver");
    BCRYPT_ALG_HANDLE algorithm = nullptr; BCRYPT_HASH_HANDLE hash = nullptr;
    std::vector<UCHAR> object; UCHAR digest[32]{}; ULONG length = 0, got = 0;
    bool ok = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0;
    if (ok) ok = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&length), sizeof(length), &got, 0) >= 0;
    if (ok) { object.resize(length); ok = BCryptCreateHash(algorithm, &hash, object.data(), length, nullptr, 0, 0) >= 0; }
    UCHAR bytes[16384]; DWORD read = 0;
    while (ok) {
        if (!ReadFile(h, bytes, sizeof(bytes), &read, nullptr)) { ok = false; break; }
        if (!read) break;
        ok = BCryptHashData(hash, bytes, read, 0) >= 0;
    }
    if (ok) ok = BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0;
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    CloseHandle(h);
    char hex[65]{};
    for (unsigned i = 0; i < 32; ++i) std::sprintf(hex + i * 2, "%02x", digest[i]);
    require(ok && std::strcmp(hex, expectedHash) == 0, "Installed driver differs from reviewed binary");
}

static void requireIdleAdapter() {
    HANDLE wlan = nullptr; DWORD version = 0;
    require(WlanOpenHandle(2, nullptr, &version, &wlan) == ERROR_SUCCESS, "WLAN service unavailable");
    PWLAN_INTERFACE_INFO_LIST interfaces = nullptr;
    DWORD result = WlanEnumInterfaces(wlan, nullptr, &interfaces);
    unsigned matches = 0; bool idle = false;
    if (result == ERROR_SUCCESS) {
        for (DWORD i = 0; i < interfaces->dwNumberOfItems; ++i) {
            const auto& item = interfaces->InterfaceInfo[i];
            if (std::wcsstr(item.strInterfaceDescription, L"MT7922")) {
                ++matches; idle = item.isState == wlan_interface_state_disconnected;
            }
        }
    }
    if (interfaces) WlanFreeMemory(interfaces);
    WlanCloseHandle(wlan, nullptr);
    require(matches == 1 && idle, "Capture requires exactly one disconnected MT7922");
}

class Radio {
    HANDLE device = INVALID_HANDLE_VALUE;
    bool started = false;
public:
    Radio() {
        verifyDriver();
        device = CreateFileW(L"\\\\.\\MTKWLAN", GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        require(device != INVALID_HANDLE_VALUE, "Vendor endpoint unavailable");
    }
    ~Radio() { if (started) stop(); if (device != INVALID_HANDLE_VALUE) CloseHandle(device); }
    ULONG command(ULONG operation, ULONG* data = nullptr) {
        // Verified dispatch entry 0x1401e43d0 -> FuncSniffer; fixed 28-byte payload.
        ULONG buffer[7]{}; buffer[0] = operation; buffer[2] = 6; // 2.4 GHz channel 6, 20 MHz
        ULONG oid = 0xfffcf01c, returned = 0;
        BOOL ok = DeviceIoControl(device, 0x170002, &oid, sizeof(oid), buffer, sizeof(buffer), &returned, nullptr);
        ULONG result = ok ? ERROR_SUCCESS : GetLastError();
        if (ok && returned != sizeof(buffer)) result = ERROR_INVALID_DATA;
        if (!result && data) std::memcpy(data, buffer + 4, 3 * sizeof(ULONG));
        return result;
    }
    void query() {
        ULONG counts[3]{}; ULONG result = command(2, counts);
        std::printf("{\"operation\":\"sniffer_counters\",\"status\":%lu,\"data\":%lu,\"management\":%lu,\"control\":%lu}\n",
            result, counts[0], counts[1], counts[2]); std::fflush(stdout);
    }
    void start() { ULONG result = command(0); status("sniffer_start", result); require(result == 0, "Sniffer start failed"); started = true; }
    void stop() { ULONG result = command(1); status("sniffer_stop", result); if (!result) started = false; }
};

struct EventCounts {
    std::map<unsigned long long, unsigned long long> metadata;
    unsigned long long frames = 0, appleAwdlActions = 0;
};
static void WINAPI eventRecord(PEVENT_RECORD event) {
    auto* counts = static_cast<EventCounts*>(event->UserContext);
    const auto& descriptor = event->EventHeader.EventDescriptor;
    auto key = (static_cast<unsigned long long>(descriptor.Id) << 32) |
        (static_cast<unsigned long long>(descriptor.Opcode) << 24) | event->UserDataLength;
    if (counts->metadata.size() < 1024 || counts->metadata.count(key)) ++counts->metadata[key];
    // WPP blob argument is encoded as a 16-bit length followed by bytes.
    // Store only counts, never packet contents or unrelated addresses.
    if (descriptor.Id != 0xb3 || event->UserDataLength < 2) return;
    const auto* bytes = static_cast<const unsigned char*>(event->UserData);
    unsigned length = bytes[0] | (static_cast<unsigned>(bytes[1]) << 8);
    if (length + 2 != event->UserDataLength || length < 2) return;
    ++counts->frames;
    bytes += 2;
    if (length >= 29 && (bytes[0] & 0xfc) == 0xd0 && bytes[24] == 0x7f &&
        bytes[25] == 0 && bytes[26] == 0x17 && bytes[27] == 0xf2 && bytes[28] == 8)
        ++counts->appleAwdlActions;
}

static void selfTest() {
    // This path runs before Radio is constructed. It never opens a device,
    // queries WLAN, starts ETW, or sends an IOCTL.
    EventCounts counts;
    auto feed = [&counts](USHORT id, const std::vector<unsigned char>& bytes) {
        EVENT_RECORD event{};
        event.UserContext = &counts;
        event.EventHeader.EventDescriptor.Id = id;
        event.UserDataLength = static_cast<USHORT>(bytes.size());
        event.UserData = const_cast<unsigned char*>(bytes.data());
        eventRecord(&event);
    };
    std::vector<unsigned char> ack(12); ack[0] = 10; ack[2] = 0xd4;
    feed(0xb3, ack);
    require(counts.frames == 1 && counts.appleAwdlActions == 0, "Short ACK frame lost");
    std::vector<unsigned char> action(31); action[0] = 29; action[2] = 0xd0;
    action[26] = 0x7f; action[27] = 0; action[28] = 0x17; action[29] = 0xf2; action[30] = 8;
    feed(0xb3, action);
    require(counts.frames == 2 && counts.appleAwdlActions == 1, "AWDL signature not detected");
    action[30] = 9; feed(0xb3, action);
    require(counts.frames == 3 && counts.appleAwdlActions == 1, "Wrong Apple subtype matched");
    action[30] = 8; action[2] = 0x08; feed(0xb3, action);
    require(counts.frames == 4 && counts.appleAwdlActions == 1, "Data frame mistaken for action");
    feed(0xb1, ack); feed(0xb3, {}); feed(0xb3, {0}); feed(0xb3, {0, 0});
    feed(0xb3, {1, 0, 0}); feed(0xb3, {255, 255, 0});
    ack[0] = 11; feed(0xb3, ack);
    ack[0] = 9; feed(0xb3, ack);
    require(counts.frames == 4 && counts.appleAwdlActions == 1, "Malformed or unrelated blob accepted");
    action.resize(30); action[0] = 28; action[2] = 0xd0; feed(0xb3, action);
    require(counts.frames == 5 && counts.appleAwdlActions == 1, "Truncated signature matched");
    std::puts("{\"self_test\":\"passed\",\"cases\":13,\"device_access\":false}");
}

class Trace {
    std::vector<unsigned char> storage;
    EVENT_TRACE_PROPERTIES* properties;
    std::wstring name;
    TRACEHANDLE session = 0, consumer = INVALID_PROCESSTRACE_HANDLE;
    std::thread worker;
public:
    EventCounts counts;
    Trace() : storage(sizeof(EVENT_TRACE_PROPERTIES) + 1024),
        properties(reinterpret_cast<EVENT_TRACE_PROPERTIES*>(storage.data())),
        name(L"AirWindows-MT7922-" + std::to_wstring(GetCurrentProcessId())) {}
    ~Trace() { stop(); }
    ULONG start() {
        properties->Wnode.BufferSize = static_cast<ULONG>(storage.size());
        properties->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
        properties->Wnode.ClientContext = 1;
        properties->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
        properties->BufferSize = 64; properties->MinimumBuffers = 4; properties->MaximumBuffers = 16;
        properties->FlushTimer = 1;
        properties->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
        ULONG result = StartTraceW(&session, name.c_str(), properties);
        status("trace_start", result);
        if (result) { session = 0; return result; }
        EVENT_TRACE_LOGFILEW input{};
        input.LoggerName = const_cast<LPWSTR>(name.c_str());
        input.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
        input.EventRecordCallback = eventRecord; input.Context = &counts;
        consumer = OpenTraceW(&input);
        if (consumer == INVALID_PROCESSTRACE_HANDLE) return GetLastError();
        worker = std::thread([handle = consumer]() mutable { ULONG code = ProcessTrace(&handle, 1, nullptr, nullptr); status("trace_consumer_exit", code); });
        result = EnableTraceEx2(session, &rxProvider, EVENT_CONTROL_CODE_ENABLE_PROVIDER, 4, 1, 0, 1000, nullptr);
        status("trace_enable", result);
        return result;
    }
    void stop() {
        if (session) {
            EnableTraceEx2(session, &rxProvider, EVENT_CONTROL_CODE_DISABLE_PROVIDER, 0, 0, 0, 1000, nullptr);
            status("trace_stop", ControlTraceW(session, name.c_str(), properties, EVENT_TRACE_CONTROL_STOP));
            session = 0;
        }
        if (consumer != INVALID_PROCESSTRACE_HANDLE) { CloseTrace(consumer); consumer = INVALID_PROCESSTRACE_HANDLE; }
        if (worker.joinable()) worker.join();
    }
    void report() {
        for (const auto& pair : counts.metadata)
            std::printf("{\"event_id\":%llu,\"opcode\":%llu,\"user_bytes\":%llu,\"count\":%llu}\n",
                pair.first >> 32, (pair.first >> 24) & 255, pair.first & 0xffffff, pair.second);
        std::printf("{\"raw_frames\":%llu,\"awdl_actions\":%llu,\"payloads_saved\":false}\n", counts.frames, counts.appleAwdlActions);
    }
};

int main(int argc, char** argv) {
    try {
        require(argc == 2, "Use --self-test, --query, --trace-probe, --capture-idle or --stop");
        const std::string mode(argv[1]);
        if (mode == "--self-test") { selfTest(); return 0; }
        require(mode == "--query" || mode == "--trace-probe" || mode == "--capture-idle" || mode == "--stop", "Unknown mode");
        Radio radio;
        if (mode == "--query") { radio.query(); return 0; }
        if (mode == "--stop") { ULONG result = radio.command(1); status("sniffer_stop", result); return result ? 1 : 0; }
        if (mode == "--capture-idle") requireIdleAdapter();
        Trace trace;
        require(trace.start() == ERROR_SUCCESS, "Trace setup failed; radio unchanged");
        if (mode == "--capture-idle") {
            radio.start(); Sleep(5000); radio.query(); radio.stop();
        }
        trace.stop(); trace.report();
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Diagnostic failed: %s\n", e.what());
        return 1;
    }
}
