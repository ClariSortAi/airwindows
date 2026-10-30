// Experimental C++/WinRT BLE HID backend; no window hooks or automatic input.
#pragma once
#include <memory>
#include <string>
#include <vector>
#include <functional>

struct NativeHidPeer { std::string fingerprint, name; bool absolute = false; };
struct NativeHidView {
    std::vector<NativeHidPeer> peers;
    std::string bound;
    bool published = false, persistent = false;
};

class NativeHidBackend {
public:
    NativeHidBackend();
    ~NativeHidBackend();
    NativeHidBackend(const NativeHidBackend&) = delete;
    NativeHidBackend& operator=(const NativeHidBackend&) = delete;

    void Initialize(bool diagnosticReads = false); // local service only, never advertises
    void Advertise(unsigned seconds);   // bounded 1..120 second discovery
    void Poll();                       // expire discovery; invoke at least 10 Hz
    bool PublicationStopFailed() const; // caller must end this backend/process
    std::string Status() const;         // JSON; no raw peer identifiers
    void Bind(const std::string& fingerprint);
    void EnableOneAction();            // expires in two seconds
    NativeHidView Inspect();           // worker only; resolves names without pairing
    void KeepPublishedWhileBound(bool keep); // GUI connected mode; CLI defaults false
    void SetActionGuard(std::function<bool()> guard); // checks queued action lease before each send
    void CancelInput();               // thread safe; no Bluetooth calls or waits
    void PressKey(unsigned char usage); // Enter, Tab, Backspace and arrows
    void Move(int dx, int dy);
    void Point(int x, int y);          // absolute 0..32767; report3, no button press
    void Tap(int x, int y);            // atomic absolute position, relative press/release
    void TapAbsolute(int x, int y);    // diagnostic: same-coordinate absolute buttons only
    void Pointer(int x, int y, bool down, bool transition); // focused GUI gesture; caller bounds hold and releases
    void Swipe(int x, int y, int endX, int endY); // bounded absolute-button drag
    void HomeButton(int x=-1,int y=-1); // button2 at optional absolute XY; user maps it to Home
    void Click();
    void Type(const std::string& text, bool releaseAll=true); // US ASCII, 1..80 chars, bounded two seconds
    void Stop();                       // cancel input and send release reports
    void Shutdown();                    // Stop, stop advertising, unregister events
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
