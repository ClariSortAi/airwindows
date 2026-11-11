// Which Bluetooth phone belongs to which AirPlay sender. Windows cannot tell that a
// Bluetooth phone and a mirroring phone are the same device, so the user chooses once
// per phone and the receiver remembers the choice.
#pragma once
#include <string>
#include <utility>
#include <vector>

// Sender and phone are the 16-character lowercase hexadecimal fingerprints used in
// settings.ini. Stored newest first as "sender=phone;sender=phone".
class PhonePairs {
public:
    static constexpr size_t kMax = 12;  // 407 characters, inside the 512 the settings reader takes
    static bool Token(const std::string& text) {
        return text.size() == 16 && text.find_first_not_of("0123456789abcdef") == std::string::npos;
    }
    // Entries that are malformed, repeat a sender or exceed kMax are dropped.
    static PhonePairs Parse(const std::string& text) {
        PhonePairs result;
        for (size_t start = 0; start < text.size();) {
            size_t end = text.find(';', start);
            if (end == std::string::npos) end = text.size();
            const std::string item = text.substr(start, end - start);
            if (item.size() == 33 && item[16] == '=') {
                const std::string sender = item.substr(0, 16), phone = item.substr(17);
                if (Token(sender) && Token(phone) && result.Phone(sender).empty() && result.pairs_.size() < kMax)
                    result.pairs_.emplace_back(sender, phone);
            }
            start = end + 1;
        }
        return result;
    }
    std::string Serialize() const {
        std::string text;
        for (const auto& pair : pairs_) {
            if (!text.empty()) text += ';';
            text += pair.first + '=' + pair.second;
        }
        return text;
    }
    // The phone chosen for this sender; empty when the sender is new.
    std::string Phone(const std::string& sender) const {
        for (const auto& pair : pairs_) if (pair.first == sender) return pair.second;
        return {};
    }
    // Moves the sender to the front. One phone may keep several senders: its AirPlay
    // identity is a private Wi-Fi address, which differs from network to network.
    void Set(const std::string& sender, const std::string& phone) {
        if (!Token(sender) || !Token(phone)) return;
        for (auto pair = pairs_.begin(); pair != pairs_.end(); ++pair) {
            if (pair->first == sender) { pairs_.erase(pair); break; }
        }
        pairs_.insert(pairs_.begin(), {sender, phone});
        if (pairs_.size() > kMax) pairs_.pop_back();
    }
    // True when this phone was chosen for some other sender and not for this one: it is
    // most likely a different iPhone from the one mirroring.
    bool Elsewhere(const std::string& phone, const std::string& sender) const {
        bool other = false;
        for (const auto& pair : pairs_) {
            if (pair.second != phone) continue;
            if (pair.first == sender) return false;
            other = true;
        }
        return other;
    }
    bool Empty() const { return pairs_.empty(); }
private:
    std::vector<std::pair<std::string, std::string>> pairs_;
};

// Which phone the receiver wants selected for the mirror that is running. Pure state:
// the control worker applies it to the Bluetooth backend. A mirror from another phone
// changes `wanted`; it never closes Bluetooth, so the other phones stay connected and
// switching back needs no pairing.
struct PhoneLink {
    PhonePairs pairs;
    std::string sender;        // AirPlay sender of the current mirror, or of the last one
    std::string wanted;        // phone to select as soon as its Bluetooth is subscribed
    std::string wantedSender;  // sender `wanted` was chosen for; empty until its first mirror
    bool learn = false;        // `wanted` was chosen with no mirror running; a new sender is its mirror
    bool force = false;        // --control-phone: the first sender is its mirror, known or not
    bool session = false;      // a sender connected and its mirror has not ended
    bool detached = false;     // Disconnect was chosen; Bluetooth stays closed until Pair or a choice

    // lastPhone and lastSender are the pair in use when the receiver last ran.
    void Start(const std::string& pairsText, const std::string& lastPhone, const std::string& lastSender, bool bootstrap) {
        pairs = PhonePairs::Parse(pairsText);
        const bool last = PhonePairs::Token(lastPhone);
        if (last && !bootstrap && PhonePairs::Token(lastSender) && pairs.Phone(lastSender).empty())
            pairs.Set(lastSender, lastPhone);  // settings written before more than one pair was kept
        const std::string known = bootstrap ? std::string() : pairs.Phone(lastSender);
        wanted = !known.empty() ? known : last ? lastPhone : std::string();
        wantedSender = !known.empty() ? lastSender : std::string();
        force = bootstrap && last;
        learn = false;  // a saved phone with no saved sender waits to be chosen again
        sender.clear();
        session = detached = false;
    }
    // A sender asked to mirror. Returns true when Bluetooth should be opened so that
    // sender's phone can connect.
    bool OnSender(const std::string& token) {
        std::string next = pairs.Phone(token);
        if (force && PhonePairs::Token(token) && !wanted.empty()) {
            pairs.Set(token, wanted);
            next = wanted;
        } else if (next.empty() && learn && PhonePairs::Token(token) && !wanted.empty()) {
            pairs.Set(token, wanted);
            next = wanted;
        } else if (!next.empty()) {
            pairs.Set(token, next);  // most recent first
        }
        force = learn = false;
        sender = token;
        wanted = next;
        wantedSender = token;
        session = true;
        return !wanted.empty() && !detached;
    }
    // The user picked a phone from the list.
    void OnChoose(const std::string& phone) {
        detached = force = false;
        wanted = phone;
        if (session && PhonePairs::Token(sender)) {
            pairs.Set(sender, phone);
            wantedSender = sender;
            learn = false;
        } else {
            wantedSender.clear();
            learn = true;
        }
    }
    void OnMirrorEnded() { session = false; }
    void OnPair() { detached = false; }
    void OnDisconnect() {
        detached = true;
        wanted.clear();
        wantedSender.clear();
        learn = force = false;
    }
    // Input may go to `selected`: it is the phone chosen for the sender on screen.
    bool Matches(const std::string& selected) const {
        return !selected.empty() && selected == wanted && !sender.empty() && sender == wantedSender;
    }
    // `selected` belongs to a different mirror from the one that is running.
    bool MustRelease(const std::string& selected) const {
        return session && !selected.empty() && selected != wanted;
    }
    // A listed phone that is probably not the one mirroring.
    bool Elsewhere(const std::string& phone) const { return session && pairs.Elsewhere(phone, sender); }
};
