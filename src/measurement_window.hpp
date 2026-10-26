#pragma once
#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <mutex>

namespace xess_vk::measurement {
enum class Metric : unsigned { PresentEntryInterval, PresentCall, SourceSubmitInterval,
    CapWait, CopyFenceWait, ConsumedWait, DxgiPresent, TransferCommands, DepthDispatches,
    ProviderDepthGpu, ProviderDepthTransferGpu, ProviderAuxGpu, NativeGpu, BridgeGpu, CallerGpu, StatisticsEmitCpu, SrGpu, Count };
inline constexpr std::size_t metricCount = static_cast<unsigned>(Metric::Count);
inline constexpr std::size_t sampleCapacity = 256;
inline constexpr std::uint64_t summaryDurationNs = 60'000'000'000;
enum class Command : unsigned { ImageCopy, ImageToBuffer, BufferToImage, Blit, DepthDispatch, D3dCopy, Count };
using Commands = std::array<std::uint64_t,static_cast<unsigned>(Command::Count)>;
enum class CopyRole : unsigned { Color, Depth, Motion, Sdk, Unspecified, Count };
inline constexpr std::size_t copyRoleCount = static_cast<unsigned>(CopyRole::Count);
enum class Counter : unsigned { MenuEnabledFrames, MenuDisabledFrames, MenuUnknownFrames,
    SdkSingleFrames, SdkDoubleFrames, CopyFailures, NoFreeSlots, SdkErrors, SdkWarnings, DeviceLost,
    SrExecutes, SrSkips, SrSkipNotReady, SrSkipInputs, SrSkipRecording, SrSkipConfiguration,
    SrSkipSdk, SrSkipOther, SdkTripleFrames, SdkQuadFrames,
    FgRequestedFrames, FgEnabledFrames, FgMultiOutput, FgSingleOutput, FgZeroOutput,
    FgQueryUnavailable, FgHistoryResets, FgResultWarnings, FgResultErrors, Count };
inline constexpr std::size_t counterCount = static_cast<unsigned>(Counter::Count);
struct Value {std::uint64_t value{},key{},owner{};};
struct Samples {
    std::uint64_t count{}, totalNs{}, maximumNs{}, over15Ms{}, over40Ms{}, dropped{};
    std::uint32_t stored{};
    std::array<Value, sampleCapacity> values{};
    void add(std::uint64_t ns,std::uint64_t key=0,std::uint64_t owner=0) noexcept {
        ++count; totalNs += ns; if(ns>maximumNs)maximumNs=ns;
        over15Ms += ns>15'000'000; over40Ms += ns>40'000'000;
        if(stored<values.size())values[stored++]={ns,key,owner}; else ++dropped;
    }
};
struct Snapshot {
    std::uint64_t sequence{}, beginNs{}, endNs{};
    std::array<Samples,metricCount> metrics{};
};
struct Percentile {
    std::uint64_t lowerNs{}, upperNs{};
};
struct SummarySamples {
    std::uint64_t count{}, totalNs{}, maximumNs{}, over15Ms{}, over25Ms{}, over40Ms{}, over100Ms{};
    Percentile p10, p50, p90, p95, p99;
    bool totalSaturated{};
};
struct SummarySnapshot {
    std::uint64_t sequence{}, beginNs{}, endNs{};
    std::array<SummarySamples,metricCount> metrics{};
    std::array<std::uint64_t,counterCount> counters{};
    std::array<Commands,copyRoleCount> copyCommands{};
};
// All values contribute. Bounds retain eight mantissa bits, giving <0.4%
// relative error above 511 ns; this is never a percentile of the detail sample.
inline constexpr unsigned histogramPrecisionBits = 8;
class Histogram {
    static constexpr std::size_t mantissas = 1u << histogramPrecisionBits;
    static constexpr std::size_t bucketCount = (65-histogramPrecisionBits)*mantissas;
    std::array<std::uint64_t,bucketCount> buckets_{};
    SummarySamples totals_;
    static std::size_t index(std::uint64_t value) noexcept {
        if(value<mantissas)return static_cast<std::size_t>(value);
        const auto shift=std::bit_width(value)-1-histogramPrecisionBits;
        return static_cast<std::size_t>(shift)*mantissas+static_cast<std::size_t>(value>>shift);
    }
    static Percentile bounds(std::size_t bucket) noexcept {
        if(bucket<mantissas)return {bucket,bucket};
        const auto shift=static_cast<unsigned>(bucket/mantissas-1);
        const auto lower=static_cast<std::uint64_t>(mantissas+bucket%mantissas)<<shift;
        return {lower,lower+((std::uint64_t{1}<<shift)-1)};
    }
public:
    void clear() noexcept {buckets_.fill(0);totals_={};}
    void add(std::uint64_t ns) noexcept {
        ++totals_.count;
        if(ns>std::numeric_limits<std::uint64_t>::max()-totals_.totalNs){
            totals_.totalNs=std::numeric_limits<std::uint64_t>::max();totals_.totalSaturated=true;
        } else totals_.totalNs+=ns;
        if(ns>totals_.maximumNs)totals_.maximumNs=ns;
        totals_.over15Ms+=ns>15'000'000;totals_.over25Ms+=ns>25'000'000;
        totals_.over40Ms+=ns>40'000'000;totals_.over100Ms+=ns>100'000'000;
        ++buckets_[index(ns)];
    }
    SummarySamples summary() const noexcept {
        auto out=totals_;
        if(!out.count)return out;
        const std::array<unsigned,5> percentages{10,50,90,95,99};
        const std::array<Percentile*,5> output{&out.p10,&out.p50,&out.p90,&out.p95,&out.p99};
        std::size_t next{};std::uint64_t accumulated{};
        for(std::size_t bucket=0;bucket<buckets_.size()&&next<output.size();++bucket){
            accumulated+=buckets_[bucket];
            while(next<output.size()){
                const auto percent=percentages[next];
                const auto rank=(out.count/100)*percent+((out.count%100)*percent+99)/100;
                if(accumulated<rank)break;
                *output[next++]=bounds(bucket);
            }
        }
        return out;
    }
};
class Window {
    std::mutex mutex_;
    Snapshot current_;
    std::array<Histogram,metricCount> summaryMetrics_{};
    SummarySnapshot summary_;
public:
    explicit Window(std::uint64_t start=0) noexcept {current_.beginNs=summary_.beginNs=start;}
    void add(Metric metric,std::uint64_t ns,std::uint64_t key=0,std::uint64_t owner=0) noexcept {
        if(static_cast<unsigned>(metric)>=metricCount)return;
        std::lock_guard lock(mutex_);
        current_.metrics[static_cast<unsigned>(metric)].add(ns,key,owner);
        summaryMetrics_[static_cast<unsigned>(metric)].add(ns);
    }
    void increment(Counter counter,std::uint64_t amount=1) noexcept {
        if(static_cast<unsigned>(counter)>=counterCount)return;
        std::lock_guard lock(mutex_);
        summary_.counters[static_cast<unsigned>(counter)]+=amount;
    }
    void addCommands(CopyRole role,const Commands& commands) noexcept {
        if(static_cast<unsigned>(role)>=copyRoleCount)return;
        std::lock_guard lock(mutex_);
        auto& target=summary_.copyCommands[static_cast<unsigned>(role)];
        for(std::size_t i=0;i<target.size();++i)target[i]+=commands[i];
    }
    bool take(std::uint64_t now,Snapshot& out,bool force=false) noexcept {
        std::lock_guard lock(mutex_);
        if(!current_.beginNs){current_={};current_.beginNs=now;return false;}
        if(now<current_.beginNs||(!force&&now-current_.beginNs<1'000'000'000))return false;
        current_.endNs=now;out=current_;
        const auto next=current_.sequence+1;
        current_={};current_.sequence=next;current_.beginNs=now;
        return true;
    }
    bool takeSummary(std::uint64_t now,SummarySnapshot& out,bool force=false) noexcept {
        std::lock_guard lock(mutex_);
        if(!summary_.beginNs){summary_.beginNs=now;return false;}
        if(now<summary_.beginNs||(!force&&now-summary_.beginNs<summaryDurationNs))return false;
        summary_.endNs=now;
        for(std::size_t i=0;i<summaryMetrics_.size();++i)summary_.metrics[i]=summaryMetrics_[i].summary();
        out=summary_;
        const auto next=summary_.sequence+1;
        summary_={};summary_.sequence=next;summary_.beginNs=now;
        for(auto& histogram:summaryMetrics_)histogram.clear();
        return true;
    }
};
// Only a live wrapper scope publishes this thread-local pointer; no retained backend owns it.
inline thread_local Window* active{};
inline thread_local Commands* activeCommands{};
class Scope {
    Window* previous_{};
    Commands* previousCommands_{};
    Commands commands_{};
public:
    explicit Scope(Window* value) noexcept:previous_(active),previousCommands_(activeCommands){active=value;activeCommands=value?&commands_:nullptr;}
    ~Scope(){active=previous_;activeCommands=previousCommands_;}
    const Commands& commands() const noexcept{return commands_;}
    Scope(const Scope&)=delete;
    Scope& operator=(const Scope&)=delete;
};
inline void observe(Metric metric,std::uint64_t ns,std::uint64_t key=0,std::uint64_t owner=0) noexcept {
    if(active)active->add(metric,ns,key,owner);
}
inline void command(Command kind) noexcept {if(activeCommands)++(*activeCommands)[static_cast<unsigned>(kind)];}
inline void increment(Counter counter,std::uint64_t amount=1) noexcept {if(active)active->increment(counter,amount);}
inline void addCommands(CopyRole role,const Commands& commands) noexcept {if(active)active->addCommands(role,commands);}
inline void submittedCommand(CopyRole role,Command kind) noexcept {
    if(!active)return;
    Commands commands{};commands[static_cast<unsigned>(kind)]=1;active->addCommands(role,commands);
}
} // namespace xess_vk::measurement
