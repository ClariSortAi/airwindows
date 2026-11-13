// Documentation this follows (Microsoft Learn, read 2026-11-13):
//   https://learn.microsoft.com/windows/win32/api/netlistmgr/nf-netlistmgr-inetworklistmanager-getnetworks
//   https://learn.microsoft.com/windows/win32/api/netlistmgr/ne-netlistmgr-nlm_network_category
//     PUBLIC "public (untrusted)", PRIVATE "private (trusted)", DOMAIN_AUTHENTICATED. "The
//     private or public network categories must never be used to assume which Windows
//     Firewall ports are open": so the firewall itself is read below, and without it the
//     wording stays conditional.
//   https://learn.microsoft.com/windows/win32/api/netlistmgr/nf-netlistmgr-inetworkconnection-getadapterid
//     "the adapter ID of the TCP/IP interface used by this network connection", matched
//     here against IP_ADAPTER_ADDRESSES.AdapterName.
//   https://learn.microsoft.com/windows/win32/api/netfw/nn-netfw-inetfwpolicy2
//     get_Rules, get_FirewallEnabled, get_BlockAllInboundTraffic. "The Windows
//     Firewall/Internet Connection Sharing service must be running to access this interface."
//   https://learn.microsoft.com/windows/win32/api/netfw/nn-netfw-inetfwrule
//     Enabled, Direction, Action, ApplicationName, Profiles, Protocol, LocalPorts; read only.
//   https://learn.microsoft.com/windows/apps/develop/launch/launch-settings
//     ms-settings:network-wifi, ms-settings:network-ethernet, ms-settings:network-status.

#include "network_check.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <netfw.h>
#include <netlistmgr.h>
#include <wrl/client.h>

#include <algorithm>
#include <cwctype>
#include <map>

#include "log.h"

using Microsoft::WRL::ComPtr;

