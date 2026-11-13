// Can this PC's Bluetooth adapter act as the iPhone's keyboard and mouse? Asked once,
// shortly after launch, so a first-time user hears it before clicking Pair Bluetooth.
// Mirroring does not depend on the answer.
#pragma once

#include <optional>
#include <string>

namespace bluetooth_check {

enum class Adapter {
    Unknown = 0,       // not asked yet, or the question failed: say nothing
    Ready = 1,         // an adapter with the Bluetooth LE peripheral role
    Missing = 2,       // no Bluetooth adapter (or it is turned off in Device Manager)
    NoPeripheral = 3,  // an adapter without the peripheral role: no HID service possible
};

// Blocks; run it on a thread of its own. Logs what it found.
Adapter Probe();

const char* Name(Adapter adapter);

// --selftest-bluetooth <none|no-peripheral|ready>. Nothing else is accepted here; the
// receiver treats "probe" itself (the real check, in a self-test).
std::optional<Adapter> ParseSelfTest(const std::wstring& value);

// The status bar's sentence, and the setup guide's longer one. Empty for Ready and Unknown.
std::wstring StatusText(Adapter adapter);
std::wstring GuideText(Adapter adapter);

}  // namespace bluetooth_check
