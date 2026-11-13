// What the Microsoft Store's answer about this app's license means for the
// receiver. Pure: no Store calls, no clock of its own, no UI. store_license.cpp
// asks the Store; LicenseCheck tests this mapping.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace store_license {

// The fields of Windows.Services.Store.StoreAppLicense the receiver uses.
struct Answer {
    bool ok = false;         // the Store returned a license; false when the call threw or timed out
    bool isActive = false;   // StoreAppLicense.IsActive
    bool isTrial = false;    // StoreAppLicense.IsTrial
    bool fromStore = false;  // StoreAppLicense.SkuStoreId is not empty: the Store issued this license
    int64_t expires = 0;     // StoreAppLicense.ExpirationDate in Unix seconds; 0 or less when missing
};

enum class Kind {
    NotStore,  // not installed from the Store, or no answer yet: no licensing UI
    Full,      // bought
    Trial,     // free trial, still running
    Inactive,  // the Store says the license is not active: the trial has ended
};

struct State {
    Kind kind = Kind::NotStore;
    int daysLeft = -1;  // Trial only: whole days left, rounded up; 0 on the last day; -1 when unknown
};

// Fails open: an answer that is missing, or a license the Store did not issue,
// never locks anything. Only an active-false license from the Store does.
State Map(const Answer& answer, int64_t now);

// What this run has learned from the Store. Pure: main.cpp keeps one, records each read
// and purchase outcome in it and asks it for the state to show.
struct Tracker {
    std::optional<Answer> last;    // the last read that succeeded (ok); failed reads are not kept
    bool purchased = false;        // a purchase came back Succeeded or AlreadyPurchased in this run
    int64_t nextExpiryCheck = 0;   // earliest time to ask the Store about a trial past its local expiry

    // A failed read changes nothing: the last successful answer stands.
    void Record(const Answer& answer);
    // Succeeded or AlreadyPurchased: Full for the rest of the run, whatever later reads say,
    // because a read right after a purchase can still return the old license.
    void RecordPurchase(bool owned);
    // Full after a purchase or before any successful read; otherwise the last answer mapped
    // at `now`. The local clock alone never locks: a trial past its expiry stays a trial
    // with 0 days left until the Store says otherwise.
    State Current(int64_t now) const;
    // True when the last answer is a Store trial whose expiry the local clock has passed, so
    // the Store must be asked; then again at most every 10 minutes until it answers otherwise.
    bool TakeExpiryCheck(int64_t now);
};

// True when `answer` should thank the buyer: a real Store answer, active and not a trial,
// arriving while the app showed a real trial or a locked state.
bool Unlocked(Kind before, const Answer& answer);

// False only for Inactive: AirPlay and Bluetooth stay off.
inline bool AllowsService(Kind kind) {
    return kind != Kind::Inactive;
}

const char* KindName(Kind kind);

// --selftest-license <trial:N|expired|licensed>: the answer the Store would give.
// N is 1 to 365 days. Nothing else is accepted.
std::optional<Answer> ParseSelfTest(const std::wstring& value, int64_t now);

}  // namespace store_license
