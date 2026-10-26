#include "upscale_callback_observer.hpp"
#include <windows.h>
#include <atomic>
#include <sstream>
#include <string>

namespace xess_vk::upscale_callback_observer {
namespace {
enum class Gate { Cold, Configuring, Configured, Active, Stopped };
struct Configuration {
    std::uintptr_t module{};
    std::uint64_t device{};
    Sink sink{};
    SourceMetadataSink metadata{};
    upscale_probe::Limits limits{};
    InvocationSink invocation{};
};
// Written by the sole successful configurer, then immutable until process exit.
Configuration configuration;
std::atomic<bool> configurationReady{};
std::atomic<Gate> gate{Gate::Cold};
struct Counters {
    std::atomic<std::uint64_t> calls{}, detailAttempts{}, refreshAttempts{}, probes{}, parsed{}, rejected{};
    std::atomic<std::uint64_t> refreshParsed{}, nativeCandidates{}, readCalls{}, bytesRequested{}, bytesCopied{};
    std::atomic<std::uint64_t> metadataAttempts{}, metadataDelivered{}, metadataFailures{}, detailEmitted{};
    std::atomic<std::uint64_t> logFailures{}, exceptions{}, stoppedResults{};
} counters;
SRWLOCK observeCpuLock = SRWLOCK_INIT;
CpuWallTime observeCpu;
struct ObserveCpuScope {
    CpuWallClock::time_point started{CpuWallClock::now()};
    ~ObserveCpuScope() {
        const auto ns = cpuWallElapsedNs(started);
        AcquireSRWLockExclusive(&observeCpuLock);
        observeCpu.add(ns);
        ReleaseSRWLockExclusive(&observeCpuLock);
    }
};
thread_local bool inside{};
struct ErrorGuard { DWORD saved = GetLastError(); ~ErrorGuard() { SetLastError(saved); } };
struct ReentryGuard { ReentryGuard() { inside = true; } ~ReentryGuard() { inside = false; } };
#ifdef XESS_UPSCALE_CALLBACK_TEST
void (*afterProbeForTest)(){};
#endif
bool active() noexcept { return gate.load(std::memory_order_acquire) == Gate::Active; }
const char* gateName(Gate state) noexcept {
    switch (state) {
    case Gate::Cold: return "cold";
    case Gate::Configuring: return "configuring";
    case Gate::Configured: return "configured";
    case Gate::Active: return "active";
    case Gate::Stopped: return "stopped";
    }
    return "unknown";
}
std::size_t reader(void*, std::uint64_t address, void* destination, std::size_t bytes, std::size_t budget) {
    if (!active() || !address || bytes > budget || bytes > UINTPTR_MAX - address) return 0;
    SIZE_T copied{};
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), destination, bytes, &copied)
        && copied == bytes ? bytes : 0;
}
void handle(std::ostringstream& out, std::uint64_t value) { out << "\"0x" << std::hex << value << std::dec << '"'; }
void flags(std::ostringstream& out) {
    out << ",\"snapshot_atomic\":false,\"lifetime_pinned\":false,\"pixels_read\":false"
        << ",\"game_functions_called\":false,\"gpu_completion_verified\":false,\"copy_permitted\":false"
        << ",\"undilated_verified\":false,\"motion_semantics_verified\":false,\"same_frame_verified\":false"
        << ",\"shared_fi_roles_selected\":false,\"xefg_integrated\":false";
}
bool emit(const char* event, const std::string& fields, bool detail) noexcept {
    // A stop is not a join: the external sink may already have been admitted.
    if (detail && !active()) { ++counters.stoppedResults; return false; }
    try {
        std::ostringstream out;
        out << "{\"event\":\"" << event << "\",\"source\":\"upscaler_prepare_eb49d0\",\"pid\":"
            << GetCurrentProcessId() << ",\"uptime_ms\":" << GetTickCount64() << ",\"thread_id\":"
            << GetCurrentThreadId() << ",\"device\":";
        handle(out, configuration.device);
        out << fields << '}';
        const auto line = out.str();
        if (line.size() >= 30000) { ++counters.logFailures; return false; }
        if (detail && !active()) { ++counters.stoppedResults; return false; }
        configuration.sink(line.c_str());
        if (detail) ++counters.detailEmitted;
        return true;
    } catch (...) { ++counters.logFailures; return false; }
}
bool validSnapshot(const upscale_probe::Result& result) noexcept {
    if (result.status != upscale_probe::Status::SnapshotParsed || !result.snapshotCaptured ||
        !result.allPropertiesPresent || !result.descriptorsUnchanged || !result.graphEntriesUnchanged ||
        !result.bindingMetadataUnchanged || !result.contextTableUnchanged || !result.graphIndexedEntries ||
        result.graphEntriesObserved != upscale_probe::bindingCount || result.allocationBoundsVerified ||
        result.graphTextureIdsRead != upscale_probe::bindingCount || result.nativeCandidatesRead != upscale_probe::bindingCount)
        return false;
    for (const auto& binding : result.bindings)
        if (binding.property == upscale_probe::Property::Unknown || binding.graphStatus != fsr_probe::Status::GraphTextureIdRead ||
            binding.native.status != fsr_probe::Status::NativeCandidateReadUnverified) return false;
    return true;
}
std::string detailFields(std::uint64_t call, std::uintptr_t payload, std::uintptr_t context,
                         const upscale_probe::Result& result) {
    std::ostringstream out;
    out << ",\"callback_call\":" << call << ",\"payload\":"; handle(out, payload);
    out << ",\"graph_context\":"; handle(out, context);
    out << ",\"status\":\"" << upscale_probe::statusName(result.status)
        << "\",\"detail_status\":\"" << fsr_probe::statusName(result.detailStatus)
        << "\",\"failure_binding_index\":" << result.failureBindingIndex
        << ",\"snapshot_captured\":" << (result.snapshotCaptured ? "true" : "false")
        << ",\"all_properties_present\":" << (result.allPropertiesPresent ? "true" : "false")
        << ",\"descriptors_unchanged\":" << (result.descriptorsUnchanged ? "true" : "false")
        << ",\"graph_entries_unchanged\":" << (result.graphEntriesUnchanged ? "true" : "false")
        << ",\"binding_metadata_unchanged\":" << (result.bindingMetadataUnchanged ? "true" : "false")
        << ",\"context_bounds_unchanged\":" << (result.contextBoundsUnchanged ? "true" : "false")
        << ",\"context_table_unchanged\":" << (result.contextTableUnchanged ? "true" : "false")
        << ",\"allocation_bounds_verified\":false,\"graph_entries_observed\":" << result.graphEntriesObserved
        << ",\"graph_begin\":"; handle(out, result.graphBegin);
    out << ",\"graph_end\":"; handle(out, result.graphEnd);
    out << ",\"graph_entry_count\":" << result.graphEntryCount << ",\"graph_texture_ids_read\":" << result.graphTextureIdsRead
        << ",\"native_candidates_read\":" << result.nativeCandidatesRead << ",\"read_calls\":" << result.readCalls
        << ",\"bytes_requested\":" << result.bytesRequested << ",\"bytes_copied\":" << result.bytesCopied << ",\"bindings\":[";
    // Reject diagnostics never publish a partially resolved binding list.
    if (validSnapshot(result)) for (std::size_t i = 0; i < result.bindings.size(); ++i) {
        const auto& b = result.bindings[i];
        if (i) out << ',';
        out << "{\"slot\":" << i << ",\"property\":\"" << upscale_probe::propertyName(b.property) << "\",\"descriptor\":";
        handle(out, b.descriptor); out << ",\"property_address\":"; handle(out, b.propertyAddress);
        out << ",\"graph_handle\":"; handle(out, b.graphHandle);
        out << ",\"graph_key\":" << b.graphKey << ",\"raw_dependency_direction\":" << static_cast<unsigned>(b.rawDependencyDirection)
            << ",\"raw_subresource\":" << b.rawSubresource << ",\"graph_entry\":"; handle(out, b.graphEntry);
        out << ",\"entry_type\":" << b.entryType << ",\"resource_payload\":"; handle(out, b.resourcePayload);
        out << ",\"texture_id\":" << b.textureId << ",\"native_object\":"; handle(out, b.native.object);
        out << ",\"width_candidate\":" << b.native.widthCandidate << ",\"height_candidate\":" << b.native.heightCandidate
            << ",\"image_candidate\":"; handle(out, b.native.imageCandidate);
        out << ",\"native_id_domain_verified\":false,\"vulkan_image_verified\":false}";
    }
    out << ']'; flags(out); return out.str();
}
} // namespace

