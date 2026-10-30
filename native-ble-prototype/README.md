# Native Windows Bluetooth backend

This directory supplies the BLE keyboard and absolute mouse used by AirWindows. The GUI owns its lifetime and input queue. Build the application from `app/scripts/build.ps1`; there is no separate service to install.

The adapter must support Bluetooth Low Energy peripheral mode. The implementation uses Windows GATT services, encrypted HID reports and a selected peer fingerprint. It remembers the selected phone through the application's settings.

`test_discovery_window.cpp` checks discovery expiry and battery reporting without opening a Bluetooth adapter. `app/tests/control_policy_check.cpp` covers coordinates, input queues, paste validation and button release behavior. Run both with `scripts/check.ps1` from the repository root.

The absolute report descriptor is adapted from [iphone-use](https://github.com/xhoantran/iphone-use). Its MIT notice is in [LICENSE-iphone-use](LICENSE-iphone-use). The surrounding AirWindows implementation is covered by the [application license](../app/LICENSE).
