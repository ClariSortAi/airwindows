// The Microsoft Store license of a packaged PhonePort: read off the UI thread,
// refreshed when the Store reports a change, and the Store's own purchase dialog.
// Unpackaged runs never create a StoreContext.
#pragma once

#include <windows.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "license_state.h"

namespace store_license {

// True when the process has package identity (installed from an MSIX package).
bool HasPackageIdentity();

// True when the process runs elevated. The Store does not support in-app purchase
// from an elevated process.
bool IsElevated();

enum class Purchase { Succeeded, AlreadyPurchased, NotPurchased, NetworkError, ServerError, NotListed, Failed };

const char* PurchaseName(Purchase outcome);

// StoreRateAndReviewStatus, plus Failed when the call threw or did not complete.
enum class Rating { Succeeded, Canceled, NetworkError, Failed };

const char* RatingName(Rating outcome);

// What Rate did.
enum class RateStart { Started, AlreadyOpen, Unavailable };

struct Event {
    enum class Type { License, Purchase, Rating } type = Type::License;
    Answer answer;                          // License
    Purchase outcome = Purchase::Failed;    // Purchase
    Rating rating = Rating::Failed;         // Rating
};

class StoreLicense {
public:
    StoreLicense();
    ~StoreLicense();
    StoreLicense(const StoreLicense&) = delete;
    StoreLicense& operator=(const StoreLicense&) = delete;

    // Reads the license on a thread of its own and again whenever the Store says it
    // changed. Each answer is queued and `message` is posted to `window`; call Take()
    // on the window's thread. With `simulated` (a self-test), no Store call is made:
    // that answer is queued instead, and Buy simulates a successful purchase.
    void Start(HWND window, UINT message, const std::optional<Answer>& simulated);
    // Opens the Store's purchase dialog for this app, owned by the window. Call on the
    // window's thread; it does not block. The outcome arrives as a Purchase event; the
    // license is read again after it (at 2, 10 and 30 s after a successful purchase).
    // False when a purchase is already in progress.
    bool Buy();
    // Opens the Store's rate-and-review dialog for this app, owned by the window
    // (StoreContext.RequestRateAndReviewAppAsync). Call on the window's thread; it does not
    // block. The outcome arrives as a Rating event. Unavailable when no Store context could
    // be made (or this run never asks the Store): the caller opens the Store's review page.
    // Never called in an elevated process: the Store does not support it there.
    RateStart Rate();
    // Queues one more license read (for a trial past its local expiry).
    void Refresh();
    // Self-test only (a simulated Start): queue the answer the Store gives when a trial
    // runs out while PhonePort is open. False, and nothing queued, for a real Store.
    bool SimulateExpiry();
    // Call on the window's thread when `message` arrives. It also opens the purchase
    // dialog once the product lookup started by Buy has finished.
    std::vector<Event> Take();
    // Call on the window's thread. Cancels a purchase or rating dialog that is still open.
    void Stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace store_license
