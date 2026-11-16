// Help > Rate PhonePort… and the one-time status bar line that points to it. Pure: no
// Store call, no clock, no UI, no settings file. main.cpp asks the Store (store_license.cpp)
// and saves the two settings.ini keys; LicenseCheck tests this file.
//
// Microsoft Learn (read 2026-11-16):
//   https://learn.microsoft.com/windows/uwp/monetize/request-ratings-and-reviews
//     "all reviews must send the user to the Store's rating mechanisms, regardless of star
//     rating chosen": every user gets the same Store dialog or Store page, nothing is filtered.
//   https://learn.microsoft.com/windows/apps/publish/store-policies (7.20): no "ratings and
//   review manipulation"; Developer Code of Conduct 3: no attempt "to manipulate the ranking,
//   search results, ratings or reviews". So nothing is offered in return for a rating.
#pragma once

#include <optional>
#include <string>

#include "license_state.h"

namespace rating {

// The Store's own review page for this product, opened when the in-app dialog is unavailable.
// Learn: launch-store-app, "ms-windows-store://review/?ProductId=... Launches the write a
// review experience for a product".
constexpr wchar_t kReviewUri[] = L"ms-windows-store://review/?ProductId=9N62X9J9KVT8";

// Separate mirror sessions in which Bluetooth control was used before the line shows. The
// first session is mostly setup (pairing, choosing the phone); by the third the controls have
// worked on this PC more than once and the user came back for them. Three also fits inside
// the seven-day trial for someone who uses PhonePort every other day.
constexpr int kSessionsBeforeNudge = 3;

// The menu offers a rating in a copy the Store issued (a Store license was read, or bought in
// this run), bought or in its trial alike. Not while the license is inactive: that screen is
// about buying, and the trial has ended.
inline bool Offered(bool storeCopy, store_license::Kind kind) {
    return storeCopy && (kind == store_license::Kind::Full || kind == store_license::Kind::Trial);
}

// The one-time line. `sessions` and `shown` are saved in settings.ini; `counted` is not.
struct Nudge {
    int sessions = 0;      // RateControlSessions: mirror sessions with control used, at most kSessionsBeforeNudge
    bool shown = false;    // RateNudgeShown: the line was on screen once; never again
    bool counted = false;  // this mirror session has already been counted

    // Call on every control tick. A session counts once, the first time the phone has the
    // keyboard and mouse while a mirror runs; it ends when the mirror does. True when
    // `sessions` changed and should be saved.
    bool Observe(bool mirroring, bool controlling) {
        if (!mirroring) {
            counted = false;
            return false;
        }
        if (!controlling || counted) {
            return false;
        }
        counted = true;
        if (shown || sessions >= kSessionsBeforeNudge) {
            return false;
        }
        ++sessions;
        return true;
    }

    // Whether to put the line up now. `offered`: Help > Rate PhonePort… exists. `quiet`: the
    // status bar is on screen with nothing more important on it (no confirmation, not
    // controlling the phone, no warning). Never while locked, and not on the trial's last day.
    bool Due(bool offered, const store_license::State& state, bool quiet) const {
        if (shown || sessions < kSessionsBeforeNudge || !offered || !quiet) {
            return false;
        }
        if (!store_license::AllowsService(state.kind)) {
            return false;
        }
        return !(state.kind == store_license::Kind::Trial && state.daysLeft == 0);
    }
};

// --selftest-rating <0-3|shown>: the saved state to start this run with. Nothing is saved.
inline std::optional<Nudge> ParseSelfTest(const std::wstring& value) {
    Nudge nudge;
    if (value == L"shown") {
        nudge.sessions = kSessionsBeforeNudge;
        nudge.shown = true;
        return nudge;
    }
    if (value.size() != 1 || value[0] < L'0' || value[0] > static_cast<wchar_t>(L'0' + kSessionsBeforeNudge)) {
        return std::nullopt;
    }
    nudge.sessions = value[0] - L'0';
    return nudge;
}

}  // namespace rating
