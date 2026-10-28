// Owns the UxPlay protocol engine (RAOP/AirPlay HTTP server plus mDNS
// advertisement) and forwards its callbacks to an IMediaSink.
#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <string>

#include "media_sink.h"

struct ServerConfig {
    std::string name;        // UTF-8 AirPlay name
    std::string macAddress;  // "aa:bb:cc:dd:ee:ff", used as the AirPlay device id
    int width = 1920;
    int height = 1080;
    int refreshRate = 60;
    int maxFps = 60;
    bool supportsH265 = false;
    bool debugLog = false;
};

class AirPlayServer {
public:
    AirPlayServer() = default;
    ~AirPlayServer();
    AirPlayServer(const AirPlayServer&) = delete;
    AirPlayServer& operator=(const AirPlayServer&) = delete;

    // `sink` and `notify` must outlive the server. Returns false and logs on failure.
    bool Start(const ServerConfig& config, IMediaSink* sink, HWND notify);
    void Stop();
    bool Running() const { return raop_ != nullptr; }

    // Drops every client connection and reopens the listener. Call from the UI
    // thread in response to WM_APP_SERVER_RESET or a watchdog timeout.
    void RestartListener();

    // Call once a second from the UI thread. Returns true when the client has
    // stopped sending feedback for too long and the listener should be restarted.
    bool WatchdogTick();

    unsigned short Port() const { return port_; }
    uint64_t Features() const { return features_; }
    int OpenConnections() const { return openConnections_.load(); }

    // Returns the MAC of the first connected Ethernet/Wi-Fi adapter, or an empty string.
    static std::string FindMacAddress();
    static std::string RandomMacAddress();

private:
    struct Callbacks;  // static trampolines, defined in the .cpp
    friend struct Callbacks;

    void* raop_ = nullptr;   // raop_t*
    void* dnssd_ = nullptr;  // dnssd_t*
    IMediaSink* sink_ = nullptr;
    HWND notify_ = nullptr;
    unsigned short port_ = 0;
    uint64_t features_ = 0;

    std::atomic<int> openConnections_{0};
    std::atomic<unsigned> missedFeedback_{0};
    std::atomic<uint64_t> remoteClockOffset_{0};
    std::atomic<double> clientVolumeDb_{0.0};
};
