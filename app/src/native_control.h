#pragma once
#include <memory>
#include <string>
#include <vector>
#include "control_queue.h"

struct ControlPeer {
    std::string token, name;
    bool elsewhere = false; // chosen for another mirror, so probably not the phone on screen
};
struct NativeControlState {
    std::vector<ControlPeer> peers;
    std::string bound, sender, message = "Bluetooth control disconnected";
    std::string pairs;      // every remembered sender and phone, as settings.ini stores them
    bool ready = false, focused = false, published = false, busy = false;
    bool waiting = false;   // the phone for this mirror is known; its Bluetooth has not connected yet
};
class NativeControl {
public:
    NativeControl();
    ~NativeControl();
    // `pairs` is every remembered sender and phone; the other two are the pair last in use.
    // With `bootstrap`, the first sender to mirror is taken as rememberedPhone's.
    void Start(const std::string& pairs = {}, std::string rememberedPhone = {}, std::string rememberedSender = {}, bool bootstrap = false);
    void Shutdown();
    void Pair();
    void Bind(std::string token);
    void Disconnect();
    void Focus(bool focused);
    void Mirror(bool active);
    void Pause(bool paused);
    void Picture(bool present, int width, int height, uint64_t generation=0);
    void AwaitPicture(uint64_t generation);
    void Invalidate();
    void Sender(const std::string& identity);
    void Submit(ControlKind kind, int x=0, int y=0, unsigned char key=0);
    void SubmitText(std::string text);
    void RejectInput(const std::string& message);
    NativeControlState State() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
