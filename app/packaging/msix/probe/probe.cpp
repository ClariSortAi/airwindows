// AirWindows MSIX capability probe. Built twice by run-probe.ps1 from this file:
//   probectl.exe        console subsystem (the iosctl slot of the manifest)
//   AirWindowsProbe.exe windows subsystem, PROBE_GUI defined (the receiver slot)
// Modes (probectl.exe; AirWindowsProbe.exe accepts 'checks' too and reports through the pipe):
//   checks [--bluetooth] [--no-wildcard]  in-package checks; one PASS/FAIL/SKIP line each
//   bt [--not-discoverable]  only the Bluetooth checks (compare packaged and unpackaged runs)
//   store                 only the read-only Store license query
//   serve                 frame-style pipe, single-instance mutex and window for 'client'
//   client                run unpackaged: reach the objects 'serve' created inside the package
//   echo ARGS...          print identity and arguments, exit 7
// AirWindowsProbe.exe reports its identity, command line and check lines to \\.\pipe\AirWindowsProbe.Report.
// TCP checks listen on loopback only and the UDP 5353 bind is skippable: a wildcard listener
// without a firewall rule for the active network profile makes Windows show a prompt.
// Never uses the HID service UUID; Bluetooth uses a random 128-bit service UUID.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <appmodel.h>
#include <iphlpapi.h>
#include <sddl.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Services.Store.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using namespace winrt;
using namespace winrt::Windows::Devices::Bluetooth;
using namespace winrt::Windows::Devices::Bluetooth::GenericAttributeProfile;

namespace {
int g_failures = 0;
constexpr wchar_t kClass[] = L"AirWindowsProbeMainWindow";
constexpr wchar_t kProperty[] = L"AirWindowsProbe.Primary";
constexpr wchar_t kMutex[] = L"Local\\AirWindowsProbe.Receiver";
constexpr wchar_t kOutsideMutex[] = L"Local\\AirWindowsProbe.FromOutside";
constexpr wchar_t kMessage[] = L"AirWindowsProbe.SecondLaunch";

std::string Utf8(const std::wstring& text) {
    if (text.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
    return out;
}

std::string g_log;  // AirWindowsProbe.exe has no console; it sends this through the report pipe

void Result(const char* status, const char* check, const std::string& detail) {
    if (strcmp(status, "FAIL") == 0) ++g_failures;
    const std::string line = std::string(status) + " " + check + ": " + detail + "\n";
    g_log += line;
    fputs(line.c_str(), stdout);
    fflush(stdout);
}
void Pass(const char* check, const std::string& detail) { Result("PASS", check, detail); }
void Fail(const char* check, const std::string& detail) { Result("FAIL", check, detail); }
void Skip(const char* check, const std::string& detail) { Result("SKIP", check, detail); }
std::string Error(const char* what, DWORD code) { return std::string(what) + " failed, error " + std::to_string(code); }

std::wstring PackageName() {
    UINT32 length = 0;
    LONG rc = GetCurrentPackageFullName(&length, nullptr);
    if (rc == APPMODEL_ERROR_NO_PACKAGE) return L"";
    std::wstring name(length, L'\0');
    if (GetCurrentPackageFullName(&length, name.data()) != ERROR_SUCCESS) return L"";
    name.resize(length ? length - 1 : 0);
    return name;
}

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR path = nullptr;
    std::wstring result;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &path))) result = path;
    CoTaskMemFree(path);
    return result;
}

std::wstring FinalPath(HANDLE file) {
    wchar_t buffer[1024];
    DWORD n = GetFinalPathNameByHandleW(file, buffer, ARRAYSIZE(buffer), FILE_NAME_NORMALIZED);
    if (!n || n >= ARRAYSIZE(buffer)) return L"(final path unavailable, error " + std::to_wstring(GetLastError()) + L")";
    return buffer;
}

// Same descriptor as frame_server.cpp: protected DACL, full access for the current user only.
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

bool ReceiverRunning() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return true;  // unknown: treat as running, never risk the user's phone
    PROCESSENTRY32W entry{sizeof(entry)};
    bool found = false;
    for (BOOL ok = Process32FirstW(snapshot, &entry); ok; ok = Process32NextW(snapshot, &entry)) {
        if (_wcsicmp(entry.szExeFile, L"AirWindowsNative.exe") == 0 || _wcsicmp(entry.szExeFile, L"AirWindows.exe") == 0) found = true;
    }
    CloseHandle(snapshot);
    return found;
}

