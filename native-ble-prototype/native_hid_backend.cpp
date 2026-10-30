#include "native_hid_backend.h"
#include "discovery_window.h"
#include "battery_level.h"
#include "absolute_buttons.h"
#include <windows.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Storage.Streams.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <iomanip>
#include <deque>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace winrt;
using namespace Windows::Devices::Bluetooth;
using namespace Windows::Devices::Bluetooth::GenericAttributeProfile;
using namespace Windows::Storage::Streams;
using Bytes = std::vector<uint8_t>;

namespace {
guid Uuid(uint16_t id) { return {id, 0, 0x1000, {0x80, 0, 0, 0x80, 0x5f, 0x9b, 0x34, 0xfb}}; }
IBuffer MakeBuffer(const Bytes& bytes) {
    DataWriter writer;
    writer.WriteBytes(bytes);
    return writer.DetachBuffer();
}
const Bytes kReportMap = {
    0x05,0x01,0x09,0x06,0xA1,0x01,0x85,0x01,0x05,0x07,0x19,0xE0,0x29,0xE7,0x15,0x00,0x25,0x01,
    0x75,0x01,0x95,0x08,0x81,0x02,0x95,0x01,0x75,0x08,0x81,0x01,
    0x05,0x08,0x19,0x01,0x29,0x05,0x95,0x05,0x75,0x01,0x91,0x02,0x95,0x01,0x75,0x03,0x91,0x01,
    0x05,0x07,0x19,0x00,0x29,0x65,0x15,0x00,0x25,0x65,0x95,0x06,0x75,0x08,0x81,0x00,0xC0,
    0x05,0x01,0x09,0x02,0xA1,0x01,0x85,0x02,0x09,0x01,0xA1,0x00,
    0x05,0x09,0x19,0x01,0x29,0x05,0x15,0x00,0x25,0x01,0x95,0x05,0x75,0x01,0x81,0x02,
    0x95,0x01,0x75,0x03,0x81,0x01,0x05,0x01,0x09,0x30,0x09,0x31,0x09,0x38,
    0x15,0x81,0x25,0x7F,0x75,0x08,0x95,0x03,0x81,0x06,
    0x05,0x0C,0x0A,0x38,0x02,0x95,0x01,0x81,0x06,0xC0,0xC0,
    // Absolute pointer adapted from xhoantran/iphone-use (MIT), report ID changed
    // to3 to preserve our qualified keyboard1 and relative mouse2. See LICENSE-iphone-use.
    0x05,0x01,0x09,0x02,0xA1,0x01,0x85,0x03,0x09,0x01,0xA1,0x00,
    0x05,0x09,0x19,0x01,0x29,0x03,0x15,0x00,0x25,0x01,
    0x95,0x03,0x75,0x01,0x81,0x02,0x95,0x01,0x75,0x05,0x81,0x01,
    0x05,0x01,0x09,0x30,0x09,0x31,0x16,0x00,0x00,0x26,0xFF,0x7F,
    0x75,0x10,0x95,0x02,0x81,0x02,0xC0,0xC0
};
std::string Fingerprint(const hstring& id) {
    uint64_t hash = 14695981039346656037ull;
    for (unsigned char c : to_string(id)) { hash ^= c; hash *= 1099511628211ull; }
    std::ostringstream out; out << std::hex << std::setfill('0') << std::setw(16) << hash;
    return out.str();
}
Bytes KeyReport(unsigned char c) {
    uint8_t key = 0, shift = 0;
    if (c >= 'a' && c <= 'z') key = c - 'a' + 4;
    else if (c >= 'A' && c <= 'Z') { key = c - 'A' + 4; shift = 2; }
    else {
        const std::string digits = "1234567890", shifted = "!@#$%^&*()_+{}|:\"~<>?";
        const std::string normal = "\n\t -=[]\\;'`,./";
        const uint8_t normalCodes[] = {40,43,44,45,46,47,48,49,51,52,53,54,55,56};
        const uint8_t shiftedCodes[] = {30,31,32,33,34,35,36,37,38,39,45,46,47,48,49,51,52,53,54,55,56};
        auto position = digits.find(static_cast<char>(c));
        if (position != std::string::npos) key = static_cast<uint8_t>(position + 30);
        else if ((position = normal.find(static_cast<char>(c))) != std::string::npos) key = normalCodes[position];
        else if ((position = shifted.find(static_cast<char>(c))) != std::string::npos) { key = shiftedCodes[position]; shift = 2; }
        else throw std::invalid_argument("Only printable US ASCII, tab and newline are supported");
    }
    return {shift,0,key,0,0,0,0,0};
}
}

