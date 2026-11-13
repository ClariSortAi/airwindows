// LicenseCheck: what a Microsoft Store license answer means for the receiver
// (app/src/license_state.cpp) and the --selftest-license parser. No Store call,
// no window, no settings.

#include <cstdio>
#include <string>

#include "license_state.h"

namespace {

using store_license::Answer;
using store_license::Kind;
using store_license::Map;
using store_license::ParseSelfTest;

constexpr int64_t kDay = 24 * 60 * 60;
constexpr int64_t kNow = 1794000000;  // a fixed "now"; the mapping takes the clock as an argument

int g_checks = 0;
int g_failures = 0;

void Check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL %s\n", what);
    }
}

Answer StoreAnswer(bool active, bool trial, int64_t expires) {
    Answer answer;
    answer.ok = true;
    answer.fromStore = true;
    answer.isActive = active;
    answer.isTrial = trial;
    answer.expires = expires;
    return answer;
}

void CheckMapping() {
    Check(Map(StoreAnswer(true, false, 253402214400), kNow).kind == Kind::Full, "active full license: full");
    Check(Map(StoreAnswer(true, false, 0), kNow).kind == Kind::Full, "active full license without expiry: full");

    const auto trial = Map(StoreAnswer(true, true, kNow + 7 * kDay), kNow);
    Check(trial.kind == Kind::Trial && trial.daysLeft == 7, "trial ending in exactly 7 days: 7 days left");
    Check(Map(StoreAnswer(true, true, kNow + 5 * kDay), kNow).daysLeft == 5, "trial ending in 5 days: 5");
    Check(Map(StoreAnswer(true, true, kNow + 5 * kDay + 1), kNow).daysLeft == 6, "5 days and a second: rounds up to 6");
    Check(Map(StoreAnswer(true, true, kNow + 6 * kDay - 1), kNow).daysLeft == 6, "a second short of 6 days: 6");
    Check(Map(StoreAnswer(true, true, kNow + kDay), kNow).daysLeft == 1, "exactly one day: 1");
    Check(Map(StoreAnswer(true, true, kNow + 1), kNow).daysLeft == 1, "one second left: 1");
    Check(Map(StoreAnswer(true, true, kNow), kNow).daysLeft == 0, "expiry now, still active: 0 (last day)");

    const auto pastActive = Map(StoreAnswer(true, true, kNow - kDay), kNow);
    Check(pastActive.kind == Kind::Trial && pastActive.daysLeft == 0,
          "expiry in the past but the Store still says active: trial, 0 days, not locked");
    const auto noExpiry = Map(StoreAnswer(true, true, 0), kNow);
    Check(noExpiry.kind == Kind::Trial && noExpiry.daysLeft == -1, "trial with zero expiry: days unknown");
    Check(Map(StoreAnswer(true, true, -11644473600), kNow).daysLeft == -1, "trial with the 1601 null date: days unknown");
    Check(Map(StoreAnswer(true, true, 253402214400), kNow).daysLeft == -1, "trial expiring in 9999: days unknown");

    Check(Map(StoreAnswer(false, true, kNow - kDay), kNow).kind == Kind::Inactive, "expired trial: inactive");
    Check(Map(StoreAnswer(false, false, 0), kNow).kind == Kind::Inactive, "license not active: inactive");
    Check(Map(StoreAnswer(false, true, kNow + 3 * kDay), kNow).kind == Kind::Inactive,
          "not active wins over a future expiry");

    // Fail open.
    Answer failed;  // the call threw or timed out
    Check(Map(failed, kNow).kind == Kind::Full, "no answer: full");
    Answer local = StoreAnswer(false, false, 0);
    local.fromStore = false;  // empty SkuStoreId: a package registered locally
    Check(Map(local, kNow).kind == Kind::NotStore, "license not issued by the Store: no licensing");
    Answer localTrial = StoreAnswer(true, true, kNow + kDay);
    localTrial.fromStore = false;
    Check(Map(localTrial, kNow).kind == Kind::NotStore, "trial flags without a Store SKU: no licensing");

    // Measured on a development PC: what GetAppLicenseAsync returns when the license
    // is not a real one. Neither may show licensing UI or stop the service.
    Answer registered = StoreAnswer(false, true, 253402214400);  // 9999-12-31, SkuStoreId ''
    registered.fromStore = false;
    Check(Map(registered, kNow).kind == Kind::NotStore,
          "locally registered package (active=0 trial=1 expires 9999 sku empty): no licensing");
    Answer unpackaged = StoreAnswer(true, false, 0);  // SkuStoreId ''
    unpackaged.fromStore = false;
    Check(Map(unpackaged, kNow).kind == Kind::NotStore, "unpackaged process (active=1 trial=0 sku empty): no licensing");
    Answer failedButActive = StoreAnswer(false, false, 0);
    failedButActive.ok = false;  // fields left over from a call that failed are not trusted
    Check(Map(failedButActive, kNow).kind == Kind::Full, "a failed call never locks, whatever its fields say");

    Check(store_license::AllowsService(Kind::NotStore) && store_license::AllowsService(Kind::Full) &&
              store_license::AllowsService(Kind::Trial) && !store_license::AllowsService(Kind::Inactive),
          "only inactive stops the service");
}