// ---------------------------------------------------------------- file system
void WriteText(const char* check, const std::wstring& folder, const std::wstring& name, DWORD disposition) {
    if (folder.empty()) { Fail(check, "known folder not found"); return; }
    if (!CreateDirectoryW(folder.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        Fail(check, Error("CreateDirectory", GetLastError()));
        return;
    }
    const std::wstring path = folder + L"\\" + name;
    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) { Fail(check, Error("CreateFile", GetLastError())); return; }
    const char text[] = "written by the packaged probe\r\n";
    DWORD written = 0;
    const bool ok = WriteFile(file, text, sizeof(text) - 1, &written, nullptr) != FALSE;
    const DWORD error = GetLastError();
    const std::wstring final = FinalPath(file);
    CloseHandle(file);
    if (!ok) { Fail(check, Error("WriteFile", error)); return; }
    Pass(check, "wrote " + Utf8(path) + " ; final path " + Utf8(final));
}

void FileSystemChecks() {
    const std::wstring local = KnownFolder(FOLDERID_LocalAppData);
    // run-probe.ps1 creates AirWindows-probe\existing.txt unpackaged before this runs.
    WriteText("fs.localappdata.existing-file", local + L"\\AirWindows-probe", L"existing.txt", OPEN_EXISTING);
    WriteText("fs.localappdata.new-file-in-existing-folder", local + L"\\AirWindows-probe", L"new.txt", CREATE_ALWAYS);
    WriteText("fs.localappdata.new-folder", local + L"\\AirWindows-probe-new", L"new.txt", CREATE_ALWAYS);
    // A real receiver file, written outside the package: size only, contents never read out.
    for (const wchar_t* name : {L"settings.ini", L"debug.txt"}) {
        const std::wstring path = local + L"\\AirWindows\\" + name;
        const std::string check = "fs.read-real-receiver-file." + Utf8(name);
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            const DWORD error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) Skip(check.c_str(), "no such file on this PC");
            else Fail(check.c_str(), Error("CreateFile", error));
            continue;
        }
        LARGE_INTEGER size{};
        char byte = 0;
        DWORD read = 0;
        const bool sized = GetFileSizeEx(file, &size) != FALSE;
        const bool readable = ReadFile(file, &byte, 1, &read, nullptr) != FALSE;
        CloseHandle(file);
        if (sized && readable) Pass(check.c_str(), "readable: yes, " + std::to_string(size.QuadPart) + " bytes");
        else Fail(check.c_str(), Error("read", GetLastError()));
    }
    WriteText("fs.pictures", KnownFolder(FOLDERID_Pictures) + L"\\AirWindows-probe", L"probe.txt", CREATE_ALWAYS);
    WriteText("fs.videos", KnownFolder(FOLDERID_Videos) + L"\\AirWindows-probe", L"probe.txt", CREATE_ALWAYS);
}

// HKCU writes, as license_store.cpp makes them. run-probe.ps1 creates Software\GraniteAi\AirWindowsProbe-existing
// unpackaged first, then reads both keys from outside the package to see where the values landed.
void RegistryChecks() {
    for (const wchar_t* path : {L"Software\\GraniteAi\\AirWindowsProbe-existing", L"Software\\GraniteAi\\AirWindowsProbe-new"}) {
        const std::string check = std::string("reg.hkcu.") + (wcsstr(path, L"existing") ? "existing-key" : "new-key");
        HKEY key = nullptr;
        LSTATUS status = RegCreateKeyExW(HKEY_CURRENT_USER, path, 0, nullptr, 0, KEY_SET_VALUE | KEY_QUERY_VALUE, nullptr, &key, nullptr);
        if (status != ERROR_SUCCESS) { Fail(check.c_str(), Error("RegCreateKeyEx", status)); continue; }
        const unsigned long long value = 0x5052'4F42'4521ULL;
        status = RegSetValueExW(key, L"PackagedWrite", 0, REG_QWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
        unsigned long long back = 0;
        DWORD size = sizeof(back);
        const LSTATUS readStatus = RegGetValueW(key, nullptr, L"PackagedWrite", RRF_RT_REG_QWORD, nullptr, &back, &size);
        RegCloseKey(key);
        if (status == ERROR_SUCCESS && readStatus == ERROR_SUCCESS && back == value) Pass(check.c_str(), "wrote and read back HKCU\\" + Utf8(path) + " PackagedWrite");
        else Fail(check.c_str(), "set " + std::to_string(status) + ", read back " + std::to_string(readStatus));
    }
}

// ---------------------------------------------------------------- network
std::vector<DWORD> Udp5353Owners() {
    std::vector<DWORD> owners;
    ULONG size = 0;
    GetExtendedUdpTable(nullptr, &size, FALSE, AF_INET, UDP_TABLE_OWNER_PID, 0);
    std::vector<uint8_t> buffer(size);
    if (GetExtendedUdpTable(buffer.data(), &size, FALSE, AF_INET, UDP_TABLE_OWNER_PID, 0) != NO_ERROR) return owners;
    auto table = reinterpret_cast<MIB_UDPTABLE_OWNER_PID*>(buffer.data());
    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        if (ntohs(static_cast<u_short>(table->table[i].dwLocalPort)) == 5353 && table->table[i].dwOwningPid != GetCurrentProcessId())
            owners.push_back(table->table[i].dwOwningPid);
    }
    return owners;
}