struct NativeHidBackend::Impl : std::enable_shared_from_this<Impl> {
    GattServiceProvider provider{nullptr};
    GattServiceProvider batteryProvider{nullptr};
    bool hidPublicationPending = false, batteryPublicationPending = false;
    std::map<std::string, GattLocalCharacteristic> chars;
    std::map<std::string, Bytes> values{{"keyboard",Bytes(8)}, {"mouse",Bytes(5)}, {"absolute",Bytes(5)}, {"leds",Bytes(1)}, {"protocol",{1}}, {"battery",{0}}};
    std::vector<std::function<void()>> unregister;
    mutable std::mutex stateMutex;
    std::mutex sendMutex;
    std::mutex advertisementMutex;
    hstring bound, lastBound;
    bool enabled = false, suspended = false, closed = false, diagnosticReads = false, stopCallReturned = false;
    DiscoveryWindow discovery;
    uint64_t enableDeadline = 0, actionDeadline = 0;
    uint64_t readCallbacks = 0, writeCallbacks = 0, subscriptionCallbacks = 0, diagnosticCount = 0;
    std::deque<std::pair<std::string, uint32_t>> diagnostics;
    std::map<std::string, std::string> lastRelease{{"keyboard","not_attempted"},{"mouse","not_attempted"}};
    std::map<std::string, uint64_t> readsByCharacteristic;
    std::map<hstring, BluetoothLEDevice> peerDevices;
    bool persistentPublication = false;
    bool haveAbsolutePosition = false;
    std::function<bool()> inputGuard;
    std::atomic<bool> cancel{true};

    void Diagnostic(const std::string& stage, uint32_t code) {
        std::lock_guard<std::mutex> lock(stateMutex);
        ++diagnosticCount;
        diagnostics.emplace_back(stage,code);
        if (diagnostics.size() > 8) diagnostics.pop_front();
        if (diagnosticCount <= 8) {
            std::ostringstream line;
            line << "{\"diagnostic\":\"" << stage << "\",\"hresult\":\"0x" << std::hex << std::setw(8)
                 << std::setfill('0') << code << "\",\"error_number\":" << std::dec << diagnosticCount << "}";
            std::cerr << line.str() << std::endl;
        }
    }

    unsigned SubscriptionCount(const char* name) const {
        auto found = chars.find(name);
        return found == chars.end() ? 0 : found->second.SubscribedClients().Size();
    }

    using ClientPair = std::pair<GattSubscribedClient, GattSubscribedClient>;
    std::map<hstring, ClientPair> Clients() const {
        std::map<hstring, GattSubscribedClient> keyboard, mouse;
        auto collect = [&](const char* name, auto& output) {
            auto found = chars.find(name);
            if (found != chars.end()) for (auto client : found->second.SubscribedClients()) {
                if (client.Session().SessionStatus() == GattSessionStatus::Active)
                    output.emplace(client.Session().DeviceId().Id(), client);
            }
        };
        collect("keyboard", keyboard); collect("mouse", mouse);
        std::map<hstring, ClientPair> result;
        for (auto& pair : keyboard) {
            auto other = mouse.find(pair.first);
            if (other != mouse.end()) result.emplace(pair.first, ClientPair{pair.second, other->second});
        }
        return result;
    }

    void SubscriptionChanged() {
        cancel = true;
        const auto clients = Clients();
        std::lock_guard<std::mutex> lock(stateMutex);
        ++subscriptionCallbacks;
        enabled = false;
        if (!bound.empty() && clients.find(bound) == clients.end()) bound = hstring{};
        values["keyboard"] = Bytes(8); values["mouse"] = Bytes(5);
    }

    void Read(const std::string& name, GattReadRequestedEventArgs const& args) {
        auto deferral = args.GetDeferral();
        try {
            { std::lock_guard<std::mutex> lock(stateMutex); ++readCallbacks; ++readsByCharacteristic[name]; }
            auto request = args.GetRequestAsync().get();
            if (request) {
                Bytes value;
                bool permitted;
                {
                    std::lock_guard<std::mutex> lock(stateMutex);
                    permitted = !closed && (bound.empty() || bound == args.Session().DeviceId().Id());
                    value = values.at(name);
                }
                bool available = true;
                if (name == "battery") {
                    SYSTEM_POWER_STATUS power{};
                    const bool powerAvailable = GetSystemPowerStatus(&power) != FALSE;
                    const auto level = BatteryLevel(powerAvailable, power.BatteryFlag, power.BatteryLifePercent);
                    available = level.has_value();
                    if (available) value = {*level};
                }
                if (!permitted) request.RespondWithProtocolError(0x08);
                else if (!available) request.RespondWithProtocolError(0x0e); // unknown is not a fabricated level
                else if (request.Offset() > value.size()) request.RespondWithProtocolError(0x07);
                else {
                    const size_t start = request.Offset(), end = std::min(value.size(), start + request.Length());
                    request.RespondWithValue(MakeBuffer(Bytes(value.begin() + start, value.begin() + end)));
                }
            }
        } catch (hresult_error const& error) { cancel = true; Diagnostic("read_"+name,static_cast<uint32_t>(error.code())); }
        catch (...) { cancel = true; Diagnostic("read_"+name,0x80004005); }
        deferral.Complete();
    }

