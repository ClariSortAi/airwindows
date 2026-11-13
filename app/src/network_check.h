// Which Windows network type (Public, Private, Domain) the networks that could carry
// AirPlay have, and whether Windows Firewall lets an iPhone reach this receiver on each.
// The warning built on it is stated as fact only when the firewall says so.
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace network_check {

enum class Category { Public, Private, Domain };
enum class Inbound { Unknown, Allowed, Blocked };

struct Network {
    std::wstring name;   // as Windows shows it, for example the Wi-Fi name
    Category category = Category::Public;
    bool wifi = false;   // carried by a Wi-Fi adapter
    Inbound inbound = Inbound::Unknown;  // Windows Firewall, for this executable, on this network's profile
};

struct Result {
    bool ok = false;                // Windows answered
    bool firewallKnown = false;     // the firewall rules could be read
    std::vector<Network> networks;  // connected networks on a physical adapter with an IPv4 address and a gateway

    std::wstring Describe() const;  // one line for the log
    bool operator==(const Result& other) const;
};

// What to tell the user about a Public network.
enum class Notice {
    None,         // nothing to say
    Hard,         // the firewall blocks AirWindows on every Public network and nothing allows it anywhere
    Conditional,  // a Public network may be blocked: "If your iPhone is on ..."
};

struct Advice {
    Notice notice = Notice::None;
    std::vector<std::wstring> names;  // the Public networks the notice is about
    const wchar_t* settingsUri = L"ms-settings:network-status";
    bool firewallReady = false;       // the firewall lets AirWindows in on every candidate network
};

// Pure: decides from a Result.
Advice Advise(const Result& result);

// Asks the Network List Manager and, read-only, Windows Firewall. Needs COM on the calling
// thread. `executable` is this program's path; `packageSid` the package's SID string when
// packaged (its manifest rules name the package), else empty. `readFirewall` false reuses
// the inbound answers of `previous` (reading every rule takes a moment).
Result Read(const std::wstring& executable, const std::wstring& packageSid, bool readFirewall, const Result& previous);

// --selftest-network <public|private|mixed|public-allowed|unknown>:
//   public          every network Public, blocked by the firewall
//   private         every network Private, allowed
//   mixed           the first network Private and allowed, the others Public and blocked
//   public-allowed  every network Public, allowed (an allow rule covers Public)
//   unknown         every network Public, firewall unreadable
// With fewer networks than the case needs, Wi-Fi and Ethernet ones are made up.
bool ValidSelfTest(const std::wstring& value);
Result Simulate(Result real, const std::wstring& value);

}  // namespace network_check