bool configureVerifiedModule(std::uintptr_t module, std::uint64_t device, Sink sink,
                             SourceMetadataSink metadata, upscale_probe::Limits limits,
                             InvocationSink invocation) noexcept {
    ErrorGuard error;
    // Largest referenced property and the entire native page-pointer table must
    // fit the address domain. Identity/mapping validation is the installer's job.
    if (!module || !device || !sink || module > UINTPTR_MAX - 0x1EDE5A8 ||
        !limits.maxGraphEntries || limits.maxGraphEntries > defaultLimits.maxGraphEntries ||
        !limits.maxBytes || limits.maxBytes > defaultLimits.maxBytes ||
        !limits.maxReadCalls || limits.maxReadCalls > defaultLimits.maxReadCalls) return false;
    auto expected = Gate::Cold;
    if (!gate.compare_exchange_strong(expected, Gate::Configuring, std::memory_order_acq_rel)) return false;
    configuration = {module, device, sink, metadata, limits, invocation};
    configurationReady.store(true, std::memory_order_release);
    expected = Gate::Configuring;
    return gate.compare_exchange_strong(expected, Gate::Configured, std::memory_order_release, std::memory_order_relaxed);
}
bool enableSampling() noexcept {
    ErrorGuard error;
    auto expected = Gate::Configured;
    return gate.compare_exchange_strong(expected, Gate::Active, std::memory_order_acq_rel);
}
void stopSampling() noexcept { ErrorGuard error; gate.store(Gate::Stopped, std::memory_order_release); }
void observe(void* payload, void* context) noexcept {
    ErrorGuard error;
    if (inside || !active()) return;
    ReentryGuard reentry;
    ObserveCpuScope elapsed;
    const auto call = ++counters.calls;
    const bool detail = call <= detailLimit;
    const bool refresh = !detail && configuration.metadata && call % refreshInterval == 0;
    if (!detail && !refresh && !configuration.invocation) return;
    if (detail) ++counters.detailAttempts;
    if (refresh) ++counters.refreshAttempts;
    try {
        upscale_probe::Request request;
        request.layout = upscale_probe::InputLayout::UnityPlayer9aa9397aUpscalerDirectIndexEight;
        request.moduleBase = configuration.module;
        request.prepareInputPayload = reinterpret_cast<std::uintptr_t>(payload);
        request.graphContext = reinterpret_cast<std::uintptr_t>(context);
        request.nativeTableBase = configuration.module + upscale_probe::nativeTableRva;
        request.nativeLayout = upscale_probe::NativeTableLayout::DirectArrayOf1024PagePointers_1024ObjectPointers;
        request.limits = configuration.limits;
        const auto result = upscale_probe::readUpscalerPrepare(request, {reader, nullptr});
        ++counters.probes;
        counters.readCalls += result.readCalls;
        counters.bytesRequested += result.bytesRequested;
        counters.bytesCopied += result.bytesCopied;
        const bool valid = validSnapshot(result);
        if (valid) {
            ++counters.parsed;
            counters.nativeCandidates += result.nativeCandidatesRead;
            if (refresh) ++counters.refreshParsed;
        } else ++counters.rejected;
#ifdef XESS_UPSCALE_CALLBACK_TEST
        if (afterProbeForTest) afterProbeForTest();
#endif
        if (!active()) { ++counters.stoppedResults; return; }
        if (configuration.invocation) {
            try {
                configuration.invocation(configuration.device,call,request.prepareInputPayload,
                    request.graphContext,result,detail);
            } catch (...) { ++counters.metadataFailures; }
        }
        if (!active()) { ++counters.stoppedResults; return; }
        if (detail) emit("upscale_source_snapshot", detailFields(call, request.prepareInputPayload, request.graphContext, result), true);
        if (valid && configuration.metadata) {
            if (!active()) { ++counters.stoppedResults; return; }
            ++counters.metadataAttempts;
            try {
                configuration.metadata(configuration.device, call, result, detail);
                ++counters.metadataDelivered;
            } catch (...) { ++counters.metadataFailures; }
        }
    } catch (...) { ++counters.exceptions; }
}
CpuWallTime observeCpuTime() noexcept {
    ErrorGuard error;
    AcquireSRWLockShared(&observeCpuLock);
    const auto result = observeCpu;
    ReleaseSRWLockShared(&observeCpuLock);
    return result;
}
void summary() noexcept {
    ErrorGuard error;
    if (inside || !configurationReady.load(std::memory_order_acquire)) return;
    ReentryGuard reentry;
    try {
        std::ostringstream out;
        const auto cpu = observeCpuTime();
        out << ",\"gate\":\"" << gateName(gate.load(std::memory_order_acquire)) << "\",\"module_base\":";
        handle(out, configuration.module);
        out << ",\"detail_limit\":" << detailLimit << ",\"refresh_interval\":" << refreshInterval
            << ",\"metadata_consumer_configured\":" << (configuration.metadata ? "true" : "false")
            << ",\"continuous_invocation_consumer_configured\":" << (configuration.invocation ? "true" : "false")
            << ",\"max_graph_entries\":" << configuration.limits.maxGraphEntries << ",\"max_bytes\":" << configuration.limits.maxBytes
            << ",\"max_read_calls\":" << configuration.limits.maxReadCalls
            << ",\"callback_calls\":" << counters.calls.load() << ",\"detail_attempts\":" << counters.detailAttempts.load()
            << ",\"refresh_attempts\":" << counters.refreshAttempts.load() << ",\"probes\":" << counters.probes.load()
            << ",\"parsed\":" << counters.parsed.load() << ",\"rejected\":" << counters.rejected.load()
            << ",\"refresh_parsed\":" << counters.refreshParsed.load() << ",\"native_candidates\":" << counters.nativeCandidates.load()
            << ",\"read_calls\":" << counters.readCalls.load() << ",\"bytes_requested\":" << counters.bytesRequested.load()
            << ",\"bytes_copied\":" << counters.bytesCopied.load() << ",\"metadata_attempts\":" << counters.metadataAttempts.load()
            << ",\"metadata_delivered\":" << counters.metadataDelivered.load() << ",\"metadata_failures\":" << counters.metadataFailures.load()
            << ",\"detail_emitted\":" << counters.detailEmitted.load() << ",\"log_failures\":" << counters.logFailures.load()
            << ",\"exceptions\":" << counters.exceptions.load() << ",\"stopped_results\":" << counters.stoppedResults.load()
            << ",\"counters_atomic_snapshot\":false"
            << ",\"cpu_upscale_observe\":{\"count\":" << cpu.count
            << ",\"total_ns\":" << cpu.totalNs << ",\"max_ns\":" << cpu.maxNs << '}'
            << ",\"cpu_observe_aggregate_across_threads\":true,\"cpu_observe_original_game_excluded\":true";
        flags(out); emit("upscale_source_summary", out.str(), false);
    } catch (...) { ++counters.exceptions; }
}

#ifdef XESS_UPSCALE_CALLBACK_TEST
void resetForTest() noexcept {
    ErrorGuard error;
    gate.store(Gate::Cold); configurationReady.store(false); configuration = {}; afterProbeForTest = nullptr;
    counters.calls = 0; counters.detailAttempts = 0; counters.refreshAttempts = 0; counters.probes = 0;
    counters.parsed = 0; counters.rejected = 0; counters.refreshParsed = 0; counters.nativeCandidates = 0;
    counters.readCalls = 0; counters.bytesRequested = 0; counters.bytesCopied = 0;
    counters.metadataAttempts = 0; counters.metadataDelivered = 0; counters.metadataFailures = 0;
    counters.detailEmitted = 0; counters.logFailures = 0; counters.exceptions = 0; counters.stoppedResults = 0;
    observeCpu = {};
}
void setAfterProbeForTest(void (*hook)()) noexcept { ErrorGuard error; afterProbeForTest = hook; }
#endif
} // namespace xess_vk::upscale_callback_observer
extern "C" void upscale_observe_before(void* payload, void* context) noexcept {
    xess_vk::upscale_callback_observer::observe(payload, context);
}
