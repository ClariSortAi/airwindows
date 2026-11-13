#include "license_state.h"

namespace store_license {
namespace {

constexpr int64_t kDay = 24 * 60 * 60;
// ExpirationDate for a license without a time limit is 31 December 9999; treat
// anything past 9000 as "no expiry".
constexpr int64_t kNoExpiry = 221845392000;  // 9000-01-01T00:00:00Z

}  // namespace

State Map(const Answer& answer, int64_t now) {
    State state;
    if (!answer.ok) {
        state.kind = Kind::Full;  // no answer: never lock out someone who may have paid
        return state;
    }
    if (!answer.fromStore) {
        state.kind = Kind::NotStore;  // registered locally, not installed from the Store
        return state;
    }
    if (!answer.isActive) {
        state.kind = Kind::Inactive;
        return state;
    }
    if (!answer.isTrial) {
        state.kind = Kind::Full;
        return state;
    }
    state.kind = Kind::Trial;
    if (answer.expires <= 0 || answer.expires >= kNoExpiry) {
        return state;  // days left unknown
    }
    const int64_t remaining = answer.expires - now;
    state.daysLeft = remaining <= 0 ? 0 : static_cast<int>((remaining + kDay - 1) / kDay);
    return state;
}

void Tracker::Record(const Answer& answer) {
    if (answer.ok) {
        last = answer;
    }
}

void Tracker::RecordPurchase(bool owned) {
    purchased = purchased || owned;
}

State Tracker::Current(int64_t now) const {
    if (purchased || !last) {
        State full;
        full.kind = Kind::Full;
        return full;
    }
    return Map(*last, now);
}

bool Tracker::TakeExpiryCheck(int64_t now) {
    if (purchased || !last || !last->fromStore || !last->isActive || !last->isTrial || last->expires <= 0 ||
        last->expires >= kNoExpiry || now < last->expires || now < nextExpiryCheck) {
        return false;
    }
    nextExpiryCheck = now + 10 * 60;
    return true;
}

bool Unlocked(Kind before, const Answer& answer) {
    return answer.ok && answer.fromStore && answer.isActive && !answer.isTrial &&
           (before == Kind::Trial || before == Kind::Inactive);
}

const char* KindName(Kind kind) {
    switch (kind) {
    case Kind::NotStore: return "not-store";
    case Kind::Full: return "full";
    case Kind::Trial: return "trial";
    case Kind::Inactive: return "inactive";
    }
    return "unknown";
}

std::optional<Answer> ParseSelfTest(const std::wstring& value, int64_t now) {
    Answer answer;
    answer.ok = true;
    answer.fromStore = true;
    if (value == L"licensed") {
        answer.isActive = true;
        return answer;
    }
    if (value == L"expired") {
        answer.isTrial = true;
        answer.expires = now - kDay;
        return answer;
    }
    const std::wstring prefix = L"trial:";
    if (value.size() <= prefix.size() || value.size() > prefix.size() + 3 || value.compare(0, prefix.size(), prefix) != 0 ||
        value.find_first_not_of(L"0123456789", prefix.size()) != std::wstring::npos) {
        return std::nullopt;
    }
    const int days = std::stoi(value.substr(prefix.size()));
    if (days < 1 || days > 365) {
        return std::nullopt;
    }
    answer.isActive = true;
    answer.isTrial = true;
    answer.expires = now + days * kDay;
    return answer;
}

}  // namespace store_license
