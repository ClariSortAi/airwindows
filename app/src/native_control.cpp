#include "native_control.h"
#include "native_hid_backend.h"
#include "phone_pairs.h"
#include "subscriber_selection.h"
#include "log.h"
#include <windows.h>
#include <winrt/base.h>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <atomic>
#include <regex>

struct NativeControl::Impl {
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::thread worker;
    std::shared_ptr<NativeHidBackend> backend;
    ControlQueue queue;
    NativeControlState state;
    bool quitting=false, pair=false, disconnect=false, stop=false;
    bool focused=false, mirror=false, paused=false, picture=false;
    bool live=false; // the chosen phone was on screen, unpaused, at the last inspection
    int width=0,height=0;
    ControlFrameGate frameGate;
    std::string bind;
    PhoneLink link;      // which phone the mirror on screen belongs to
    bool reopen=false;   // a remembered phone started mirroring: publish so its Bluetooth can connect
    bool refresh=false;  // the sender changed: inspect now, not at the next quarter second
    bool selectRefused=false; // worker only: the last attempt to select the saved phone failed; logged once
    bool held=false;
    int pointerX=0,pointerY=0;
    uint64_t pressedAt=0, holdExpires=0, holdLimit=0;
    uint64_t inspectAt=0;
    std::string loggedStatus; // last backend status written to the log while pairing
    std::atomic<bool> processing{false};