void CheckSelfTestParser() {
    const auto five = ParseSelfTest(L"trial:5", kNow);
    Check(five && Map(*five, kNow).kind == Kind::Trial && Map(*five, kNow).daysLeft == 5, "trial:5 maps to 5 days left");
    Check(five && Map(*five, kNow + 1).daysLeft == 5, "trial:5 read a second later: still 5");
    const auto one = ParseSelfTest(L"trial:1", kNow);
    Check(one && Map(*one, kNow).daysLeft == 1, "trial:1");
    Check(ParseSelfTest(L"trial:365", kNow).has_value(), "trial:365 accepted");
    const auto expired = ParseSelfTest(L"expired", kNow);
    Check(expired && Map(*expired, kNow).kind == Kind::Inactive, "expired maps to inactive");
    const auto licensed = ParseSelfTest(L"licensed", kNow);
    Check(licensed && Map(*licensed, kNow).kind == Kind::Full, "licensed maps to full");
    for (const wchar_t* bad : {L"", L"trial", L"trial:", L"trial:0", L"trial:366", L"trial:1000", L"trial:-1",
                               L"trial:5x", L"trial: 5", L"TRIAL:5", L"Expired", L"license-expired", L"full"}) {
        std::string what = "rejects '";
        for (const wchar_t* c = bad; *c; ++c) what.push_back(static_cast<char>(*c));
        what += "'";
        Check(!ParseSelfTest(bad, kNow).has_value(), what.c_str());
    }
}

// The run's memory of Store answers: failed reads, a purchase, the clock moving on.
void CheckTracker() {
    using store_license::Tracker;
    using store_license::Unlocked;
    Answer failed;  // a read that threw or timed out

    Tracker none;
    none.Record(failed);
    Check(none.Current(kNow).kind == Kind::Full, "failed read with no earlier answer: full");

    Tracker trial;
    trial.Record(StoreAnswer(true, true, kNow + 5 * kDay));
    trial.Record(failed);
    const auto keptTrial = trial.Current(kNow);
    Check(keptTrial.kind == Kind::Trial && keptTrial.daysLeft == 5, "failed read after a trial: still the trial, 5 days");
    Check(!Unlocked(Kind::Trial, failed), "failed read after a trial: no thank-you");

    Tracker inactive;
    inactive.Record(StoreAnswer(false, true, kNow - kDay));
    inactive.Record(failed);
    Check(inactive.Current(kNow).kind == Kind::Inactive, "failed read after inactive: stays locked");

    Tracker full;
    full.Record(StoreAnswer(true, false, 0));
    full.Record(failed);
    Check(full.Current(kNow).kind == Kind::Full, "failed read after full: full");

    Tracker bought;
    bought.Record(StoreAnswer(false, true, kNow - kDay));
    bought.RecordPurchase(true);
    bought.Record(StoreAnswer(false, true, kNow - kDay));  // stale read right after the purchase
    Check(bought.Current(kNow).kind == Kind::Full, "purchase grace: a stale inactive read after Succeeded stays full");
    Check(bought.Current(kNow + 30 * kDay).kind == Kind::Full, "purchase grace lasts the run");
    Tracker cancelled;
    cancelled.Record(StoreAnswer(true, true, kNow + 2 * kDay));
    cancelled.RecordPurchase(false);
    Check(cancelled.Current(kNow).kind == Kind::Trial, "a purchase that was not completed changes nothing");

    Check(Unlocked(Kind::Trial, StoreAnswer(true, false, 0)), "real full answer after a trial: thank-you");
    Check(Unlocked(Kind::Inactive, StoreAnswer(true, false, 0)), "real full answer after locked: thank-you");
    Check(!Unlocked(Kind::Full, StoreAnswer(true, false, 0)), "full after full: no thank-you");
    Check(!Unlocked(Kind::Trial, StoreAnswer(true, true, kNow + kDay)), "trial after trial: no thank-you");
    Answer localFull = StoreAnswer(true, false, 0);
    localFull.fromStore = false;
    Check(!Unlocked(Kind::Trial, localFull), "an answer without a Store SKU never thanks");

    Tracker counting;
    counting.Record(StoreAnswer(true, true, kNow + 3 * kDay));
    Check(counting.Current(kNow).daysLeft == 3, "day count at the read: 3");
    Check(counting.Current(kNow + kDay).daysLeft == 2, "a day later, no new read: 2");
    Check(counting.Current(kNow + 3 * kDay - 60).daysLeft == 1, "a minute before expiry: 1");
    Check(!counting.TakeExpiryCheck(kNow + 3 * kDay - 1), "before the expiry: no Store check");
    const auto ended = counting.Current(kNow + 3 * kDay + 60);
    Check(ended.kind == Kind::Trial && ended.daysLeft == 0, "expiry passed on the local clock only: trial, ends today, not locked");
    Check(counting.TakeExpiryCheck(kNow + 3 * kDay + 60), "expiry passed: ask the Store");
    Check(!counting.TakeExpiryCheck(kNow + 3 * kDay + 120), "asked a minute ago: wait");
    counting.Record(failed);
    Check(counting.Current(kNow + 3 * kDay + 300).kind == Kind::Trial, "Store check failed: still a trial");
    Check(counting.TakeExpiryCheck(kNow + 3 * kDay + 60 + 600), "Store check failed: ask again after 10 minutes");
    counting.Record(StoreAnswer(false, true, kNow + 3 * kDay));
    Check(counting.Current(kNow + 3 * kDay + 700).kind == Kind::Inactive, "Store confirms the trial ended: locked");
    Check(!counting.TakeExpiryCheck(kNow + 4 * kDay), "after the Store's answer: no more checks");
}

}  // namespace

int main() {
    CheckMapping();
    CheckSelfTestParser();
    CheckTracker();
    if (g_failures) {
        std::printf("LicenseCheck: %d of %d checks failed\n", g_failures, g_checks);
        return 1;
    }
    std::printf("LicenseCheck: all %d checks passed\n", g_checks);
    return 0;
}