IP_ADAPTER_ADDRESSES* Adapters(ULONG family, std::vector<uint8_t>& buffer) {
    ULONG size = 32768;
    buffer.assign(size, 0);
    auto list = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_INCLUDE_GATEWAYS;
    const ULONG rc = GetAdaptersAddresses(family, flags, nullptr, list, &size);
    if (rc != NO_ERROR) {
        printf("INFO adapters: GetAdaptersAddresses error %lu\n", rc);
        return nullptr;
    }
    return list;
}

ULONG Ipv6InterfaceIndex() {
    std::vector<uint8_t> buffer;
    for (auto a = Adapters(AF_UNSPEC, buffer); a; a = a->Next) {
        if (a->OperStatus == IfOperStatusUp && a->IfType != IF_TYPE_SOFTWARE_LOOPBACK && a->FirstGatewayAddress && a->Ipv6IfIndex)
            return a->Ipv6IfIndex;
    }
    return 0;
}

void MdnsCheck() {
    const auto owners = Udp5353Owners();
    std::string held = std::to_string(owners.size()) + " other socket(s) already on UDP 5353";
    if (owners.empty()) held += " (shared-port case not exercised)";
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    BOOL one = TRUE;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));
    sockaddr_in any{AF_INET, htons(5353)};
    if (bind(s, reinterpret_cast<sockaddr*>(&any), sizeof(any)) == SOCKET_ERROR) {
        Fail("net.mdns.ipv4", Error("bind UDP 5353 with SO_REUSEADDR", WSAGetLastError()) + "; " + held);
        closesocket(s);
        return;
    }
    ip_mreq group{};
    inet_pton(AF_INET, "224.0.0.251", &group.imr_multiaddr);
    if (setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<const char*>(&group), sizeof(group)) == SOCKET_ERROR) {
        Fail("net.mdns.ipv4", Error("IP_ADD_MEMBERSHIP 224.0.0.251", WSAGetLastError()) + "; " + held);
        closesocket(s);
        return;
    }
    // A harmless mDNS question for a random name; the probe hears its own multicast through loopback.
    char name[32];
    snprintf(name, sizeof(name), "awprobe%08lx", GetTickCount() ^ GetCurrentProcessId());
    std::string query("\0\0\0\0\0\x01\0\0\0\0\0\0", 12);
    query += static_cast<char>(strlen(name));
    query += name;
    query += std::string("\x05local\0\0\x01\0\x01", 11);
    sockaddr_in to{AF_INET, htons(5353)};
    to.sin_addr = group.imr_multiaddr;
    DWORD timeout = 3000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    if (sendto(s, query.data(), static_cast<int>(query.size()), 0, reinterpret_cast<sockaddr*>(&to), sizeof(to)) == SOCKET_ERROR) {
        Fail("net.mdns.ipv4", Error("sendto 224.0.0.251:5353", WSAGetLastError()));
        closesocket(s);
        return;
    }
    const ULONGLONG deadline = GetTickCount64() + 3000;
    bool heard = false;
    char packet[1500];
    while (!heard && GetTickCount64() < deadline) {
        int n = recv(s, packet, sizeof(packet), 0);
        if (n == SOCKET_ERROR) break;
        heard = std::string(packet, n).find(name) != std::string::npos;
    }
    closesocket(s);
    if (heard) Pass("net.mdns.ipv4", "bound UDP 5353 with SO_REUSEADDR, joined 224.0.0.251, received own multicast query; " + held);
    else Fail("net.mdns.ipv4", "bound and joined, but own multicast query not received in 3 s; " + held);

    // mdnsd_win.c joins ff02::fb per interface index; do the same on the first usable adapter.
    SOCKET s6 = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    setsockopt(s6, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));
    sockaddr_in6 any6{AF_INET6, htons(5353)};
    ipv6_mreq group6{};
    inet_pton(AF_INET6, "ff02::fb", &group6.ipv6mr_multiaddr);
    group6.ipv6mr_interface = Ipv6InterfaceIndex();
    if (!group6.ipv6mr_interface)
        Skip("net.mdns.ipv6", "no IPv6 adapter with a gateway");
    else if (bind(s6, reinterpret_cast<sockaddr*>(&any6), sizeof(any6)) == SOCKET_ERROR)
        Fail("net.mdns.ipv6", Error("bind [::]:5353 with SO_REUSEADDR", WSAGetLastError()));
    else if (setsockopt(s6, IPPROTO_IPV6, IPV6_JOIN_GROUP, reinterpret_cast<const char*>(&group6), sizeof(group6)) == SOCKET_ERROR)
        Fail("net.mdns.ipv6", Error("IPV6_JOIN_GROUP ff02::fb", WSAGetLastError()));
    else Pass("net.mdns.ipv6", "bound [::]:5353 with SO_REUSEADDR and joined ff02::fb on interface " + std::to_string(group6.ipv6mr_interface));
    closesocket(s6);
}

