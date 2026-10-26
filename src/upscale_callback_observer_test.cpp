#include "upscale_callback_observer.hpp"
#include <windows.h>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
namespace observer = xess_vk::upscale_callback_observer;
namespace probe = xess_vk::upscale_probe;
std::atomic<unsigned long long> checks{};
unsigned cases{};
std::mutex logMutex;
std::vector<std::string> logs, allLogs;
struct Delivery { std::uint64_t call, image; bool detail; };
std::vector<Delivery> deliveries;
std::atomic<unsigned> sinkErrors{}, recursiveCalls{}, metadataCount{}, detailMetadataCount{};
std::atomic<bool> recursive{}, throwingLog{}, throwingMetadata{}, stopInLog{};
std::atomic<std::uint64_t> expectedImage{0xa00}, expectedDevice{0x777};
std::atomic<unsigned> invocationCalls{}, invalidInvocations{};
void* recursionPayload{};
void* recursionContext{};
void require(bool okay, const char* message) { ++checks; if (!okay) throw std::runtime_error(message); }
std::uintptr_t address(const void* value) { return reinterpret_cast<std::uintptr_t>(value); }
void u32(void* where, std::uint32_t value) { std::memcpy(where, &value, sizeof(value)); }
void u64(void* where, std::uint64_t value) { std::memcpy(where, &value, sizeof(value)); }