namespace network_check {
namespace {

constexpr long kDomain = NET_FW_PROFILE2_DOMAIN, kPrivate = NET_FW_PROFILE2_PRIVATE, kPublic = NET_FW_PROFILE2_PUBLIC;

std::wstring Lower(std::wstring text) {
    std::transform(text.begin(), text.end(), text.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return text;
}

const wchar_t* CategoryName(Category category) {
    switch (category) {
    case Category::Public: return L"Public";
    case Category::Private: return L"Private";
    case Category::Domain: return L"Domain";
    }
    return L"Public";
}

const wchar_t* InboundName(Inbound inbound) {
    switch (inbound) {
    case Inbound::Allowed: return L"inbound allowed";
    case Inbound::Blocked: return L"inbound blocked";
    case Inbound::Unknown: break;
    }
    return L"inbound unknown";
}

long ProfileOf(Category category) {
    return category == Category::Domain ? kDomain : category == Category::Private ? kPrivate : kPublic;
}

// A failure is logged when it first happens or changes, not on every poll.
void Report(std::wstring& last, const std::wstring& now) {
    if (now == last) return;
    last = now;
    if (!now.empty()) LOGW("[NET] %ls", now.c_str());
}
std::wstring g_networkFailure, g_firewallFailure;

std::wstring Hex(HRESULT hr) {
    wchar_t text[16];
    swprintf_s(text, L"0x%08lX", static_cast<unsigned long>(hr));
    return text;
}

// Adapters that could carry AirPlay: up, with a usable IPv4 address and a default gateway.
// The gateway leaves out host-only virtual switches (WSL, Hyper-V), which Windows lists as
// unidentified Public networks but no iPhone can reach. Tunnels, PPP and virtual adapters
// (VPNs such as WireGuard or Tailscale) are left out too: their type is not the user's to
// change for AirPlay. Keyed by "{GUID}" in lower case; the value is true for Wi-Fi.
std::map<std::wstring, bool> CandidateAdapters() {
    std::map<std::wstring, bool> result;
    ULONG size = 16 * 1024;
    std::vector<unsigned char> buffer(size);
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_INCLUDE_GATEWAYS;
    auto* addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    ULONG rc = GetAdaptersAddresses(AF_INET, flags, nullptr, addresses, &size);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(size);
        addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        rc = GetAdaptersAddresses(AF_INET, flags, nullptr, addresses, &size);
    }
    if (rc != NO_ERROR) {
        Report(g_networkFailure, L"GetAdaptersAddresses failed (error " + std::to_wstring(rc) + L"); network type unknown");
        return result;
    }
    for (auto* adapter = addresses; adapter; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK ||
            adapter->IfType == IF_TYPE_TUNNEL || adapter->IfType == IF_TYPE_PPP || adapter->IfType == IF_TYPE_PROP_VIRTUAL ||
            !adapter->FirstGatewayAddress) {
            continue;
        }
        bool usable = false;
        for (auto* unicast = adapter->FirstUnicastAddress; unicast && !usable; unicast = unicast->Next) {
            const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(unicast->Address.lpSockaddr);
            usable = (ntohl(ipv4->sin_addr.s_addr) & 0xFFFF0000u) != 0xA9FE0000u;  // not 169.254.x.x
        }
        if (!usable || !adapter->AdapterName) {
            continue;
        }
        const std::string name = adapter->AdapterName;
        result[Lower(std::wstring(name.begin(), name.end()))] = adapter->IfType == IF_TYPE_IEEE80211;
    }
    return result;
}

// Inbound TCP and UDP for this executable, per firewall profile (bit masks).
struct Firewall {
    bool ok = false;
    long allowTcp = 0, allowUdp = 0, blockTcp = 0, blockUdp = 0;
    long off = 0;       // profiles with the firewall turned off
    long blockAll = 0;  // profiles set to block all inbound traffic
    int rules = 0;      // rules that name this executable or package
};

std::wstring Expand(const std::wstring& path) {
    wchar_t expanded[MAX_PATH * 2];
    const DWORD length = ExpandEnvironmentStringsW(path.c_str(), expanded, ARRAYSIZE(expanded));
    return length > 0 && length <= ARRAYSIZE(expanded) ? std::wstring(expanded) : path;
}

Firewall ReadFirewall(const std::wstring& executable, const std::wstring& packageSid) {
    Firewall firewall;
    ComPtr<INetFwPolicy2> policy;
    HRESULT hr = CoCreateInstance(__uuidof(NetFwPolicy2), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&policy));
    if (FAILED(hr)) {
        Report(g_firewallFailure, L"Windows Firewall cannot be read (" + Hex(hr) + L"); the network notice stays conditional");
        return firewall;
    }
    for (long profile : {kDomain, kPrivate, kPublic}) {
        VARIANT_BOOL on = VARIANT_TRUE, blockAll = VARIANT_FALSE;
        if (SUCCEEDED(policy->get_FirewallEnabled(static_cast<NET_FW_PROFILE_TYPE2>(profile), &on)) && on == VARIANT_FALSE) {
            firewall.off |= profile;
        }
        if (SUCCEEDED(policy->get_BlockAllInboundTraffic(static_cast<NET_FW_PROFILE_TYPE2>(profile), &blockAll)) &&
            blockAll != VARIANT_FALSE) {
            firewall.blockAll |= profile;
        }
    }
    ComPtr<INetFwRules> rules;
    ComPtr<IUnknown> enumerator;
    ComPtr<IEnumVARIANT> items;
    hr = policy->get_Rules(&rules);
    if (SUCCEEDED(hr)) hr = rules->get__NewEnum(&enumerator);
    if (SUCCEEDED(hr)) hr = enumerator.As(&items);
    if (FAILED(hr)) {
        Report(g_firewallFailure, L"Windows Firewall rules cannot be read (" + Hex(hr) + L"); the network notice stays conditional");
        return firewall;
    }
    const std::wstring exe = Lower(executable);
    for (;;) {
        VARIANT item;
        VariantInit(&item);
        ULONG fetched = 0;
        if (items->Next(1, &item, &fetched) != S_OK || fetched == 0) break;
        ComPtr<INetFwRule> rule;
        if (item.vt == VT_DISPATCH && item.pdispVal) item.pdispVal->QueryInterface(IID_PPV_ARGS(&rule));
        VariantClear(&item);
        if (!rule) continue;
        VARIANT_BOOL enabled = VARIANT_FALSE;
        NET_FW_RULE_DIRECTION direction = NET_FW_RULE_DIR_OUT;
        if (FAILED(rule->get_Enabled(&enabled)) || enabled == VARIANT_FALSE) continue;
        if (FAILED(rule->get_Direction(&direction)) || direction != NET_FW_RULE_DIR_IN) continue;
        bool mine = false;
        BSTR application = nullptr;
        if (SUCCEEDED(rule->get_ApplicationName(&application)) && application) {
            mine = Lower(Expand(application)) == exe;
            SysFreeString(application);
        }
        if (!mine && !packageSid.empty()) {
            ComPtr<INetFwRule3> rule3;
            BSTR package = nullptr;
            if (SUCCEEDED(rule.As(&rule3)) && SUCCEEDED(rule3->get_LocalAppPackageId(&package)) && package) {
                mine = _wcsicmp(package, packageSid.c_str()) == 0;
                SysFreeString(package);
            }
        }
        if (!mine) continue;
        ++firewall.rules;
        NET_FW_ACTION action = NET_FW_ACTION_BLOCK;
        long profiles = 0, protocol = NET_FW_IP_PROTOCOL_ANY;
        BSTR ports = nullptr;
        rule->get_Action(&action);
        rule->get_Profiles(&profiles);
        rule->get_Protocol(&protocol);
        // AirPlay listens on ports Windows picks, so only a rule for every port counts.
        bool allPorts = true;
        if (SUCCEEDED(rule->get_LocalPorts(&ports)) && ports) {
            allPorts = SysStringLen(ports) == 0 || wcscmp(ports, L"*") == 0;
            SysFreeString(ports);
        }
        if (!allPorts) continue;
        const bool tcp = protocol == NET_FW_IP_PROTOCOL_ANY || protocol == NET_FW_IP_PROTOCOL_TCP;
        const bool udp = protocol == NET_FW_IP_PROTOCOL_ANY || protocol == NET_FW_IP_PROTOCOL_UDP;
        if (action == NET_FW_ACTION_ALLOW) {
            if (tcp) firewall.allowTcp |= profiles;
            if (udp) firewall.allowUdp |= profiles;
        } else {
            if (tcp) firewall.blockTcp |= profiles;
            if (udp) firewall.blockUdp |= profiles;
        }
    }
    firewall.ok = true;
    Report(g_firewallFailure, L"");
    return firewall;
}