    void RefreshFocus() { // mutex held
        const bool usable = focused && mirror && !paused && picture && state.ready;
        // Taking control again clears an earlier complaint ("Paste rejected...") from the status line.
        if (usable && !queue.Armed()) state.message="Click the mirrored phone to control it; click elsewhere to return";
        if (usable != queue.Armed()) {
            queue.Focus(usable);
            if (!usable) stop=true;
        }
        state.focused = usable;
    }
    void Cancel() {
        auto current = std::atomic_load(&backend);
        if (current) current->CancelInput();
        wake.notify_one();
    }
    void DropInput() { // mutex held
        queue.Focus(false); state.focused=false; stop=true;
    }
    void Close() { // worker only
        auto current = std::atomic_exchange(&backend,std::shared_ptr<NativeHidBackend>{});
        if (current) current->Shutdown(); // releases precede withdrawal
        held=false;
        std::lock_guard<std::mutex> lock(mutex);
        DropInput(); state.ready=false; state.published=false; state.waiting=false; state.bound.clear(); state.peers.clear();
    }
    void FailedAction(const std::optional<ControlAction>& action,const std::string& message) {
        {std::lock_guard<std::mutex> lock(mutex);
          if(!action || queue.CancelIfCurrent(*action)){
              if(!action)queue.Focus(false);
              state.focused=false;stop=true;focused=false;state.message=message;
          } else LOGI("[CONTROL] Older action canceled; preserving newer focused input");}
        // A newer epoch may be queued, but its action has not run yet. Release
        // only the old held state before the worker starts that new action.
        auto current=std::atomic_load(&backend);if(current)current->Stop();held=false;
    }
    void Loop() {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        for (;;) {
            bool doPair,doDisconnect,doStop,quit,doReopen,doRefresh;
            std::string doBind;
            std::optional<ControlAction> action;
            {
                std::unique_lock<std::mutex> lock(mutex);
                if(queue.Size()==0)wake.wait_for(lock,std::chrono::milliseconds(30));
                quit=quitting; doPair=pair; pair=false; doDisconnect=disconnect; disconnect=false;
                doReopen=reopen; reopen=false; doRefresh=refresh; refresh=false;
                doStop=stop; stop=false; doBind.swap(bind);
                action=queue.Pop(GetTickCount64());
                processing=action.has_value();
                if(queue.LastFailure()==ControlQueue::Failure::Expired){
                    DropInput();focused=false;state.message="Input stopped: queued action expired. Remaining input was discarded.";doStop=true;
                }
            }
            struct ActionDone { std::atomic<bool>& flag; ~ActionDone(){flag=false;} } actionDone{processing};
            try {
                if (quit) { Close(); break; }
                if (doDisconnect) { Close(); std::lock_guard<std::mutex> lock(mutex); state.message="Bluetooth control disconnected"; continue; }
                auto current=std::atomic_load(&backend);
                if (current && (doStop || (held && (!queue.Armed() || GetTickCount64()>=holdExpires || GetTickCount64()>=holdLimit)))) {
                    current->Stop(); held=false;
                }
                // doReopen: a remembered phone began to mirror. Its Bluetooth can only reach a
                // published service, and the window from the last Pair click may have closed.
                if (doPair || doReopen) {
                    if (!current) {
                        current=std::make_shared<NativeHidBackend>(); current->Initialize(true);
                        current->SetTrace([](const std::string& line){LOGI("[BLE] %s",line.c_str());});
                        std::atomic_store(&backend,current);
                    }
                    const bool open=current->Inspect().published;
                    current->OpenDiscovery(120);
                    if (!open) LOGI("[CONTROL] Native Bluetooth publication opened for 120 seconds");
                    else LOGI("[CONTROL] Native Bluetooth publication already open; extended to 120 seconds from now");
                    if (doPair) {
                        std::lock_guard<std::mutex> lock(mutex);
                        state.message="Pairing is open for two minutes: on the iPhone, open Settings > Bluetooth and tap this PC";
                    }
                }
                if (!current) continue;
                if (!doBind.empty()) {
                    current->Bind(doBind); current->KeepPublishedWhileBound(true);
                    std::lock_guard<std::mutex> lock(mutex);
                    link.OnChoose(doBind);
                    LOGI("[CONTROL] Selected phone %s; persistent publication; %s",doBind.c_str(),
                         link.wantedSender.empty()?"its mirror is not known yet":("remembered for mirror "+link.wantedSender).c_str());
                    state.pairs=link.pairs.Serialize();
                    state.bound=doBind; state.message="Click the mirrored phone to control it; click elsewhere to return";
                }
                current->Poll();
                // While control is not ready, log every change in the backend's Bluetooth state
                // (subscriptions, reads/writes, publication, diagnostics) so pairing is traceable.
                bool ready; {std::lock_guard<std::mutex> lock(mutex); ready=state.ready;}
                if (!ready) {
                    static const std::regex volatileFields(R"re("(discovery_remaining_ms|battery_level|battery_present)":[^,]*,)re");
                    auto status = std::regex_replace(current->Status(), volatileFields, "");
                    if (status != loggedStatus) { loggedStatus = status; LOGI("[CONTROL] backend %s", status.c_str()); }
                }
                if (current->PublicationStopFailed()) {
                    // Throwing here repeated every 30 ms and never ended the backend.
                    LOGW("[CONTROL] Bluetooth publication could not stop; disconnecting");
                    Close(); std::lock_guard<std::mutex> lock(mutex);
                    state.message="Bluetooth publication could not stop; disconnected"; continue;
                }
                if (GetTickCount64() >= inspectAt || !doBind.empty() || doPair || doReopen || doRefresh) {
                    inspectAt=GetTickCount64()+250;
                    auto view=current->Inspect();
                    std::string remembered;
                    bool release;
                    {std::lock_guard<std::mutex> lock(mutex);remembered=link.wanted;release=link.MustRelease(view.bound);}
                    if(release) {
                        // The mirror now comes from another iPhone. Let go of this one without closing
                        // Bluetooth: it stays connected, so switching back needs no pairing.
                        const std::string left=view.bound;
                        current->Unbind(); held=false; view=current->Inspect();
                        LOGI("[CONTROL] Released phone %s; the mirror comes from another iPhone",left.c_str());
                        std::lock_guard<std::mutex> lock(mutex);
                        state.bound.clear();state.ready=false;
                    }
                    const auto savedPeer=UniqueSubscriber(view.peers,[&](const auto& peer){return peer.absolute && peer.fingerprint==remembered;});
                    if(view.bound.empty() && !remembered.empty() && savedPeer!=view.peers.end()) {
                        try {
                            current->Bind(remembered); current->KeepPublishedWhileBound(true); view=current->Inspect();
                            LOGI("[CONTROL] Reconnected saved phone %s",remembered.c_str());
                            selectRefused=false;
                            std::lock_guard<std::mutex> lock(mutex);
                            state.bound=remembered;state.message="Click the mirrored phone to control it; click elsewhere to return";
                        } catch (const winrt::hresult_error& error) {
                            // Windows can refuse for a moment while it restores the phone's link (seen as
                            // 0x80070057 a third of a second after a drop). The next inspection tries again.
                            if(!selectRefused)LOGW("[CONTROL] Saved phone %s cannot be selected yet (hr=0x%08x); trying again",
                                remembered.c_str(),static_cast<uint32_t>(error.code()));
                            selectRefused=true;
                        }
                    }
                    bool lost=false;
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        state.peers.clear();
                        bool candidate=false; // a listed phone that could be the one on screen
                        for (auto& peer:view.peers) if (peer.absolute) {
                            const bool elsewhere=link.Elsewhere(peer.fingerprint);
                            candidate=candidate || !elsewhere;
                            state.peers.push_back({peer.fingerprint,peer.name,elsewhere});
                        }
                        state.published=view.published;
                        const auto selected=UniqueSubscriber(state.peers,[&](const auto& peer){return peer.token==state.bound;});
                        const bool subscribed=!state.bound.empty() && view.bound==state.bound && selected!=state.peers.end();
                        const bool ready=subscribed && link.Matches(state.bound);
                        if(ready!=state.ready)LOGI("[CONTROL] Ready=%d; matching Bluetooth and AirPlay identities",ready?1:0);
                        state.ready=ready;
                        state.sender=state.ready?link.wantedSender:"";
                        lost=!state.bound.empty()&&!subscribed;
                        state.waiting=!lost && state.bound.empty() && !link.wanted.empty() && view.published;
                        // "Live": the chosen phone is the one on screen right now. The status line follows
                        // that rather than the identity match alone, which outlives a mirror that stopped.
                        const bool nowLive=state.ready && mirror && picture && !paused;
                        const bool becameLive=nowLive && !live;
                        live=nowLive;
                        if (lost) state.message="Phone disconnected. Waiting for the saved phone.";
                        else if(becameLive)state.message="Click the mirrored phone to control it; click elsewhere to return";
                        else if(subscribed && state.ready && mirror && paused)state.message="Mirroring is paused. Unlock the iPhone to continue.";
                        else if(subscribed && !nowLive)state.message="Bluetooth connected; mirror the selected iPhone";
                        // The phone on screen is a remembered one whose Bluetooth has not arrived. Do not
                        // ask for a choice: the phones listed meanwhile are other phones.
                        else if(state.waiting)state.message=std::string("Waiting for ")+(link.session?"this":"the saved")+
                            " iPhone's Bluetooth. If it does not connect, open Settings > Bluetooth on the iPhone and tap this PC.";
                        // A phone has paired and nothing is chosen yet: say what the next click is.
                        else if(state.bound.empty() && !state.peers.empty())state.message=
                            !link.session ? "iPhone paired. Click the phone button and choose it to connect."
                            : candidate ? "Click the phone button and choose the iPhone you are mirroring."
                            : "This iPhone has no Bluetooth link yet. Click the phone button, then Pair Bluetooth.";
                        else if (!view.published) state.message="Pairing closed after two minutes. Click the phone button to open it again.";
                        RefreshFocus();
                    }
                    if (lost) {
                        // The selected phone's Bluetooth dropped. The service stays up: the phone comes
                        // back to it by itself, and the other phones never notice. Closing and reopening
                        // the service here made every phone reconnect, and on 2026-11-11 that reconnection
                        // dropped the link again, five times in a row.
                        LOGI("[CONTROL] The selected phone's Bluetooth dropped; keeping the service open for its return");
                        // Pending input is dropped, but the phone window keeps the keyboard: when the same
                        // phone is selected again a moment later, control resumes without another click.
                        {std::lock_guard<std::mutex> lock(mutex); DropInput(); state.bound.clear(); state.ready=false;}
                        current->Unbind(); held=false;
                        current->OpenDiscovery(120); // while no phone is connected, wait two minutes, as after Pair
                        inspectAt=0;
                        continue;
                    }
                }
                if (action && queue.Valid(*action,GetTickCount64())) {
                    const auto command=*action;
                    current->SetActionGuard([this,command]{return queue.Valid(command,GetTickCount64());});
                    if(held && (command.kind==ControlKind::Text || command.kind==ControlKind::Key || command.kind==ControlKind::Home || command.kind==ControlKind::Secondary || command.kind==ControlKind::ScrollUp || command.kind==ControlKind::ScrollDown)) {current->Stop();held=false;}
                    current->EnableOneAction();
                    switch (command.kind) {
                    case ControlKind::Point:
                        pointerX=command.x;pointerY=command.y;
                        current->Pointer(pointerX,pointerY,held,false);
                        if(held)holdExpires=GetTickCount64()+2000;
                        break;
                    case ControlKind::Click:
                        current->TapAbsolute(command.x,command.y); held=false; break;
                    case ControlKind::Down:
                        LOGI("[CONTROL] Pointer down at %d,%d",command.x,command.y);
                        pointerX=command.x;pointerY=command.y;
                        current->Pointer(pointerX,pointerY,true,true); held=true;
                        pressedAt=GetTickCount64();holdExpires=pressedAt+2000;holdLimit=pressedAt+10000; break;
                    case ControlKind::Up:
                        {const auto now=GetTickCount64();if(held && now<pressedAt+80)std::this_thread::sleep_for(std::chrono::milliseconds(pressedAt+80-now));}
                        if(command.x>=0 && command.y>=0){pointerX=command.x;pointerY=command.y;}
                        current->Pointer(pointerX,pointerY,false,false);held=false;LOGI("[CONTROL] Pointer released"); break;
                    case ControlKind::ScrollUp: case ControlKind::ScrollDown: {
                        // Keep the whole swipe off the top and bottom edges, which iOS reserves for
                        // Notification Center and Home; an unclamped start could also reverse direction.
                        const int start=std::clamp(command.y,8000,24767);
                        current->Swipe(command.x,start,command.x,start+(command.kind==ControlKind::ScrollUp?7000:-7000)); break;
                    }
                    case ControlKind::Text: current->Type(command.text,false); break;
                    case ControlKind::Key: current->PressKey(command.key); break;
                    case ControlKind::Home: current->HomeButton();LOGI("[CONTROL] Button2 pulse submitted; Home requires AssistiveTouch mapping");break;
                    case ControlKind::Secondary: current->HomeButton(command.x,command.y);LOGI("[CONTROL] Right-click button2 pulse submitted");break;
                    }
                }
            } catch (const winrt::hresult_error& error) {
                const auto code=static_cast<uint32_t>(error.code());
                LOGW("[CONTROL] Bluetooth failure hr=0x%08x; input stopped",code);
                // Opening the service fails like this when the radio is off; say so instead of blaming an action.
                FailedAction(action,(doPair || doReopen) && !action
                    ? "Bluetooth could not start. Check that Bluetooth is on in Windows, then click Pair Bluetooth."
                    : "Bluetooth action failed. Click the mirror to retry; actions are never replayed.");
            } catch (const std::exception& error) {
                LOGW("[CONTROL] %s",error.what());
                FailedAction(action,error.what());
            }
        }
        winrt::uninit_apartment();
    }
};
NativeControl::NativeControl():impl_(std::make_unique<Impl>()){}
NativeControl::~NativeControl(){Shutdown();}
void NativeControl::Start(const std::string& pairs,std::string rememberedPhone,std::string rememberedSender,bool bootstrap){
    {std::lock_guard<std::mutex> lock(impl_->mutex);impl_->link.Start(pairs,rememberedPhone,rememberedSender,bootstrap);
      impl_->state.pairs=impl_->link.pairs.Serialize();impl_->pair=!impl_->link.wanted.empty();}
    if(!impl_->worker.joinable())impl_->worker=std::thread([this]{impl_->Loop();});
}
void NativeControl::Shutdown(){
    {std::lock_guard<std::mutex> lock(impl_->mutex);impl_->quitting=true;impl_->DropInput();}
    impl_->Cancel();if(impl_->worker.joinable())impl_->worker.join();
}
void NativeControl::Pair(){
    {std::lock_guard<std::mutex> lock(impl_->mutex);impl_->DropInput();impl_->focused=false;impl_->pair=true;impl_->link.OnPair();}
    impl_->Cancel();
}
void NativeControl::Bind(std::string token){
    {std::lock_guard<std::mutex> lock(impl_->mutex);impl_->DropInput();impl_->focused=false;impl_->bind=std::move(token);}
    impl_->Cancel();
}
void NativeControl::Disconnect(){
    {std::lock_guard<std::mutex> lock(impl_->mutex);impl_->DropInput();impl_->focused=false;impl_->disconnect=true;impl_->link.OnDisconnect();}
    impl_->Cancel();
}
void NativeControl::Focus(bool value){
    {std::lock_guard<std::mutex> lock(impl_->mutex);impl_->focused=value;impl_->RefreshFocus();}
    if(!value)impl_->Cancel();else impl_->wake.notify_one();
}
void NativeControl::Mirror(bool value){
    {std::lock_guard<std::mutex> lock(impl_->mutex);if(impl_->mirror!=value){impl_->DropInput();impl_->focused=false;}impl_->mirror=value;
      if(!value)impl_->link.OnMirrorEnded();impl_->RefreshFocus();}
    impl_->Cancel();
}
void NativeControl::Pause(bool value){
    {std::lock_guard<std::mutex> lock(impl_->mutex);if(impl_->paused!=value){impl_->DropInput();impl_->focused=false;}impl_->paused=value;impl_->RefreshFocus();}
    impl_->Cancel();
}
void NativeControl::Picture(bool present,int width,int height,uint64_t generation){
    bool changed;
    {std::lock_guard<std::mutex> lock(impl_->mutex);if(present && !impl_->frameGate.Accept(generation))return;
      changed=present!=impl_->picture||width!=impl_->width||height!=impl_->height;
      if(changed)impl_->DropInput();impl_->picture=present;impl_->width=width;impl_->height=height;impl_->RefreshFocus();}
    if(changed)impl_->Cancel();
}
void NativeControl::AwaitPicture(uint64_t generation){
    {std::lock_guard<std::mutex> lock(impl_->mutex);if(!impl_->frameGate.Await(generation))return;
      impl_->picture=false;impl_->DropInput();impl_->RefreshFocus();}
    impl_->Cancel();
}
// Geometry and picture changes drop pending input (its coordinates are stale) but keep
// control on while the phone window still has focus; input waits for a valid picture.
// Failures and queue expiry still clear focus, so they need a fresh click.
void NativeControl::Invalidate(){
    {std::lock_guard<std::mutex> lock(impl_->mutex);impl_->DropInput();impl_->RefreshFocus();}
    impl_->Cancel();
}
void NativeControl::Submit(ControlKind kind,int x,int y,unsigned char key){
    bool failed;
    {std::lock_guard<std::mutex> lock(impl_->mutex);if(!impl_->queue.Armed())return;
      failed=!impl_->queue.Push({kind,x,y,key},GetTickCount64());
      if(failed){impl_->DropInput();impl_->focused=false;impl_->state.message="Input stopped: queue capacity exceeded. Remaining input was discarded.";}}
    if(failed)impl_->Cancel();
    impl_->wake.notify_one();
}
void NativeControl::SubmitText(std::string text){
    if(text.empty())return;
    bool failed;
    {std::lock_guard<std::mutex> lock(impl_->mutex);if(!impl_->queue.Armed())return;
      ControlAction action{ControlKind::Text};action.text=std::move(text);
      failed=!impl_->queue.Push(std::move(action),GetTickCount64());
      if(failed){impl_->DropInput();impl_->focused=false;impl_->state.message="Input stopped: text exceeds 1,024 pending characters. Remaining input was discarded.";}}
    if(failed)impl_->Cancel();impl_->wake.notify_one();
}
void NativeControl::RejectInput(const std::string& message){
    {std::lock_guard<std::mutex> lock(impl_->mutex);impl_->DropInput();impl_->focused=false;impl_->state.message=message;}
    impl_->Cancel();
}
void NativeControl::Sender(const std::string& identity){
    uint64_t hash=14695981039346656037ull;for(unsigned char c:identity){hash^=c;hash*=1099511628211ull;}
    std::ostringstream out;out<<std::hex<<std::setfill('0')<<std::setw(16)<<hash;
    const std::string token=identity.empty()?"":out.str();
    std::string wanted;
    bool changed,known;
    {std::lock_guard<std::mutex> lock(impl_->mutex);
      impl_->DropInput();impl_->focused=false;
      changed=token!=impl_->link.sender;
      // A different iPhone no longer closes Bluetooth. The worker lets go of the phone that was
      // selected and selects this sender's phone, which normally is still connected.
      if(impl_->link.OnSender(token))impl_->reopen=true;
      wanted=impl_->link.wanted;known=!wanted.empty();
      if(!impl_->link.Matches(impl_->state.bound))impl_->state.ready=false; // no input until the worker has switched
      impl_->state.pairs=impl_->link.pairs.Serialize();
      impl_->refresh=true;}
    if(changed)LOGI("[CONTROL] Mirror sender %s; %s",token.empty()?"unknown":token.c_str(),
        known?("its phone is "+wanted).c_str():"no phone is remembered for it");
    impl_->Cancel();
}
NativeControlState NativeControl::State()const{
    std::lock_guard<std::mutex>lock(impl_->mutex);
    auto result=impl_->state;result.busy=impl_->processing || impl_->queue.Size()!=0;return result;
}
