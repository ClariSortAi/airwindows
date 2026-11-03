#pragma once
#include <cstdint>

// Failed StopAdvertising calls must retain active state and the stop deadline.
struct DiscoveryWindow {
    bool active = false, stopping = false;
    uint64_t deadline = 0, stopSince = 0, lastAttempt = 0, started = 0;
    unsigned attempts = 0;
    static constexpr uint64_t kMaxOpenMs = 600000; // phone activity never keeps discovery open past 10 minutes
    void Start(uint64_t now, unsigned seconds) {
        active = true; stopping = false; deadline = now + seconds * 1000ull; started = now;
        stopSince = lastAttempt = 0; attempts = 0;
    }
    // Pairing, reads and subscriptions keep the window open at least `ms` longer, so a
    // phone that is still setting up is not cut off by a timer started at the Pair click.
    void Extend(uint64_t now, uint64_t ms) {
        if (!active || stopping) return;
        const uint64_t wanted = now + ms, cap = started + kMaxOpenMs;
        if (wanted > deadline) deadline = wanted < cap ? wanted : cap;
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