void recurse() {
    if (!recursive.load()) return;
    ++recursiveCalls;
    // Calling both paths exercises their shared per-thread recursion guard.
    upscale_observe_before(recursionPayload, recursionContext);
    observer::summary();
}
void logSink(const char* line) {
    SetLastError(0xd00d);
    if (throwingLog.load()) throw std::runtime_error("synthetic log exception");
    recurse();
    const std::string value(line);
    if (value.size() >= 30000 || value.front() != '{' || value.back() != '}') ++sinkErrors;
    {
        std::lock_guard lock(logMutex);
        logs.emplace_back(value); allLogs.emplace_back(value);
    }
    if (stopInLog.load() && value.find("upscale_source_snapshot") != std::string::npos) observer::stopSampling();
}
void metadataSink(std::uint64_t device, std::uint64_t call, const probe::Result& result, bool detail) {
    SetLastError(0xf00d);
    ++metadataCount; if (detail) ++detailMetadataCount;
    if (throwingMetadata.load()) throw std::runtime_error("synthetic source exception");
    if (device != expectedDevice.load() || !call || result.status != probe::Status::SnapshotParsed ||
        !result.snapshotCaptured || !result.allPropertiesPresent || !result.descriptorsUnchanged ||
        !result.graphEntriesUnchanged || !result.bindingMetadataUnchanged || !result.contextTableUnchanged || result.allocationBoundsVerified ||
        result.graphTextureIdsRead != 8 || result.nativeCandidatesRead != 8 || result.readCalls != 100 ||
        result.bytesRequested != 1360 || result.bytesCopied != 1360 || result.snapshotAtomic || result.lifetimePinned ||
        result.pixelsRead || result.gameFunctionsCalled || result.gpuCompletionVerified || result.copyPermitted ||
        result.undilatedVerified || result.motionSemanticsVerified || result.sameFrameVerified || result.xeFGIntegrated) ++sinkErrors;
    for (std::size_t i = 0; i < result.bindings.size(); ++i) {
        const auto& b = result.bindings[i];
        if (b.property != static_cast<probe::Property>(i + 1) || b.graphKey != i + 1 || b.textureId != i + 1 ||
            b.rawDependencyDirection != (i < 3 ? 0 : 1) || b.native.imageCandidate != expectedImage.load() + i ||
            b.native.widthCandidate != 1128 || b.native.heightCandidate != 634 ||
            b.native.idDomainVerified || b.native.vulkanImageVerified) ++sinkErrors;
    }
    {
        std::lock_guard lock(logMutex);
        deliveries.push_back({call, result.bindings[0].native.imageCandidate, detail});
    }
    recurse();
}
struct Fixture {
    std::array<unsigned char, 0x140> payload{};
    std::array<std::uint64_t, 4> context{};
    std::array<std::array<unsigned char, 16>, 9> graph{};
    std::array<std::uint32_t, 8> resources{1,2,3,4,5,6,7,8};
    std::array<std::uint64_t, 1024> table{}, page{};
    std::array<std::array<unsigned char, 64>, 8> objects{};
    std::uintptr_t module{};
    static constexpr std::array<std::uint64_t, 8> properties{
        0x1EDE538,0x1EDE53C,0x1EDE540,0x1EDE594,0x1EDE598,0x1EDE59C,0x1EDE5A0,0x1EDE5A4};
    Fixture() {
        module = address(table.data()) - probe::nativeTableRva;
        table[0] = address(page.data());
        context[2] = address(graph.data()); context[3] = context[2] + sizeof(graph);
        for (std::size_t i = 0; i < 8; ++i) {
            u64(payload.data()+0x28+i*0x20, 0xCAFE00000000ull | (i+1));
            payload[0x30+i*0x20] = i<3 ? 0 : 1;
            u64(payload.data()+0x38+i*0x20, module+properties[i]);
            u32(payload.data()+0x40+i*0x20, static_cast<std::uint32_t>(i));
            u32(graph[i+1].data()+12, 2);
            u64(graph[i+1].data(), address(&resources[i]));
            page[i+1] = address(objects[i].data());
            u32(objects[i].data()+0x14, 1128); u32(objects[i].data()+0x18, 634);
        }
        images(0xa00);
    }
    void images(std::uint64_t base) {
        for (std::size_t i=0;i<8;++i) u64(objects[i].data()+0x38, base+i);
    }
    void call() { upscale_observe_before(payload.data(), context.data()); }
};
void reset(Fixture& f, bool activate = true, observer::SourceMetadataSink metadata = metadataSink,
           probe::Limits limits = observer::defaultLimits, observer::InvocationSink invocation = nullptr) {
    observer::resetForTest();
    { std::lock_guard lock(logMutex); logs.clear(); deliveries.clear(); }
    sinkErrors=0; recursiveCalls=0; metadataCount=0; detailMetadataCount=0;
    recursive=false; throwingLog=false; throwingMetadata=false; stopInLog=false;
    expectedImage=0xa00; expectedDevice=0x777; recursionPayload=f.payload.data(); recursionContext=f.context.data();
    require(observer::configureVerifiedModule(f.module, 0x777, logSink, metadata, limits,invocation), "configuration rejected");
    if (activate) require(observer::enableSampling(), "activation rejected");
}
std::string latestSummary() {
    observer::summary();
    std::lock_guard lock(logMutex);
    require(!logs.empty() && logs.back().find("upscale_source_summary") != std::string::npos, "summary missing");
    return logs.back();
}
std::uint64_t number(const std::string& json, const char* key) {
    const std::string token = std::string("\"") + key + "\":";
    const auto found=json.find(token); require(found != std::string::npos, "numeric summary field missing");
    return std::stoull(json.substr(found + token.size()));
}
void rejectedSnapshot(const char* expectedStatus) {
    require(metadataCount == 0 && sinkErrors == 0, "failed snapshot reached typed consumer");
    std::lock_guard lock(logMutex);
    require(logs.size()==1 && logs.back().find(expectedStatus)!=std::string::npos, "wrong rejected detail");
    require(logs.back().find("\"bindings\":[]")!=std::string::npos &&
            logs.back().find("\"snapshot_captured\":false")!=std::string::npos, "partial detail bindings published");
}
void gatesAndErrorState() {
    ++cases; Fixture f; reset(f, false);
    SetLastError(0x11223344); f.call(); require(GetLastError()==0x11223344 && logs.empty(), "configured observer ran");
    require(observer::enableSampling() && GetLastError()==0x11223344, "enable changed error");
    require(!observer::enableSampling(), "second enable succeeded");
    recursive=true; f.call(); require(GetLastError()==0x11223344, "observer changed error");
    require(logs.size()==1 && metadataCount==1 && recursiveCalls==2 && sinkErrors==0, "valid RPM or recursion failed");
    auto sum=latestSummary(); require(GetLastError()==0x11223344, "summary changed error");
    require(number(sum,"callback_calls")==1 && number(sum,"probes")==1 && number(sum,"read_calls")==100 &&
            number(sum,"bytes_requested")==1360, "bounded RPM accounting");
    require(sum.find("\"shared_fi_roles_selected\":false")!=std::string::npos, "shared role claim");
    require(!observer::configureVerifiedModule(f.module, 0x888, logSink, metadataSink), "configuration replaced while active");
    f.call(); require(sinkErrors==0 && metadataCount==2, "reconfiguration changed device");
    observer::stopSampling(); require(GetLastError()==0x11223344, "stop changed error");
    f.call(); require(metadataCount==2 && !observer::enableSampling(), "stop not sticky");
    require(!observer::configureVerifiedModule(f.module, 0x777, logSink, metadataSink), "stop permitted reconfiguration");
    sum=latestSummary(); require(sum.find("\"gate\":\"stopped\"")!=std::string::npos, "stopped summary unavailable");

    ++cases; observer::resetForTest(); observer::stopSampling();
    require(!observer::configureVerifiedModule(f.module,0x777,logSink,metadataSink) && !observer::enableSampling(), "cold stop revived");
    ++cases; observer::resetForTest();
    require(!observer::configureVerifiedModule(0,0x777,logSink) && !observer::configureVerifiedModule(f.module,0,logSink) &&
            !observer::configureVerifiedModule(f.module,0x777,nullptr) &&
            !observer::configureVerifiedModule(UINTPTR_MAX-1,0x777,logSink), "invalid identity accepted");
    for (unsigned i=0;i<6;++i) {
        auto limits=observer::defaultLimits;
        if(i<2)limits.maxGraphEntries=i?8193:0;
        else if(i<4)limits.maxBytes=i==3?262145:0;
        else limits.maxReadCalls=i==5?129:0;
        require(!observer::configureVerifiedModule(f.module,0x777,logSink,nullptr,limits), "invalid observer budget accepted");
    }
    require(observer::configureVerifiedModule(f.module,0x777,logSink), "rejected validation consumed gate");
}
void rejectionsAndBudgets() {
    for(unsigned slot=0;slot<8;++slot) for(unsigned type=0;type<3;++type) {
        ++cases; Fixture f; reset(f);
        const auto invalid=type==0?0:(type==1?f.module+0x1D10784:f.module+Fixture::properties[(slot+1)%8]);
        u64(f.payload.data()+0x38+slot*0x20,invalid);
        SetLastError(0x5678);f.call();require(GetLastError()==0x5678,"property rejection changed error");
        rejectedSnapshot(type==2?"DuplicateProperty":"UnknownProperty");
    }
    for(unsigned slot=0;slot<8;++slot) {
        ++cases;Fixture f;reset(f);f.page[slot+1]=0;f.call();rejectedSnapshot("NullNativeObject");
        ++cases;Fixture missing;reset(missing);u64(missing.payload.data()+0x28+slot*0x20,8192);missing.call();rejectedSnapshot("GraphEntryLimitExceeded");
    }
    // Lower limits are testable production configuration, never parser bypasses.
    for(std::size_t calls=1;calls<100;++calls) {
        ++cases;Fixture f;auto limits=observer::defaultLimits;limits.maxReadCalls=calls;reset(f,true,metadataSink,limits);
        f.call();rejectedSnapshot("ReadCallBudgetExceeded");
        auto sum=latestSummary();require(number(sum,"read_calls")<=calls && number(sum,"bytes_requested")<=limits.maxBytes,"read budget exceeded");
    }
    for(const auto bytes:std::array<std::size_t,8>{1,255,256,271,272,527,1000,1359}) {
        ++cases;Fixture f;auto limits=observer::defaultLimits;limits.maxBytes=bytes;reset(f,true,metadataSink,limits);
        f.call();rejectedSnapshot("ByteBudgetExceeded");
        auto sum=latestSummary();require(number(sum,"bytes_requested")<=bytes,"byte budget exceeded");
    }
    ++cases;Fixture f;auto limits=observer::defaultLimits;limits.maxGraphEntries=7;reset(f,true,metadataSink,limits);
    f.call();rejectedSnapshot("GraphEntryLimitExceeded");
}
struct Allocation {
    void* memory=VirtualAlloc(nullptr,8192,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    Allocation() {require(memory!=nullptr,"virtual fixture allocation");DWORD old{};require(VirtualProtect(static_cast<unsigned char*>(memory)+4096,4096,PAGE_NOACCESS,&old),"noaccess protection");}
    ~Allocation(){if(memory)VirtualFree(memory,0,MEM_RELEASE);}
    void* noaccess() const{return static_cast<unsigned char*>(memory)+4096;}
};
void inaccessibleReads() {
    Allocation allocation;
    for(unsigned target=0;target<6;++target) {
        ++cases;Fixture f;reset(f);SetLastError(0xabcd);
        if(target==0) upscale_observe_before(allocation.noaccess(),f.context.data());
        if(target==1) upscale_observe_before(f.payload.data(),allocation.noaccess());
        if(target==2) {f.context[2]=address(allocation.noaccess());f.context[3]=f.context[2]+256;f.call();}
        if(target==3) {u64(f.graph[8].data(),address(allocation.noaccess()));f.call();}
        if(target==4) {f.table[0]=address(allocation.noaccess());f.call();}
        if(target==5) {f.page[8]=address(allocation.noaccess());f.call();}
        require(GetLastError()==0xabcd,"NOACCESS failure changed error");rejectedSnapshot("Unreadable");
    }
    ++cases;Fixture f;reset(f);
    auto* crossing=static_cast<unsigned char*>(allocation.memory)+4096-0x100;
    std::memcpy(crossing,f.payload.data(),0x100);
    upscale_observe_before(crossing,f.context.data());rejectedSnapshot("Unreadable");
    ++cases;reset(f);upscale_observe_before(reinterpret_cast<void*>(UINTPTR_MAX-16),f.context.data());rejectedSnapshot("AddressOverflow");
}
void quotaAndRefresh() {
    ++cases;Fixture f;reset(f);
    for(unsigned i=0;i<32;++i) f.call();
    require(logs.size()==32 && metadataCount==32 && detailMetadataCount==32,"detail quota failed");
    f.images(0xb00);expectedImage=0xb00;
    for(unsigned i=33;i<=119;++i) f.call();
    require(logs.size()==32 && metadataCount==32,"unsampled calls read or delivered");
    SetLastError(0x9988);f.call();require(GetLastError()==0x9988,"refresh changed error");
    require(logs.size()==32 && metadataCount==33 && detailMetadataCount==32 && sinkErrors==0,"exhausted quota did not refresh changed image");
    require(deliveries.back().call==120 && deliveries.back().image==0xb00 && !deliveries.back().detail,"wrong refresh association");
    u32(f.graph[1].data()+12,0);
    for(unsigned i=121;i<=240;++i) f.call();
    require(metadataCount==33 && logs.size()==32,"failed refresh delivered partial metadata or detail");
    u32(f.graph[1].data()+12,2);f.images(0xc00);expectedImage=0xc00;
    for(unsigned i=241;i<=360;++i) f.call();
    require(metadataCount==34 && deliveries.back().image==0xc00 && !deliveries.back().detail && sinkErrors==0,"refresh did not recover");
    const auto sum=latestSummary();
    require(number(sum,"callback_calls")==360 && number(sum,"detail_attempts")==32 && number(sum,"refresh_attempts")==3 &&
            number(sum,"refresh_parsed")==2 && number(sum,"parsed")==34 && number(sum,"rejected")==1 &&
            number(sum,"detail_emitted")==32,"refresh counter mismatch");
    ++cases;Fixture noConsumer;reset(noConsumer,true,nullptr);
    for(unsigned i=0;i<20000;++i)noConsumer.call();
    const auto noConsumerSum=latestSummary();
    require(number(noConsumerSum,"probes")==32 && number(noConsumerSum,"refresh_attempts")==0 && metadataCount==0,"unused refresh performed reads");
}
void exceptionsAndRecursion() {
    ++cases;Fixture f;reset(f);throwingLog=true;
    SetLastError(0x12ab);f.call();require(GetLastError()==0x12ab && metadataCount==1,"log exception escaped or prevented metadata");
    throwingLog=false;auto sum=latestSummary();require(number(sum,"log_failures")==1,"log exception uncounted");
    ++cases;reset(f);throwingMetadata=true;f.call();require(GetLastError()==0x12ab,"typed exception changed error");
    throwingMetadata=false;f.call();sum=latestSummary();
    require(number(sum,"metadata_failures")==1 && number(sum,"metadata_delivered")==1 && number(sum,"metadata_attempts")==2,"typed exception damaged future sampling");
    ++cases;reset(f);observer::setAfterProbeForTest(+[]{SetLastError(0xbeef);throw std::runtime_error("test scheduling exception");});
    f.call();require(GetLastError()==0x12ab && metadataCount==0 && logs.empty(),"probe exception escaped or published");
    observer::setAfterProbeForTest(nullptr);f.call();sum=latestSummary();require(number(sum,"exceptions")==1 && metadataCount==1,"exception did not restore recursion guard");
    ++cases;reset(f);recursive=true;
    for(unsigned i=0;i<240;++i) f.call();
    sum=latestSummary();require(number(sum,"callback_calls")==240 && number(sum,"probes")==34 && sinkErrors==0,"recursive callbacks consumed quotas");
}
std::mutex pauseMutex;
std::condition_variable pauseCv;
bool paused{}, releasePause{};
void pauseAfterProbe() {
    std::unique_lock lock(pauseMutex);paused=true;pauseCv.notify_all();
    pauseCv.wait(lock,[]{return releasePause;});
}
void stopRaces() {
    ++cases;Fixture f;reset(f);
    {std::lock_guard lock(pauseMutex);paused=false;releasePause=false;}
    observer::setAfterProbeForTest(pauseAfterProbe);
    std::atomic<bool> workerError{};
    std::thread worker([&]{SetLastError(0x314159);f.call();workerError=GetLastError()!=0x314159;});
    bool reached{};
    {std::unique_lock lock(pauseMutex);reached=pauseCv.wait_for(lock,std::chrono::seconds(10),[]{return paused;});}
    observer::stopSampling();
    {std::lock_guard lock(pauseMutex);releasePause=true;}pauseCv.notify_all();worker.join();
    require(reached && !workerError,"stop-race fixture failed");
    observer::setAfterProbeForTest(nullptr);
    require(logs.empty() && metadataCount==0 && !observer::enableSampling(),"post-stop parsed result delivered");
    auto sum=latestSummary();require(number(sum,"stopped_results")==1 && number(sum,"probes")==1,"stopped result uncounted");
    ++cases;reset(f);stopInLog=true;f.call();
    require(logs.size()==1 && metadataCount==0,"stop inside log did not suppress typed delivery");
    sum=latestSummary();require(number(sum,"stopped_results")==1,"stop between sinks uncounted");
    // A configuration racing with an early stop must never revive the gate.
    for(unsigned round=0;round<64;++round) {
        ++cases;observer::resetForTest();std::atomic<bool> go{};
        std::thread configure([&]{while(!go.load())std::this_thread::yield();observer::configureVerifiedModule(f.module,0x777,logSink,metadataSink);});
        std::thread stop([&]{while(!go.load())std::this_thread::yield();observer::stopSampling();});
        go=true;configure.join();stop.join();
        require(!observer::enableSampling() && !observer::configureVerifiedModule(f.module,0x777,logSink),"configure/stop race revived gate");
    }
}
void concurrentCallbacks() {
    ++cases;Fixture f;reset(f);recursive=true;
    std::array<std::thread,8> threads;
    std::atomic<unsigned> errorMismatches{};
    for(unsigned index=0;index<threads.size();++index) threads[index]=std::thread([&,index]{
        for(unsigned i=0;i<3000;++i){SetLastError(100+index);f.call();if(GetLastError()!=100+index)++errorMismatches;}
    });
    for(auto& thread:threads)thread.join();
    require(errorMismatches==0 && sinkErrors==0,"concurrent metadata/LastError corruption");
    require(logs.size()==32 && metadataCount==232 && detailMetadataCount==32,"concurrent quota exceeded/lost");
    std::set<std::uint64_t> calls;
    for(const auto& delivery:deliveries) {
        require(calls.insert(delivery.call).second,"duplicate callback sample");
        require(delivery.detail ? delivery.call<=32 : delivery.call%120==0,"wrong concurrent sample schedule");
    }
    const auto sum=latestSummary();
    require(number(sum,"callback_calls")==24000 && number(sum,"probes")==232 && number(sum,"refresh_attempts")==200 &&
            number(sum,"read_calls")==232*100 && number(sum,"bytes_requested")==232*1360,"concurrent read accounting");
    const auto cpu = observer::observeCpuTime();
    require(cpu.count==24000 && cpu.maxNs<=cpu.totalNs && sum.find("\"cpu_upscale_observe\":{")!=std::string::npos,
            "concurrent admitted callbacks aggregate one own-work sample each, including unsampled early returns");
    ++cases;observer::stopSampling();const auto before=metadataCount.load();
    for(auto& thread:threads)thread=std::thread([&]{for(unsigned i=0;i<1000;++i)f.call();});
    for(auto& thread:threads)thread.join();
    require(metadataCount==before && number(latestSummary(),"callback_calls")==24000,"disabled callbacks admitted");
    require(observer::observeCpuTime().count==cpu.count && observer::observeCpuTime().totalNs==cpu.totalNs,
            "stopped callbacks preserve cumulative observe CPU work without new samples");
}
void continuousInvocations() {
    ++cases; Fixture fixture; invocationCalls=invalidInvocations=0;
    const auto sink=[](std::uint64_t device,std::uint64_t call,std::uintptr_t payload,std::uintptr_t context,
                       const probe::Result& result,bool detail) {
        ++invocationCalls;
        require(device==0x777&&call==invocationCalls&&payload==address(recursionPayload)&&context==address(recursionContext),
            "continuous callback retains exact invocation payload/context");
        require(detail==(call<=32),"continuous detail budget unchanged");
        if(!result.snapshotCaptured) {++invalidInvocations;require(result.graphTextureIdsRead==0,"rejection exposes no old resources");}
        SetLastError(0xD00D);
    };
    reset(fixture,true,metadataSink,observer::defaultLimits,sink);
    for(unsigned i=0;i<240;++i)fixture.call();
    require(invocationCalls==240&&logs.size()==32,"continuous callback survives detail and periodic quotas");
    u32(fixture.graph[1].data()+12,1);SetLastError(0xF00D);fixture.call();
    require(invocationCalls==241&&invalidInvocations==1&&GetLastError()==0xF00D,"invalid callback revokes input after detail quota");
    u32(fixture.graph[1].data()+12,2);throwingLog=true;fixture.call();throwingLog=false;
    require(invocationCalls==242&&invalidInvocations==1,"valid callback resumes without a stale rejected snapshot");
    const auto sum=latestSummary();require(number(sum,"probes")==242&&number(sum,"rejected")==1,"all continuous callbacks are bounded probes");
    observer::stopSampling();fixture.call();require(invocationCalls==242,"stop prevents continuous delivery");
    ++cases;invocationCalls=invalidInvocations=0;
    reset(fixture,true,metadataSink,observer::defaultLimits,sink);throwingLog=true;fixture.call();throwingLog=false;
    require(invocationCalls==1,"diagnostic exception cannot skip input delivery");
}

} // namespace

int main(int argc,char** argv) {
    try {
        if(argc!=1 && (argc!=3 || std::string(argv[1])!="--log-jsonl")) throw std::runtime_error("Usage: upscale-observer-test [--log-jsonl path]");
        gatesAndErrorState();rejectionsAndBudgets();inaccessibleReads();quotaAndRefresh();exceptionsAndRecursion();stopRaces();concurrentCallbacks();continuousInvocations();
        if(argc==3) {std::ofstream output(argv[2],std::ios::binary);require(bool(output),"open test log");for(const auto& line:allLogs)output<<line<<'\n';require(bool(output),"write test log");}
        std::cout<<"{\"status\":\"passed\",\"cases\":"<<cases<<",\"checks\":"<<checks.load()<<",\"fixture_log_lines\":"<<allLogs.size()
                 <<",\"game_read\":false,\"hook_installed\":false,\"gpu_used\":false}\n";
        return 0;
    } catch(const std::exception& error) {std::cerr<<"FAIL: "<<error.what()<<" after "<<cases<<" cases / "<<checks.load()<<" checks\n";return 1;}
}