// Listens on the loopback address only: a wildcard listener in an executable without a
// firewall rule for the active profile makes Windows show a prompt on the desktop.
void TcpCheck(const char* check, int family, const char* target) {
    SOCKET listener = socket(family, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_storage address{};
    int length = family == AF_INET ? sizeof(sockaddr_in) : sizeof(sockaddr_in6);
    address.ss_family = static_cast<ADDRESS_FAMILY>(family);
    if (family == AF_INET) inet_pton(AF_INET, target, &reinterpret_cast<sockaddr_in*>(&address)->sin_addr);
    else inet_pton(AF_INET6, target, &reinterpret_cast<sockaddr_in6*>(&address)->sin6_addr);
    if (bind(listener, reinterpret_cast<sockaddr*>(&address), length) == SOCKET_ERROR || listen(listener, 1) == SOCKET_ERROR ||
        getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) == SOCKET_ERROR) {
        Fail(check, Error("bind/listen on an OS-chosen port", WSAGetLastError()));
        closesocket(listener);
        return;
    }
    const u_short port = ntohs(family == AF_INET ? reinterpret_cast<sockaddr_in*>(&address)->sin_port
                                                 : reinterpret_cast<sockaddr_in6*>(&address)->sin6_port);
    sockaddr_storage peer{};
    peer.ss_family = static_cast<ADDRESS_FAMILY>(family);
    if (family == AF_INET) {
        inet_pton(AF_INET, target, &reinterpret_cast<sockaddr_in*>(&peer)->sin_addr);
        reinterpret_cast<sockaddr_in*>(&peer)->sin_port = htons(port);
    } else {
        inet_pton(AF_INET6, target, &reinterpret_cast<sockaddr_in6*>(&peer)->sin6_addr);
        reinterpret_cast<sockaddr_in6*>(&peer)->sin6_port = htons(port);
    }
    SOCKET client = socket(family, SOCK_STREAM, IPPROTO_TCP);
    const bool connected = connect(client, reinterpret_cast<sockaddr*>(&peer), length) != SOCKET_ERROR;
    const int connectError = WSAGetLastError();
    SOCKET accepted = connected ? accept(listener, nullptr, nullptr) : INVALID_SOCKET;
    const bool ok = connected && accepted != INVALID_SOCKET && send(client, "x", 1, 0) == 1;
    char byte = 0;
    const bool received = ok && recv(accepted, &byte, 1, 0) == 1 && byte == 'x';
    if (accepted != INVALID_SOCKET) closesocket(accepted);
    closesocket(client);
    closesocket(listener);
    if (received) Pass(check, std::string("listened on port ") + std::to_string(port) + ", accepted a connection to " + target + " and read a byte");
    else Fail(check, connected ? "connected but accept/read failed" : Error("connect", connectError));
}

void NetworkChecks(bool wildcard) {
    if (wildcard) MdnsCheck();
    else Skip("net.mdns", "wildcard UDP 5353 bind not run (--no-wildcard: a Public network is connected and the rules cover private/domain only, so Windows would prompt)");
    TcpCheck("net.tcp.ipv4-loopback", AF_INET, "127.0.0.1");
    TcpCheck("net.tcp.ipv6-loopback", AF_INET6, "::1");
    Skip("net.lan-inbound", "a connection from another device was not tested; local connections bypass Windows Firewall filtering");
}

