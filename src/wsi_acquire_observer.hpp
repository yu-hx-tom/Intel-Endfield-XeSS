#pragma once
#include "cpu_wall_time.hpp"
#include <array>
#include <cstdint>
#include <mutex>

namespace xess_vk::wsi_acquire {
enum class Api : unsigned { Acquire, Acquire2, Status, Count };
inline constexpr unsigned apiCount = static_cast<unsigned>(Api::Count);
inline constexpr unsigned valueLimit = 8, sampleLimitPerApi = 4, periodicSummaryLimit = 120;
inline constexpr std::uint64_t summaryInterval = 300;
inline constexpr std::uint64_t slowCallThresholdNs = 25'000'000, slowCallLimit = 256;
struct QueueCall {
    std::uint64_t entryNs{}, driverEntryNs{}, returnNs{};
    std::int32_t result{};
    bool slow() const noexcept {
        return returnNs - entryNs >= slowCallThresholdNs;
    }
};
// Non-owning device callback; called after releasing the queue gate, never across a GPU wait.
using QueueObserver = void (*)(void*, const char*, std::uint64_t, const QueueCall&) noexcept;
struct Call {
    Api api{};
    std::uint64_t swapchain{}, generation{}, timeout{}, entryNs{}, elapsedNs{};
    std::uint32_t thread{}, presentThread{}, imageIndex{}, deviceMask{};
    std::int32_t result{};
    bool argumentsKnown{}, semaphore{}, fence{}, imageIndexKnown{}, pNext{};
    CpuWallTime noSlotPollCpu, noSlotWaitCpu;
    QueueCall signal;
};
template<class T> struct ValueCount { T value{}; std::uint64_t count{}; };
struct ApiStats {
    std::uint64_t calls{}, totalNs{}, maxNs{}, timeoutValueOverflow{}, resultValueOverflow{};
    std::array<ValueCount<std::uint64_t>, valueLimit> timeouts{};
    std::array<ValueCount<std::int32_t>, valueLimit> results{};
    // none, semaphore only, fence only, both; unknown arguments are separate.
    std::array<std::uint64_t, 4> synchronization{};
    std::uint64_t unknownArguments{}, threadUnknown{}, threadSame{}, threadDifferent{};
    std::uint64_t pNextCalls{}, deviceMaskNotOneCalls{};
    Call latest{};
};
struct Snapshot {
    std::uint64_t calls{}, periodicSummaries{}, omittedPeriodicSummaries{};
    std::array<ApiStats, apiCount> api{};
};
struct Emission { bool sample{}, summary{}; };

// Metadata only; callers must release this lock before any driver or log call.
class Observer {
    mutable std::mutex mutex_;
    Snapshot totals_;
    template<class T> static void add(std::array<ValueCount<T>, valueLimit>& values, T value,
                                      std::uint64_t& overflow) noexcept {
        for (auto& entry : values) if (entry.count && entry.value == value) { ++entry.count; return; }
        for (auto& entry : values) if (!entry.count) { entry = {value, 1}; return; }
        ++overflow;
    }
public:
    Emission record(const Call& call) {
        std::lock_guard lock(mutex_);
        const auto index = static_cast<unsigned>(call.api);
        if (index >= apiCount) return {};
        auto& stats = totals_.api[index];
        ++totals_.calls; ++stats.calls;
        stats.totalNs += call.elapsedNs;
        if (call.elapsedNs > stats.maxNs) stats.maxNs = call.elapsedNs;
        add(stats.results, call.result, stats.resultValueOverflow);
        if (call.api != Api::Status) {
            if (call.argumentsKnown) {
                add(stats.timeouts, call.timeout, stats.timeoutValueOverflow);
                ++stats.synchronization[unsigned(call.semaphore) | (unsigned(call.fence) << 1)];
                if(call.api==Api::Acquire2){stats.pNextCalls+=call.pNext;stats.deviceMaskNotOneCalls+=call.deviceMask!=1;}
            } else ++stats.unknownArguments;
        }
        if (!call.presentThread) ++stats.threadUnknown;
        else if (call.thread == call.presentThread) ++stats.threadSame;
        else ++stats.threadDifferent;
        stats.latest = call;
        Emission out{stats.calls <= sampleLimitPerApi, false};
        if (totals_.calls % summaryInterval == 0) {
            if (totals_.periodicSummaries < periodicSummaryLimit) { ++totals_.periodicSummaries; out.summary = true; }
            else ++totals_.omittedPeriodicSummaries;
        }
        return out;
    }
    Snapshot snapshot() const { std::lock_guard lock(mutex_); return totals_; }
};
} // namespace xess_vk::wsi_acquire