Inbound InboundFor(const Firewall& firewall, Category category) {
    if (!firewall.ok) return Inbound::Unknown;
    const long profile = ProfileOf(category);
    if (firewall.off & profile) return Inbound::Allowed;
    if (firewall.blockAll & profile) return Inbound::Blocked;
    // A block rule wins over an allow rule.
    if ((firewall.blockTcp | firewall.blockUdp) & profile) return Inbound::Blocked;
    return (firewall.allowTcp & profile) && (firewall.allowUdp & profile) ? Inbound::Allowed : Inbound::Blocked;
}

}  // namespace

std::wstring Result::Describe() const {
    if (!ok) return L"unknown";
    if (networks.empty()) return L"no connected network with a gateway";
    std::wstring text;
    for (const Network& network : networks) {
        text += (text.empty() ? L"" : L"; ") + network.name + L" (" + (network.wifi ? L"Wi-Fi" : L"wired") + L"): " +
                CategoryName(network.category) + L", " + InboundName(network.inbound);
    }
    return text;
}

bool Result::operator==(const Result& other) const {
    if (ok != other.ok || firewallKnown != other.firewallKnown || networks.size() != other.networks.size()) return false;
    for (size_t i = 0; i < networks.size(); ++i) {
        if (networks[i].name != other.networks[i].name || networks[i].category != other.networks[i].category ||
            networks[i].wifi != other.networks[i].wifi || networks[i].inbound != other.networks[i].inbound) {
            return false;
        }
    }
    return true;
}

Advice Advise(const Result& result) {
    Advice advice;
    if (!result.ok || result.networks.empty()) return advice;
    bool anyAllowed = false, allAllowed = true, allFlaggedBlocked = true, wifi = false, wired = false;
    for (const Network& network : result.networks) {
        anyAllowed = anyAllowed || network.inbound == Inbound::Allowed;
        allAllowed = allAllowed && network.inbound == Inbound::Allowed;
        if (network.category != Category::Public || network.inbound == Inbound::Allowed) continue;
        advice.names.push_back(network.name);
        allFlaggedBlocked = allFlaggedBlocked && network.inbound == Inbound::Blocked;
        (network.wifi ? wifi : wired) = true;
    }
    advice.firewallReady = result.firewallKnown && allAllowed;
    advice.settingsUri = wifi ? L"ms-settings:network-wifi" : wired ? L"ms-settings:network-ethernet" : L"ms-settings:network-status";
    if (advice.names.empty()) return advice;
    advice.notice = result.firewallKnown && !anyAllowed && allFlaggedBlocked ? Notice::Hard : Notice::Conditional;
    return advice;
}