// ---------------------------------------------------------------- bluetooth
void BluetoothChecks(bool requested, bool discoverable = true) {
    if (!requested) { Skip("bt", "not requested (run-probe.ps1 passes --bluetooth only when no receiver is running)"); return; }
    if (ReceiverRunning()) { Skip("bt", "an AirWindows receiver is running; Bluetooth step not run"); return; }
    try {
        auto adapter = BluetoothAdapter::GetDefaultAsync().get();
        if (!adapter) { Fail("bt.adapter", "no default Bluetooth adapter"); return; }
        Pass("bt.adapter", "default adapter found, LE supported: " + std::string(adapter.IsLowEnergySupported() ? "yes" : "no"));
        if (!adapter.IsPeripheralRoleSupported()) { Fail("bt.peripheral-role", "IsPeripheralRoleSupported is false"); return; }
        Pass("bt.peripheral-role", "IsPeripheralRoleSupported is true");
        GUID random{};
        CoCreateGuid(&random);
        const winrt::guid service(random);
        auto created = GattServiceProvider::CreateAsync(service).get();
        if (created.Error() != BluetoothError::Success) {
            Fail("bt.gatt-provider", "CreateAsync error " + std::to_string(static_cast<int>(created.Error())));
            return;
        }
        auto provider = created.ServiceProvider();
        GattLocalCharacteristicParameters parameters;
        parameters.CharacteristicProperties(GattCharacteristicProperties::Read);
        parameters.ReadProtectionLevel(GattProtectionLevel::Plain);
        winrt::Windows::Storage::Streams::DataWriter writer;
        writer.WriteByte(1);
        parameters.StaticValue(writer.DetachBuffer());
        GUID characteristic{};
        CoCreateGuid(&characteristic);
        auto made = provider.Service().CreateCharacteristicAsync(winrt::guid(characteristic), parameters).get();
        if (made.Error() != BluetoothError::Success) {
            Fail("bt.gatt-provider", "CreateCharacteristicAsync error " + std::to_string(static_cast<int>(made.Error())));
            return;
        }
        Pass("bt.gatt-provider", "GattServiceProvider created for a random 128-bit service UUID");
        std::atomic<int> lastError{-1};
        auto token = provider.AdvertisementStatusChanged([&](auto&&, GattServiceProviderAdvertisementStatusChangedEventArgs const& args) {
            lastError = static_cast<int>(args.Error());
        });
        GattServiceProviderAdvertisingParameters advertising;
        advertising.IsConnectable(true);
        advertising.IsDiscoverable(discoverable);
        provider.StartAdvertising(advertising);
        auto status = provider.AdvertisementStatus();
        for (int i = 0; i < 100 && status != GattServiceProviderAdvertisementStatus::Started &&
                        status != GattServiceProviderAdvertisementStatus::Aborted; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            status = provider.AdvertisementStatus();
        }
        const std::string how = std::string("connectable, ") + (discoverable ? "discoverable" : "not discoverable");
        if (status == GattServiceProviderAdvertisementStatus::Started) Pass("bt.advertise", "AdvertisementStatus reached Started (" + how + ")");
        else Fail("bt.advertise", "AdvertisementStatus is " + std::to_string(static_cast<int>(status)) + ", not Started (2), BluetoothError " +
                  std::to_string(lastError.load()) + " (" + how + ")");
        provider.AdvertisementStatusChanged(token);
        provider.StopAdvertising();
        for (int i = 0; i < 50 && provider.AdvertisementStatus() == GattServiceProviderAdvertisementStatus::Started; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        status = provider.AdvertisementStatus();
        if (status != GattServiceProviderAdvertisementStatus::Started) Pass("bt.stop", "advertising stopped, status " + std::to_string(static_cast<int>(status)));
        else Fail("bt.stop", "still Started 5 s after StopAdvertising");
    } catch (const winrt::hresult_error& e) {
        Fail("bt", "WinRT error 0x" + [&] { char b[16]; snprintf(b, sizeof(b), "%08X", static_cast<unsigned>(e.code())); return std::string(b); }() +
                       " " + Utf8(std::wstring(e.message())));
    }
}

// Read-only Store license query: what a package that did not come from the Store sees.
// No purchase API is called. No owner window is set: GetAppLicenseAsync shows no dialog.
void StoreLicenseCheck() {
    try {
        auto context = winrt::Windows::Services::Store::StoreContext::GetDefault();
        auto license = context.GetAppLicenseAsync().get();
        if (!license) { Result("INFO", "store.license", "GetAppLicenseAsync returned null"); return; }
        std::string expiry = "(none)";
        const FILETIME ft = winrt::clock::to_FILETIME(license.ExpirationDate());
        SYSTEMTIME st{};
        if (FileTimeToSystemTime(&ft, &st)) {
            char text[48];
            snprintf(text, sizeof(text), "%04u-%02u-%02uT%02u:%02u:%02uZ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
            expiry = text;
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::seconds>(license.TrialTimeRemaining()).count();
        Result("INFO", "store.license",
               std::string("IsActive=") + (license.IsActive() ? "true" : "false") + " IsTrial=" + (license.IsTrial() ? "true" : "false") +
               " ExpirationDate=" + expiry + " SkuStoreId='" + Utf8(std::wstring(license.SkuStoreId())) + "' TrialTimeRemaining=" +
               std::to_string(remaining) + "s IsTrialOwnedByThisUser=" + (license.IsTrialOwnedByThisUser() ? "true" : "false") +
               " AddOnLicenses=" + std::to_string(license.AddOnLicenses().Size()));
    } catch (const winrt::hresult_error& e) {
        char code[16];
        snprintf(code, sizeof(code), "%08X", static_cast<unsigned>(e.code()));
        Result("INFO", "store.license", std::string("GetAppLicenseAsync threw HRESULT 0x") + code + " " + Utf8(std::wstring(e.message())));
    }
}

bool HasArg(int argc, wchar_t** argv, const wchar_t* flag) {
    for (int i = 2; i < argc; ++i) if (wcscmp(argv[i], flag) == 0) return true;
    return false;
}

int Checks(int argc, wchar_t** argv) {
    const std::wstring package = PackageName();
    if (package.empty()) Fail("identity", "no package identity (GetCurrentPackageFullName: APPMODEL_ERROR_NO_PACKAGE)");
    else Pass("identity", Utf8(package));
    StoreLicenseCheck();
    BluetoothChecks(HasArg(argc, argv, L"--bluetooth"));
    NetworkChecks(!HasArg(argc, argv, L"--no-wildcard"));
    FileSystemChecks();
    RegistryChecks();
    return g_failures ? 1 : 0;
}

// ---------------------------------------------------------------- serve / client
struct ServeState {
    HWND window = nullptr;
    UINT message = 0;
    WPARAM messageValue = 0;
    bool gotMessage = false;
    std::wstring copied;
};
ServeState g_serve;

LRESULT CALLBACK ServeProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == g_serve.message && message) {
        g_serve.gotMessage = true;
        g_serve.messageValue = wParam;
        return 0;
    }
    if (message == WM_COPYDATA) {
        auto data = reinterpret_cast<COPYDATASTRUCT*>(lParam);
        g_serve.copied.assign(static_cast<const wchar_t*>(data->lpData), data->cbData / sizeof(wchar_t));
        return TRUE;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

int Serve() {
    const std::wstring package = PackageName();
    if (package.empty()) Fail("serve.identity", "no package identity");
    else Pass("serve.identity", Utf8(package));
    HANDLE mutex = CreateMutexW(nullptr, FALSE, kMutex);
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) { Fail("serve.mutex", Error("CreateMutex (fresh)", GetLastError())); return 1; }
    g_serve.message = RegisterWindowMessageW(kMessage);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = ServeProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    // Hidden top-level window, like the receiver's: found by class and property, not shown.
    g_serve.window = CreateWindowExW(0, kClass, L"AirWindows probe", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_serve.window) { Fail("serve.window", Error("CreateWindowEx", GetLastError())); return 1; }
    SetPropW(g_serve.window, kProperty, reinterpret_cast<HANDLE>(static_cast<INT_PTR>(1)));
    auto security = UserOnlySecurity();
    if (!security) { Fail("serve.pipe", "could not build the user-only security descriptor"); return 1; }
    const std::wstring name = L"\\\\.\\pipe\\AirWindowsProbe.Frames." + std::to_wstring(GetCurrentProcessId());
    SECURITY_ATTRIBUTES sa{sizeof(sa), security, FALSE};
    HANDLE pipe = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                   PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 4096, 1000, &sa);
    if (pipe == INVALID_HANDLE_VALUE) { Fail("serve.pipe", Error("CreateNamedPipe", GetLastError())); return 1; }
    printf("READY pid=%lu\n", GetCurrentProcessId());
    fflush(stdout);
    bool outsideVisible = false, clientDone = false;
    std::thread worker([&] {
        if (!ConnectNamedPipe(pipe, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED) {
            printf("INFO serve.pipe: ConnectNamedPipe ended, error %lu\n", GetLastError());
            return;
        }
        const std::string meta = "{\"protocol\":1,\"receiver_pid\":" + std::to_string(GetCurrentProcessId()) + "}";
        const uint32_t length = static_cast<uint32_t>(meta.size());
        DWORD n = 0;
        WriteFile(pipe, &length, 4, &n, nullptr);
        WriteFile(pipe, meta.data(), length, &n, nullptr);
        char ack = 0;
        if (ReadFile(pipe, &ack, 1, &n, nullptr) && ack == 'D') {
            clientDone = true;
            HANDLE outside = OpenMutexW(SYNCHRONIZE, FALSE, kOutsideMutex);
            outsideVisible = outside != nullptr;
            if (outside) CloseHandle(outside);
            const char reply = 'K';
            WriteFile(pipe, &reply, 1, &n, nullptr);
            FlushFileBuffers(pipe);
        }
        PostMessageW(g_serve.window, WM_CLOSE, 0, 0);
    });
    SetTimer(g_serve.window, 1, 30000, [](HWND w, UINT, UINT_PTR, DWORD) { PostMessageW(w, WM_CLOSE, 0, 0); });
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_CLOSE || (msg.hwnd == g_serve.window && msg.message == WM_CLOSE)) break;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        if (!IsWindow(g_serve.window)) break;
    }
    if (!clientDone) { CancelSynchronousIo(worker.native_handle()); DisconnectNamedPipe(pipe); }
    worker.join();
    // Drain the queue once more for a late posted message.
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
    if (!clientDone) Fail("serve.client", "no unpackaged client completed within 30 s");
    if (g_serve.gotMessage) Pass("serve.posted-message", "registered message from the unpackaged client arrived, wParam " + std::to_string(g_serve.messageValue));
    else Fail("serve.posted-message", "registered message from the unpackaged client did not arrive");
    if (!g_serve.copied.empty()) Pass("serve.copydata", "WM_COPYDATA arrived: " + Utf8(g_serve.copied));
    else Fail("serve.copydata", "WM_COPYDATA from the unpackaged client did not arrive");
    if (outsideVisible) Pass("serve.outside-mutex", "mutex created by the unpackaged client is visible inside the package");
    else Fail("serve.outside-mutex", "mutex created by the unpackaged client is not visible inside the package");
    CloseHandle(pipe);
    LocalFree(security);
    CloseHandle(mutex);
    DestroyWindow(g_serve.window);
    return g_failures ? 1 : 0;
}