    void Write(const std::string& name, GattWriteRequestedEventArgs const& args) {
        auto deferral = args.GetDeferral();
        try {
            { std::lock_guard<std::mutex> lock(stateMutex); ++writeCallbacks; }
            auto request = args.GetRequestAsync().get();
            if (request) {
                const auto reader = DataReader::FromBuffer(request.Value());
                bool valid = request.Offset() == 0 && reader.UnconsumedBufferLength() == 1;
                const uint8_t value = valid ? reader.ReadByte() : 0;
                if (name != "leds") valid = valid && value <= 1;
                if (valid) {
                    std::lock_guard<std::mutex> lock(stateMutex);
                    if (name == "control") suspended = value == 0;
                    else values[name] = {value};
                    if (name != "leds") {
                        cancel = true; enabled = false;
                        values["keyboard"] = Bytes(8); values["mouse"] = Bytes(5);
                    }
                }
                if (request.Option() == GattWriteOption::WriteWithResponse) {
                    if (valid) request.Respond(); else request.RespondWithProtocolError(0x13);
                }
            }
        } catch (hresult_error const& error) { cancel = true; Diagnostic("write_"+name,static_cast<uint32_t>(error.code())); }
        catch (...) { cancel = true; Diagnostic("write_"+name,0x80004005); }
        deferral.Complete();
    }

    void Characteristic(const std::string& name, uint16_t id, GattCharacteristicProperties properties,
                        const Bytes* fixed = nullptr, Bytes reference = {}) {
        GattLocalCharacteristicParameters parameters;
        parameters.CharacteristicProperties(properties);
        parameters.ReadProtectionLevel(GattProtectionLevel::EncryptionRequired);
        parameters.WriteProtectionLevel(GattProtectionLevel::EncryptionRequired);
        if (fixed && !diagnosticReads) parameters.StaticValue(MakeBuffer(*fixed));
        if (fixed && diagnosticReads) values[name] = *fixed;
        auto service = name == "battery" ? batteryProvider.Service() : provider.Service();
        auto result = service.CreateCharacteristicAsync(Uuid(id), parameters).get();
        if (result.Error() != BluetoothError::Success) throw std::runtime_error("Characteristic creation failed: " + name);
        auto characteristic = result.Characteristic();
        chars.emplace(name, characteristic);
        auto self = shared_from_this();
        if ((!fixed || diagnosticReads) && (properties & GattCharacteristicProperties::Read) != GattCharacteristicProperties::None) {
            auto token = characteristic.ReadRequested([self,name](auto const&, auto const& args) { self->Read(name,args); });
            unregister.push_back([characteristic,token] { characteristic.ReadRequested(token); });
        }
        if ((properties & (GattCharacteristicProperties::Write | GattCharacteristicProperties::WriteWithoutResponse)) != GattCharacteristicProperties::None) {
            auto token = characteristic.WriteRequested([self,name](auto const&, auto const& args) { self->Write(name,args); });
            unregister.push_back([characteristic,token] { characteristic.WriteRequested(token); });
        }
        if ((properties & GattCharacteristicProperties::Notify) != GattCharacteristicProperties::None) {
            auto token = characteristic.SubscribedClientsChanged([self](auto const&, auto const&) {
                try { self->SubscriptionChanged(); }
                catch (hresult_error const& error) { self->cancel = true; self->Diagnostic("subscriptions",static_cast<uint32_t>(error.code())); }
                catch (...) { self->cancel = true; self->Diagnostic("subscriptions",0x80004005); }
            });
            unregister.push_back([characteristic,token] { characteristic.SubscribedClientsChanged(token); });
        }
        if (!reference.empty()) {
            GattLocalDescriptorParameters descriptor;
            descriptor.ReadProtectionLevel(GattProtectionLevel::EncryptionRequired);
            descriptor.StaticValue(MakeBuffer(reference));
            auto descriptorResult = characteristic.CreateDescriptorAsync(Uuid(0x2908), descriptor).get();
            if (descriptorResult.Error() != BluetoothError::Success) throw std::runtime_error("Report Reference creation failed");
        }
    }

