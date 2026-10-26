// Independent helper: existing PMDP read-only input, five-second completed
// display-event intervals, optional owned RTSS OSD. Never linked into the game.
#include <windows.h>
#include <cstdint>
#include <PMDPSharedMemory.h>
#include "rtss_display_stats.hpp"
#include "rtss_osd_adapter.hpp"
#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace ds = xess_vk::display_stats;
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
std::uint64_t number(std::string_view text) {
    int base = 10;
    if (text.starts_with("0x")) { text.remove_prefix(2); base = 16; }
    std::uint64_t value{};
    const auto result = std::from_chars(text.data(), text.data()+text.size(), value, base);
    require(!text.empty() && result.ec == std::errc{} && result.ptr == text.data()+text.size(), "Invalid integer");
    return value;
}
struct Handle {
    HANDLE value{};
    explicit Handle(HANDLE h=nullptr) : value(h) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
std::uint64_t filetime(const FILETIME& value) { return (std::uint64_t(value.dwHighDateTime)<<32)|value.dwLowDateTime; }
bool identityMatches(std::wstring_view image, std::uint64_t actual, std::uint64_t expected) {
    const auto slash = image.find_last_of(L"\\/");
    image.remove_prefix(slash == image.npos ? 0 : slash+1);
    return expected && actual == expected && image.size() == 12 && !_wcsnicmp(image.data(), L"Endfield.exe", 12);
}
struct Target {
    Handle process;
    std::uint64_t created{};
    std::wstring image;
    explicit Target(DWORD pid, std::uint64_t expected=0) : process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,pid)) {
        require(process.value != nullptr, "Cannot open target with limited-query/synchronize access");
        FILETIME begin{}, end{}, kernel{}, user{};
        require(GetProcessTimes(process.value,&begin,&end,&kernel,&user), "Cannot read target creation time");
        created=filetime(begin);
        wchar_t path[32768]{}; DWORD count=static_cast<DWORD>(std::size(path));
        require(QueryFullProcessImageNameW(process.value,0,path,&count), "Cannot read target image path");
        image.assign(path,count);
        require(identityMatches(image,created,expected ? expected : created), "Target is not the expected Endfield.exe instance");
        require(alive(), "Target already exited");
    }
    bool alive() const {
        const auto status=WaitForSingleObject(process.value,0);
        require(status==WAIT_TIMEOUT || status==WAIT_OBJECT_0, "Target wait failed");
        return status==WAIT_TIMEOUT;
    }
};
std::uint64_t qpc() { LARGE_INTEGER value{}; require(QueryPerformanceCounter(&value),"QPC unavailable"); return value.QuadPart; }
DWORD foregroundPid() { DWORD pid{}; GetWindowThreadProcessId(GetForegroundWindow(),&pid); return pid; }