Result Read(const std::wstring& executable, const std::wstring& packageSid, bool readFirewall, const Result& previous) {
    Result result;
    const auto adapters = CandidateAdapters();
    ComPtr<INetworkListManager> manager;
    HRESULT hr = CoCreateInstance(__uuidof(NetworkListManager), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&manager));
    if (FAILED(hr)) {
        Report(g_networkFailure, L"the Network List Manager is unavailable (" + Hex(hr) + L"); network type unknown");
        return result;
    }
    ComPtr<IEnumNetworks> networks;
    hr = manager->GetNetworks(NLM_ENUM_NETWORK_CONNECTED, &networks);
    if (FAILED(hr) || !networks) {
        Report(g_networkFailure, L"GetNetworks failed (" + Hex(hr) + L"); network type unknown");
        return result;
    }
    Report(g_networkFailure, L"");
    result.ok = true;
    for (;;) {
        ComPtr<INetwork> network;
        ULONG fetched = 0;
        if (networks->Next(1, &network, &fetched) != S_OK || fetched == 0) break;
        NLM_NETWORK_CATEGORY category = NLM_NETWORK_CATEGORY_PUBLIC;
        if (FAILED(network->GetCategory(&category))) continue;
        ComPtr<IEnumNetworkConnections> connections;
        if (FAILED(network->GetNetworkConnections(&connections)) || !connections) continue;
        bool candidate = false, wifi = false;
        for (;;) {
            ComPtr<INetworkConnection> connection;
            ULONG got = 0;
            if (connections->Next(1, &connection, &got) != S_OK || got == 0) break;
            GUID adapter{};
            if (FAILED(connection->GetAdapterId(&adapter))) continue;
            wchar_t text[64];
            if (!StringFromGUID2(adapter, text, ARRAYSIZE(text))) continue;
            const auto found = adapters.find(Lower(text));
            if (found != adapters.end()) {
                candidate = true;
                wifi = wifi || found->second;
            }
        }
        if (!candidate) continue;
        BSTR name = nullptr;
        Network entry;
        if (SUCCEEDED(network->GetName(&name)) && name) {
            entry.name = name;
            SysFreeString(name);
        }
        if (entry.name.empty()) entry.name = wifi ? L"Wi-Fi" : L"Ethernet";
        entry.category = category == NLM_NETWORK_CATEGORY_PRIVATE ? Category::Private
                         : category == NLM_NETWORK_CATEGORY_DOMAIN_AUTHENTICATED ? Category::Domain
                                                                                 : Category::Public;
        entry.wifi = wifi;
        result.networks.push_back(entry);
    }
    std::sort(result.networks.begin(), result.networks.end(), [](const Network& a, const Network& b) { return a.name < b.name; });
    if (readFirewall) {
        const Firewall firewall = ReadFirewall(executable, packageSid);
        result.firewallKnown = firewall.ok;
        for (Network& network : result.networks) network.inbound = InboundFor(firewall, network.category);
    } else {
        // The rules did not change since `previous` was read; a network's answer follows its type.
        result.firewallKnown = previous.firewallKnown;
        for (Network& network : result.networks) {
            for (const Network& known : previous.networks) {
                if (known.category == network.category) network.inbound = known.inbound;
            }
            if (!result.firewallKnown) network.inbound = Inbound::Unknown;
        }
    }
    return result;
}

bool ValidSelfTest(const std::wstring& value) {
    return value == L"public" || value == L"private" || value == L"mixed" || value == L"public-allowed" || value == L"unknown";
}

Result Simulate(Result real, const std::wstring& value) {
    real.ok = true;
    const size_t needed = value == L"mixed" ? 2 : 1;
    while (real.networks.size() < needed) {
        Network made;
        made.wifi = real.networks.empty();
        made.name = made.wifi ? L"Wi-Fi" : L"Ethernet";
        real.networks.push_back(made);
    }
    real.firewallKnown = value != L"unknown";
    for (size_t i = 0; i < real.networks.size(); ++i) {
        Network& network = real.networks[i];
        if (value == L"private" || (value == L"mixed" && i == 0)) {
            network.category = Category::Private;
            network.inbound = Inbound::Allowed;
        } else {
            network.category = Category::Public;
            network.inbound = value == L"public-allowed" ? Inbound::Allowed : value == L"unknown" ? Inbound::Unknown : Inbound::Blocked;
        }
    }
    return real;
}

}  // namespace network_check
