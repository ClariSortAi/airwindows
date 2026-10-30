#pragma once
#include <cstdint>

// Failed StopAdvertising calls must retain active state and the stop deadline.
struct DiscoveryWindow {
    bool active = false, stopping = false;
    uint64_t deadline = 0, stopSince = 0, lastAttempt = 0;
    unsigned attempts = 0;
    void Start(uint64_t now, unsigned seconds) {
        active = true; stopping = false; deadline = now + seconds * 1000ull;
        stopSince = lastAttempt = 0; attempts = 0;
    }
    bool Due(uint64_t now) const { return stopping || (active && now >= deadline); }
    bool ShouldAttempt(uint64_t now) const { return Due(now) && (!attempts || now - lastAttempt >= 250); }
    void Attempt(uint64_t now) {
        if (!stopping) { stopping = true; stopSince = now; }
        lastAttempt = now; ++attempts;
    }
    void ConfirmStopped() { active = stopping = false; }
    bool Failed(uint64_t now) const { return stopping && now - stopSince >= 2000; }
};
