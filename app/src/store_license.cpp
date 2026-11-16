// The Microsoft Store license, through Windows.Services.Store. Documentation this
// follows (Microsoft Learn, read 2026-11-13):
//   https://learn.microsoft.com/windows/uwp/monetize/implement-a-trial-version-of-your-app
//     StoreContext.GetDefault, GetAppLicenseAsync, OfflineLicensesChanged. A time-limited
//     trial that expired before launch does not launch: Windows shows its own dialog
//     offering the purchase. A trial that expires while the app runs is the app's to handle.
//   https://learn.microsoft.com/windows/uwp/monetize/in-app-purchases-and-trials
//     A Win32 app must call IInitializeWithWindow::Initialize on the StoreContext (header
//     shobjidl.h) or the context "will return inaccurate data or errors". An app the Store
//     does not know gets ExtendedError 0x803F6107. There is no license simulator for this
//     namespace: a real license needs the app published and installed from the Store once.
//     Not supported in elevated processes.
//   https://learn.microsoft.com/uwp/api/windows.services.store.storeproduct.requestpurchaseasync
//     "Important: This method must be called on the UI thread." An ErrorCode of 0x80070578
//     (ERROR_INVALID_WINDOW_HANDLE) means it was not, or that a desktop app did not set the
//     owner window with IInitializeWithWindow.
//   https://learn.microsoft.com/uwp/api/windows.services.store.storeapplicense
//     IsActive, IsTrial, ExpirationDate, SkuStoreId. TrialTimeRemaining applies only to
//     usage-limited trials, so it is not used.
//   https://learn.microsoft.com/uwp/api/windows.services.store.storeapplicense.expirationdate
//     Relative to the system clock; 31 December 9999 when there is no time limit.
//   https://learn.microsoft.com/uwp/api/windows.services.store.storecontext.getstoreproductforcurrentappasync
//   https://learn.microsoft.com/uwp/api/windows.services.store.storecontext.requestrateandreviewappasync
//     (read 2026-11-16) Windows 10, version 1809 (10.0.17763.0), StoreContract v4. "This
//     method must be called on the UI thread." "In-app rate and review functionality is not
//     supported in elevated applications." 0x80070578 has the same meaning as for purchases:
//     the wrong thread, or a desktop app that did not set the owner window.
//   https://learn.microsoft.com/uwp/api/windows.services.store.storerateandreviewstatus
//     Succeeded 0, CanceledByUser 1, NetworkError 2, Error 3.
//
// License reads run on this file's own thread (multi-threaded apartment) and are posted
// to the window as events. The purchase runs on the window's thread, as Learn requires,
// and never blocks it: each step finishes in a completion handler that queues an event
// and posts to the window. Any failure is logged and reported as "no answer"; what that
// means is decided by store_license::Tracker (license_state.cpp).

#include "store_license.h"

#include <unknwn.h>
#include <appmodel.h>
#include <shobjidl_core.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Services.Store.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <thread>

#include "log.h"

namespace store_license {
namespace {

using winrt::Windows::Foundation::AsyncStatus;
using winrt::Windows::Foundation::IAsyncOperation;
using winrt::Windows::Services::Store::StoreAppLicense;
using winrt::Windows::Services::Store::StoreContext;
using winrt::Windows::Services::Store::StoreProduct;
using winrt::Windows::Services::Store::StoreProductResult;
using winrt::Windows::Services::Store::StorePurchaseResult;
using winrt::Windows::Services::Store::StorePurchaseStatus;
using winrt::Windows::Services::Store::StoreRateAndReviewResult;
using winrt::Windows::Services::Store::StoreRateAndReviewStatus;
using Clock = std::chrono::steady_clock;

constexpr int32_t kNotKnownToStore = static_cast<int32_t>(0x803F6107);
constexpr std::chrono::seconds kLicenseTimeout{15};
// After a purchase, the Store's own license can lag behind its purchase answer.
constexpr int kRereadAfterPurchase[] = {2, 10, 30};

enum class Task { Refresh, Quit };

unsigned long Code(const winrt::hresult_error& error) {
    return static_cast<unsigned long>(static_cast<int32_t>(error.code()));
}

// State shared with the reader thread and with completion handlers, which can run on
// any thread and after Stop: they hold this, never the StoreLicense.
struct Shared {
    HWND window = nullptr;
    UINT message = 0;
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<Task> tasks;
    std::vector<Clock::time_point> later;  // scheduled re-reads
    std::vector<Event> events;
    winrt::agile_ref<StoreProduct> product;  // looked up for a purchase; started on the window's thread
    bool productReady = false;
    bool closed = false;  // Stop ran: nothing is queued or posted any more
    std::atomic<bool> quitting{false};

