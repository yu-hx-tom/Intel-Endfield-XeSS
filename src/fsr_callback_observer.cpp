#include "fsr_callback_observer.hpp"
#include "fsr_input_probe.hpp"
#include <windows.h>
#include <array>
#include <atomic>
#include <cstring>
#include <sstream>
#include <string>

namespace xess_vk::fsr_callback_observer {
namespace {
std::atomic<Sink> output{};
std::atomic<ImageMetadataSink> imageMetadataOutput{};
std::atomic<std::uintptr_t> base{};
std::atomic<std::uint64_t> device{}, calls{}, sampled{}, other{}, unreadable{}, failures{}, emitted{}, unknownCalls{}, unknownSamples{}, unknownLastTick{};
struct PassCounters {
    std::atomic<std::uint64_t> matches{}, regularSamples{}, resumeSamples{}, lastTick{};
    std::atomic<std::uint64_t> refreshAttempts{}, refreshParsed{}, refreshNativeCandidates{}, lastRefreshTick{};
};
std::array<PassCounters,3> passes;
#ifdef XESS_FSR_CALLBACK_TEST
std::atomic<std::uint64_t> testClock{};
#endif
enum class Gate { Cold, Configured, Active, Stopped };
std::atomic<Gate> gate{Gate::Cold};
thread_local bool inside = false;
struct ErrorGuard { DWORD value = GetLastError(); ~ErrorGuard() { SetLastError(value); } };
std::uint64_t now() noexcept {
#ifdef XESS_FSR_CALLBACK_TEST
    if(const auto value=testClock.load()) return value;
#endif
    return GetTickCount64();
}
std::uint64_t gap(std::atomic<std::uint64_t>& last,std::uint64_t tick) noexcept {
    auto previous=last.load();
    while(tick>previous) {
        if(last.compare_exchange_weak(previous,tick)) return previous?tick-previous:0;
    }
    return 0;
}
bool reserve(std::atomic<std::uint64_t>& counter,std::uint64_t limit) noexcept {
    auto value=counter.load();
    while(value<limit) if(counter.compare_exchange_weak(value,value+1)) return true;
    return false;
}
bool read(std::uintptr_t address, void* destination, std::size_t bytes) noexcept {
    if (!address || bytes > UINTPTR_MAX - address) return false;
    SIZE_T copied=0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), destination, bytes, &copied) && copied==bytes;
}
std::size_t reader(void*, std::uint64_t address, void* destination, std::size_t bytes, std::size_t budget) {
    return bytes<=budget && read(address,destination,bytes) ? bytes : 0;
}
void handle(std::ostringstream& s,std::uint64_t value) { s << "\"0x" << std::hex << value << std::dec << '"'; }
void emit(const char* event,const std::string& fields) noexcept {
    const auto sink=output.load(std::memory_order_acquire);
    if (!sink) return;
    try {
        std::ostringstream s;
        s << "{\"event\":\"" << event << "\",\"pid\":" << GetCurrentProcessId()
          << ",\"uptime_ms\":" << GetTickCount64() << ",\"thread_id\":" << GetCurrentThreadId() << ",\"device\":";
        handle(s,device.load()); s << fields << '}';
        const auto line=s.str();
        if (line.size()>=30000) { ++failures; return; }
        sink(line.c_str()); ++emitted;
    } catch (...) { ++failures; }
}
void constants(std::ostringstream& s,const fsr_probe::ConstantsSnapshot& c) {
    s<<",\"constants_fi\":{\"status\":\""<<fsr_probe::statusName(c.status)<<"\",\"descriptor\":";handle(s,c.descriptor);
    s<<",\"buffer_id\":"<<c.bufferId<<",\"byte_offset\":"<<c.byteOffset<<",\"byte_size\":"<<c.byteSize
     <<",\"unknown_bc\":"<<c.unknownBC<<",\"cpu_pointer\":";handle(s,c.cpuPointer);
    s<<",\"descriptor_read\":"<<(c.descriptorRead?"true":"false")<<",\"descriptor_unchanged\":"<<(c.descriptorUnchanged?"true":"false")
     <<",\"graph_bounds_unchanged\":"<<(c.graphBoundsUnchanged?"true":"false")<<",\"captured\":"<<(c.captured?"true":"false");
    if(c.captured) {
        s<<",\"raw_u32_bits\":[";
        for(std::size_t i=0;i<c.bytes.size()/4;++i) {
            std::uint32_t bits{};std::memcpy(&bits,c.bytes.data()+i*4,4);
            if(i)s<<',';s<<bits;
        }
        s<<']';
    }
    s<<",\"snapshot_atomic\":false,\"lifetime_pinned\":false,\"shader_semantics_verified\":false}";
}
}
void setImageMetadataSink(ImageMetadataSink sink) noexcept { imageMetadataOutput.store(sink, std::memory_order_release); }
void configureVerifiedModule(std::uintptr_t module, std::uint64_t targetDevice, Sink sink) noexcept {
    ErrorGuard error;
    // Published before enabling the detour; module identity and lifetime are
    // the installer's responsibility. No late module replacement is supported.
    base.store(module); device.store(targetDevice); output.store(sink);
    auto expected=Gate::Cold;
    if (module && sink) gate.compare_exchange_strong(expected,Gate::Configured,std::memory_order_release);
}
bool enableSampling() noexcept {
    auto expected=Gate::Configured;
    return gate.compare_exchange_strong(expected,Gate::Active,std::memory_order_acq_rel);
}
void stopSampling() noexcept { gate.store(Gate::Stopped,std::memory_order_release); }
void observe(void* payload,void* context) noexcept {
    ErrorGuard error;
    if (gate.load(std::memory_order_acquire)!=Gate::Active || inside) return;
    inside=true; ++calls;
    try {
        const auto module=base.load();
        const auto at=reinterpret_cast<std::uintptr_t>(payload);
        // Shared callback also services other FI passes. Read exactly the five
        // binding descriptors first and require the full unique property set.
        std::array<std::uint64_t,20> descriptors{};
        if (at>UINTPTR_MAX-0xb0 || !read(at+0x10,descriptors.data(),sizeof(descriptors))) {
            ++unreadable; inside=false; return;
        }
        std::array<fsr_probe::Address,5> properties{};
        for(std::size_t i=0;i<5;++i) properties[i]=descriptors[i*4+2];
        const auto pass=fsr_probe::classifyFiveProperties(module,properties);
        if(pass!=fsr_probe::PassKind::PrepareInput) ++other;
        if(pass==fsr_probe::PassKind::Unknown) {
            const auto count=++unknownCalls;
            const auto since=gap(unknownLastTick,now());
            // Four initial unknown sets plus at most four later gap samples.
            // No unbounded set/map of property addresses is retained.
            if((count<=4 || since>1500)&&reserve(unknownSamples,8)) {
                std::ostringstream s;
                s<<",\"unknown_call\":"<<count<<",\"callback_gap_ms\":"<<since<<",\"payload\":";handle(s,at);
                s<<",\"graph_context\":";handle(s,reinterpret_cast<std::uintptr_t>(context));
                s<<",\"properties\":[";
                for(std::size_t i=0;i<properties.size();++i) {
                    if(i)s<<',';s<<"{\"slot\":"<<i<<",\"address\":";handle(s,properties[i]);s<<",\"rva\":";
                    if(properties[i]>=module) s<<properties[i]-module;else s<<"null";
                    s<<",\"property\":\""<<fsr_probe::propertyName(fsr_probe::propertyAtAddress(module,properties[i]))<<"\"}";
                }
                s<<"],\"pixels_read\":false,\"game_frame_inputs_verified\":false,\"xefg_integrated\":false";
                emit("fsr_unknown_property_set",s.str());
            }
            inside=false;return;
        }
        auto& counters=passes[static_cast<std::size_t>(pass)-1];
        const auto count=++counters.matches;
        const auto since=gap(counters.lastTick,now());
        const bool regular=count<=4 || (count%120==0 && count/120<=60);
        const bool resumed=!regular&&since>1500&&reserve(counters.resumeSamples,8);
        const bool detail=regular||resumed;
        const auto metadata=imageMetadataOutput.load(std::memory_order_acquire);
        const bool refresh=!detail && metadata && count%120==0;
        if(!detail&&!refresh) {inside=false;return;}
        if(regular) ++counters.regularSamples;
        if(detail) ++sampled;
        if(refresh) { ++counters.refreshAttempts; gap(counters.lastRefreshTick,now()); }
        fsr_probe::Request request;
        request.layout=fsr_probe::InputLayout::UnityPlayerBee7be52SharedFivePass;
        request.expectedPass=pass;
        request.captureConstantsFI=true;
        request.moduleBase=module; request.prepareInputPayload=at;
        request.graphContext=reinterpret_cast<std::uintptr_t>(context);
        if (module<=UINTPTR_MAX-fsr_probe::nativeTableRva) {
            request.nativeTableBase=module+fsr_probe::nativeTableRva;
            request.nativeLayout=fsr_probe::NativeTableLayout::DirectArrayOf1024PagePointers_1024ObjectPointers;
        }
        const auto result=fsr_probe::readPrepareInput(request,{reader,nullptr});
        const bool valid=result.pass==pass && result.status==fsr_probe::Status::SnapshotParsed &&
            result.allPassPropertiesPresent && result.contextBoundsUnchanged;
        if(refresh) {
            if(valid) { ++counters.refreshParsed; counters.refreshNativeCandidates+=result.nativeCandidatesRead; }
            if(valid && metadata) metadata(device.load(),count,result,false);
            inside=false;return;
        }
        std::ostringstream s;
        s << ",\"pass\":\""<<fsr_probe::passName(pass)<<"\",\"parsed_pass\":\""<<fsr_probe::passName(result.pass)
          <<"\",\"matched_call\":" << count <<",\"sample_reason\":\""<<(resumed?"resume_gap":"regular")
          <<"\",\"callback_gap_ms\":"<<since<< ",\"payload\":"; handle(s,at);
        s << ",\"graph_context\":"; handle(s,reinterpret_cast<std::uintptr_t>(context));
        s << ",\"status\":\"" << fsr_probe::statusName(result.status) << "\",\"graph_entry_count\":" << result.graphEntryCount
          << ",\"parser_read_calls\":" << result.readCalls << ",\"parser_bytes_requested\":" << result.bytesRequested
          << ",\"prefilter_read_calls\":1,\"prefilter_bytes_requested\":160"
          << ",\"payload_extent\":"<<result.payloadExtent
          << ",\"all_pass_properties\":"<<(result.allPassPropertiesPresent?"true":"false")
          << ",\"all_prepare_properties\":" << (result.allFivePreparePropertiesPresent?"true":"false")
          << ",\"context_bounds_unchanged\":" << (result.contextBoundsUnchanged?"true":"false") << ",\"bindings\":[";
        for (std::size_t i=0;i<result.bindings.size();++i) {
            if (i) s << ','; const auto& b=result.bindings[i];
            s << "{\"property\":\"" << fsr_probe::propertyName(b.property) << "\",\"property_address\":"; handle(s,b.propertyAddress);
            s << ",\"graph_handle\":"; handle(s,b.graphHandle);
            s << ",\"graph_status\":\"" << fsr_probe::statusName(b.graphStatus) << "\",\"entry_type\":" << b.entryType
              << ",\"texture_id\":" << b.textureId << ",\"native_status\":\"" << fsr_probe::statusName(b.native.status)
              << "\",\"native_object_candidate\":"; handle(s,b.native.object);
            s << ",\"image_candidate\":"; handle(s,b.native.imageCandidate);
            s << ",\"width_candidate\":" << b.native.widthCandidate << ",\"height_candidate\":" << b.native.heightCandidate
              << ",\"native_id_domain_verified\":false,\"vulkan_image_verified\":false}";
        }
        s << ']';constants(s,result.constantsFI);
        s << ",\"snapshot_atomic\":false,\"pixels_read\":false,\"game_frame_inputs_verified\":false,\"xefg_integrated\":false";
        const char* event=pass==fsr_probe::PassKind::PrepareInput?"fsr_prepare_input_candidate":
            pass==fsr_probe::PassKind::ReconstructPreviousDepth?"fsr_reconstruct_previous_depth_candidate":"fsr_frame_inpainting_candidate";
        emit(event,s.str());
        // A racing property-set change is not assigned to the prefilter's pass
        // counter in the typed sink, even if another valid set was observed.
        if(valid && metadata) metadata(device.load(), count, result, true);
    } catch (...) { ++failures; }
    inside=false;
}
void summary() noexcept {
    ErrorGuard error;
    if (!output.load() || inside) return;
    inside=true;
    try {
        std::ostringstream s;
        s << ",\"sampling_enabled\":" << (gate.load()==Gate::Active?"true":"false") << ",\"callback_calls\":" << calls.load()
          << ",\"prepare_property_matches\":" << passes[0].matches.load() << ",\"samples\":" << sampled.load()
          << ",\"reconstruct_property_matches\":"<<passes[1].matches.load()<<",\"inpainting_property_matches\":"<<passes[2].matches.load()
          << ",\"prepare_samples\":"<<passes[0].regularSamples.load()+passes[0].resumeSamples.load()
          << ",\"reconstruct_samples\":"<<passes[1].regularSamples.load()+passes[1].resumeSamples.load()
          << ",\"inpainting_samples\":"<<passes[2].regularSamples.load()+passes[2].resumeSamples.load()
          << ",\"prepare_resume_samples\":"<<passes[0].resumeSamples.load()<<",\"reconstruct_resume_samples\":"<<passes[1].resumeSamples.load()
          << ",\"inpainting_resume_samples\":"<<passes[2].resumeSamples.load()<<",\"unknown_property_combinations\":"<<unknownCalls.load()
          << ",\"unknown_property_samples\":"<<unknownSamples.load()
          << ",\"other_property_combinations\":" << other.load() << ",\"unreadable_payloads\":" << unreadable.load()
          << ",\"trace_failures\":" << failures.load() << ",\"emitted\":" << emitted.load()
          << ",\"game_frame_inputs_verified\":false,\"pixels_read\":false,\"xefg_integrated\":false";
        constexpr std::array<const char*,3> labels{"prepare","reconstruct","inpainting"};
        for(std::size_t i=0;i<passes.size();++i) {
            const auto& p=passes[i]; const auto* label=labels[i];
            s<<",\""<<label<<"_refresh_attempts\":"<<p.refreshAttempts.load()
             <<",\""<<label<<"_refresh_parsed\":"<<p.refreshParsed.load()
             <<",\""<<label<<"_refresh_native_candidates\":"<<p.refreshNativeCandidates.load()
             <<",\""<<label<<"_refresh_last_uptime_ms\":"<<p.lastRefreshTick.load();
        }
        emit("fsr_callback_summary",s.str());
    } catch (...) { ++failures; }
    inside=false;
}
#ifdef XESS_FSR_CALLBACK_TEST
void resetForTest(std::uintptr_t module,std::uint64_t targetDevice,Sink sink,bool activate) noexcept {
    stopSampling(); calls=0;sampled=0;other=0;unreadable=0;failures=0;emitted=0;unknownCalls=0;unknownSamples=0;unknownLastTick=0;
    for(auto& p:passes) {
        p.matches=0;p.regularSamples=0;p.resumeSamples=0;p.lastTick=0;
        p.refreshAttempts=0;p.refreshParsed=0;p.refreshNativeCandidates=0;p.lastRefreshTick=0;
    }
    gate.store(Gate::Cold);
    configureVerifiedModule(module,targetDevice,sink);
    if (activate) enableSampling();
}
void setClockForTest(std::uint64_t value) noexcept {testClock=value;}
#endif
}
extern "C" void fsr_observe_before(void* payload,void* context) noexcept { xess_vk::fsr_callback_observer::observe(payload,context); }
