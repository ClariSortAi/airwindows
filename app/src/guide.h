// The setup guide: a panel docked beside the picture that walks a new user
// from "on the network" to "controlling the phone". Each step ticks itself off
// when the receiver sees it happen, so the panel never asks "did that work?".
#pragma once

#include <windows.h>

#include <memory>
#include <string>
#include <vector>

struct GuideState {
    std::wstring receiverName;
    std::wstring addresses;            // empty while the PC has no network
    bool mirroring = false;
    std::wstring mirrorDevice;         // name the mirroring device reports
    bool pairingOpen = false;          // the PC is visible in the phone's Bluetooth list
    std::vector<std::wstring> phones;  // paired phones that can be chosen
    bool chosen = false;               // one of them is selected
    bool ready = false;                // and it is the phone being mirrored
    bool controlled = false;           // the user has clicked or typed on the phone
    std::wstring bluetoothStatus;      // the status line, repeated under the pairing step

    bool operator==(const GuideState& other) const;
};

class Guide {
public:
    // WM_COMMAND ids sent to the parent: pair, choose phone N (firstPhone + N),
    // and close (the user is done with the guide or wants it out of the way).
    struct Commands {
        int pair = 0;
        int firstPhone = 0;
        int close = 0;
    };
    static constexpr int kMaxPhones = 3;

    Guide();
    ~Guide();
    Guide(const Guide&) = delete;
    Guide& operator=(const Guide&) = delete;

    bool Create(HWND parent, HINSTANCE instance, const Commands& commands);
    HWND Window() const;
    // Fonts stay owned by the caller and must outlive the panel or be replaced first.
    void SetFonts(HFONT text, HFONT bold, HFONT title, HFONT icons);
    void Update(const GuideState& state);
    // True once every step is ticked.
    bool Complete() const;

    // Panel width for `reference`'s DPI.
    static int Width(HWND reference);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