    void Notify() {
        if (!PostMessageW(window, message, 0, 0)) {
            LOGW("[STORE] could not notify the window (error %lu)", GetLastError());
        }
    }
    void Push(const Event& event) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (closed) return;
            events.push_back(event);
        }
        Notify();
    }
    void PushPurchase(Purchase outcome) {
        Event event;
        event.type = Event::Type::Purchase;
        event.outcome = outcome;
        Push(event);
    }
    void PushRating(Rating outcome) {
        Event event;
        event.type = Event::Type::Rating;
        event.rating = outcome;
        Push(event);
    }
    void Enqueue(Task task) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (closed && task != Task::Quit) return;
            tasks.push_back(task);
        }
        wake.notify_one();
    }
    void Schedule(std::chrono::seconds delay) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (closed) return;
            later.push_back(Clock::now() + delay);
        }
        wake.notify_one();
    }
};

}  // namespace

bool HasPackageIdentity() {
    UINT32 length = 0;
    return GetCurrentPackageFullName(&length, nullptr) != APPMODEL_ERROR_NO_PACKAGE;
}

bool IsElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        LOGW("[STORE] could not open the process token (error %lu); assuming not elevated", GetLastError());
        return false;
    }
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const BOOL ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
    const DWORD error = ok ? 0 : GetLastError();
    CloseHandle(token);
    if (!ok) {
        LOGW("[STORE] could not read the token elevation (error %lu); assuming not elevated", error);
        return false;
    }
    return elevation.TokenIsElevated != 0;
}

const char* PurchaseName(Purchase outcome) {
    switch (outcome) {
    case Purchase::Succeeded: return "succeeded";
    case Purchase::AlreadyPurchased: return "already-purchased";
    case Purchase::NotPurchased: return "not-purchased";
    case Purchase::NetworkError: return "network-error";
    case Purchase::ServerError: return "server-error";
    case Purchase::NotListed: return "not-listed";
    case Purchase::Failed: return "failed";
    }
    return "unknown";
}

const char* RatingName(Rating outcome) {
    switch (outcome) {
    case Rating::Succeeded: return "succeeded";
    case Rating::Canceled: return "canceled";
    case Rating::NetworkError: return "network-error";
    case Rating::Failed: return "failed";
    }
    return "unknown";
}

struct StoreLicense::Impl {
    std::shared_ptr<Shared> shared = std::make_shared<Shared>();
    bool simulated = false;
    std::thread worker;
    // Window thread only.
    bool buying = false;
    bool rating = false;
    StoreContext purchaseContext{nullptr};  // also opens the rating dialog
    IAsyncOperation<StoreProductResult> lookup{nullptr};
    IAsyncOperation<StorePurchaseResult> request{nullptr};
    IAsyncOperation<StoreRateAndReviewResult> rateRequest{nullptr};

    // A context of the window's thread, owned by the window, for the dialogs. Throws.
    void EnsureDialogContext() {
        if (!purchaseContext) {
            StoreContext context = StoreContext::GetDefault();
            winrt::check_hresult(context.as<::IInitializeWithWindow>()->Initialize(shared->window));
            purchaseContext = context;
        }
    }

    void PushLicense(const Answer& answer) {
        Event event;
        event.type = Event::Type::License;
        event.answer = answer;
        shared->Push(event);
    }

