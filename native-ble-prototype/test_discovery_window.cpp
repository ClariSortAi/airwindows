#include "discovery_window.h"
#include "battery_level.h"
#include <cassert>
#include <iostream>
int main() {
    DiscoveryWindow window;
    window.Start(1000, 120);
    assert(!window.Due(120999));
    assert(window.ShouldAttempt(121000));
    window.Attempt(121000);
    assert(window.active && window.stopping);
    assert(!window.ShouldAttempt(121249));
    assert(window.ShouldAttempt(121250));
    window.Attempt(121250);
    assert(window.active && window.stopSince == 121000);
    assert(!window.Failed(122999));
    assert(window.Failed(123000));
    window.ConfirmStopped();
    assert(!window.active && !window.stopping && !window.Failed(123001));
    window.Start(200000, 120);
    window.Attempt(200100);
    assert(window.Failed(202100));
    // A successful StopAdvertising may leave Windows' status at Started (2).
    // Polling must not issue a second stop based on that stale OS property.
    window.Start(300000, 1);
    const int staleWindowsStatus = 2;
    unsigned stopCalls = 0;
    auto poll = [&](uint64_t now) {
        if (window.ShouldAttempt(now)) {
            window.Attempt(now);
            ++stopCalls;
            window.ConfirmStopped(); // successful API return
        }
    };
    poll(301000); poll(301250); poll(310000);
    assert(staleWindowsStatus == 2 && stopCalls == 1);
    assert(!window.Failed(310000));
    window.Start(400000, 1);
    assert(window.attempts == 0 && window.stopSince == 0);
    assert(BatteryLevel(true,1,100) == 100);
    assert(BatteryLevel(true,2,45) == 45);
    assert(BatteryLevel(true,128,255) == 0); // explicitly no battery
    assert(!BatteryLevel(true,255,100)); // unknown flags, not a real measurement
    assert(!BatteryLevel(true,1,255));
    assert(!BatteryLevel(false,1,100));
    std::cout << "discovery and battery policies: 19 assertions passed\n";
}