// Public SDK ring reader and cursor adapted from the existing pmdp-readonly.cpp.
struct Header { DWORD signature,version,stride,offset,capacity,count,position,status; };
static_assert(sizeof(Header)==offsetof(PMDP_SHARED_MEMORY,arrFrame));
bool valid(const Header& h,std::size_t bytes) {
    return h.signature==0x504d4450 && h.version==0x00020000 && h.stride>=sizeof(PMDP_FRAME_DATA) && h.stride<=4096 &&
        h.capacity>=2 && h.capacity<=8192 && !(h.capacity&(h.capacity-1)) && h.offset>=sizeof(Header) &&
        h.offset<=bytes && h.capacity<=(bytes-h.offset)/h.stride && h.position==(h.count&(h.capacity-1));
}
bool sameLayout(const Header& a,const Header& b) {
    return a.signature==b.signature && a.version==b.version && a.stride==b.stride && a.offset==b.offset && a.capacity==b.capacity;
}
struct RingSnapshot {
    Header header{};
    std::vector<PMDP_FRAME_DATA> first,rows;
};
bool snapshot(const unsigned char* view,std::size_t bytes,RingSnapshot& out) {
    if (!view || bytes<sizeof(Header)) return false;
    for (unsigned attempt=0;attempt!=6;++attempt) {
        Header before{},middle{},after{};
        std::memcpy(&before,view,sizeof(before));
        if (!valid(before,bytes)) return false;
        out.first.resize(before.capacity); out.rows.resize(before.capacity);
        for (DWORD n=0;n<before.capacity;++n) std::memcpy(&out.first[n],view+before.offset+n*before.stride,sizeof(PMDP_FRAME_DATA));
        MemoryBarrier(); std::memcpy(&middle,view,sizeof(middle));
        for (DWORD n=0;n<before.capacity;++n) std::memcpy(&out.rows[n],view+before.offset+n*before.stride,sizeof(PMDP_FRAME_DATA));
        MemoryBarrier(); std::memcpy(&after,view,sizeof(after));
        if (!std::memcmp(&before,&middle,sizeof(before)) && !std::memcmp(&before,&after,sizeof(before)) &&
            !std::memcmp(out.first.data(),out.rows.data(),out.rows.size()*sizeof(PMDP_FRAME_DATA))) {
            out.header=before; return true;
        }
    }
    return false;
}
struct Cursor {
    bool initialized{};
    DWORD previous{};
    std::uint64_t ordinal{},lost{},resets{};
    DWORD take(const Header& h) {
        const auto safeCapacity=h.capacity-1;
        if (!initialized) { initialized=true; previous=h.count; ordinal=h.count; return 0; }
        const DWORD delta=h.count-previous; previous=h.count;
        if (delta>0x7fffffff) { ++resets; ordinal=h.count; return 0; }
        ordinal+=delta;
        if (delta>safeCapacity) lost+=delta-safeCapacity;
        return std::min(delta,safeCapacity);
    }
};
struct Mapping {
    Handle handle;
    const unsigned char* view{};
    std::size_t bytes{};
    Mapping() : handle(OpenFileMappingW(FILE_MAP_READ,FALSE,L"PMDPSharedMemory")) {
        require(handle.value!=nullptr,"Existing PMDPSharedMemory is unavailable; helper does not start a provider");
        view=static_cast<const unsigned char*>(MapViewOfFile(handle.value,FILE_MAP_READ,0,0,0));
        require(view!=nullptr,"Read-only PMDP mapping failed");
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(view,&info,sizeof(info))!=sizeof(info) || info.State!=MEM_COMMIT || (info.Protect&(PAGE_GUARD|PAGE_NOACCESS))) {
            UnmapViewOfFile(view); view=nullptr; throw std::runtime_error("Invalid PMDP mapping region");
        }
        bytes=info.RegionSize;
    }
    ~Mapping() { if(view) UnmapViewOfFile(view); }
};
enum class Conversion { Displayed,NotDisplayed,Invalid };
Conversion convert(ds::DisplaySample& result,const PMDP_FRAME_DATA& row,std::uint64_t frequency) {
    const auto& s=row.data1;
    result={s.ProcessID,s.SwapChainAddress,static_cast<int>(s.Runtime),0,0,0,s.msBetweenDisplayChange,false};
    if (!s.ProcessID || !s.SwapChainAddress || !s.qpcTime || !frequency || !std::isfinite(s.msUntilDisplayed) ||
        !std::isfinite(s.msBetweenDisplayChange) || s.msUntilDisplayed<0 || s.msBetweenDisplayChange<0) return Conversion::Invalid;
    if (!s.msUntilDisplayed || !s.msBetweenDisplayChange) return Conversion::NotDisplayed;
    const auto ticks=[&](double ms,std::uint64_t& value) {
        const auto rounded=std::floor(static_cast<long double>(ms)*frequency/1000+0.5L);
        if (!std::isfinite(rounded) || rounded<0 || rounded>=std::numeric_limits<std::uint64_t>::max()) return false;
        value=static_cast<std::uint64_t>(rounded); return true;
    };
    std::uint64_t delay{},duration{};
    if (!ticks(s.msUntilDisplayed,delay) || !ticks(s.msBetweenDisplayChange,duration) || !duration ||
        delay>UINT64_MAX-s.qpcTime) return Conversion::Invalid;
    result.displayEndQpc=s.qpcTime+delay;
    if (duration>=result.displayEndQpc) return Conversion::Invalid;
    // V1 is the past interval ending here, unlike the PM-v2 forward duration.
    result.displayStartQpc=result.displayEndQpc-duration; result.displayTimeValid=true;
    return Conversion::Displayed;
}
struct Session {
    DWORD pid;
    std::uint64_t frequency,generation{},watermark{},lastArrival{},boundary{};
    bool active{};
    ds::DisplayStats stats;
    std::vector<ds::DisplaySample> pending;
    Session(DWORD p,std::uint64_t f) : pid(p),frequency(f) { require(f && f<=UINT64_MAX/60,"Invalid QPC frequency"); reset(0); }
    void reset(std::uint64_t now) {
        require(stats.begin(pid,++generation,frequency,5),"Invalid five-second session identity");
        watermark=lastArrival=0; boundary=active ? now : 0; pending.clear();
    }
    void foreground(bool value,std::uint64_t now) { if(value!=active) { active=value; reset(now); } }
    void consume(const std::vector<PMDP_FRAME_DATA>& rows,std::uint64_t now) {
        if(!active) return;
        pending.clear(); auto latest=watermark;
        for(const auto& row:rows) {
            if(row.data1.ProcessID!=pid || row.data1.Runtime!=ds::dxgiRuntime) continue;
            ds::DisplaySample sample;
            const auto converted=convert(sample,row,frequency);
            if(converted==Conversion::Invalid) { reset(now); return; }
            if(converted!=Conversion::Displayed || sample.displayStartQpc<boundary) continue;
            if(sample.displayEndQpc>now) { reset(now); return; }
            latest=std::max(latest,sample.displayEndQpc); pending.push_back(sample);
        }
        if(!pending.empty()) {
            stats.update(pending,latest);
            if(latest>watermark) lastArrival=now;
            watermark=latest;
        }
    }
    std::string text(std::uint64_t now,std::string_view failure={}) const {
        if(!active) failure="target inactive";
        else if(failure.empty() && lastArrival && now>=lastArrival && now-lastArrival>2*frequency) failure="source stale";
        std::string result;
        if(failure.empty()) result=ds::formatOsd(stats.snapshot(watermark));
        else result="Display FT(last): N/A\nDisplay 1% Low(Integral,5s): N/A ("+std::string(failure)+")";
        std::ostringstream age;
        if(watermark && now>=watermark) age<<std::fixed<<std::setprecision(2)<<"\nDisplay source age: "<<double(now-watermark)/frequency<<" s";
        return result+age.str();
    }
};
struct Stream {
    Cursor cursor;
    Header layout{};
    bool healthy{};
    std::uint64_t discarded{};
    std::vector<PMDP_FRAME_DATA> batch;
    void consume(const RingSnapshot& input,Session& session,std::uint64_t now) {
        const auto& h=input.header;
        if(h.status!=0) { if(healthy) session.reset(now); healthy=false; cursor={}; return; }
        if(!healthy || !sameLayout(layout,h)) { session.reset(now); cursor={}; layout=h; healthy=true; }
        const auto previousLoss=cursor.lost,previousReset=cursor.resets;
        const auto count=cursor.take(h);
        if(cursor.lost!=previousLoss || cursor.resets!=previousReset) { discarded+=count; session.reset(now); return; }
        batch.clear();
        const auto begin=(h.position-count)&(h.capacity-1);
        for(DWORD n=0;n<count;++n) batch.push_back(input.rows[(begin+n)&(h.capacity-1)]);
        session.consume(batch,now);
    }
};
template<class Clear,class Pause> ds::OsdResult clearOwned(Clear clear,Pause pause) {
    ds::OsdResult result=ds::OsdResult::NoSlot;
    for(unsigned attempt=0;attempt<5;++attempt) {
        result=clear(); if(result!=ds::OsdResult::Busy) break;
        if(attempt+1<5) pause();
    }
    return result;
}
struct OsdCleanup {
    std::unique_ptr<ds::OsdWriter>& writer;
    bool finalized{};
    ds::OsdResult result{ds::OsdResult::NoSlot};
    ds::OsdResult finish() {
        if(finalized) return result;
        finalized=true;
        if(writer) result=clearOwned([&]{return writer->clear();},[]{Sleep(50);});
        if(result!=ds::OsdResult::Updated && result!=ds::OsdResult::NoSlot)
            std::cerr<<"Owned OSD cleanup incomplete: "<<static_cast<int>(result)<<'\n';
        return result;
    }
    ~OsdCleanup() { finish(); }
};
int cleanupExitCode(ds::OsdResult result) { return result==ds::OsdResult::Updated || result==ds::OsdResult::NoSlot ? 0 : 2; }
std::atomic_bool stopRequested{};
std::atomic_bool cleanupDone{};
BOOL WINAPI interrupt(DWORD event) {
    if(event==CTRL_C_EVENT || event==CTRL_BREAK_EVENT || event==CTRL_CLOSE_EVENT || event==CTRL_LOGOFF_EVENT || event==CTRL_SHUTDOWN_EVENT) {
        stopRequested.store(true);
        // For close/logoff/shutdown Windows terminates after this handler
        // returns. Give the main thread time to clear its owner first.
        if(event!=CTRL_C_EVENT && event!=CTRL_BREAK_EVENT) {
            const auto deadline=GetTickCount64()+2000;
            while(!cleanupDone.load() && GetTickCount64()<deadline) Sleep(10);
        }
        return TRUE;
    }
    return FALSE;
}
int run(DWORD pid,std::uint64_t created,unsigned seconds,bool osd) {
    Target target(pid,created);
    Mapping mapping; RingSnapshot input;
    require(snapshot(mapping.view,mapping.bytes,input),"Existing PMDP layout/stable snapshot unavailable");
    LARGE_INTEGER frequency{}; require(QueryPerformanceFrequency(&frequency) && frequency.QuadPart>0,"QPC frequency unavailable");
    Session session(pid,frequency.QuadPart); Stream stream;
    stopRequested.store(false); cleanupDone.store(false);
    require(SetConsoleCtrlHandler(interrupt,TRUE),"Cannot install helper console cleanup handler");
    struct RemoveHandler { ~RemoveHandler(){SetConsoleCtrlHandler(interrupt,FALSE);} } handler;
    struct Complete { ~Complete(){cleanupDone.store(true);} } complete;
    std::unique_ptr<ds::OsdWriter> writer; OsdCleanup cleanup{writer};
    // Constructing an owner and opening RTSS for write occurs only in this
    // explicitly selected live/OSD mode, never help/self-test/replay/identity.
    if(osd) writer=std::make_unique<ds::OsdWriter>();
    const auto began=qpc(); auto nextPrint=began;
    std::uint64_t polls{},missed{};
    std::cout<<"Existing PMDP only; target="<<pid<<" created="<<created<<"; window=5s; OSD="<<(osd?"on":"off")<<"\n";
    while(!stopRequested.load() && target.alive()) {
        auto now=qpc(); if(now-began>=std::uint64_t(seconds)*session.frequency) break;
        session.foreground(foregroundPid()==pid,now);
        std::string_view failure;
        if(snapshot(mapping.view,mapping.bytes,input)) {
            now=qpc();
            stream.consume(input,session,now);
            if(input.header.status) failure="PMDP provider unavailable";
        } else { now=qpc(); ++missed; failure="PMDP snapshot unavailable"; }
        const auto text=session.text(now,failure);
        if(writer) {
            const auto result=writer->publish(text,pid);
            if(result!=ds::OsdResult::Updated && result!=ds::OsdResult::NoSlot && result!=ds::OsdResult::Busy)
                throw std::runtime_error("RTSS OSD unavailable or rejected; helper will clean its owner and exit");
        } else if(now>=nextPrint) { std::cout<<text<<"\n"; nextPrint=now+session.frequency; }
        ++polls; Sleep(ds::updateIntervalMs);
    }
    const auto cleared=cleanup.finish();
    writer.reset(); // Preserve adapter's best-effort destructor fallback.
    cleanupDone.store(true);
    std::cout<<"Helper ended; polls="<<polls<<" unstable="<<missed<<" streamResets="<<stream.cursor.resets<<" lost="<<stream.cursor.lost
        <<" cleanupStatus="<<static_cast<int>(cleared)<<"\n";
    return cleanupExitCode(cleared);
}