    // Blocks the reader thread until the read finishes; closing PhonePort cancels it.
    template <typename Operation>
    bool Wait(const Operation& operation, std::chrono::milliseconds limit) {
        const auto deadline = Clock::now() + limit;
        while (operation.Status() == AsyncStatus::Started) {
            if (shared->quitting || Clock::now() > deadline) {
                operation.Cancel();
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return true;
    }

    Answer ReadLicense(const StoreContext& context) {
        Answer answer;
        if (!context) {
            return answer;  // the reason was logged when the context failed
        }
        try {
            const auto operation = context.GetAppLicenseAsync();
            if (!Wait(operation, kLicenseTimeout)) {
                LOGW("[STORE] GetAppLicenseAsync %s; keeping the last answer", shared->quitting ? "cancelled" : "timed out");
                return answer;
            }
            const StoreAppLicense license = operation.GetResults();
            if (!license) {
                LOGW("[STORE] GetAppLicenseAsync returned no license; keeping the last answer");
                return answer;
            }
            answer.ok = true;
            answer.isActive = license.IsActive();
            answer.isTrial = license.IsTrial();
            const winrt::hstring sku = license.SkuStoreId();
            answer.fromStore = !sku.empty();
            answer.expires = static_cast<int64_t>(winrt::clock::to_time_t(license.ExpirationDate()));
            LOGI("[STORE] license: active=%d trial=%d sku=%ls expires=%lld", answer.isActive ? 1 : 0,
                 answer.isTrial ? 1 : 0, sku.empty() ? L"(none)" : sku.c_str(), static_cast<long long>(answer.expires));
        } catch (const winrt::hresult_error& error) {
            LOGW("[STORE] GetAppLicenseAsync failed (0x%08lX: %ls); keeping the last answer", Code(error),
                 error.message().c_str());
            answer = Answer{};
        } catch (const std::exception& error) {
            LOGW("[STORE] GetAppLicenseAsync failed (%s); keeping the last answer", error.what());
            answer = Answer{};
        }
        return answer;
    }

    void Loop() {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        StoreContext context{nullptr};
        winrt::event_token changed{};
        try {
            context = StoreContext::GetDefault();
            // A desktop app names the window that owns the Store's dialogs before any call.
            winrt::check_hresult(context.as<::IInitializeWithWindow>()->Initialize(shared->window));
            const std::shared_ptr<Shared> state = shared;
            changed = context.OfflineLicensesChanged([state](const StoreContext&, const winrt::Windows::Foundation::IInspectable&) {
                LOGI("[STORE] the Store reports a license change");
                state->Enqueue(Task::Refresh);
            });
        } catch (const winrt::hresult_error& error) {
            LOGW("[STORE] no StoreContext (0x%08lX: %ls); no license reads in this run", Code(error), error.message().c_str());
            context = nullptr;
        }
        for (;;) {
            Task task;
            {
                std::unique_lock<std::mutex> lock(shared->mutex);
                for (;;) {
                    if (!shared->tasks.empty()) break;
                    // A scheduled re-read that is due becomes a task.
                    const auto now = Clock::now();
                    const auto due = std::find_if(shared->later.begin(), shared->later.end(), [&](auto at) { return at <= now; });
                    if (due != shared->later.end()) {
                        shared->later.erase(due);
                        shared->tasks.push_back(Task::Refresh);
                        break;
                    }
                    if (shared->later.empty()) {
                        shared->wake.wait(lock);
                    } else {
                        shared->wake.wait_until(lock, *std::min_element(shared->later.begin(), shared->later.end()));
                    }
                }
                task = shared->tasks.front();
                shared->tasks.pop_front();
            }
            if (task == Task::Quit) {
                break;
            }
            PushLicense(ReadLicense(context));
        }
        if (context && changed.value != 0) {
            try {
                context.OfflineLicensesChanged(changed);
            } catch (const winrt::hresult_error& error) {
                LOGW("[STORE] could not unsubscribe from license changes (0x%08lX)", Code(error));
            }
        }
        context = nullptr;
        winrt::uninit_apartment();
    }

    // Window thread: the product is known, open the Store's purchase dialog.
    void StartPurchaseDialog(const StoreProduct& product) {
        const std::shared_ptr<Shared> state = shared;
        try {
            LOGI("[STORE] opening the purchase dialog for %ls", product.StoreId().c_str());
            request = product.RequestPurchaseAsync();
            request.Completed([state](const IAsyncOperation<StorePurchaseResult>& operation, AsyncStatus status) {
                Purchase outcome = Purchase::Failed;
                try {
                    if (status == AsyncStatus::Completed) {
                        const auto result = operation.GetResults();
                        const auto purchase = result.Status();
                        LOGI("[STORE] purchase status %d (extended 0x%08lX)", static_cast<int>(purchase),
                             static_cast<unsigned long>(result.ExtendedError()));
                        switch (purchase) {
                        case StorePurchaseStatus::Succeeded: outcome = Purchase::Succeeded; break;
                        case StorePurchaseStatus::AlreadyPurchased: outcome = Purchase::AlreadyPurchased; break;
                        case StorePurchaseStatus::NotPurchased: outcome = Purchase::NotPurchased; break;
                        case StorePurchaseStatus::NetworkError: outcome = Purchase::NetworkError; break;
                        case StorePurchaseStatus::ServerError: outcome = Purchase::ServerError; break;
                        }
                    } else {
                        LOGW("[STORE] purchase dialog ended with status %d", static_cast<int>(status));
                    }
                } catch (const winrt::hresult_error& error) {
                    LOGW("[STORE] purchase failed (0x%08lX: %ls)", Code(error), error.message().c_str());
                }
                state->PushPurchase(outcome);
                if (outcome == Purchase::Succeeded || outcome == Purchase::AlreadyPurchased) {
                    for (int seconds : kRereadAfterPurchase) state->Schedule(std::chrono::seconds(seconds));
                } else {
                    state->Enqueue(Task::Refresh);
                }
            });
        } catch (const winrt::hresult_error& error) {
            LOGW("[STORE] the purchase dialog could not open (0x%08lX: %ls)", Code(error), error.message().c_str());
            state->PushPurchase(Purchase::Failed);
        }
    }
};

StoreLicense::StoreLicense() : impl_(std::make_unique<Impl>()) {}

StoreLicense::~StoreLicense() {
    Stop();
}

void StoreLicense::Start(HWND window, UINT message, const std::optional<Answer>& simulated) {
    Impl& s = *impl_;
    if (s.worker.joinable() || s.simulated) {
        return;
    }
    s.shared->window = window;
    s.shared->message = message;
    if (simulated) {
        s.simulated = true;
        LOGI("[STORE] self-test: simulated license, no Store call");
        s.PushLicense(*simulated);
        return;
    }
    s.shared->tasks.push_back(Task::Refresh);
    s.worker = std::thread([&s] { s.Loop(); });
}

bool StoreLicense::Buy() {
    Impl& s = *impl_;
    if (s.simulated) {
        LOGI("[STORE] self-test: purchase simulated as succeeded");
        s.shared->PushPurchase(Purchase::Succeeded);
        Answer bought;
        bought.ok = bought.isActive = bought.fromStore = true;
        s.PushLicense(bought);
        return true;
    }
    if (s.buying || !s.worker.joinable()) {
        return false;
    }
    s.buying = true;
    const std::shared_ptr<Shared> state = s.shared;
    try {
        s.EnsureDialogContext();
        s.lookup = s.purchaseContext.GetStoreProductForCurrentAppAsync();
        s.lookup.Completed([state](const IAsyncOperation<StoreProductResult>& operation, AsyncStatus status) {
            Purchase failure = Purchase::Failed;
            try {
                if (status == AsyncStatus::Completed) {
                    const auto result = operation.GetResults();
                    const int32_t error = result.ExtendedError();
                    if (error == 0 && result.Product()) {
                        {
                            std::lock_guard<std::mutex> lock(state->mutex);
                            if (state->closed) return;
                            state->product = winrt::make_agile(result.Product());
                            state->productReady = true;
                        }
                        state->Notify();  // the window's thread opens the dialog (see Take)
                        return;
                    }
                    LOGW("[STORE] this app's Store listing could not be read (0x%08lX)", static_cast<unsigned long>(error));
                    failure = error == kNotKnownToStore ? Purchase::NotListed : Purchase::Failed;
                } else {
                    LOGW("[STORE] GetStoreProductForCurrentAppAsync ended with status %d", static_cast<int>(status));
                    failure = status == AsyncStatus::Canceled ? Purchase::Failed : Purchase::NetworkError;
                }
            } catch (const winrt::hresult_error& error) {
                LOGW("[STORE] this app's Store listing could not be read (0x%08lX: %ls)", Code(error), error.message().c_str());
            }
            state->PushPurchase(failure);
            state->Enqueue(Task::Refresh);
        });
    } catch (const winrt::hresult_error& error) {
        LOGW("[STORE] the purchase could not start (0x%08lX: %ls)", Code(error), error.message().c_str());
        state->PushPurchase(Purchase::Failed);
    }
    return true;
}

RateStart StoreLicense::Rate() {
    Impl& s = *impl_;
    if (s.simulated) {
        // main.cpp answers a self-test itself; this is the second guard: never the real dialog.
        LOGI("[STORE] self-test: rating simulated, no Store call");
        s.shared->PushRating(Rating::Canceled);
        return RateStart::Started;
    }
    if (s.rating) {
        return RateStart::AlreadyOpen;
    }
    if (!s.worker.joinable()) {
        LOGW("[STORE] this run does not ask the Store; no rating dialog");
        return RateStart::Unavailable;
    }
    const std::shared_ptr<Shared> state = s.shared;
    try {
        s.EnsureDialogContext();
        LOGI("[STORE] opening the rating dialog");
        s.rateRequest = s.purchaseContext.RequestRateAndReviewAppAsync();
        s.rating = true;
        s.rateRequest.Completed([state](const IAsyncOperation<StoreRateAndReviewResult>& operation, AsyncStatus status) {
            Rating outcome = Rating::Failed;
            try {
                if (status == AsyncStatus::Completed) {
                    const auto result = operation.GetResults();
                    const auto answer = result.Status();
                    LOGI("[STORE] rating status %d (extended 0x%08lX, updated %d): %ls", static_cast<int>(answer),
                         static_cast<unsigned long>(result.ExtendedError()), result.WasUpdated() ? 1 : 0,
                         result.ExtendedJsonData().c_str());
                    switch (answer) {
                    case StoreRateAndReviewStatus::Succeeded: outcome = Rating::Succeeded; break;
                    case StoreRateAndReviewStatus::CanceledByUser: outcome = Rating::Canceled; break;
                    case StoreRateAndReviewStatus::NetworkError: outcome = Rating::NetworkError; break;
                    case StoreRateAndReviewStatus::Error: outcome = Rating::Failed; break;
                    }
                } else {
                    LOGW("[STORE] rating dialog ended with status %d", static_cast<int>(status));
                }
            } catch (const winrt::hresult_error& error) {
                LOGW("[STORE] rating failed (0x%08lX: %ls)", Code(error), error.message().c_str());
            } catch (const std::exception& error) {
                LOGW("[STORE] rating failed (%s)", error.what());
            }
            state->PushRating(outcome);
        });
    } catch (const winrt::hresult_error& error) {
        LOGW("[STORE] the rating dialog could not open (0x%08lX: %ls)", Code(error), error.message().c_str());
        s.rating = false;
        s.rateRequest = nullptr;
        return RateStart::Unavailable;
    }
    return RateStart::Started;
}

bool StoreLicense::SimulateExpiry() {
    Impl& s = *impl_;
    if (!s.simulated) {
        return false;
    }
    LOGI("[STORE] self-test: the trial runs out now");
    Answer ended;
    ended.ok = ended.isTrial = ended.fromStore = true;
    ended.isActive = false;
    s.PushLicense(ended);
    return true;
}

void StoreLicense::Refresh() {
    Impl& s = *impl_;
    if (s.simulated) {
        LOGI("[STORE] self-test: a Store read was asked for; the simulated answer stands");
        return;
    }
    if (s.worker.joinable()) {
        s.shared->Enqueue(Task::Refresh);
    }
}

std::vector<Event> StoreLicense::Take() {
    Impl& s = *impl_;
    std::vector<Event> taken;
    StoreProduct product{nullptr};
    {
        std::lock_guard<std::mutex> lock(s.shared->mutex);
        taken.swap(s.shared->events);
        if (s.shared->productReady) {
            s.shared->productReady = false;
            product = s.shared->product.get();
            s.shared->product = nullptr;
        }
    }
    if (product) {
        s.StartPurchaseDialog(product);  // this is the window's thread
    }
    for (const Event& event : taken) {
        if (event.type == Event::Type::Purchase) {
            s.buying = false;
        } else if (event.type == Event::Type::Rating) {
            s.rating = false;
        }
    }
    return taken;
}

void StoreLicense::Stop() {
    Impl& s = *impl_;
    {
        std::lock_guard<std::mutex> lock(s.shared->mutex);
        s.shared->closed = true;
    }
    s.shared->quitting = true;
    // A purchase or rating dialog still open is cancelled, not waited for: its handler finds
    // the shared state closed and posts nothing.
    const auto cancel = [](const auto& operation, const char* what) {
        if (!operation || operation.Status() != AsyncStatus::Started) return;
        try {
            operation.Cancel();
            LOGI("[STORE] %s still open at close: cancelled", what);
        } catch (const winrt::hresult_error& error) {
            LOGW("[STORE] could not cancel the %s on close (0x%08lX)", what, Code(error));
        }
    };
    cancel(s.lookup, "purchase");
    cancel(s.request, "purchase");
    cancel(s.rateRequest, "rating");
    s.lookup = nullptr;
    s.request = nullptr;
    s.rateRequest = nullptr;
    s.purchaseContext = nullptr;
    if (!s.worker.joinable()) {
        return;
    }
    s.shared->Enqueue(Task::Quit);
    s.worker.join();
}

}  // namespace store_license
