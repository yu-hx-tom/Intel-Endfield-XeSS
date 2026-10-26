#include "fsr_callback_observer.hpp"
#include "fsr_input_probe.hpp"
#include <windows.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <fstream>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
namespace {
std::vector<std::string> logs;
std::mutex logsMutex;
bool throwing=false, recursive=false;
std::atomic<unsigned> metadataCalls{}, metadataDetailCalls{}, metadataFailures{};
std::atomic<std::uint64_t> expectedImageBase{0xa00};
void metadataSink(std::uint64_t device, std::uint64_t sample, const xess_vk::fsr_probe::Result& result, bool emitDetail) {
    ++metadataCalls;
    if(emitDetail) ++metadataDetailCalls;
    if (device != 0x777 || !sample || result.nativeCandidatesRead != 5 || result.gameFrameInputsVerified ||
        result.pass==xess_vk::fsr_probe::PassKind::Unknown || !result.allPassPropertiesPresent || !result.constantsFI.captured) ++metadataFailures;
    if(result.pass==xess_vk::fsr_probe::PassKind::FrameInpainting &&
       (result.bindings[2].property!=xess_vk::fsr_probe::Property::PresentBackbuffer || result.bindings[2].native.imageCandidate!=expectedImageBase+2 ||
        result.bindings[4].property!=xess_vk::fsr_probe::Property::Output || result.bindings[4].native.imageCandidate!=expectedImageBase+4)) ++metadataFailures;
    for(std::size_t i=0;i<result.bindings.size();++i)
        if(result.bindings[i].native.imageCandidate!=expectedImageBase+i) ++metadataFailures;
    SetLastError(0xdead);
    fsr_observe_before(nullptr, nullptr); // Same observer's recursion guard applies.
}
void throwingMetadataSink(std::uint64_t, std::uint64_t, const xess_vk::fsr_probe::Result&, bool) { throw std::runtime_error("metadata sink failure"); }
void sink(const char* s) {
    if (throwing) throw std::runtime_error("diagnostic failure");
    if (recursive) fsr_observe_before(nullptr,nullptr);
    std::lock_guard lock(logsMutex);
    logs.emplace_back(s);
}
void require(bool good,const char* message) { if(!good) throw std::runtime_error(message); }
std::uintptr_t addr(const void* p) { return reinterpret_cast<std::uintptr_t>(p); }
void word(void* p,std::uint32_t n) { std::memcpy(p,&n,4); }
void pointer(void* p,std::uint64_t n) { std::memcpy(p,&n,8); }
}
int main(int argc,char** argv) {
    using namespace xess_vk;
    try {
        if(argc!=1 && (argc!=3 || std::string(argv[1])!="--log-jsonl")) throw std::runtime_error("Usage: xess-fsr-callback-observer-test [--log-jsonl output-path]");
        std::array<std::uint64_t,26> payload{};
        std::array<unsigned char,224> constants{};
        std::array<std::uint64_t,4> context{};
        std::array<std::array<unsigned char,32>,5> graph{};
        std::array<std::uint32_t,5> resourceIds{1,2,3,4,5};
        std::array<std::uint64_t,1024> table{},page{};
        std::array<std::array<unsigned char,64>,5> objects{};
        const auto module=addr(table.data())-fsr_probe::nativeTableRva;
        table[0]=addr(page.data());
        constexpr std::array<std::uint64_t,5> properties{0x1D10784,0x1D10788,0x1D107E4,0x1D107E8,0x1D107EC};
        for(std::size_t i=0;i<5;++i) {
            payload[2+i*4]=i+1; payload[4+i*4]=module+properties[i];
            word(graph[i].data(),static_cast<std::uint32_t>(i+1)); word(graph[i].data()+8,2);
            pointer(graph[i].data()+0x10,addr(&resourceIds[i]));
            page[i+1]=addr(objects[i].data());
            word(objects[i].data()+0x14,17); word(objects[i].data()+0x18,19);
            pointer(objects[i].data()+0x38,0xa00+i);
        }
        context[2]=addr(graph.data()); context[3]=context[2]+sizeof(graph);
        auto* payloadBytes=reinterpret_cast<unsigned char*>(payload.data());
        word(payloadBytes+0xB0,17);word(payloadBytes+0xB4,128);word(payloadBytes+0xB8,224);word(payloadBytes+0xBC,0xF012ABCD);
        pointer(payloadBytes+0xC0,addr(constants.data()));
        for(unsigned i=0;i<constants.size();++i) constants[i]=static_cast<unsigned char>(i*7+11);
        fsr_callback_observer::setClockForTest(100000);
        fsr_callback_observer::resetForTest(module,0x777,sink,false);
        fsr_observe_before(payload.data(),context.data());
        require(logs.empty(),"configure started sampling before post-install checks");
        require(fsr_callback_observer::enableSampling(),"configured gate did not activate");
        recursive=true;
        SetLastError(12345); fsr_observe_before(payload.data(),context.data());
        require(GetLastError()==12345,"callback changed LastError");
        require(logs.size()==1,"recursive sink was not guarded");
        require(logs.back().find("\"status\":\"SnapshotParsed\"")!=std::string::npos,"valid synthetic snapshot not read");
        require(logs.back().find("\"image_candidate\":\"0xa00\"")!=std::string::npos,"native candidate missing");
        require(logs.back().find("\"vulkan_image_verified\":false")!=std::string::npos,"candidate falsely verified");
        require(logs.back().find("\"captured\":true")!=std::string::npos&&logs.back().find("\"raw_u32_bits\":[")!=std::string::npos,
            "Bounded opaque constants were not logged");
        const auto before=logs.size();
        payload[4]=module+0x1D10610; fsr_observe_before(payload.data(),context.data());
        require(logs.size()==before+1&&logs.back().find("\"event\":\"fsr_unknown_property_set\"")!=std::string::npos,
            "Unknown property set was not separately bounded/reported");
        payload[4]=module+properties[0];
        void* inaccessible=VirtualAlloc(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_NOACCESS);
        require(inaccessible!=nullptr,"noaccess fixture allocation");
        fsr_observe_before(inaccessible,context.data()); VirtualFree(inaccessible,0,MEM_RELEASE);
        require(logs.size()==before+1,"unreadable payload produced false candidate");
        throwing=true; fsr_observe_before(payload.data(),context.data()); throwing=false;
        for(int i=0;i<10000;++i) fsr_observe_before(payload.data(),context.data());
        require(logs.size()<=65,"sampling plus bounded unknown set unbounded");
        fsr_callback_observer::summary();
        require(logs.back().find("\"samples\":64")!=std::string::npos,"sampling schedule unexpected");
        require(logs.back().find("\"trace_failures\":1")!=std::string::npos,"throwing diagnostic not contained/countable");
        require(logs.back().find("\"other_property_combinations\":1")!=std::string::npos,"shared pass distinction lost");
        require(logs.back().find("\"unreadable_payloads\":1")!=std::string::npos,"unreadable count lost");
        fsr_callback_observer::stopSampling(); const auto stopped=logs.size();
        fsr_observe_before(payload.data(),context.data()); require(logs.size()==stopped,"stopSampling ineffective");
        fsr_callback_observer::configureVerifiedModule(module,0x777,sink);
        require(!fsr_callback_observer::enableSampling(),"configure/enable revived sticky stopped state");
        fsr_observe_before(payload.data(),context.data()); require(logs.size()==stopped,"sticky stop ineffective");

        // Exercise the actual sampling atomics and concurrent sink contract.
        // All fixture memory is immutable for the complete worker lifetime.
        recursive=false; logs.clear();
        fsr_callback_observer::resetForTest(module,0x777,sink);
        fsr_callback_observer::setImageMetadataSink(metadataSink);
        std::atomic<unsigned> changedLastError{};
        std::vector<std::jthread> workers;
        for(unsigned t=0;t<8;++t) workers.emplace_back([&,t] {
            const DWORD expectedError=0x12340000+t;
            for(unsigned i=0;i<2000;++i) {
                SetLastError(expectedError);
                fsr_observe_before(payload.data(),context.data());
                if(GetLastError()!=expectedError) ++changedLastError;
            }
        });
        for(auto& worker:workers) worker.join();
        require(changedLastError==0,"concurrent callbacks changed thread-local LastError");
        require(logs.size()==64,"concurrent sampling did not produce exactly 64 events");
        require(metadataDetailCalls == 64 && metadataCalls == 137 && metadataFailures == 0,
            "typed metadata consumer must refresh after bounded sample quota without more detail logs");
        std::set<unsigned long long> sampledCalls;
        const std::string marker="\"matched_call\":";
        for(const auto& line:logs) {
            const auto at=line.find(marker);
            require(at!=std::string::npos,"concurrent callback emitted unexpected event");
            sampledCalls.insert(std::stoull(line.substr(at+marker.size())));
        }
        require(sampledCalls.size()==64,"concurrent calls reused sample sequence numbers");
        for(unsigned i=1;i<=4;++i) require(sampledCalls.contains(i),"missing concurrent startup sample");
        for(unsigned i=1;i<=60;++i) require(sampledCalls.contains(120*i),"missing concurrent spaced sample");
        fsr_callback_observer::summary();
        require(logs.back().find("\"callback_calls\":16000")!=std::string::npos,"concurrent callback counter lost calls");
        require(logs.back().find("\"prepare_property_matches\":16000")!=std::string::npos,"concurrent matching counter lost calls");
        require(logs.back().find("\"samples\":64")!=std::string::npos,"concurrent summary sample budget wrong");
        require(logs.back().find("\"trace_failures\":0")!=std::string::npos,"concurrent diagnostics failed");
        fsr_callback_observer::stopSampling();
        fsr_callback_observer::resetForTest(module,0x777,sink);
        fsr_callback_observer::setImageMetadataSink(throwingMetadataSink);
        SetLastError(54321); fsr_observe_before(payload.data(),context.data());
        require(GetLastError() == 54321, "throwing metadata changed LastError");
        fsr_callback_observer::summary();
        require(logs.back().find("\"trace_failures\":1") != std::string::npos, "metadata exception not contained");
        fsr_callback_observer::setImageMetadataSink(nullptr);

        // Three exact property sets share the callback but have independent
        // counters/quotas. The two other payloads are exactly C8, not D0.
        std::array<std::uint64_t,25> reconstruct{},inpainting{};
        std::memcpy(reconstruct.data(),payload.data(),sizeof(reconstruct));
        std::memcpy(inpainting.data(),payload.data(),sizeof(inpainting));
        constexpr std::array<std::uint64_t,5> reconstructProperties{0x1D1073C,0x1D10740,0x1D10744,0x1D10748,0x1D107A8};
        constexpr std::array<std::uint64_t,5> inpaintingProperties{0x1D10738,0x1D1076C,0x1D1077C,0x1D10744,0x1D107E0};
        for(unsigned i=0;i<5;++i) {reconstruct[4+i*4]=module+reconstructProperties[i];inpainting[4+i*4]=module+inpaintingProperties[i];}
        logs.clear();metadataCalls=0;metadataDetailCalls=0;metadataFailures=0;changedLastError=0;workers.clear();
        fsr_callback_observer::setClockForTest(200000);
        fsr_callback_observer::resetForTest(module,0x777,sink);
        fsr_callback_observer::setImageMetadataSink(metadataSink);
        for(unsigned t=0;t<8;++t) workers.emplace_back([&,t] {
            for(unsigned i=0;i<1200;++i) {
                for(void* p : {static_cast<void*>(payload.data()),static_cast<void*>(reconstruct.data()),static_cast<void*>(inpainting.data())}) {
                    SetLastError(0xB000+t);fsr_observe_before(p,context.data());
                    if(GetLastError()!=0xB000+t) ++changedLastError;
                }
            }
        });
        for(auto& worker:workers) worker.join();
        require(logs.size()==192&&metadataDetailCalls==192&&metadataCalls==252&&metadataFailures==0&&changedLastError==0,
            "Independent three-pass concurrent quotas/typed identity/LastError failed");
        constexpr std::array<const char*,3> passNames{"prepare_input","reconstruct_previous_depth","frame_inpainting"};
        for(const auto* pass : passNames) {
            std::set<unsigned long long> sequences;
            for(const auto& line:logs) if(line.find(std::string("\"pass\":\"")+pass+"\"")!=std::string::npos) {
                const auto at=line.find(marker);require(at!=std::string::npos,"pass sample missing sequence");
                sequences.insert(std::stoull(line.substr(at+marker.size())));
            }
            require(sequences.size()==64&&sequences.contains(1)&&sequences.contains(7200),"Pass quotas collided or lost their independent sequence");
        }
        // Exact threshold is not a resume, and only the two other passes return
        // after the gap. Prepare's count stays flat: never infer its recovery.
        fsr_callback_observer::setClockForTest(201500);
        for(void* p : {static_cast<void*>(payload.data()),static_cast<void*>(reconstruct.data()),static_cast<void*>(inpainting.data())})
            fsr_observe_before(p,context.data());
        require(logs.size()==192,"1500ms exact boundary incorrectly used resume quota");
        std::uint64_t clock=203001;
        for(unsigned i=0;i<8;++i,clock+=2000) {
            fsr_callback_observer::setClockForTest(clock);
            fsr_observe_before(reconstruct.data(),context.data());fsr_observe_before(inpainting.data(),context.data());
        }
        require(logs.size()==208&&metadataDetailCalls==208,"Non-Prepare recovery had no reserved samples after normal quota exhaustion");
        fsr_callback_observer::summary();
        require(logs.back().find("\"prepare_property_matches\":9601")!=std::string::npos &&
            logs.back().find("\"reconstruct_property_matches\":9609")!=std::string::npos &&
            logs.back().find("\"prepare_resume_samples\":0")!=std::string::npos,"Other callbacks falsely implied Prepare recovery");
        logs.pop_back();
        for(unsigned i=0;i<8;++i,clock+=2000) {
            fsr_callback_observer::setClockForTest(clock);fsr_observe_before(payload.data(),context.data());
        }
        require(logs.size()==216&&metadataDetailCalls==216&&metadataFailures==0,"Total recognized callback bound is not 3*(64+8)");
        for(unsigned i=0;i<12;++i,clock+=2000) {
            fsr_callback_observer::setClockForTest(clock);
            for(void* p : {static_cast<void*>(payload.data()),static_cast<void*>(reconstruct.data()),static_cast<void*>(inpainting.data())})
                fsr_observe_before(p,context.data());
        }
        require(logs.size()==216&&metadataDetailCalls==216,"Resume quota exceeded eight samples per pass");
        // Both regular and gap quotas are exhausted. Consumers must still see
        // new valid inputs at the low-frequency cadence without any detail log.
        const auto beforeRefresh=metadataCalls.load();
        expectedImageBase=0xb00;
        for(std::size_t i=0;i<objects.size();++i) pointer(objects[i].data()+0x38,expectedImageBase+i);
        for(unsigned n=0;n<120;++n) for(void* p : {static_cast<void*>(payload.data()),static_cast<void*>(reconstruct.data()),static_cast<void*>(inpainting.data())})
            fsr_observe_before(p,context.data());
        require(metadataCalls==beforeRefresh+3 && metadataDetailCalls==216 && metadataFailures==0 && logs.size()==216,
            "metadata refresh stopped with exhausted diagnostic quotas or emitted extra detail logs");
        const auto beforeInvalid=metadataCalls.load();
        for(unsigned n=0;n<120;++n) fsr_observe_before(payload.data(),nullptr);
        require(metadataCalls==beforeInvalid && logs.size()==216,"invalid refresh snapshot escaped to typed consumer");
        fsr_callback_observer::summary();
        require(logs.back().find("\"prepare_refresh_attempts\":22")!=std::string::npos &&
            logs.back().find("\"prepare_refresh_parsed\":21")!=std::string::npos,
            "bounded refresh counters failed to distinguish read failure from a valid snapshot");
        logs.pop_back();
        auto unknown=inpainting;unknown[4]=module+0xDEAD;
        for(unsigned i=0;i<10;++i) fsr_observe_before(unknown.data(),context.data());
        require(logs.size()==220,"Unknown startup samples were not capped at four");
        for(unsigned i=0;i<12;++i,clock+=2000) {
            fsr_callback_observer::setClockForTest(clock);fsr_observe_before(unknown.data(),context.data());
        }
        require(logs.size()==224&&metadataDetailCalls==216&&metadataCalls==beforeInvalid,
            "Unknown bounded records leaked into typed candidates or exceeded eight");
        fsr_callback_observer::summary();
        require(logs.back().find("\"samples\":216")!=std::string::npos&&logs.back().find("\"unknown_property_samples\":8")!=std::string::npos&&
            logs.back().find("\"prepare_samples\":72")!=std::string::npos&&logs.back().find("\"reconstruct_samples\":72")!=std::string::npos&&
            logs.back().find("\"inpainting_samples\":72")!=std::string::npos,"Multipass summary lost the hard event bound");
        fsr_callback_observer::setImageMetadataSink(nullptr);
        for(unsigned i=0;i<128;++i) {
            fsr_callback_observer::resetForTest(module,0x777,sink,false);
            std::jthread activate([] { fsr_callback_observer::enableSampling(); });
            std::jthread stop([] { fsr_callback_observer::stopSampling(); });
            activate.join(); stop.join();
            require(!fsr_callback_observer::enableSampling(),"activation race revived stopped state");
            const auto beforeStoppedCall=logs.size();
            fsr_observe_before(payload.data(),context.data());
            require(logs.size()==beforeStoppedCall,"activation/stop race left sampling enabled");
        }
        if(argc==3) {
            std::ofstream output(argv[2],std::ios::binary|std::ios::trunc);
            require(output.is_open(),"Cannot create requested fixture JSONL output");
            for(const auto& line:logs) output<<line<<'\n';
            output.close();require(!output.fail(),"Failed writing fixture JSONL output");
        }
        std::cout << "{\"passed\":true,\"readProcessMemoryFixture\":true,\"pageNoAccessContained\":true,\"boundedSamplesPerPass\":72,\"concurrentThreads\":8,\"prepareConcurrentCalls\":16000,\"threePassConcurrentCalls\":28800,\"threePassRegularSamples\":192,\"totalCandidateSamples\":216,\"unknownSetSamples\":8,\"maxDetailEvents\":224,\"prepareFlatDuringOtherPassResume\":true,\"opaqueConstantsCaptured\":true,\"sampleSequenceUnique\":true,\"threadLocalLastErrorPreserved\":true,\"postInstallGateVerified\":true,\"stickyStopRaces\":128,\"gameTested\":false}\n";
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
