#include "../Source/SubtitleModel.h"
#include <cstdlib>
#include <iostream>
#include <map>

using namespace edf5sub;
int checks = 0;
void check(bool condition, const char* reason) {
    ++checks;
    if (!condition) { std::cerr << "FAIL: " << reason << '\n'; std::exit(1); }
}
bool close(double a, double b) { return std::abs(a-b) < 0.0001; }
int main() {
    Timing timing;
    std::map<std::uint32_t,int> states{{0x10001,1},{0x20003,2}};
    auto query = [&](std::uint32_t id) { return states.at(id); };
    SubtitleInstance a;
    a.voiceId="AE103_037_S"; a.playbackIds={0x10001};
    SubtitleInstance b=a; b.serial=2; b.playbackIds={0x20003};
    for (int i=0;i<60*60;++i) { a.Advance(1.0/60,false,timing,query); b.Advance(1.0/60,false,timing,query); }
    check(!a.ended && close(a.Alpha(timing),1),"60-second broadcast stays visible while sound is active");
    check(!b.ended,"same voice name with another instance is independent");
    states[0x10001]=3;
    a.Advance(1.0/60,false,timing,query);
    b.Advance(1.0/60,false,timing,query);
    check(a.ended && close(a.afterVoice,0),"actual end starts a fresh exit clock");
    check(!b.ended,"ending one instance cannot end another");
    a.Advance(.9,false,timing,query);
    check(close(a.Alpha(timing),1),"native hold retained after end");
    a.Advance(.6,false,timing,query);
    check(close(a.Alpha(timing),.5),"fade only after voice and hold end");
    a.Advance(100,true,timing,query);
    check(close(a.Alpha(timing),.5),"pause freezes fade and scroll clock");
    a.Advance(.5,false,timing,query);
    check(a.Finished(timing),"expired entry can be removed without name-based deletion");
    SubtitleInstance grouped; grouped.playbackIds={0x10001,0x20003};
    grouped.Advance(1,false,timing,query);
    check(!grouped.ended,"all captured instances must end before exit");
    states[0x20003]=3;
    grouped.Advance(1,false,timing,query);
    check(grouped.ended,"stopped or invalid handles end via native state 3");
    for (double fps : {30.,60.,120.,240.}) {
        SubtitleInstance item; item.ended=true;
        for (int i=0;i<static_cast<int>(fps*1.5);++i) item.Advance(1/fps,false,timing,query);
        check(close(item.Alpha(timing),.5),"fade seconds invariant across frame rates");
    }
    check(close(ReferenceLayout::Top(1080),720),"EDF6 origin at 1080p");
    check(close(ReferenceLayout::Top(2160),1440),"EDF6 origin at 4K");
    check(close(ReferenceLayout::Scroll(256,256,12),0),"long text waits before scrolling");
    check(close(ReferenceLayout::Scroll(256,256,20),70),"native scroll uses 10 reference pixels per second");
    check(close(ReferenceLayout::Scroll(256,256,100),128),"long text stops at last line");
    check(close(ReferenceLayout::RowHeight(64),64),"short text is not padded into a fixed-height window");
    check(DisplayCaption("caption",false)==u8"『caption』","nonspatial caption decoration");
    check(DisplayCaption("caption",true)==u8"「caption」","spatial caption decoration");
    std::cout << "PASS " << checks << " playback lifecycle and layout checks\n";
}
