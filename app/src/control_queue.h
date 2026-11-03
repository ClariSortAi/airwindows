#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <cmath>

enum class ControlKind { Point, Click, Down, Up, ScrollUp, ScrollDown, Text, Key, Home, Secondary };
// WM_MOUSEWHEEL may deliver fractions of a 120-unit notch. Carry only the
// current focused picture's short-lived remainder, never a previous gesture.
class WheelSteps {
public:
    void Reset() { remainder_=0; generation_=0; last_=0; }
    int Add(int delta,uint64_t generation,uint64_t now) {
        if(generation!=generation_ || now<last_ || now-last_>500)remainder_=0;
        generation_=generation;last_=now;
        // ponytail: bound a single event to four swipes; no new gesture queue.
        remainder_+=std::clamp(delta,-480,480);
        const int steps=remainder_/120;remainder_%=120;return steps;
    }
private:
    int remainder_=0;
    uint64_t generation_=0,last_=0;
};
struct ControlAction {
    ControlKind kind;
    int x = 0, y = 0;
    unsigned char key = 0;
    uint64_t epoch = 0, deadline = 0;
    std::string text;
};
// Queue mutation is serialized by its owner. Valid() is also used by the BLE
// send path and reads only atomics, allowing focus loss to invalidate an action.
class ControlQueue {
public:
    enum class Failure { None, Capacity, Expired };
    void Focus(bool enabled) {
        armed_.store(false);
        epoch_.fetch_add(1);
        actions_.clear();
        textBytes_=0;failure_=Failure::None;
        armed_.store(enabled);
    }
    bool Armed() const { return armed_.load(); }
    bool Push(ControlAction action, uint64_t now) {
        if (!Armed()) return false;
        // Text gets about 60 ms per character (two BLE reports each), never less than 30 s.
        const auto textDeadline=[now](size_t bytes){return now+std::max<uint64_t>(30000,60*bytes);};
        action.epoch = epoch_.load(); action.deadline = action.kind==ControlKind::Text?textDeadline(action.text.size()):now+2000;
        if(action.kind==ControlKind::Text){
            if(action.text.empty())action.text.assign(1,char(action.key));
            if(textBytes_+action.text.size()>1024){Focus(false);failure_=Failure::Capacity;return false;}
            textBytes_+=action.text.size();
            if(!actions_.empty() && actions_.back().kind==ControlKind::Text){
                auto& back=actions_.back();back.text+=action.text;
                back.deadline=std::max(back.deadline,textDeadline(back.text.size()));return true;
            }
        }
        // ponytail: a swipe takes ~0.5 s over BLE, so more than two pending in one
        // direction would pass the 2 s deadline and stop control. Extra wheel steps
        // are dropped instead; a fast flick scrolls less rather than disconnecting.
        if((action.kind==ControlKind::ScrollUp || action.kind==ControlKind::ScrollDown) &&
           std::count_if(actions_.begin(),actions_.end(),[&](const ControlAction& queued){return queued.kind==action.kind;})>=2)
            return true;
        if (action.kind == ControlKind::Point && !actions_.empty() && actions_.back().kind == ControlKind::Point)
            actions_.back() = action;
        else {
            if (actions_.size() >= 32) { Focus(false);failure_=Failure::Capacity; return false; }
            actions_.push_back(action);
        }
        return true;
    }
    bool Valid(const ControlAction& action, uint64_t now) const {
        return Armed() && action.epoch == epoch_.load() && now <= action.deadline;
    }
    bool CancelIfCurrent(const ControlAction& failed) {
        if(failed.epoch!=epoch_.load())return false;
        Focus(false);return true;
    }
    std::optional<ControlAction> Pop(uint64_t now) {
        if (actions_.empty()) return {};
        auto value = actions_.front();
        if (!Valid(value,now)) { Focus(false);failure_=Failure::Expired; return {}; }
        if(value.kind==ControlKind::Text){
            // Short chunks retain the original job deadline and ordering. Each
            // chunk acquires a fresh backend action lease, never an input replay.
            const auto count=std::min<size_t>(4,value.text.size());
            value.text.resize(count);textBytes_-=count;
            if(actions_.front().text.size()>count)actions_.front().text.erase(0,count);
            else actions_.pop_front();
        } else actions_.pop_front();
        return value;
    }
    size_t Size() const { return actions_.size(); }
    Failure LastFailure()const{return failure_;}
private:
    std::atomic<uint64_t> epoch_{1};
    std::atomic<bool> armed_{false};
    std::deque<ControlAction> actions_;
    size_t textBytes_=0;
    Failure failure_=Failure::None;
};