// CPU-only fixtures and sealed CSV replay share the actual session/stream code.
unsigned checks{};
void check(bool value,const char* message) { ++checks; require(value,message); }
PMDP_FRAME_DATA frame(std::uint64_t end,double ms=10,DWORD pid=123,std::uint64_t chain=10) {
    PMDP_FRAME_DATA f{}; f.data1.ProcessID=pid; f.data1.SwapChainAddress=chain; f.data1.Runtime=1;
    f.data1.qpcTime=end-20000; f.data1.msUntilDisplayed=2; f.data1.msBetweenDisplayChange=ms; return f;
}
void selfTest() {
    ds::DisplayStats invalid;
    check(!invalid.begin(123,1,10000000,0) && !invalid.begin(123,1,10000000,61) && !invalid.begin(123,1,UINT64_MAX,5),"invalid window/overflow refused");
    check(identityMatches(L"D:\\game\\Endfield.exe",5,5) && !identityMatches(L"Endfield.exe",5,6) &&
          !identityMatches(L"Other.exe",5,5) && !identityMatches(L"Endfield.exe",0,0),"target identity guard");
    auto memory=std::make_unique<PMDP_SHARED_MEMORY>();
    Header h{0x504d4450,0x00020000,sizeof(PMDP_FRAME_DATA),offsetof(PMDP_SHARED_MEMORY,arrFrame),8,12,4,0};
    std::memcpy(memory.get(),&h,sizeof(h)); memory->arrFrame[3]=frame(1000000);
    RingSnapshot snap;
    check(snapshot(reinterpret_cast<unsigned char*>(memory.get()),sizeof(*memory),snap) && snap.rows[3].data1.ProcessID==123,"owned stable SDK layout");
    for(unsigned which=0;which<5;++which) {
        auto bad=h;
        if(which==0) bad.version=1; if(which==1) bad.stride=1; if(which==2) bad.capacity=3;
        if(which==3) bad.offset=sizeof(*memory); if(which==4) bad.position=5;
        check(!valid(bad,sizeof(*memory)),"invalid ring layout refused");
    }
    Cursor c; check(c.take(h)==0 && c.take(h)==0,"initial history and duplicate snapshot excluded");
    h.count=15;h.position=7;check(c.take(h)==3 && c.ordinal==15,"incremental provider ordinal");
    h.count=23;check(c.take(h)==7 && c.lost==1,"full ring delta omits in-flight slot");
    h.count=40;h.position=0;check(c.take(h)==7 && c.lost==11,"overwritten unread rows counted");
    h.count=2;h.position=2;check(c.take(h)==0 && c.resets==1,"reset batch discarded");
    Cursor wrap;h.count=0xfffffffe;h.position=6;wrap.take(h);h.count=2;h.position=2;
    check(wrap.take(h)==4 && !wrap.resets && wrap.ordinal==0x100000002ull,"DWORD wrap retained");
    ds::DisplaySample sample;auto f=frame(1000000);
    check(convert(sample,f,10000000)==Conversion::Displayed && sample.displayStartQpc==900000 && !sample.frameType,"V1 past interval; no FrameType guess");
    f.data1.msUntilDisplayed=0;check(convert(sample,f,10000000)==Conversion::NotDisplayed,"unshown marker skipped");
    f.data1.msUntilDisplayed=-1;check(convert(sample,f,10000000)==Conversion::Invalid,"invalid marker refused");
    f=frame(1000000);f.data1.qpcTime=UINT64_MAX;check(convert(sample,f,10000000)==Conversion::Invalid,"QPC overflow refused");
    Session s(123,10000000);const std::uint64_t base=1000000000,lag=15000000; s.foreground(true,base);
    std::uint64_t end=base;
    for(unsigned i=0;i<500;++i) {end+=100000;s.consume({frame(end)},end+lag);}
    check(s.stats.snapshot(s.watermark).state==ds::State::Ready && s.stats.snapshot(s.watermark).intervals==500,"five-second source watermark window despite 1.5s reporting lag");
    check(s.text(end+lag).find("5s")!=std::string::npos && s.text(end+lag).find("1.50 s")!=std::string::npos,"explicit 5s and data age");
    s.consume({frame(end)},end+lag+1000000);
    check(s.lastArrival==end+lag,"duplicate display event cannot refresh freshness");
    check(s.text(end+lag+20000001).find("source stale")!=std::string::npos,"arrival timeout gives N/A");
    end+=200000000;s.consume({frame(end,20000)},end+lag);
    check(s.stats.snapshot(s.watermark).maximumMs==20000 && s.stats.snapshot(s.watermark).integralOnePercentLow==0.05,"completed long freeze remains whole");
    s.consume({frame(end,10,999)},end+lag+1000000);
    check(s.lastArrival==end+lag,"foreign PID cannot refresh freshness");
    s.consume({frame(end,10,123,20)},end+lag);
    check(s.stats.snapshot(s.watermark).state==ds::State::Ambiguous,"multiple displayed DXGI chains give N/A");
    s.foreground(false,end+lag);check(s.text(end+lag).find("target inactive")!=std::string::npos,"background hides current metrics");
    s.foreground(true,end+lag+1000000);s.consume({frame(end)},end+lag+1000000);
    check(!s.watermark,"foreground restore excludes prior display intervals");
    Stream st;Session fresh(123,10000000);fresh.foreground(true,base);RingSnapshot input;input.rows.resize(8);
    input.header={0x504d4450,0x00020000,sizeof(PMDP_FRAME_DATA),offsetof(PMDP_SHARED_MEMORY,arrFrame),8,6,6,0};
    st.consume(input,fresh,base);check(!fresh.watermark,"stream first snapshot excludes history");
    input.header.count=7;input.header.position=7;input.rows[6]=frame(base+100000);
    st.consume(input,fresh,base+lag+100000);check(fresh.watermark==base+100000,"stream consumes only new ordinal");
    input.header.status=1;st.consume(input,fresh,base+lag+200000);check(!fresh.watermark && !st.healthy,"provider failure resets stats");
    input.header.status=0;st.consume(input,fresh,base+lag+300000);check(!fresh.watermark,"provider recovery starts new-only baseline");
    input.header.count=30;input.header.position=6;st.consume(input,fresh,base+lag+400000);check(!fresh.watermark && st.cursor.lost,"loss batch never warms stats");
    input.header.count=2;input.header.position=2;st.consume(input,fresh,base+lag+500000);check(!fresh.watermark && st.cursor.resets==1,"provider reset never imports retained history");
    input.header.capacity=16;input.rows.resize(16);st.consume(input,fresh,base+lag+600000);
    check(!fresh.watermark && st.cursor.initialized && st.cursor.previous==2,"layout change takes a new baseline");
    Session sharedOrigin(123,10000000);sharedOrigin.foreground(true,base);
    auto a=frame(base+100000),b=frame(base+200000);b.data1.qpcTime=a.data1.qpcTime;b.data1.msUntilDisplayed=12;
    sharedOrigin.consume({a,b},base+lag+200000);
    check(sharedOrigin.pending.size()==2 && sharedOrigin.watermark==base+200000,"distinct display events sharing a CPU origin are retained");
    auto hidden=frame(base+300000);hidden.data1.msUntilDisplayed=0;
    sharedOrigin.consume({hidden},base+lag+400000);
    check(sharedOrigin.lastArrival==base+lag+200000,"unshown records cannot refresh freshness");
    Session agesOut(123,10000000);agesOut.foreground(true,base);auto current=base;
    for(unsigned i=0;i<500;++i){current+=100000;agesOut.consume({frame(current)},current+lag);}
    current+=10000000;agesOut.consume({frame(current,1000)},current+lag);
    for(unsigned i=0;i<499;++i){current+=100000;agesOut.consume({frame(current)},current+lag);}
    check(agesOut.stats.snapshot(agesOut.watermark).maximumMs==1000,"stall survives through 4.99 source seconds");
    current+=100000;agesOut.consume({frame(current)},current+lag);
    check(agesOut.stats.snapshot(agesOut.watermark).maximumMs==10,"stall rolls out at exactly five source seconds");
    unsigned calls{},pauses{};
    check(clearOwned([&]{return ++calls<3?ds::OsdResult::Busy:ds::OsdResult::Updated;},[&]{++pauses;})==ds::OsdResult::Updated && calls==3 && pauses==2,"owned cleanup retries only Busy");
    calls=pauses=0;
    check(clearOwned([&]{++calls;return ds::OsdResult::Busy;},[&]{++pauses;})==ds::OsdResult::Busy && calls==5 && pauses==4,"cleanup retry bound");
    calls=0;check(clearOwned([&]{++calls;return ds::OsdResult::Unavailable;},[]{})==ds::OsdResult::Unavailable && calls==1,"unavailable cleanup does not spin");
    check(cleanupExitCode(ds::OsdResult::Updated)==0 && cleanupExitCode(ds::OsdResult::NoSlot)==0 && cleanupExitCode(ds::OsdResult::Busy)==2 && cleanupExitCode(ds::OsdResult::Unavailable)==2,"incomplete cleanup cannot report successful exit");
    std::cout<<"{\"stage\":\"pmdp-helper-cpu\",\"passed\":true,\"checks\":"<<checks<<",\"liveMappingOpened\":false,\"osdOwnerCreated\":false}\n";
}
std::vector<std::string> csvFields(const std::string& text) {
    std::vector<std::string> result;std::string field;bool quoted{};
    for(char c:text) {if(c=='"') quoted=!quoted;else if(c==','&&!quoted){result.push_back(field);field.clear();}else field+=c;}
    if(!field.empty()&&field.back()=='\r')field.pop_back();result.push_back(field);require(!quoted,"Malformed CSV quotes");return result;
}
void replay(const char* path,std::uint64_t frequency) {
    std::ifstream file(path);require(bool(file),"Replay CSV unavailable");std::string line;std::getline(file,line);const auto header=csvFields(line);
    const auto col=[&](const char* key){const auto found=std::find(header.begin(),header.end(),key);require(found!=header.end(),"Missing PMDP CSV column");return static_cast<std::size_t>(found-header.begin());};
    const auto pid=col("ProcessID"),chain=col("SwapChainAddress"),runtime=col("Runtime"),q=col("qpcTime"),until=col("msUntilDisplayed"),ms=col("msBetweenDisplayChange"),
        read=col("ReadQPC"),ordinal=col("ProviderOrdinal"),epoch=col("StreamEpoch"),initial=col("InitialSnapshot");
    std::unique_ptr<Session> session;std::uint64_t lastOrdinal{},firstEpoch{},lastRead{},rows{};
    std::vector<ds::DisplaySample> reference;
    while(std::getline(file,line)) {
        const auto fields=csvFields(line);require(fields.size()==header.size(),"Malformed PMDP CSV row");
        const auto rowPid=number(fields[pid]),rowRuntime=number(fields[runtime]);
        require(rowPid && rowPid<=MAXDWORD && rowRuntime<=MAXDWORD,"Replay PID/runtime out of range");
        PMDP_FRAME_DATA row{};row.data1.ProcessID=static_cast<DWORD>(rowPid);row.data1.SwapChainAddress=number(fields[chain]);row.data1.Runtime=static_cast<DWORD>(rowRuntime);
        row.data1.qpcTime=number(fields[q]);row.data1.msUntilDisplayed=std::stod(fields[until]);row.data1.msBetweenDisplayChange=std::stod(fields[ms]);
        const auto currentEpoch=number(fields[epoch]),currentOrdinal=number(fields[ordinal]);lastRead=number(fields[read]);
        require(!number(fields[initial]),"Replay requires new-only PMDP CSV");
        if(!session){session=std::make_unique<Session>(row.data1.ProcessID,frequency);session->foreground(true,0);firstEpoch=currentEpoch;}
        require(row.data1.ProcessID==session->pid && currentEpoch==firstEpoch && (!lastOrdinal || currentOrdinal==lastOrdinal+1),"Replay identity/epoch/sequence changed");lastOrdinal=currentOrdinal;
        session->consume({row},lastRead);++rows;
        ds::DisplaySample sample;if(convert(sample,row,frequency)==Conversion::Displayed)reference.push_back(sample);
    }
    require(session && !reference.empty(),"No replay display data");const auto result=session->stats.snapshot(session->watermark);require(result.state==ds::State::Ready,"Replay never warmed to Ready");
    std::vector<double> values;long double total{};
    for(const auto& s:reference)if(s.swapchain==result.swapchain&&s.runtime==1&&s.displayEndQpc<=session->watermark&&session->watermark-s.displayEndQpc<5*frequency){values.push_back(s.displayedMs);total+=s.displayedMs;}
    std::sort(values.begin(),values.end(),std::greater<double>{});long double sum{};double expected{};
    for(double value:values){if(sum+value>total/100){expected=1000/value;break;}sum+=value;}
    require(std::abs(expected-result.integralOnePercentLow)<1e-9 && values.size()==result.intervals,"Offline reference mismatch");
    std::cout<<std::setprecision(12)<<"{\"stage\":\"pmdp-helper-replay\",\"ready\":true,\"rows\":"<<rows<<",\"windowSeconds\":5,\"intervals\":"<<result.intervals
        <<",\"integral1Low5\":"<<result.integralOnePercentLow<<",\"maximumMs5\":"<<result.maximumMs<<",\"sourceAgeSeconds\":"<<double(lastRead-session->watermark)/frequency
        <<",\"liveMappingOpened\":false,\"osdOwnerCreated\":false}\n";
}
void usage(){std::cout<<"EndfieldDisplayHelper --self-test\n  --replay FILE --frequency QPC_HZ\n  --identity PID\n  --run --target-pid PID --created FILETIME [--seconds 60] [--osd]\nNo ETW/provider startup; --osd is explicit. Live modes were not run during preparation.\n";}
}
int main(int argc,char** argv) {
    try {
        if(argc==1 || (argc==2 && std::string_view(argv[1])=="--help")){usage();return 0;}
        if(argc==2 && std::string_view(argv[1])=="--self-test"){selfTest();return 0;}
        if(argc==5 && std::string_view(argv[1])=="--replay" && std::string_view(argv[3])=="--frequency"){replay(argv[2],number(argv[4]));return 0;}
        if(argc==3 && std::string_view(argv[1])=="--identity") {
            const auto pid=number(argv[2]);require(pid&&pid<=MAXDWORD,"Invalid PID");Target target(static_cast<DWORD>(pid));
            std::cout<<"targetPid="<<pid<<" created="<<target.created<<"\n";return 0;
        }
        require(argc>=6 && std::string_view(argv[1])=="--run","Use --help for supported modes");
        std::uint64_t pid{},created{};unsigned seconds=60;bool osd{},seenPid{},seenCreated{},seenSeconds{};
        for(int i=2;i<argc;++i) {
            const std::string_view flag=argv[i];if(flag=="--osd"){require(!osd,"Duplicate --osd");osd=true;continue;}
            require(i+1<argc,"Missing option value");const auto value=number(argv[++i]);
            if(flag=="--target-pid"){require(!seenPid,"Duplicate target PID");seenPid=true;pid=value;}
            else if(flag=="--created"){require(!seenCreated,"Duplicate creation time");seenCreated=true;created=value;}
            else if(flag=="--seconds"){require(!seenSeconds,"Duplicate seconds");seenSeconds=true;require(value>=1&&value<=3600,"Seconds must be 1..3600");seconds=static_cast<unsigned>(value);}
            else throw std::runtime_error("Unknown live option");
        }
        require(pid && pid<=MAXDWORD && created,"Live mode requires PID and exact nonzero creation FILETIME");
        return run(static_cast<DWORD>(pid),created,seconds,osd);
    } catch(const std::exception& error){std::cerr<<"PMDP_HELPER_ERROR: "<<error.what()<<'\n';return 1;}
}
