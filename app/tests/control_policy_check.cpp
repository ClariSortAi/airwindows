#include "control_queue.h"
#include "absolute_buttons.h"
#include "subscriber_selection.h"
#include <iostream>
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
    queue.Focus(true);queue.Push(paste,3000);check(!queue.Pop(33001) && queue.LastFailure()==ControlQueue::Failure::Expired);
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
    std::cout<<checks<<" control queue and coordinate checks passed\n";
}
