// Documentation this follows (Microsoft Learn, read 2026-11-13):
//   https://learn.microsoft.com/uwp/api/windows.devices.bluetooth.bluetoothadapter.getdefaultasync
//     The default adapter. A null result is treated as "no adapter", as the backend does.
//   https://learn.microsoft.com/uwp/api/windows.devices.bluetooth.bluetoothadapter.isperipheralrolesupported
//     "if the adapater supports LowEnergy peripheral role", which a GATT HID service needs.
//     Needs the bluetooth capability, which the MSIX manifest declares.
// The same two checks run again in native-ble-prototype/native_hid_backend.cpp when the
// user pairs; the wording here follows the messages there.

#include "bluetooth_check.h"

#include <windows.h>

#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Foundation.h>

#include <exception>

#include "log.h"

namespace bluetooth_check {

Adapter Probe() {
    Adapter result = Adapter::Unknown;
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (const winrt::hresult_error& error) {
        LOGW("[BT] adapter check could not start COM (0x%08lX)", static_cast<unsigned long>(static_cast<int32_t>(error.code())));
        return result;
    }
    try {
        const auto adapter = winrt::Windows::Devices::Bluetooth::BluetoothAdapter::GetDefaultAsync().get();
        if (!adapter) {
            result = Adapter::Missing;
            LOGW("[BT] adapter check: no Bluetooth adapter; phone control is unavailable, mirroring is not affected");
        } else {
            const bool peripheral = adapter.IsPeripheralRoleSupported();
            result = peripheral ? Adapter::Ready : Adapter::NoPeripheral;
            LOGI("[BT] adapter check: low energy=%d peripheral role=%d central role=%d",
                 adapter.IsLowEnergySupported() ? 1 : 0, peripheral ? 1 : 0, adapter.IsCentralRoleSupported() ? 1 : 0);
            if (!peripheral) {
                LOGW("[BT] adapter check: the adapter has no Bluetooth LE peripheral role; phone control is unavailable");
            }
        }
    } catch (const winrt::hresult_error& error) {
        LOGW("[BT] adapter check failed (0x%08lX: %ls); saying nothing until Pair Bluetooth is clicked",
             static_cast<unsigned long>(static_cast<int32_t>(error.code())), error.message().c_str());
        result = Adapter::Unknown;
    } catch (const std::exception& error) {
        LOGW("[BT] adapter check failed (%s); saying nothing until Pair Bluetooth is clicked", error.what());
        result = Adapter::Unknown;
    }
    winrt::uninit_apartment();
    return result;
}

const char* Name(Adapter adapter) {
    switch (adapter) {
    case Adapter::Unknown: return "unknown";
    case Adapter::Ready: return "ready";
    case Adapter::Missing: return "none";
    case Adapter::NoPeripheral: return "no-peripheral";
    }
    return "unknown";
}

std::optional<Adapter> ParseSelfTest(const std::wstring& value) {
    if (value == L"none") return Adapter::Missing;
    if (value == L"no-peripheral") return Adapter::NoPeripheral;
    if (value == L"ready") return Adapter::Ready;
    return std::nullopt;
}

std::wstring StatusText(Adapter adapter) {
    switch (adapter) {
    case Adapter::Missing:
        return L"No Bluetooth adapter found. Mirroring works; phone control needs one.";
    case Adapter::NoPeripheral:
        return L"This PC's Bluetooth adapter cannot act as a keyboard and mouse. Mirroring still works.";
    case Adapter::Unknown:
    case Adapter::Ready:
        break;
    }
    return {};
}

std::wstring GuideText(Adapter adapter) {
    switch (adapter) {
    case Adapter::Missing:
        return L"No Bluetooth adapter found. Mirroring works without one. To control the iPhone from this PC, "
               L"turn Bluetooth on in Windows, or add a Bluetooth adapter that can act as a keyboard and mouse "
               L"(Bluetooth LE peripheral role).";
    case Adapter::NoPeripheral:
        return L"This PC's Bluetooth adapter cannot act as a keyboard and mouse (no Bluetooth LE peripheral "
               L"role). Mirroring still works. To control the iPhone from this PC, use a Bluetooth adapter that "
               L"supports the peripheral role.";
    case Adapter::Unknown:
    case Adapter::Ready:
        break;
    }
    return {};
}

}  // namespace bluetooth_check