BOOL CALLBACK FindProbeWindow(HWND window, LPARAM found) {
    wchar_t name[64];
    if (GetClassNameW(window, name, ARRAYSIZE(name)) && wcscmp(name, kClass) == 0 && GetPropW(window, kProperty)) {
        *reinterpret_cast<HWND*>(found) = window;
        return FALSE;
    }
    return TRUE;
}

int Client() {
    const std::wstring package = PackageName();
    if (package.empty()) Pass("client.identity", "unpackaged, as intended");
    else Fail("client.identity", "client unexpectedly has package identity " + Utf8(package));
    HWND window = nullptr;
    for (int i = 0; i < 50 && !window; ++i) {
        EnumWindows(FindProbeWindow, reinterpret_cast<LPARAM>(&window));
        if (!window) Sleep(100);
    }
    if (!window) { Fail("client.window", "packaged probe window not found by class and property"); return 1; }
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    Pass("client.window", "found the packaged window by class and property (pid " + std::to_string(pid) + ")");
    HANDLE existing = CreateMutexW(nullptr, FALSE, kMutex);
    const DWORD mutexError = GetLastError();
    if (existing && mutexError == ERROR_ALREADY_EXISTS) Pass("client.mutex", "CreateMutex(Local\\AirWindowsProbe.Receiver) returned ERROR_ALREADY_EXISTS");
    else Fail("client.mutex", "packaged mutex not visible (CreateMutex error " + std::to_string(mutexError) + ")");
    if (existing) CloseHandle(existing);
    HANDLE outside = CreateMutexW(nullptr, FALSE, kOutsideMutex);
    const UINT message = RegisterWindowMessageW(kMessage);
    if (PostMessageW(window, message, 1, 0)) Pass("client.post", "PostMessage of the registered message succeeded");
    else Fail("client.post", Error("PostMessage", GetLastError()));
    const std::wstring uri = L"second launch: --frames";
    COPYDATASTRUCT data{1, static_cast<DWORD>(uri.size() * sizeof(wchar_t)), const_cast<wchar_t*>(uri.c_str())};
    DWORD_PTR answer = 0;
    if (SendMessageTimeoutW(window, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data), SMTO_ABORTIFHUNG, 3000, &answer) && answer)
        Pass("client.copydata", "WM_COPYDATA accepted by the packaged window");
    else Fail("client.copydata", Error("SendMessageTimeout(WM_COPYDATA)", GetLastError()));
    const std::wstring name = L"\\\\.\\pipe\\AirWindowsProbe.Frames." + std::to_wstring(pid);
    HANDLE pipe = INVALID_HANDLE_VALUE;
    for (int i = 0; i < 2 && pipe == INVALID_HANDLE_VALUE; ++i) {
        pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE && GetLastError() == ERROR_PIPE_BUSY) WaitNamedPipeW(name.c_str(), 3000);
        else if (pipe == INVALID_HANDLE_VALUE) break;
    }
    if (pipe == INVALID_HANDLE_VALUE) {
        Fail("client.pipe", Error("CreateFile on the packaged pipe", GetLastError()));
        if (outside) CloseHandle(outside);
        return 1;
    }
    ULONG server = 0;
    GetNamedPipeServerProcessId(pipe, &server);
    uint32_t length = 0;
    DWORD n = 0;
    std::string meta;
    if (ReadFile(pipe, &length, 4, &n, nullptr) && n == 4 && length < 8192) {
        meta.resize(length);
        DWORD got = 0;
        while (got < length && ReadFile(pipe, meta.data() + got, length - got, &n, nullptr) && n) got += n;
        meta.resize(got);
    }
    if (server == pid && meta.find("\"receiver_pid\":" + std::to_string(pid)) != std::string::npos)
        Pass("client.pipe", "connected to the packaged user-only pipe, server pid matches, read " + meta);
    else Fail("client.pipe", "server pid " + std::to_string(server) + ", metadata '" + meta + "'");
    const char done = 'D';
    WriteFile(pipe, &done, 1, &n, nullptr);
    char reply = 0;
    ReadFile(pipe, &reply, 1, &n, nullptr);
    CloseHandle(pipe);
    if (outside) CloseHandle(outside);
    return g_failures ? 1 : 0;
}

