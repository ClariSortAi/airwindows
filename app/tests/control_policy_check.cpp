#include "control_queue.h"
#include "absolute_buttons.h"
#include "subscriber_selection.h"
#include "peer_state.h"
#include "phone_pairs.h"
#include <iostream>
#include <set>
#include <stdexcept>
#include <vector>
int main(){
    unsigned checks=0;
    const auto check=[&](bool ok){++checks;if(!ok)throw std::runtime_error("control policy check failed");};
    ControlQueue queue;
    check(!queue.Push({ControlKind::Click,1,2},0));
    queue.Focus(true);
    queue.Push({ControlKind::Point,1,2},100);
    queue.Push({ControlKind::Point,3,4},101);
    check(queue.Size()==1);
    queue.Push({ControlKind::Click,5,6},102);
    queue.Push({ControlKind::Point,7,8},103);
    check(queue.Size()==3);
    const auto first=queue.Pop(110);check(first&&first->x==3);
    const auto click=queue.Pop(110);check(click&&click->kind==ControlKind::Click&&click->x==5);
    queue.Focus(false);check(!queue.Valid(*click,111)&&queue.Size()==0);
    queue.Focus(true);check(!queue.Valid(*click,111)); // a new focus cannot revive an old action
    queue.Push({ControlKind::Key,0,0,40},200);check(!queue.Pop(2201)&&!queue.Armed());
    queue.Focus(true);
    for(int i=0;i<32;++i)check(queue.Push({ControlKind::Key,0,0,40},300));
    check(!queue.Push({ControlKind::Click,1,1},301)&&!queue.Armed()&&queue.Size()==0);
    check(!MirrorPoint(10,500,1000,1000,498,1080)); // left bar
    check(!MirrorPoint(500,10,1000,1000,1080,498)); // top bar
    const auto exact=MirrorPoint(249,540,498,1080,498,1080);
    check(exact&&exact->first==16416&&exact->second==16399);
    const auto scaled=MirrorPoint(500,500,1000,1000,498,1080);
    check(scaled&&scaled->first>16300&&scaled->first<16500&&scaled->second>16300&&scaled->second<16500);
    check(!MirrorPoint(0,0,0,0,498,1080));
    // A fitted window leaves a ~0.08 px "bar"; pixel 0 is drawn with phone content, so it is a target.
    const auto fractional=MirrorPoint(0,0,433,938,886,1920);check(fractional&&fractional->first==0&&fractional->second==0);
    // Bezel: in a 269 px side bar, 10 px from the picture snaps to the edge; 40 px does not.
    const auto bezel=MirrorPoint(259,500,1000,1000,498,1080,16);check(bezel&&bezel->first==0);
    check(!MirrorPoint(229,500,1000,1000,498,1080,16) && !MirrorPoint(259,500,1000,1000,498,1080));
    const auto pinned=MirrorPoint(-500,-500,1000,1000,498,1080,2000);check(pinned&&pinned->first==0&&pinned->second==0);
    queue.Focus(true);
    for(int i=0;i<80;++i)check(queue.Push({ControlKind::Text,0,0,static_cast<unsigned char>('a'+i%26)},1000+i));
    check(queue.Size()==1); // an injected burst is one ordered text job, not80 actions
    queue.Push({ControlKind::Click,10,20},1100);
    std::string received;
    while(queue.Size()>1){const auto part=queue.Pop(1200);check(part&&part->kind==ControlKind::Text);received+=part->text;}
    check(received.size()==80 && received.substr(0,26)=="abcdefghijklmnopqrstuvwxyz");
    check(queue.Pop(1200)->kind==ControlKind::Click);
    ControlAction paste{ControlKind::Text};paste.text=std::string(1024,'x');
    check(queue.Push(paste,2000));check(!queue.Push({ControlKind::Text,0,0,'y'},2001));
    check(queue.LastFailure()==ControlQueue::Failure::Capacity && queue.Size()==0);
    queue.Focus(true);queue.Push(paste,2000);const auto part=queue.Pop(2100);check(part&&part->text.size()==4);
    queue.Focus(false);check(!queue.Valid(*part,2101) && queue.Size()==0);
    queue.Focus(true);queue.Push(paste,3000);check(queue.Pop(33001).has_value()); // 1,024 characters get 60 ms each, not a flat 30 s
    check(!queue.Pop(3000+61441) && queue.LastFailure()==ControlQueue::Failure::Expired);
    queue.Focus(true);for(int i=0;i<4;++i)check(queue.Push({ControlKind::ScrollDown,10,20},4000+i));
    check(queue.Size()==2); // extra wheel steps are dropped, not left to expire
    check(queue.Push({ControlKind::ScrollUp,10,20},4010) && queue.Size()==3);
    const auto normalized=PrintablePaste(L"first\r\nsecond\tline\rthird");check(normalized&&*normalized=="first\nsecond\tline\nthird");
    check(!PrintablePaste(L"caf\u00e9"));check(!PrintablePaste(std::wstring(1025,L'a')));
    check(PrintablePaste(std::wstring(1024,L'a'))->size()==1024);
    ControlFrameGate gate;
    check(gate.Accept(5));check(!gate.Await(5)); // decoder wins the scheduling race
    check(gate.Await(6));check(!gate.Accept(5));check(gate.Accept(6));
    check(!gate.Await(5));check(gate.Accept(7));check(!gate.Await(7));
    const std::vector<uint8_t> last{1,0x34,0x12,0x78,0x56};
    const auto homeDown=AbsoluteButtons(last,true,2);
    check(homeDown==std::vector<uint8_t>({2,0x34,0x12,0x78,0x56})); // button2, never button1
    const auto homeUp=AbsoluteButtons(homeDown,true,0);
    check(homeUp==std::vector<uint8_t>({0,0x34,0x12,0x78,0x56}));
    check(AbsoluteButtons(homeUp,true,0)==homeUp); // retry unknown-delivery release, retain XY
    const auto centered=AbsoluteButtons({},false,0);
    check(centered==std::vector<uint8_t>({0,0,64,0,64}));
    check(AbsoluteButtons(centered,true,2)==std::vector<uint8_t>({2,0,64,0,64}));
    bool badMask=false;try{AbsoluteButtons(last,true,8);}catch(const std::invalid_argument&){badMask=true;}check(badMask);
    bool badSize=false;try{AbsoluteButtons({0,0,0,0},true,2);}catch(const std::invalid_argument&){badSize=true;}check(badSize);
    bool badRange=false;try{AbsoluteButtons({0,0,128,0,0},true,2);}catch(const std::invalid_argument&){badRange=true;}check(badRange);
    queue.Focus(true);queue.Push({ControlKind::Text,0,0,'a'},4000);const auto oldText=queue.Pop(4001);
    queue.Push({ControlKind::Down,100,200},4002);
    queue.Focus(false);queue.Focus(true); // toolbar Home cancels earlier input before refocusing
    check(!queue.Valid(*oldText,4003) && queue.Size()==0);
    check(queue.Push({ControlKind::Home},4004));
    check(!queue.CancelIfCurrent(*oldText) && queue.Size()==1 && queue.Armed()); // old in-flight catch must not erase Home
    const auto home=queue.Pop(4005);
    check(home&&home->kind==ControlKind::Home&&queue.Valid(*home,4005));
    queue.Focus(false);check(!queue.Valid(*home,4006));
    queue.Focus(true);queue.Push({ControlKind::Home},5000);check(!queue.Pop(7001));
    queue.Focus(true);queue.Push({ControlKind::Home},8000);const auto sameEpoch=queue.Pop(8001);
    check(queue.CancelIfCurrent(*sameEpoch) && !queue.Armed()); // current action failure still disarms
    const auto portraitFit=FitMirrorClient(1104,660,498,1080,1904,1000,634,72);
    check(portraitFit&&portraitFit->first>=634&&portraitFit->second<=1000&&portraitFit->second>72);
    const auto landscapeFit=FitMirrorClient(1104,660,1080,498,1264,680,634,72);
    check(landscapeFit&&landscapeFit->first>=634&&landscapeFit->first<=1264&&landscapeFit->second<=680);
    const auto dpiFit=FitMirrorClient(2208,1320,498,1080,1888,940,1268,144);
    check(dpiFit&&dpiFit->first>=1268&&dpiFit->second<=940);
    const auto shortMonitor=FitMirrorClient(1104,660,498,1080,784,400,634,72);
    check(shortMonitor&&shortMonitor->first>=634&&shortMonitor->second<=400);
    check(!FitMirrorClient(1104,660,498,1080,100,50,634,72));
    // A fitted portrait keeps its toolbar width and still rejects its side bars.
    check(portraitFit&&!MirrorPoint(0,100,portraitFit->first,portraitFit->second-72,498,1080));
    queue.Focus(true);check(queue.Push({ControlKind::Secondary,16416,16399},9000));
    const auto secondary=queue.Pop(9001);check(secondary&&secondary->kind==ControlKind::Secondary&&secondary->x==16416);
    queue.Focus(false);check(!queue.Valid(*secondary,9002));
    std::vector<std::string> subscribers{"selected"};
    const auto selected=[](const auto& peer){return peer=="selected";};
    check(UniqueSubscriber(subscribers,selected)!=subscribers.end());
    subscribers.insert(subscribers.begin(),"other");
    check(*UniqueSubscriber(subscribers,selected)=="selected"); // extra peer never takes over
    subscribers.push_back("third");check(*UniqueSubscriber(subscribers,selected)=="selected");
    subscribers.erase(subscribers.begin()+1);check(UniqueSubscriber(subscribers,selected)==subscribers.end()); // loss does not pick a fallback
    subscribers.push_back("selected");check(*UniqueSubscriber(subscribers,selected)=="selected"); // exact reconnect
    subscribers.push_back("selected");check(UniqueSubscriber(subscribers,selected)==subscribers.end()); // ambiguous identity
    subscribers.clear();check(UniqueSubscriber(subscribers,selected)==subscribers.end());
    WheelSteps wheel;
    for(int i=0;i<7;++i)check(wheel.Add(15,1,100+i*10)==0);
    check(wheel.Add(15,1,170)==1); // eight trackpad packets are ONE swipe
    check(wheel.Add(-240,1,180)==-2); // two conventional notches
    check(wheel.Add(0,1,190)==0);
    check(wheel.Add(60,1,200)==0);check(wheel.Add(-60,1,210)==0);
    check(wheel.Add(-120,1,220)==-1); // reverse without an extra swipe
    check(wheel.Add(60,1,230)==0);wheel.Reset();check(wheel.Add(60,1,240)==0);
    check(wheel.Add(60,2,250)==0); // new picture discards old remainder
    check(wheel.Add(60,2,800)==0); // separate gestures do not accumulate
    check(wheel.Add(60,2,810)==1);
    check(wheel.Add(32767,2,820)==4);check(wheel.Add(-32768,2,830)==-4);
    // Two phones. A is locked on the desk, B is the one mirroring and selected.
    const std::string phoneA="7753000000000001",phoneB="01ba000000000002",none;
    PeerHidStates<std::string> hid;
    check(!hid.Suspended(phoneB) && hid.Protocol(phoneB)==1); // a phone never heard from is awake, in report mode
    check(!hid.Suspend(phoneA,true,phoneB));                  // A locking is not the selected phone's business
    check(hid.Suspended(phoneA) && !hid.Suspended(phoneB));   // and must not stop B's input
    check(hid.Suspend(phoneB,true,phoneB) && hid.Suspended(phoneB)); // the selected phone locking does stop it
    check(hid.Suspend(phoneB,false,phoneB) && !hid.Suspended(phoneB));
    check(!hid.Suspend(phoneA,true,none));                    // nothing selected: nothing to cancel
    check(!hid.SetProtocol(phoneA,0,phoneB) && hid.Protocol(phoneA)==0 && hid.Protocol(phoneB)==1);
    check(hid.SuspendedCount()==1);
    hid.Retain(std::set<std::string>{phoneB});                // A disconnects
    check(!hid.Suspended(phoneA) && hid.Protocol(phoneA)==1 && hid.SuspendedCount()==0); // and comes back fresh
    hid.Suspend(phoneB,true,phoneB);hid.Retain(std::set<std::string>{phoneA,phoneB});check(hid.Suspended(phoneB));

    const std::string senderA="92870000000000aa",senderB="62340000000000bb",senderC="36600000000000cc";
    PhonePairs pairs;
    check(pairs.Empty() && pairs.Phone(senderA).empty() && pairs.Serialize().empty());
    pairs.Set(senderA,phoneA);pairs.Set(senderB,phoneB);
    check(pairs.Phone(senderA)==phoneA && pairs.Phone(senderB)==phoneB);
    check(pairs.Serialize()==senderB+"="+phoneB+";"+senderA+"="+phoneA); // newest first
    check(PhonePairs::Parse(pairs.Serialize()).Serialize()==pairs.Serialize());
    pairs.Set(senderA,phoneA);check(pairs.Serialize().substr(0,16)==senderA); // used again: back to the front
    pairs.Set(senderB,phoneA);check(pairs.Phone(senderB)==phoneA);          // a corrected choice replaces the old one
    pairs.Set("not-a-token",phoneA);pairs.Set(senderC,"short");check(pairs.Phone("not-a-token").empty() && pairs.Phone(senderC).empty());
    check(PhonePairs::Parse("garbage;"+senderA+"="+phoneA+";;"+senderA+"="+phoneB+";"+senderB+"=XYZ").Serialize()==senderA+"="+phoneA);
    check(PhonePairs::Parse("").Empty() && PhonePairs::Parse(";;;").Empty());
    PhonePairs many;
    for(int i=0;i<20;++i){std::string sender=senderA;sender[15]=char('a'+i%6);sender[14]=char('0'+i/6);many.Set(sender,phoneA);}
    check(PhonePairs::Parse(many.Serialize()).Serialize()==many.Serialize() && many.Serialize().size()<511); // fits the settings reader
    PhonePairs two;two.Set(senderA,phoneA);two.Set(senderB,phoneB);
    check(two.Elsewhere(phoneA,senderB) && !two.Elsewhere(phoneA,senderA)); // A's phone is not the one B mirrors
    check(!two.Elsewhere("ffff000000000003",senderB));                      // a phone never chosen could be anyone's
    two.Set(senderC,phoneA);check(!two.Elsewhere(phoneA,senderC));          // one phone, two networks, two senders

    // First phone, set up the way the guide says: mirror, then choose.
    PhoneLink link;
    link.Start("","","",false);
    check(link.wanted.empty() && !link.Matches(phoneA));
    check(!link.OnSender(senderA));                 // unknown sender: nothing to open Bluetooth for
    check(link.wanted.empty() && link.session);
    link.OnChoose(phoneA);
    check(link.Matches(phoneA) && link.pairs.Phone(senderA)==phoneA);
    link.OnMirrorEnded();check(link.Matches(phoneA)); // the phone stays selected between mirrors
    // The second phone mirrors: A is let go, nothing is closed, and the user is asked once.
    check(!link.OnSender(senderB));
    check(link.MustRelease(phoneA) && !link.Matches(phoneA) && link.wanted.empty());
    check(link.Elsewhere(phoneA) && !link.Elsewhere(phoneB)); // the list marks A as a different iPhone
    link.OnChoose(phoneB);
    check(link.Matches(phoneB) && !link.MustRelease(phoneB) && link.pairs.Phone(senderA)==phoneA);
    // From here on, switching needs no choice in either direction.
    link.OnMirrorEnded();
    check(link.OnSender(senderA));                  // true: make sure A's Bluetooth can reach the PC
    check(link.wanted==phoneA && link.MustRelease(phoneB) && !link.Matches(phoneB) && link.Matches(phoneA));
    link.OnMirrorEnded();
    check(link.OnSender(senderB) && link.wanted==phoneB && link.MustRelease(phoneA) && link.Matches(phoneB));
    check(link.OnSender(senderB) && link.Matches(phoneB) && !link.MustRelease(phoneB)); // the same phone reconnecting
    // The morning that prompted this: A's Bluetooth was chosen for B's mirror. Choosing B corrects it.
    link.OnChoose(phoneA);check(link.pairs.Phone(senderB)==phoneA && link.Matches(phoneA));
    link.OnChoose(phoneB);check(link.pairs.Phone(senderB)==phoneB && link.Matches(phoneB) && link.MustRelease(phoneA));
    // A restart keeps every pair, and the phone last used is the one to reconnect.
    PhoneLink restarted;
    restarted.Start(link.pairs.Serialize(),phoneB,senderB,false);
    check(restarted.wanted==phoneB && !restarted.Matches(phoneB) && !restarted.MustRelease(phoneB)); // no mirror yet
    check(restarted.OnSender(senderA) && restarted.wanted==phoneA && restarted.MustRelease(phoneB));
    // Settings from a version that kept one pair become the first entry.
    PhoneLink migrated;
    migrated.Start("",phoneA,senderA,false);
    check(migrated.pairs.Phone(senderA)==phoneA && migrated.wanted==phoneA);
    check(migrated.OnSender(senderA) && migrated.Matches(phoneA));
    // A phone chosen while nothing mirrors belongs to the next new sender, never to a remembered one.
    PhoneLink early;
    early.Start(senderA+"="+phoneA,"","",false);
    early.OnChoose(phoneB);
    check(early.wanted==phoneB && !early.Matches(phoneB) && !early.MustRelease(phoneB));
    check(early.OnSender(senderA) && early.wanted==phoneA && early.MustRelease(phoneB) && early.pairs.Phone(senderA)==phoneA);
    early.OnMirrorEnded();early.OnChoose(phoneB);
    check(early.OnSender(senderB) && early.Matches(phoneB) && early.pairs.Phone(senderB)==phoneB);
    // A sender with no identity is never remembered and never matches.
    PhoneLink anonymous;
    anonymous.Start(senderA+"="+phoneA,phoneA,senderA,false);
    check(!anonymous.OnSender("") && anonymous.MustRelease(phoneA) && !anonymous.Matches(phoneA));
    anonymous.OnChoose(phoneA);check(!anonymous.Matches(phoneA) && anonymous.pairs.Serialize()==senderA+"="+phoneA);
    // Disconnect keeps Bluetooth closed through later mirrors until the user asks again.
    PhoneLink detached;
    detached.Start(senderA+"="+phoneA,phoneA,senderA,false);
    detached.OnDisconnect();
    check(detached.wanted.empty() && !detached.OnSender(senderA) && detached.wanted==phoneA);
    detached.OnPair();detached.OnMirrorEnded();check(detached.OnSender(senderA));
    // --control-phone: the first sender is that phone's, whatever was remembered before.
    PhoneLink forced;
    forced.Start(senderA+"="+phoneA,phoneB,"",true);
    check(forced.wanted==phoneB && forced.OnSender(senderA) && forced.Matches(phoneB) && forced.pairs.Phone(senderA)==phoneB);
    forced.OnMirrorEnded();check(!forced.OnSender(senderC) && forced.wanted.empty()); // only the first
    std::cout<<checks<<" control queue, coordinate and phone selection checks passed\n";
}