    void Send(const std::string& name, const Bytes& value, bool release = false) {
        std::lock_guard<std::mutex> serialized(sendMutex);
        hstring target;
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            if (!release && (cancel || (inputGuard && !inputGuard()) || GetTickCount64() > actionDeadline || suspended || values["protocol"] != Bytes{1}))
                throw std::runtime_error("Input canceled, expired, suspended or not in report mode");
            target = release && bound.empty() ? lastBound : bound;
        }
        GattSubscribedClient client{nullptr};
        if (release) {
            if (!target.empty()) for (auto subscriber : chars.at(name).SubscribedClients()) {
                if (subscriber.Session().DeviceId().Id() == target && subscriber.Session().SessionStatus() == GattSessionStatus::Active)
                    client = subscriber;
            }
        } else {
            const auto clients = Clients();
            auto found = clients.find(target);
            if (!target.empty() && clients.size() == 1 && found != clients.end()) {
                for (auto subscriber : chars.at(name).SubscribedClients()) {
                    if (subscriber.Session().DeviceId().Id() == target && subscriber.Session().SessionStatus() == GattSessionStatus::Active)
                        client = subscriber;
                }
            }
        }
        if (!client) throw std::runtime_error("Bound client lost report subscriptions; observe and bind again");
        if (value.size() > client.MaxNotificationSize()) throw std::runtime_error("Report exceeds notification size");
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            values[name] = value;
            if(name == "absolute") haveAbsolutePosition = true;
        }
        auto operation = chars.at(name).NotifyValueAsync(MakeBuffer(value), client);
        if (operation.wait_for(std::chrono::seconds(1)) != Windows::Foundation::AsyncStatus::Completed) {
            operation.Cancel();
            throw std::runtime_error("Notification timeout; outcome unknown, do not replay");
        }
        if (operation.GetResults().Status() != GattCommunicationStatus::Success)
            throw std::runtime_error("Notification failed; outcome unknown, do not replay");
        std::this_thread::sleep_for(std::chrono::milliseconds(12));
    }

    void BeginAction() {
        std::lock_guard<std::mutex> lock(stateMutex);
        if (!enabled || closed || (inputGuard && !inputGuard()) || discovery.Failed(GetTickCount64()) || GetTickCount64() > enableDeadline)
            throw std::runtime_error("Enable one action while watching the bound phone");
        enabled = false; actionDeadline = GetTickCount64() + 2000; cancel = false;
    }

    void StopAdvertising() {
        std::lock_guard<std::mutex> serialized(advertisementMutex);
        if (!provider) return;
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            if (!discovery.active && !discovery.stopping) return;
        }
        { std::lock_guard<std::mutex> lock(stateMutex); discovery.Attempt(GetTickCount64()); }
        auto stopOne = [&](const char* name, GattServiceProvider const& item, bool& pending) {
            { std::lock_guard<std::mutex> lock(stateMutex); if (!pending) return; }
            try {
                item.StopAdvertising();
                // Windows may leave AdvertisementStatus at Started after a
                // successful stop. Track each successful API return exactly once.
                std::lock_guard<std::mutex> lock(stateMutex); pending = false;
            } catch (hresult_error const& error) {
                Diagnostic(std::string("stop_")+name,static_cast<uint32_t>(error.code()));
            } catch (...) { Diagnostic(std::string("stop_")+name,0x80004005); }
        };
        stopOne("hid",provider,hidPublicationPending);
        stopOne("battery",batteryProvider,batteryPublicationPending);
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            if (!hidPublicationPending && !batteryPublicationPending) {
                stopCallReturned = true;
                discovery.ConfirmStopped();
            }
        }
    }
};