int Echo(int argc, wchar_t** argv) {
    const std::wstring package = PackageName();
    printf("identity=%s\n", package.empty() ? "(none)" : Utf8(package).c_str());
    for (int i = 0; i < argc; ++i) printf("argv[%d]=%s\n", i, Utf8(argv[i]).c_str());
    return 7;
}

// AirWindowsProbe.exe: report identity, the raw command line and any check lines to the harness.
int Report() {
    const std::wstring package = PackageName();
    const std::string text = "identity=" + (package.empty() ? std::string("(none)") : Utf8(package)) + "\ncmdline=" +
                             Utf8(GetCommandLineW()) + "\n" + g_log;
    const wchar_t* name = L"\\\\.\\pipe\\AirWindowsProbe.Report";
    if (!WaitNamedPipeW(name, 10000)) return 2;
    HANDLE pipe = CreateFileW(name, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) return 3;
    DWORD n = 0;
    WriteFile(pipe, text.data(), static_cast<DWORD>(text.size()), &n, nullptr);
    FlushFileBuffers(pipe);
    CloseHandle(pipe);
    return 0;
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    const std::wstring mode = argc > 1 ? argv[1] : L"";
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { Fail("startup", "WSAStartup failed"); return 1; }
#ifdef PROBE_GUI
    // The receiver slot: the manifest's firewall rules name this executable.
    if (mode == L"checks") Checks(argc, argv);
    return Report();
#else
    if (mode == L"checks") return Checks(argc, argv);
    if (mode == L"bt") { BluetoothChecks(true, !(argc > 2 && std::wstring(argv[2]) == L"--not-discoverable")); return g_failures ? 1 : 0; }
    if (mode == L"store") { StoreLicenseCheck(); return 0; }
    if (mode == L"serve") return Serve();
    if (mode == L"client") return Client();
    if (mode == L"echo") return Echo(argc, argv);
    printf("usage: probectl checks [--bluetooth] [--no-wildcard] | bt [--not-discoverable] | store | serve | client | echo ARGS...\n");
    return 2;
#endif
}
