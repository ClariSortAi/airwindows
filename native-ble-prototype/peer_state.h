#pragma once
#include <cstdint>
#include <map>

// HID Control Point (suspend) and Protocol Mode belong to the phone that wrote them.
// One shared flag let a locked phone that was merely connected stop the selected
// phone's input, so each phone keeps its own.
template<class Key>
class PeerHidStates {
public:
    // Both return true when the selected phone wrote, so its input in progress must stop.
    bool Suspend(const Key& peer, bool suspended, const Key& selected) {
        states_[peer].suspended = suspended;
        return peer == selected;
    }
    bool SetProtocol(const Key& peer, uint8_t mode, const Key& selected) {
        states_[peer].protocol = mode;
        return peer == selected;
    }
    bool Suspended(const Key& peer) const {
        const auto found = states_.find(peer);
        return found != states_.end() && found->second.suspended;
    }
    // 1 is report mode, which a HID device returns to on every new connection.
    uint8_t Protocol(const Key& peer) const {
        const auto found = states_.find(peer);
        return found == states_.end() ? uint8_t{1} : found->second.protocol;
    }
    // Forget phones that left, so a phone that reconnects starts awake and in report mode.
    // `present` is any container with count(), such as std::set<Key>.
    template<class Present>
    void Retain(const Present& present) {
        for (auto entry = states_.begin(); entry != states_.end();) {
            if (present.count(entry->first)) ++entry;
            else entry = states_.erase(entry);
        }
    }
    unsigned SuspendedCount() const {
        unsigned count = 0;
        for (const auto& entry : states_) if (entry.second.suspended) ++count;
        return count;
    }
private:
    struct State { bool suspended = false; uint8_t protocol = 1; };
    std::map<Key, State> states_;
};