// Decode may notify the UI before the protocol thread reaches Await(). A newer
// decoded generation is safe to adopt; Await of that same generation cannot
// discard the only notification. The UI also checks FrameServer's current generation.
class ControlFrameGate {
public:
    bool Await(uint64_t generation){if(generation<=generation_)return false;generation_=generation;return true;}
    bool Accept(uint64_t generation){if(generation<generation_)return false;generation_=generation;return true;}
private:
    uint64_t generation_=0;
};

inline std::optional<std::string> PrintablePaste(std::wstring_view input){
    std::string output;
    for(size_t i=0;i<input.size();++i){
        const auto c=input[i];
        if(c==L'\r'){if(i+1<input.size() && input[i+1]==L'\n')++i;output+='\n';}
        else if(c==L'\n' || c==L'\t' || (c>=32 && c<=126))output+=char(c);
        else return {};
        if(output.size()>1024)return {};
    }
    return output;
}

// Aspect-fit geometry matches VideoPipeline's centered viewport. A pixel is part of the
// picture when its centre is inside it, which is how Direct3D draws it. Within `bezel`
// pixels of the picture a point snaps to the nearest edge, like starting a gesture on
// the phone's bezel; further out in the bars it is not a target.
inline std::optional<std::pair<int,int>> MirrorPoint(int x, int y, int viewWidth, int viewHeight, int imageWidth, int imageHeight, int bezel = 0) {
    if (viewWidth <= 1 || viewHeight <= 1 || imageWidth <= 1 || imageHeight <= 1) return {};
    const double scale = std::min(double(viewWidth)/imageWidth,double(viewHeight)/imageHeight);
    const double width = imageWidth*scale, height = imageHeight*scale;
    const double left = (viewWidth-width)/2, top = (viewHeight-height)/2;
    const double cx = x+.5, cy = y+.5;
    if (cx < left-bezel || cy < top-bezel || cx >= left+width+bezel || cy >= top+height+bezel) return {};
    const auto normalize = [](double coordinate, double extent) {
        return std::clamp(int(coordinate * 32767 / std::max(1.0,extent-1) + .5),0,32767);
    };
    return std::make_pair(normalize(x-left,width),normalize(y-top,height));
}

// Fit the video plus toolbar within the monitor. A portrait picture may retain
// side bars when the toolbar minimum is wider than the available image height.
inline std::optional<std::pair<int,int>> FitMirrorClient(int currentWidth,int currentHeight,int imageWidth,int imageHeight,
    int maximumWidth,int maximumHeight,int minimumWidth,int toolbarHeight){
    if(imageWidth<=0 || imageHeight<=0 || maximumWidth<=0 || maximumHeight<=toolbarHeight)return {};
    const double aspect=double(imageWidth)/imageHeight;
    const double area=double(std::max(1,currentWidth))*std::max(1,currentHeight-toolbarHeight);
    double width=std::sqrt(area*aspect),height=width/aspect;
    const double scale=std::min({1.0,double(maximumWidth)/width,double(maximumHeight-toolbarHeight)/height});
    width*=scale;height*=scale;
    const int fitWidth=std::clamp(std::max(minimumWidth,int(std::ceil(width))),1,maximumWidth);
    const int fitHeight=std::clamp(int(std::ceil(height))+toolbarHeight,toolbarHeight+1,maximumHeight);
    return std::make_pair(fitWidth,fitHeight);
}