NativeHidBackend::NativeHidBackend() : impl_(std::make_shared<Impl>()) {}
NativeHidBackend::~NativeHidBackend() { try { Shutdown(); } catch (...) {} }
void NativeHidBackend::Initialize(bool diagnosticReads) {
    auto& s = *impl_;
    s.diagnosticReads = diagnosticReads;
    auto adapter = BluetoothAdapter::GetDefaultAsync().get();
    if (!adapter || !adapter.IsPeripheralRoleSupported()) throw std::runtime_error("BLE peripheral role is unavailable");
    auto result = GattServiceProvider::CreateAsync(Uuid(0x1812)).get();
    if (result.Error() != BluetoothError::Success) throw std::runtime_error("HID service creation failed");
    s.provider = result.ServiceProvider();
    const auto read = GattCharacteristicProperties::Read, write = GattCharacteristicProperties::WriteWithoutResponse;
    const Bytes info{0x11,0x01,0,2};
    s.Characteristic("info",0x2A4A,read,&info);
    s.Characteristic("map",0x2A4B,read,&kReportMap);
    s.Characteristic("control",0x2A4C,write);
    s.Characteristic("protocol",0x2A4E,read|write);
    s.Characteristic("keyboard",0x2A4D,read|GattCharacteristicProperties::Notify,nullptr,{1,1});
    s.Characteristic("leds",0x2A4D,read|write|GattCharacteristicProperties::Write,nullptr,{1,2});
    s.Characteristic("mouse",0x2A4D,read|GattCharacteristicProperties::Notify,nullptr,{2,1});
    s.Characteristic("absolute",0x2A4D,read|GattCharacteristicProperties::Notify,nullptr,{3,1});
    auto batteryResult = GattServiceProvider::CreateAsync(Uuid(0x180f)).get();
    if (batteryResult.Error() != BluetoothError::Success) throw std::runtime_error("Battery service creation failed");
    s.batteryProvider = batteryResult.ServiceProvider();
    s.Characteristic("battery",0x2A19,read);
}
void NativeHidBackend::Advertise(unsigned seconds) {
    if (seconds < 1 || seconds > 120) throw std::invalid_argument("Discovery must be 1..120 seconds");
    std::lock_guard<std::mutex> serialized(impl_->advertisementMutex);
    {
        std::lock_guard<std::mutex> lock(impl_->stateMutex);
        if (impl_->discovery.active || impl_->discovery.stopping) throw std::runtime_error("Discovery already active");
        impl_->discovery.Start(GetTickCount64(),seconds); impl_->stopCallReturned = false;
    }
    GattServiceProviderAdvertisingParameters batteryParameters;
    batteryParameters.IsConnectable(true); batteryParameters.IsDiscoverable(true);
    impl_->batteryProvider.StartAdvertising(batteryParameters);
    { std::lock_guard<std::mutex> lock(impl_->stateMutex); impl_->batteryPublicationPending = true; }
    GattServiceProviderAdvertisingParameters parameters;
    parameters.IsConnectable(true); parameters.IsDiscoverable(true);
    impl_->provider.StartAdvertising(parameters);
    { std::lock_guard<std::mutex> lock(impl_->stateMutex); impl_->hidPublicationPending = true; }
}
void NativeHidBackend::Poll() {
    bool expired;
    {
        std::lock_guard<std::mutex> lock(impl_->stateMutex);
        expired = impl_->discovery.ShouldAttempt(GetTickCount64()) &&
            !(impl_->persistentPublication && !impl_->bound.empty());
        if (impl_->enabled && GetTickCount64() > impl_->enableDeadline) impl_->enabled = false;
    }
    if (expired) impl_->StopAdvertising();
}
bool NativeHidBackend::PublicationStopFailed() const {
    std::lock_guard<std::mutex> lock(impl_->stateMutex);
    return impl_->discovery.Failed(GetTickCount64());
}
void NativeHidBackend::KeepPublishedWhileBound(bool keep) {
    std::lock_guard<std::mutex> lock(impl_->stateMutex);
    impl_->persistentPublication = keep;
}
void NativeHidBackend::SetActionGuard(std::function<bool()> guard) {
    std::lock_guard<std::mutex> lock(impl_->stateMutex);
    impl_->inputGuard = std::move(guard);
}
void NativeHidBackend::CancelInput() {
    impl_->cancel = true;
    std::lock_guard<std::mutex> lock(impl_->stateMutex);
    impl_->enabled = false;
}
NativeHidView NativeHidBackend::Inspect() {
    NativeHidView view;
    for (const auto& entry : impl_->Clients()) {
        auto found = impl_->peerDevices.find(entry.first);
        if (found == impl_->peerDevices.end()) {
            auto device = BluetoothLEDevice::FromIdAsync(entry.first).get();
            if (!device) continue;
            found = impl_->peerDevices.emplace(entry.first,device).first;
        }
        if (found->second.ConnectionStatus() != BluetoothConnectionStatus::Connected) continue;
        NativeHidPeer peer{Fingerprint(entry.first),to_string(found->second.Name()),false};
        for (auto absolute : impl_->chars.at("absolute").SubscribedClients())
            if (absolute.Session().DeviceId().Id() == entry.first && absolute.Session().SessionStatus() == GattSessionStatus::Active) peer.absolute = true;
        view.peers.push_back(std::move(peer));
    }
    std::lock_guard<std::mutex> lock(impl_->stateMutex);
    view.bound = impl_->bound.empty() ? "" : Fingerprint(impl_->bound);
    view.published = impl_->discovery.active;
    view.persistent = impl_->persistentPublication && !impl_->bound.empty();
    return view;
}
std::string NativeHidBackend::Status() const {
    const auto clients = impl_->Clients();
    const auto keyboardCount = impl_->SubscriptionCount("keyboard"), mouseCount = impl_->SubscriptionCount("mouse");
    const auto absoluteCount = impl_->SubscriptionCount("absolute");
    const auto advertisementStatus = impl_->provider ? static_cast<int>(impl_->provider.AdvertisementStatus()) : -1;
    const auto batteryAdvertisementStatus = impl_->batteryProvider ? static_cast<int>(impl_->batteryProvider.AdvertisementStatus()) : -1;
    SYSTEM_POWER_STATUS power{};
    const bool powerAvailable = GetSystemPowerStatus(&power) != FALSE;
    const auto batteryLevel = BatteryLevel(powerAvailable,power.BatteryFlag,power.BatteryLifePercent);
    const auto now = GetTickCount64();
    std::lock_guard<std::mutex> lock(impl_->stateMutex);
    std::ostringstream out;
    out << "{\"backend\":\"native_cpp_winrt\",\"characteristics\":" << impl_->chars.size()
        << ",\"advertisement_status\":" << advertisementStatus
        << ",\"battery_advertisement_status\":" << batteryAdvertisementStatus
        << ",\"battery_level\":" << (batteryLevel ? std::to_string(*batteryLevel) : "null")
        << ",\"battery_source\":\"GetSystemPowerStatus\""
        << ",\"battery_present\":" << (!powerAvailable || power.BatteryFlag == 255 ? "null" : (power.BatteryFlag & 128) ? "false" : "true")
        << ",\"hid_publication_pending\":" << (impl_->hidPublicationPending ? "true" : "false")
        << ",\"battery_publication_pending\":" << (impl_->batteryPublicationPending ? "true" : "false")
        << ",\"discovery_active\":" << (impl_->discovery.active ? "true" : "false")
        << ",\"stop_pending\":" << (impl_->discovery.stopping ? "true" : "false")
        << ",\"stop_attempts\":" << impl_->discovery.attempts
        << ",\"stop_call_returned\":" << (impl_->stopCallReturned ? "true" : "false")
        << ",\"discovery_remaining_ms\":" << (impl_->discovery.active && now < impl_->discovery.deadline ? impl_->discovery.deadline-now : 0)
        << ",\"publication_stop_failed\":" << (impl_->discovery.Failed(GetTickCount64()) ? "true" : "false")
        << ",\"keyboard_subscriptions\":" << keyboardCount << ",\"mouse_subscriptions\":" << mouseCount
        << ",\"absolute_subscriptions\":" << absoluteCount
        << ",\"read_callbacks\":" << impl_->readCallbacks << ",\"write_callbacks\":" << impl_->writeCallbacks
        << ",\"subscription_callbacks\":" << impl_->subscriptionCallbacks
        << ",\"input_enabled\":" << (impl_->enabled ? "true" : "false")
        << ",\"suspended\":" << (impl_->suspended ? "true" : "false")
        << ",\"bound\":\"" << (impl_->bound.empty() ? "" : Fingerprint(impl_->bound)) << "\",\"candidates\":[";
    bool first = true;
    for (auto& client : clients) { if (!first) out << ','; first = false; out << '"' << Fingerprint(client.first) << '"'; }
    out << "],\"read_write_encryption_required\":true,\"active_encryption_verified\":false,\"outcome_verified\":false"
        << ",\"last_release\":{\"keyboard\":\"" << impl_->lastRelease.at("keyboard") << "\",\"mouse\":\"" << impl_->lastRelease.at("mouse") << "\"}"
        << ",\"diagnostic_count\":" << impl_->diagnosticCount << ",\"diagnostics\":[";
    first = true;
    for (auto& entry : impl_->diagnostics) {
        if (!first) out << ','; first = false;
        out << "{\"stage\":\"" << entry.first << "\",\"hresult\":\"0x" << std::hex << std::setfill('0') << std::setw(8) << entry.second << "\"}" << std::dec;
    }
    out << "],\"diagnostic_reads\":" << (impl_->diagnosticReads ? "true" : "false") << ",\"reads_by_characteristic\":{";
    first = true;
    for (auto& entry : impl_->readsByCharacteristic) {
        if (!first) out << ','; first = false;
        out << '"' << entry.first << "\":" << entry.second;
    }
    out << "}}";
    return out.str();
}
void NativeHidBackend::Bind(const std::string& fingerprint) {
    const auto clients = impl_->Clients();
    if (clients.size() != 1 || Fingerprint(clients.begin()->first) != fingerprint)
        throw std::runtime_error("Require the fingerprint of exactly one keyboard/mouse subscriber");
    auto device = BluetoothLEDevice::FromIdAsync(clients.begin()->first).get();
    if (!device) throw std::runtime_error("Cannot inspect subscribed peer");
    const bool paired = device.DeviceInformation().Pairing().IsPaired();
    device.Close();
    if (!paired) throw std::runtime_error("Complete Windows/iPhone pairing first");
    {
        std::lock_guard<std::mutex> lock(impl_->stateMutex);
        impl_->bound = impl_->lastBound = clients.begin()->first; impl_->enabled = false; impl_->cancel = true;
    }
    // StopAdvertising withdraws the GATT service and destroys report subscriptions.
    // Binding only selects the peer; publication remains until the existing deadline.
}
void NativeHidBackend::EnableOneAction() {
    const auto clients = impl_->Clients();
    std::lock_guard<std::mutex> lock(impl_->stateMutex);
    if (impl_->closed || impl_->discovery.Failed(GetTickCount64()) || impl_->bound.empty() || clients.size() != 1 || clients.find(impl_->bound) == clients.end())
        throw std::runtime_error("Bind the intended phone first");
    impl_->enabled = true; impl_->enableDeadline = GetTickCount64() + 2000;
}
void NativeHidBackend::Move(int dx, int dy) {
    impl_->BeginAction();
    try {
        if (dx < -127 || dx > 127 || dy < -127 || dy > 127) throw std::invalid_argument("Movement is -127..127");
        impl_->Send("mouse",{0,static_cast<uint8_t>(dx),static_cast<uint8_t>(dy),0,0});
    } catch (...) { Stop(); throw; }
    Stop();
}
void NativeHidBackend::Click() {
    impl_->BeginAction();
    try {
        impl_->Send("mouse",{1,0,0,0,0});
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
    } catch (...) { Stop(); throw; }
    Stop();
}
void NativeHidBackend::Point(int x, int y) {
    impl_->BeginAction();
    try {
        if (x < 0 || x > 32767 || y < 0 || y > 32767) throw std::invalid_argument("Point coordinates are0..32767");
        impl_->Send("absolute",{0,static_cast<uint8_t>(x),static_cast<uint8_t>(x>>8),static_cast<uint8_t>(y),static_cast<uint8_t>(y>>8)});
    } catch (...) { Stop(); throw; }
    // Releases only keyboard/relative buttons. A zero absolute report would warp the pointer.
    Stop();
}
void NativeHidBackend::Tap(int x, int y) {
    impl_->BeginAction();
    try {
        if (x < 0 || x > 32767 || y < 0 || y > 32767) throw std::invalid_argument("Tap coordinates are 0..32767");
        impl_->Send("absolute",{0,static_cast<uint8_t>(x),static_cast<uint8_t>(x>>8),static_cast<uint8_t>(y),static_cast<uint8_t>(y>>8)});
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        impl_->Send("mouse",{1,0,0,0,0});
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        impl_->Send("mouse",{0,0,0,0,0});
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    } catch (...) { Stop(); throw; }
    Stop();
}
void NativeHidBackend::TapAbsolute(int x, int y) {
    impl_->BeginAction();
    try {
        if (x < 0 || x > 32767 || y < 0 || y > 32767) throw std::invalid_argument("Tap coordinates are 0..32767");
        Bytes position{0,static_cast<uint8_t>(x),static_cast<uint8_t>(x>>8),static_cast<uint8_t>(y),static_cast<uint8_t>(y>>8)};
        impl_->Send("absolute",position);
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        position[0]=1;impl_->Send("absolute",position);
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        position[0]=0;impl_->Send("absolute",position);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    } catch (...) { Stop(); throw; }
    Stop();
}
void NativeHidBackend::Pointer(int x, int y, bool down, bool transition) {
    impl_->BeginAction();
    try {
        if(x<0 || x>32767 || y<0 || y>32767) throw std::invalid_argument("Pointer coordinates are 0..32767");
        Bytes position{0,static_cast<uint8_t>(x),static_cast<uint8_t>(x>>8),static_cast<uint8_t>(y),static_cast<uint8_t>(y>>8)};
        if(down && transition) {
            impl_->Send("absolute",position);
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
        }
        position[0]=down?1:0; impl_->Send("absolute",position);
    } catch (...) { Stop(); throw; }
}
void NativeHidBackend::Swipe(int x, int y, int endX, int endY) {
    impl_->BeginAction();
    try {
        if(x<0 || x>32767 || y<0 || y>32767 || endX<0 || endX>32767 || endY<0 || endY>32767)
            throw std::invalid_argument("Swipe coordinates are 0..32767");
        auto position=[](int a,int b,bool down){return Bytes{static_cast<uint8_t>(down?1:0),static_cast<uint8_t>(a),static_cast<uint8_t>(a>>8),static_cast<uint8_t>(b),static_cast<uint8_t>(b>>8)};};
        impl_->Send("absolute",position(x,y,false));
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        impl_->Send("absolute",position(x,y,true));
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        for(int step=1;step<=8;++step) {
            impl_->Send("absolute",position(x+(endX-x)*step/8,y+(endY-y)*step/8,true));
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        impl_->Send("absolute",position(endX,endY,false));
    } catch (...) { Stop(); throw; }
    Stop();
}
void NativeHidBackend::HomeButton(int x,int y) {
    impl_->BeginAction();
    try {
        Bytes position;bool known;
        {std::lock_guard<std::mutex> lock(impl_->stateMutex);
          known=impl_->haveAbsolutePosition;position=AbsoluteButtons(impl_->values["absolute"],known,0);}
        if(x!=-1 || y!=-1){
            if(x<0 || x>32767 || y<0 || y>32767)throw std::invalid_argument("Secondary click coordinates are 0..32767");
            position={0,static_cast<uint8_t>(x),static_cast<uint8_t>(x>>8),static_cast<uint8_t>(y),static_cast<uint8_t>(y>>8)};
            known=false; // explicit right-click position must precede the button pulse
        }
        if(!known){
            impl_->Send("absolute",position); // first-use neutral positioning, never a left click
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
        }
        impl_->Send("absolute",AbsoluteButtons(position,true,2));
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        impl_->Send("absolute",AbsoluteButtons(position,true,0));
    } catch (...) { Stop(); throw; }
    Stop();
}
void NativeHidBackend::PressKey(unsigned char usage) {
    impl_->BeginAction();
    try {
        if (usage != 40 && usage != 42 && usage != 43 && (usage < 79 || usage > 82))
            throw std::invalid_argument("Unsupported focused-window key");
        impl_->Send("keyboard",{0,0,usage,0,0,0,0,0});
    } catch (...) { Stop(); throw; }
    Stop();
}
void NativeHidBackend::Type(const std::string& text,bool releaseAll) {
    impl_->BeginAction();
    try {
        if (text.empty() || text.size() > 80) throw std::invalid_argument("Type accepts 1..80 US ASCII characters");
        std::vector<Bytes> reports;
        for (unsigned char c : text) reports.push_back(KeyReport(c));
        for (auto& report : reports) { impl_->Send("keyboard",report); impl_->Send("keyboard",Bytes(8)); }
    } catch (...) { Stop(); throw; }
    if(releaseAll)Stop(); // every character already sent its keyboard-up report
}
void NativeHidBackend::Stop() {
    auto& s = *impl_;
    s.cancel = true;
    Bytes absoluteRelease;
    {
        std::lock_guard<std::mutex> lock(s.stateMutex);
        s.enabled = false; s.values["keyboard"] = Bytes(8); s.values["mouse"] = Bytes(5);
        // A failed up notification still needs a release. Retain known XY rather
        // than assuming the last submitted report reached the phone.
        if(s.haveAbsolutePosition)absoluteRelease=AbsoluteButtons(s.values["absolute"],true,0);
    }
    if (!s.provider) return;
    if(!absoluteRelease.empty())try{s.Send("absolute",absoluteRelease,true);}
    catch(...){s.Diagnostic("release_absolute",0x80004005);}
    for (auto& pair : {std::make_pair("keyboard",8),std::make_pair("mouse",5)}) {
        std::string result = "submitted";
        bool wasBound;
        { std::lock_guard<std::mutex> lock(s.stateMutex); wasBound = !s.lastBound.empty(); }
        if (!wasBound) result = "not_bound";
        else try { s.Send(pair.first,Bytes(pair.second),true); }
        catch (hresult_error const& error) {
            std::ostringstream text; text << "error_0x" << std::hex << static_cast<uint32_t>(error.code()); result = text.str();
            s.Diagnostic(std::string("release_")+pair.first,static_cast<uint32_t>(error.code()));
        } catch (...) { result = "not_confirmed"; s.Diagnostic(std::string("release_")+pair.first,0x80004005); }
        { std::lock_guard<std::mutex> lock(s.stateMutex); s.lastRelease[pair.first] = result; }
    }
}
void NativeHidBackend::Shutdown() {
    {
        std::lock_guard<std::mutex> lock(impl_->stateMutex);
        if (impl_->closed) return;
        impl_->closed = true;
    }
    Stop(); impl_->StopAdvertising();
    for (auto it = impl_->unregister.rbegin(); it != impl_->unregister.rend(); ++it) (*it)();
    impl_->unregister.clear();
    for (auto& peer : impl_->peerDevices) peer.second.Close();
    impl_->peerDevices.clear();
}
