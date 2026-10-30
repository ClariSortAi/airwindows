#include "native_control.h"
#include "native_hid_backend.h"
#include "log.h"
#include <windows.h>
#include <winrt/base.h>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <chrono>
#include <sstream>
#include <iomanip>

struct NativeControl::Impl {
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::thread worker;
    std::shared_ptr<NativeHidBackend> backend;
    ControlQueue queue;
    NativeControlState state;
    bool quitting=false, pair=false, disconnect=false, stop=false;
    bool focused=false, mirror=false, paused=false, picture=false;
    int width=0,height=0;
    ControlFrameGate frameGate;
    std::string bind, expected, sender, expectedSender;
    bool learnSender=false;
    bool held=false;
    int pointerX=0,pointerY=0;
    uint64_t pressedAt=0, holdExpires=0, holdLimit=0;
    uint64_t inspectAt=0;

    void RefreshFocus() { // mutex held
        const bool usable = focused && mirror && !paused && picture && state.ready;
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
        DropInput(); state.ready=false; state.published=false; state.bound.clear(); state.peers.clear();
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
            bool doPair,doDisconnect,doStop,quit;
            std::string doBind;
            std::optional<ControlAction> action;
            {
                std::unique_lock<std::mutex> lock(mutex);
                if(queue.Size()==0)wake.wait_for(lock,std::chrono::milliseconds(30));
                quit=quitting; doPair=pair; pair=false; doDisconnect=disconnect; disconnect=false;
                doStop=stop; stop=false; doBind.swap(bind);
                action=queue.Pop(GetTickCount64());
                if(queue.LastFailure()==ControlQueue::Failure::Expired){
                    DropInput();focused=false;state.message="Input stopped: queued action expired. Remaining input was discarded.";doStop=true;
                }
            }
            try {
                if (quit) { Close(); break; }
                if (doDisconnect) { Close(); std::lock_guard<std::mutex> lock(mutex); state.message="Bluetooth control disconnected"; continue; }
                auto current=std::atomic_load(&backend);
                if (current && (doStop || (held && (!queue.Armed() || GetTickCount64()>=holdExpires || GetTickCount64()>=holdLimit)))) {
                    current->Stop(); held=false;
                }
                if (doPair) {
                    if (!current) {
                        current=std::make_shared<NativeHidBackend>(); current->Initialize(true);
                        std::atomic_store(&backend,current);
                    }
                    if (!current->Inspect().published) current->Advertise(120);
                    LOGI("[CONTROL] Native Bluetooth publication opened for 120 seconds");
                    std::lock_guard<std::mutex> lock(mutex);
                    state.message="Pair this PC in iPhone Settings > Bluetooth; select the phone below";
                }
                if (!current) continue;
                if (!doBind.empty()) {
                    current->Bind(doBind); current->KeepPublishedWhileBound(true);
                    LOGI("[CONTROL] Selected phone %s; persistent publication",doBind.c_str());
                    std::lock_guard<std::mutex> lock(mutex);
                    expected=doBind;expectedSender=sender;learnSender=sender.empty();
                    state.bound=doBind; state.message="Click the mirrored phone to control it; click elsewhere to return";
                }
                current->Poll();
                if (current->PublicationStopFailed()) throw std::runtime_error("Bluetooth publication could not stop; disconnecting");
                if (GetTickCount64() >= inspectAt || !doBind.empty() || doPair) {
                    inspectAt=GetTickCount64()+250;
                    auto view=current->Inspect();
                    std::string remembered;
                    {std::lock_guard<std::mutex> lock(mutex);remembered=expected;}
                    if(view.bound.empty() && !remembered.empty() && view.peers.size()==1 && view.peers.front().absolute && view.peers.front().fingerprint==remembered) {
                        current->Bind(remembered); current->KeepPublishedWhileBound(true); view=current->Inspect();
                        LOGI("[CONTROL] Reconnected saved phone %s",remembered.c_str());
                        std::lock_guard<std::mutex> lock(mutex);
                        state.bound=remembered;state.message="Click the mirrored phone to control it; click elsewhere to return";
                    }
                    bool lost=false;
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        state.peers.clear();
                        for (auto& peer:view.peers) if (peer.absolute) state.peers.push_back({peer.fingerprint,peer.name});
                        state.published=view.published;
                        const bool subscribed=state.peers.size()==1 && !state.bound.empty() && view.bound==state.bound && state.peers.front().token==state.bound;
                        const bool ready=subscribed && !sender.empty() && sender==expectedSender;
                        if(ready!=state.ready)LOGI("[CONTROL] Ready=%d; matching Bluetooth and AirPlay identities",ready?1:0);
                        state.ready=ready;
                        state.sender=state.ready?expectedSender:"";
                        lost=!state.bound.empty()&&!subscribed;
                        if (lost) state.message="Phone disconnected. Waiting for the saved phone.";
                        else if(subscribed && !state.ready)state.message="Bluetooth connected; mirror the selected iPhone";
                        else if (!view.published) state.message="Pairing window expired. Click Pair to reopen.";
                        RefreshFocus();
                    }
                    if (lost) { Close(); std::lock_guard<std::mutex> lock(mutex); focused=false; pair=!expected.empty(); continue; }
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
                        const int end=std::clamp(command.y+(command.kind==ControlKind::ScrollUp?7000:-7000),1000,31767);
                        current->Swipe(command.x,command.y,command.x,end); break;
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
                FailedAction(action,"Bluetooth action failed. Click the mirror to retry; actions are never replayed.");
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
void NativeControl::Start(std::string rememberedPhone,std::string rememberedSender,bool bootstrap){
    {std::lock_guard<std::mutex> lock(impl_->mutex);impl_->expected=std::move(rememberedPhone);impl_->expectedSender=std::move(rememberedSender);
      impl_->learnSender=bootstrap;impl_->pair=!impl_->expected.empty();}
    if(!impl_->worker.joinable())impl_->worker=std::thread([this]{impl_->Loop();});
}
void NativeControl::Shutdown(){
    {std::lock_guard<std::mutex> lock(impl_->mutex);impl_->quitting=true;impl_->DropInput();}
    impl_->Cancel();if(impl_->worker.joinable())impl_->worker.join();
}
void NativeControl::Pair(){
    {std::lock_guard<std::mutex> lock(impl_->mutex);impl_->DropInput();impl_->focused=false;impl_->pair=true;}
    impl_->Cancel();
}
void NativeControl::Bind(std::string token){
    {std::lock_guard<std::mutex> lock(impl_->mutex);impl_->DropInput();impl_->focused=false;impl_->bind=std::move(token);}
    impl_->Cancel();
}
void NativeControl::Disconnect(){
    {std::lock_guard<std::mutex> lock(impl_->mutex);impl_->DropInput();impl_->focused=false;impl_->disconnect=true;impl_->expected.clear();}
    impl_->Cancel();
}
void NativeControl::Focus(bool value){
    {std::lock_guard<std::mutex> lock(impl_->mutex);impl_->focused=value;impl_->RefreshFocus();}
    if(!value)impl_->Cancel();else impl_->wake.notify_one();
}
void NativeControl::Mirror(bool value){
    {std::lock_guard<std::mutex> lock(impl_->mutex);if(impl_->mirror!=value){impl_->DropInput();impl_->focused=false;}impl_->mirror=value;impl_->RefreshFocus();}
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
      if(changed){impl_->DropInput();impl_->focused=false;}impl_->picture=present;impl_->width=width;impl_->height=height;impl_->RefreshFocus();}
    if(changed)impl_->Cancel();
}
void NativeControl::AwaitPicture(uint64_t generation){
    {std::lock_guard<std::mutex> lock(impl_->mutex);if(!impl_->frameGate.Await(generation))return;
      impl_->picture=false;impl_->DropInput();impl_->focused=false;}
    impl_->Cancel();
}
void NativeControl::Invalidate(){
    {std::lock_guard<std::mutex> lock(impl_->mutex);impl_->DropInput();impl_->focused=false;}
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
    {std::lock_guard<std::mutex> lock(impl_->mutex);
      impl_->DropInput();impl_->focused=false;
      if(!token.empty() && impl_->learnSender){impl_->expectedSender=token;impl_->learnSender=false;}
      if((!impl_->expectedSender.empty() && impl_->expectedSender!=token) || (!impl_->sender.empty() && impl_->sender!=token)){
          impl_->disconnect=true;impl_->expected.clear();impl_->state.ready=false;
      }
      impl_->sender=token;}
    impl_->Cancel();
}
NativeControlState NativeControl::State()const{std::lock_guard<std::mutex>lock(impl_->mutex);return impl_->state;}
