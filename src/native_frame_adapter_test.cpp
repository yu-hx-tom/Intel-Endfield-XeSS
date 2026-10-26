// CPU-only: exercise the exact adapter used by embedded native presentation.
// No Presenter/InteropBridge instance, DLL load, SDK call or GPU operation.
#define XESS_PRESENTER_EMBEDDED
#include "presenter.cpp"
#include <limits>

namespace {
unsigned adapterChecks{};
void requireAdapter(bool condition, const char* reason) {
    ++adapterChecks;
    if (!condition) throw std::runtime_error(reason);
}
xess_vk::frame_input::FrameConstantsSample sample() {
    namespace fi = xess_vk::frame_input;
    fi::FrameConstantsSample s;
    s.validityMask = fi::View | fi::Projection | fi::Jitter | fi::MotionScale | fi::Extents;
    s.view = {0.8f,0,-0.6f,0, 0,1,0,0, 0.6f,0,0.8f,0, -7,2,-3,1};
    s.projection = {1.3f,0,0,0, 0,2.1f,0,0, 0,0,0.0002f,-1, 0,0,0.10002f,0};
    s.jitter = {-0.5f, 0.5f}; s.motionVectorScale = {0.25f, -0.75f};
    s.renderSize = {640, 360}; s.upscaleSize = {1280, 720};
    s.deltaSeconds = std::numeric_limits<float>::quiet_NaN(); s.reset = 99;
    return s;
}
template<class Mutation> void rejects(Mutation mutate, const char* reason) {
    auto s = sample(); mutate(s);
    bool refused{};
    try { (void)nativeFrameConstants(s, false); } catch (const std::invalid_argument&) { refused = true; }
    requireAdapter(refused, reason);
}
void callerCommandCompletion() {
    struct CompletionFailure : std::runtime_error { using std::runtime_error::runtime_error; };
    struct Bridge {
        std::uint64_t value{};
        bool signalled{}, fail{};
        unsigned calls{};
        void waitUntilConsumed() {
            ++calls;
            if (!value) return;
            if (!signalled) throw CompletionFailure("consumed signal missing");
            if (fail) throw CompletionFailure("consumed wait failed");
        }
    } bridge;
    xess_vk::CpuWallTime cpu;
    waitForCallerCommands(bridge,cpu);
    requireAdapter(bridge.calls==1 && cpu.count==1,"No prior publication must not require a completion signal");
    bridge.value=1;bridge.signalled=true;
    waitForCallerCommands(bridge,cpu);
    requireAdapter(bridge.calls==2 && cpu.count==2 && cpu.maxNs<=cpu.totalNs,
                   "Actual caller-list wait must be retained in the existing cumulative wall counter");
    for(const bool signalled : {false,true}) {
        bridge.signalled=signalled;bridge.fail=signalled;
        const auto before=cpu.count;bool resetEntered{},failed{};
        try { waitForCallerCommands(bridge,cpu);resetEntered=true; }
        catch(const CompletionFailure& error) {
            failed=std::string(error.what())==(signalled?"consumed wait failed":"consumed signal missing");
        }
        requireAdapter(failed && !resetEntered && cpu.count==before+1,
                       "Missing signal or failed completion propagates unchanged before allocator reuse and still records CPU wall");
    }
}
void exactFrameReceipts() {
    using namespace xess_vk;
    using State = FrameCompletionState;
    struct Fence { std::uint64_t done{}; unsigned queries{}; std::uint64_t GetCompletedValue() noexcept { ++queries; return done; } } fence;
    FrameCompletion a{{0,1}}, b{{1,1}};
    requireAdapter(detail::freshFrameCompletion(a) && detail::freshFrameCompletion(b), "Both caller-selected K2 slots start fresh");
    requireAdapter(detail::frameSlotCanReuse({}, a.key, State::Invalid) && detail::frameSlotCanReuse({}, b.key, State::Invalid),
                   "First reservation must not require an invented previous completion");
    for (auto* receipt : {&a,&b}) {
        receipt->producedValue = receipt->consumedValue = receipt->key.index + 1;
        receipt->sdkTailValue = 10 + receipt->key.index;
        receipt->producedValid = receipt->consumedValid = receipt->sdkTailValid = true;
        receipt->d3dWaitQueued = receipt->callerSubmitted = true;
    }
    requireAdapter(frameReceiptState(&fence,a,a) == State::Pending && frameReceiptState(&fence,b,b) == State::Pending,
                   "Two unfinished frames must both remain pending without an event wait");
    requireAdapter(!detail::frameSlotCanReuse(a.key,{0,2},State::Pending) && !detail::frameSlotCanReuse(b.key,{1,2},State::Pending),
                   "Third reservation cannot reset either pending slot");
    fence.done = a.sdkTailValue;
    requireAdapter(frameReceiptState(&fence,a,a) == State::Complete && frameReceiptState(&fence,b,b) == State::Pending,
                   "Completion of one receipt cannot borrow the other slot's latest fence");
    requireAdapter(detail::frameSlotCanReuse(a.key,{0,2},State::Complete) &&
        !detail::frameSlotCanReuse(a.key,a.key,State::Complete) && !detail::frameSlotCanReuse(a.key,{1,2},State::Complete) &&
        !detail::frameSlotCanReuse(a.key,{0,0},State::Complete), "Reuse must keep the same index and advance its nonzero generation");
    const auto before = fence.queries;
    for (unsigned mutation = 0; mutation < 11; ++mutation) {
        auto wrong = a;
        if (mutation == 0) wrong.key = b.key;
        if (mutation == 1) ++wrong.key.generation;
        if (mutation == 2) ++wrong.producedValue;
        if (mutation == 3) ++wrong.consumedValue;
        if (mutation == 4) ++wrong.sdkTailValue;
        if (mutation == 5) wrong.sdkTailValid = false;
        if (mutation == 6) wrong.callerSubmitted = false;
        if (mutation == 7) wrong.poisoned = true;
        if (mutation == 8) wrong.sharedKey = {2,1,1};
        if (mutation == 9) wrong.sharedAuxiliary = true;
        if (mutation == 10) wrong.postPresentSubmitted = true;
        requireAdapter(frameReceiptState(&fence,a,wrong) == State::Invalid, "Stale, mismatched or partial receipt must not prove completion");
    }
    requireAdapter(fence.queries == before, "Invalid receipts must not query a different/latest fence");
    auto direct = a; direct.sharedKey = {2,3,7}; direct.sharedAuxiliary = true;
    requireAdapter(frameReceiptState(&fence,direct,direct) == State::Invalid,
                   "Shared N3 slot cannot retire on the pre-Present caller receipt");
    direct.postPresentSubmitted = true;
    requireAdapter(frameReceiptState(&fence,direct,direct) == State::Complete,
                   "Shared N3 index 2 retires independently of the K2 command index only after the exact post-Present tail");
    for (unsigned mutation = 0; mutation < 7; ++mutation) {
        auto wrong = direct;
        if (mutation == 0) ++wrong.sharedKey.ringGeneration;
        if (mutation == 1) ++wrong.sharedKey.leaseGeneration;
        if (mutation == 2) --wrong.sharedKey.index;
        if (mutation == 3) wrong.sharedAuxiliary = false;
        if (mutation == 4) wrong.postPresentSubmitted = false;
        if (mutation == 5) wrong.consumedValid = false;
        if (mutation == 6) wrong.sdkTailValid = false;
        requireAdapter(frameReceiptState(&fence,direct,wrong) == State::Invalid,
                       "Shared generation/mode/partial-stage substitutions cannot borrow another frame's completion");
    }
    for (const auto invalid : {SharedPlaneKey{3,1,1}, SharedPlaneKey{0,0,1}, SharedPlaneKey{0,1,0}}) {
        auto malformed = direct; malformed.sharedKey = invalid; malformed.sharedAuxiliary = false;
        requireAdapter(frameReceiptState(&fence,malformed,malformed) == State::Invalid,
                       "Noncanonical shared keys cannot masquerade as an old legacy receipt");
        FrameCompletion fresh{a.key}; fresh.sharedKey = invalid;
        requireAdapter(!detail::freshFrameCompletion(fresh), "Malformed shared keys must be rejected before admission");
    }
    for (const auto invalid : {std::uint64_t{0}, UINT64_MAX}) {
        auto malformed = a; malformed.producedValue = malformed.consumedValue = invalid;
        requireAdapter(frameReceiptState(&fence,malformed,malformed) == State::Invalid,
                       "Missing or removal-sentinel bridge timelines cannot prove retirement");
    }
    auto partial = a; partial.sdkTailValid = false; partial.sdkTailValue = 0;
    requireAdapter(frameReceiptState(&fence,partial,partial) == State::Invalid && !detail::freshFrameCompletion(partial),
                   "Successful bridge/caller submissions without post-Present tail stay owned, never fresh or complete");
    fence.done = UINT64_MAX;
    requireAdapter(frameReceiptState(&fence,a,a) == State::DeviceLost &&
        !detail::frameSlotCanReuse(a.key,{0,2},State::DeviceLost), "D3D12 removal sentinel cannot free a frame slot");

    auto stored = a;
    FrameCompletion rejected{a.key};
    poisonPreparedFrameReceipt(rejected, &stored, false);
    requireAdapter(sameFrameReceipt(stored,a) && detail::freshFrameCompletion(rejected),
                   "Rejected duplicate slot admission must preserve the original in-flight receipt and the rejected caller");
    poisonPreparedFrameReceipt(stored, &stored, false);
    requireAdapter(sameFrameReceipt(stored,a), "Rejected reuse of the original receipt must not poison its real completion proof");
    for (unsigned stage = 1; stage <= 5; ++stage) {
        FrameCompletion submitted{a.key};
        submitted.producedValue = 7; submitted.producedValid = true;
        if (stage >= 2) submitted.d3dWaitQueued = true;
        if (stage >= 3) submitted.callerSubmitted = true;
        if (stage >= 4) { submitted.consumedValue = 7; submitted.consumedValid = true; }
        if (stage >= 5) { submitted.sdkTailValue = 12; submitted.sdkTailValid = true; }
        const auto beforeFailure = submitted;
        stored = {a.key};
        poisonPreparedFrameReceipt(submitted, &stored, true);
        auto expected = beforeFailure; expected.poisoned = true;
        requireAdapter(sameFrameReceipt(submitted,expected) && sameFrameReceipt(stored,expected) &&
            frameReceiptState(&fence,stored,submitted) == State::Invalid,
            "An admitted failure must retain every actual submitted stage and quarantine without inventing completion");
    }
    stored = b;
    auto wrongSlot = a;
    poisonPreparedFrameReceipt(wrongSlot, &stored, true);
    requireAdapter(wrongSlot.poisoned && sameFrameReceipt(stored,b), "Poison cannot overwrite a different prepared key");
}
void sdkTailExhaustion() {
    requireAdapter(nextSdkTailValue(0) == 1 && nextSdkTailValue(UINT64_MAX - 2) == UINT64_MAX - 1,
                   "Tail allocation must preserve the full non-sentinel timeline range");
    for (const auto previous : {UINT64_MAX - 1, UINT64_MAX}) {
        bool rejected{};
        try { (void)nextSdkTailValue(previous); }
        catch (const std::overflow_error&) { rejected = true; }
        requireAdapter(rejected, "Present and shutdown must both reject removal sentinel or wrapped tail values before Signal");
    }
}
void callerGpuTimestampLifecycle() {
    struct Resources {
        unsigned creates{}, begins{}, resolves{}, reads{}, calibrations{};
        bool supported{true}, readable{true}, completed{}, calibrated{true};
        std::uint64_t frequencyHz{1'000'000}, qpcFrequency{10'000'000}, now{1000};
        bool initialize() noexcept { ++creates; return supported; }
        void begin() noexcept { ++begins; }
        void endAndResolve() noexcept { ++resolves; }
        std::uint64_t qpcNow() noexcept { return ++now; }
        xess_vk::GpuTimestampCalibration calibrate() noexcept {
            ++calibrations;
            xess_vk::GpuTimestampCalibration calibration;
            calibration.available = calibrated;
            calibration.gpuTicks = 100;
            calibration.qpcTicks = 1000;
            return calibration;
        }
        bool read(std::uint64_t& begin, std::uint64_t& end) noexcept {
            ++reads;
            if (!completed || !readable) return false;
            begin = 100;
            end = 125;
            return true;
        }
    } off;
    xess_vk::detail::CallerGpuTimestamps disabled;
    disabled.initialize(false, off);
    disabled.begin(off, 1); disabled.end(off); disabled.submitted(off); disabled.completed(off);
    requireAdapter(!disabled.latest().requested && !off.creates && !off.begins && !off.resolves &&
        !off.reads && !off.calibrations, "Default OFF allocated or accessed a GPU timestamp");

    Resources resources;
    xess_vk::detail::CallerGpuTimestamps timestamps;
    timestamps.initialize(true, resources);
    timestamps.completed(resources);
    requireAdapter(timestamps.latest().requested && !resources.reads,
        "No submitted caller list must not read an unresolved query");
    timestamps.begin(resources, 17); timestamps.end(resources);
    timestamps.completed(resources);
    requireAdapter(!resources.reads, "Recorded but unsubmitted query was read");
    timestamps.submitted(resources);
    timestamps.begin(resources, 99); timestamps.end(resources);
    requireAdapter(resources.begins == 1 && resources.resolves == 1 && !resources.reads &&
        !timestamps.latest().available, "Pending query was overwritten or read without completion");
    resources.completed = true;
    timestamps.completed(resources);
    const auto first = timestamps.latest();
    requireAdapter(first.available && first.durationKnown && first.durationNs == 25000 &&
        first.sdkPresentIdKnown && first.sdkPresentId == 17 && first.sequence == 1 && first.generation == 1 &&
        !first.sourceCopyTokenKnown && !first.sourceEngineFrameKnown && first.validBits == 64 &&
        first.frequencyHz == resources.frequencyHz && first.submitQpcTicks < first.fenceObservedQpcTicks &&
        first.calibration.available && !first.calibration.maxDeviationKnown,
        "Delayed caller sample lost its recording identity, duration, or unknown calibration error");
    timestamps.completed(resources);
    requireAdapter(resources.reads == 1, "Completed caller sample was read twice");
    timestamps.rebuilt();
    requireAdapter(timestamps.latest().sdkPresentId == 17 && timestamps.latest().generation == 1,
        "Resize relabeled a historical sample");
    resources.calibrated = false;
    timestamps.begin(resources, 23); timestamps.end(resources); timestamps.submitted(resources);
    timestamps.completed(resources); // The same completion path is used by the existing shutdown full-tail wait.
    requireAdapter(timestamps.latest().sdkPresentId == 23 && timestamps.latest().sequence == 2 &&
        timestamps.latest().generation == 2 && timestamps.latest().durationKnown &&
        !timestamps.latest().calibration.available,
        "Rebuilt/exit sample or optional calibration failure changed duration collection");

    resources.readable = false;
    timestamps.begin(resources, 24); timestamps.end(resources); timestamps.submitted(resources);
    timestamps.completed(resources);
    const auto begins = resources.begins;
    timestamps.begin(resources, 25); timestamps.end(resources); timestamps.submitted(resources);
    timestamps.completed(resources);
    requireAdapter(timestamps.latest().requested && !timestamps.latest().available &&
        !timestamps.latest().durationKnown && timestamps.latest().sdkPresentId == 24 && resources.begins == begins,
        "Readback failure did not disable only the diagnostic");
    Resources unsupported;
    unsupported.supported = false;
    xess_vk::detail::CallerGpuTimestamps unavailable;
    unavailable.initialize(true, unsupported);
    unavailable.begin(unsupported, 1); unavailable.end(unsupported); unavailable.submitted(unsupported);
    unavailable.completed(unsupported);
    requireAdapter(unavailable.latest().requested && !unavailable.latest().available && unsupported.creates == 1 &&
        !unsupported.begins && !unsupported.resolves && !unsupported.reads,
        "Unsupported/failed timestamp resources were used");
    Resources firstSlot, secondSlot;
    xess_vk::detail::CallerGpuTimestamps firstTimer, secondTimer;
    firstTimer.initialize(true, firstSlot); secondTimer.initialize(true, secondSlot);
    firstTimer.begin(firstSlot, 31, 101, 7); firstTimer.end(firstSlot); firstTimer.submitted(firstSlot);
    secondTimer.begin(secondSlot, 32, 102, 7); secondTimer.end(secondSlot); secondTimer.submitted(secondSlot);
    requireAdapter(firstSlot.reads == 0 && secondSlot.reads == 0 && firstSlot.begins == 1 && secondSlot.begins == 1,
                   "Independent slots must both remain pending without sharing query/readback recording state");
    secondSlot.completed = true; secondTimer.completed(secondSlot);
    firstTimer.rebuilt(); // A misuse must preserve pending ownership rather than allow an early reset.
    const auto firstBegins = firstSlot.begins;
    firstTimer.begin(firstSlot, 99, 103, 8);
    requireAdapter(firstSlot.begins == firstBegins && firstSlot.reads == 0, "Rebuild cannot discard a pending query or permit early reuse");
    firstSlot.completed = true; firstTimer.completed(firstSlot);
    requireAdapter(firstTimer.latest().sdkPresentId == 31 && firstTimer.latest().sequence == 101 && firstTimer.latest().generation == 7 &&
        secondTimer.latest().sdkPresentId == 32 && secondTimer.latest().sequence == 102 && secondTimer.latest().generation == 7,
        "Reverse retirement or a slot first initialized after resize lost owner serial/resource generation/source identity");
    firstTimer.completed(firstSlot); secondTimer.completed(secondSlot);
    requireAdapter(firstSlot.reads == 1 && secondSlot.reads == 1, "Per-slot completion cannot read resolved queries twice");
}
void directSharedInputs() {
    struct Config {
        std::uint32_t width{1920}, height{1200};
        VkExtent2D input{1128,634};
        VkExtent2D extent(xess_vk::Format format) const {
            return format == xess_vk::Format::ColorRGBA8 ? VkExtent2D{width,height} : input;
        }
    };
    const auto resource = [](std::uintptr_t id) { return reinterpret_cast<ID3D12Resource*>(id); };
    struct Bridge {
        Config cfg;
        std::array<ID3D12Resource*,3> textures;
        std::vector<unsigned> requests;
        xess_vk::SharedPlaneKey expectedKey;
        const Config& config() const { return cfg; }
        const Config& config(xess_vk::SharedPlaneKey key) const {
            requireAdapter(key == expectedKey, "Shared tagging must read the exact ring/lease extent"); return cfg;
        }
        ID3D12Resource* texture(xess_vk::Format format) {
            const auto index = static_cast<unsigned>(format);
            requests.push_back(index);
            requireAdapter(textures[index] != nullptr, "color-only OFF must never request absent shared inputs");
            return textures[index];
        }
        ID3D12Resource* texture(xess_vk::Format format, xess_vk::SharedPlaneKey key) {
            requireAdapter(key == expectedKey, "Shared recording must select the exact N3 lease"); return texture(format);
        }
    } bridge{{},{resource(1),resource(2),resource(3)},{}};
    struct List {
        std::map<ID3D12Resource*,D3D12_RESOURCE_STATES> states;
        std::vector<std::pair<ID3D12Resource*,ID3D12Resource*>> copies;
        unsigned barriers{};
        void ResourceBarrier(UINT count,const D3D12_RESOURCE_BARRIER* values) {
            requireAdapter(count==1, "actual recording emits one transition at each shared-resource boundary");
            const auto& b=values[0];const auto& t=b.Transition;
            requireAdapter(b.Type==D3D12_RESOURCE_BARRIER_TYPE_TRANSITION&&
                t.Subresource==D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES&&states[t.pResource]==t.StateBefore,
                "production barrier uses the resource's actual incoming state");
            states[t.pResource]=t.StateAfter;++barriers;
        }
        void CopyResource(ID3D12Resource* destination,ID3D12Resource* source) {
            requireAdapter(states[source]==D3D12_RESOURCE_STATE_COPY_SOURCE&&states[destination]==D3D12_RESOURCE_STATE_COPY_DEST,
                "color copy remains between valid source/destination states");
            copies.emplace_back(destination,source);
        }
    };
    const auto backbuffer=resource(4);
    const auto record = [&](bool enabled,int failTag,std::vector<xefg_swapchain_d3d12_resource_data_t>& tags,List& list) {
        const auto region=backbufferRegionTag({bridge.cfg.width,bridge.cfg.height},{});
        recordPublishedResources(&list,bridge,backbuffer,enabled,region,[&](const auto& tag) {
            tags.push_back(tag);
            if(tag.type!=XEFG_SWAPCHAIN_RES_BACKBUFFER) {
                const auto index=tag.type==XEFG_SWAPCHAIN_RES_DEPTH?1u:2u;
                requireAdapter(tag.pResource==bridge.textures[index]&&tag.validity==XEFG_SWAPCHAIN_RV_ONLY_NOW&&
                    tag.incomingState==D3D12_RESOURCE_STATE_COPY_SOURCE&&list.states[tag.pResource]==tag.incomingState,
                    "actual SDK tag receives the current shared texture in ONLY_NOW COPY_SOURCE state");
                requireAdapter(tag.resourceBase.x==0&&tag.resourceBase.y==0&&tag.resourceSize.x==bridge.cfg.input.width&&
                    tag.resourceSize.y==bridge.cfg.input.height,"tag uses the live shared input extent, not the output extent");
            }
            sdkCheck(static_cast<int>(tag.type)==failTag?XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_ARGUMENT:XEFG_SWAPCHAIN_RESULT_SUCCESS,
                "inert TagFrameResource");
        });
    };
    for(unsigned phase=0;phase<2;++phase) {
        if(phase) {bridge.textures={resource(11),resource(12),resource(13)};bridge.cfg={1280,800,{640,360}};}
        List list;std::vector<xefg_swapchain_d3d12_resource_data_t> tags;bridge.requests.clear();
        record(true,-1,tags,list);
        requireAdapter(list.copies==std::vector<std::pair<ID3D12Resource*,ID3D12Resource*>>{{backbuffer,bridge.textures[0]}}&&
            list.barriers==8,"production ON retains one color copy and deletes both shared-to-private input copies");
        requireAdapter(tags.size()==3&&tags[0].type==XEFG_SWAPCHAIN_RES_BACKBUFFER&&tags[1].type==XEFG_SWAPCHAIN_RES_DEPTH&&
            tags[2].type==XEFG_SWAPCHAIN_RES_MOTION_VECTOR,"backbuffer/depth/motion SDK tagging order is preserved");
        requireAdapter(bridge.requests==std::vector<unsigned>({0,1,2})&&
            list.states[bridge.textures[0]]==D3D12_RESOURCE_STATE_COMMON&&
            list.states[bridge.textures[1]]==D3D12_RESOURCE_STATE_COMMON&&list.states[bridge.textures[2]]==D3D12_RESOURCE_STATE_COMMON&&
            list.states[backbuffer]==D3D12_RESOURCE_STATE_PRESENT,"all shared textures return COMMON before the original consumed signal");
    }
    for(bool colorOnly:{false,true}) {
        if(colorOnly)bridge.textures[1]=bridge.textures[2]=nullptr;
        List list;std::vector<xefg_swapchain_d3d12_resource_data_t> tags;bridge.requests.clear();
        record(false,-1,tags,list);
        requireAdapter(tags.empty()&&bridge.requests==std::vector<unsigned>{0}&&list.copies.size()==1&&list.barriers==4,
            "both three-plane synthetic OFF and color-only native OFF copy color without any auxiliary lookup/tag");
    }
    bridge.textures={resource(21),resource(22),resource(23)};
    for(const auto failTag:{XEFG_SWAPCHAIN_RES_BACKBUFFER,XEFG_SWAPCHAIN_RES_DEPTH,XEFG_SWAPCHAIN_RES_MOTION_VECTOR}) {
        List list;std::vector<xefg_swapchain_d3d12_resource_data_t> tags;unsigned submitted{};bool refused{};
        try { record(true,static_cast<int>(failTag),tags,list);++submitted; }
        catch(const std::runtime_error&) {refused=true;}
        requireAdapter(refused&&!submitted&&tags.back().type==failTag,
            "SDK tag failure propagates from production recording before caller list submission");
    }
    for (unsigned index = 0; index < xess_vk::sharedPlaneSlotCount; ++index) {
        bridge.expectedKey = {index,9,20+index};
        bridge.textures = {resource(30+index*3),resource(31+index*3),resource(32+index*3)};
        for (const bool enabled : {false,true}) {
            List caller, post; bridge.requests.clear();
            recordSharedPublishedResources(&caller,bridge,bridge.expectedKey,backbuffer,enabled);
            requireAdapter(caller.copies == std::vector<std::pair<ID3D12Resource*,ID3D12Resource*>>{{backbuffer,bridge.textures[0]}} &&
                caller.barriers == (enabled ? 6u : 4u), "Direct shared caller records one color copy and no auxiliary copies");
            requireAdapter(caller.states[bridge.textures[0]] == D3D12_RESOURCE_STATE_COMMON &&
                caller.states[backbuffer] == D3D12_RESOURCE_STATE_PRESENT,
                "Direct shared color/backbuffer return to COMMON/PRESENT in the caller list");
            if (!enabled) {
                requireAdapter(bridge.requests == std::vector<unsigned>{0},
                    "OFF, including an auxiliary-capable lease, does not access or transition auxiliary resources");
                continue;
            }
            std::vector<xefg_swapchain_d3d12_resource_data_t> tags;
            tagSharedPublishedResources(bridge,bridge.expectedKey,backbufferRegionTag({bridge.cfg.width,bridge.cfg.height},{}),
                [&](ID3D12GraphicsCommandList* commandList,const auto& tag) {
                    requireAdapter(!commandList, "UNTIL_NEXT_PRESENT SDK tagging must use a null command list");
                    tags.push_back(tag);
                    if (tag.type != XEFG_SWAPCHAIN_RES_BACKBUFFER) {
                        const unsigned plane = tag.type == XEFG_SWAPCHAIN_RES_DEPTH ? 1u : 2u;
                        requireAdapter(tag.pResource == bridge.textures[plane] &&
                            tag.validity == XEFG_SWAPCHAIN_RV_UNTIL_NEXT_PRESENT &&
                            tag.incomingState == D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE &&
                            caller.states[tag.pResource] == tag.incomingState &&
                            tag.resourceSize.x == bridge.cfg.input.width && tag.resourceSize.y == bridge.cfg.input.height,
                            "Shared SDK tags pin the selected NPSR planes and their input extent until Present");
                    }
                });
            requireAdapter(tags.size() == 3 && tags[0].type == XEFG_SWAPCHAIN_RES_BACKBUFFER &&
                caller.states[bridge.textures[1]] == D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE &&
                caller.states[bridge.textures[2]] == D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                "Tagging leaves both auxiliary inputs in NPSR instead of restoring before Present");
            post.states = caller.states;
            restoreSharedPublishedResources(&post,bridge,bridge.expectedKey);
            requireAdapter(post.copies.empty() && post.barriers == 2 &&
                post.states[bridge.textures[1]] == D3D12_RESOURCE_STATE_COMMON &&
                post.states[bridge.textures[2]] == D3D12_RESOURCE_STATE_COMMON,
                "The separate post-Present list restores both exact auxiliary resources without copying");
        }
    }
    List partial;
    recordSharedPublishedResources(&partial,bridge,bridge.expectedKey,backbuffer,true);
    bool failed{}; unsigned tags{};
    try {
        tagSharedPublishedResources(bridge,bridge.expectedKey,backbufferRegionTag({bridge.cfg.width,bridge.cfg.height},{}),
            [&](ID3D12GraphicsCommandList*,const auto&) {
                if (++tags == 2) throw std::runtime_error("injected shared tag failure after caller submission");
            });
    } catch (const std::runtime_error&) { failed = true; }
    requireAdapter(failed && tags == 2 && partial.states[bridge.textures[1]] == D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE &&
        partial.states[bridge.textures[2]] == D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        "A tag failure after caller recording propagates and cannot pretend the auxiliary resources were restored");
}
}
int main() {
    try {
        callerCommandCompletion();
        exactFrameReceipts();
        sdkTailExhaustion();
        callerGpuTimestampLifecycle();
        directSharedInputs();
        auto s = sample();
        const auto c = nativeFrameConstants(s, false);
        requireAdapter(std::memcmp(c.viewMatrix, s.view.data(), sizeof(c.viewMatrix)) == 0 &&
            std::memcmp(c.projectionMatrix, s.projection.data(), sizeof(c.projectionMatrix)) == 0,
            "real nonidentity row-major matrices were changed");
        requireAdapter(c.viewMatrix[12] == -7 && c.viewMatrix[14] == -3 &&
            c.projectionMatrix[11] == -1 && c.projectionMatrix[15] == 0,
            "adapter inserted identity or transposed camera matrices");
        requireAdapter(c.jitterOffsetX == -0.5f && c.jitterOffsetY == 0.5f, "already-adapted jitter was negated or clamped");
        requireAdapter(c.motionVectorScaleX == 0.5f && c.motionVectorScaleY == 1.5f, "UV-to-NDC scale or Y sign incorrect");
        requireAdapter(c.frameRenderTime == 0 && c.resetHistory == 0 && nativeFrameConstants(s, true).resetHistory == 1,
            "render duration invented or reset argument ignored");
        requireAdapter(continuousInitFlags == (XEFG_SWAPCHAIN_INIT_FLAG_INVERTED_DEPTH |
            XEFG_SWAPCHAIN_INIT_FLAG_USE_NDC_VELOCITY | XEFG_SWAPCHAIN_INIT_FLAG_JITTERED_MV) &&
            !(continuousInitFlags & XEFG_SWAPCHAIN_INIT_FLAG_HIGH_RES_MV), "continuous SDK input flags disagree");
        rejects([](auto& v) { v.validityMask &= ~xess_vk::frame_input::View; }, "missing real view accepted");
        rejects([](auto& v) { v.view[12] = std::numeric_limits<float>::infinity(); }, "nonfinite real matrix accepted");
        rejects([](auto& v) { v.projection[7] = std::numeric_limits<float>::quiet_NaN(); }, "NaN projection accepted");
        rejects([](auto& v) { v.jitter[0] = 0.50001f; }, "out-of-range X jitter accepted");
        rejects([](auto& v) { v.jitter[1] = -0.50001f; }, "out-of-range Y jitter accepted");
        rejects([](auto& v) { v.motionVectorScale[0] = 0; }, "zero motion scale accepted");
        rejects([](auto& v) { v.motionVectorScale[1] = std::numeric_limits<float>::max(); }, "overflowing NDC scale accepted");
        rejects([](auto& v) { v.renderSize[0] = 0; }, "zero render extent accepted");
        const auto full = nativeOutputInterpolationRect(s, {1280,720}, {640,360}, {640,360}, {});
        requireAdapter(full.offset.x == 0 && full.offset.y == 0 && full.extent.width == 1280 && full.extent.height == 720,
                       "Legacy exact-size bundle did not retain full-output interpolation");
        auto mapped = s;
        mapped.renderSize = {1128,634}; mapped.upscaleSize = {1920,1080};
        const auto rawUpscale = mapped.upscaleSize;
        const VkRect2D region{{0,90},{2880,1620}};
        const auto resolved = nativeOutputInterpolationRect(mapped, {2880,1800}, {1128,634}, {1128,634}, region);
        requireAdapter(!interpolationRectChanged(region, resolved) && mapped.upscaleSize == rawUpscale,
                       "Explicit same-frame output region changed raw reconstruction dimensions");
        const auto tag = backbufferRegionTag({2880,1800}, resolved);
        requireAdapter(tag.type == XEFG_SWAPCHAIN_RES_BACKBUFFER && tag.resourceBase.x == 0 && tag.resourceBase.y == 90 &&
                       tag.resourceSize.x == 2880 && tag.resourceSize.y == 1620 && !tag.pResource &&
                       tag.validity == XEFG_SWAPCHAIN_RV_UNTIL_NEXT_PRESENT,
                       "SDK backbuffer region tag did not preserve actual output coordinates");
        requireAdapter(nativeFrameConstants(mapped,false).motionVectorScaleX == c.motionVectorScaleX &&
                       nativeFrameConstants(mapped,false).motionVectorScaleY == c.motionVectorScaleY,
                       "Output mapping rescaled normalized motion vectors");
        const VkRect2D lowRegion{{0,60},{1920,1080}};
        const auto lowResolved=nativeOutputInterpolationRect(mapped,{1920,1200},{1128,634},{1128,634},lowRegion);
        const auto lowTag=backbufferRegionTag({1920,1200},lowResolved);
        requireAdapter(!interpolationRectChanged(lowRegion,lowResolved)&&lowTag.resourceBase.y==60&&
                       lowTag.resourceSize.x==1920&&lowTag.resourceSize.y==1080&&mapped.upscaleSize==rawUpscale,
                       "Low complete-WSI canvas did not keep its mapped logical scene region and original constants");
        const auto asymmetric = nativeOutputInterpolationRect(s, {1280,720}, {640,360}, {640,360}, {{17,23},{1200,650}});
        requireAdapter(asymmetric.offset.x == 17 && asymmetric.offset.y == 23 && asymmetric.extent.height == 650,
                       "Valid asymmetric interpolation rectangle was rejected");
        const auto refusesRegion = [&](auto value, VkExtent2D output, VkExtent2D depth, VkExtent2D motion, VkRect2D rect) {
            bool refused{};
            try { (void)nativeOutputInterpolationRect(value,output,depth,motion,rect); }
            catch (const std::invalid_argument&) { refused=true; }
            requireAdapter(refused,"Invalid or unproven output interpolation rectangle accepted");
        };
        refusesRegion(mapped,{2880,1800},{1128,634},{1128,634},{});
        refusesRegion(s,{1280,720},{640,360},{640,360},{{-1,0},{1280,720}});
        refusesRegion(s,{1280,720},{640,360},{640,360},{{0,0},{0,720}});
        refusesRegion(s,{1280,720},{640,360},{640,360},{{1,0},{1280,720}});
        refusesRegion(s,{1280,720},{640,360},{640,360},{{std::numeric_limits<std::int32_t>::max(),0},
                                                                   {std::numeric_limits<std::uint32_t>::max(),720}});
        refusesRegion(s,{1280,720},{640,360},{640,360},{{0,0},{639,720}});
        refusesRegion(s,{1280,720},{640,360},{641,360},{});
        auto badRender=s; badRender.renderSize[1]=359;
        refusesRegion(badRender,{1280,720},{640,360},{640,360},{});
        auto badUpscale=s; badUpscale.upscaleSize[0]=0;
        refusesRegion(badUpscale,{1280,720},{640,360},{640,360},{{0,0},{1280,720}});
        requireAdapter(!interpolationRectChanged(full,full) && interpolationRectChanged(full,asymmetric) &&
                       interpolationRectChanged(region,{{0,89},{2880,1620}}),
                       "Interpolation history did not distinguish extent and offset changes");
        const auto restored = backbufferRegionTag({1280,720},{});
        requireAdapter(restored.resourceBase.x == 0 && restored.resourceBase.y == 0 &&
                       restored.resourceSize.x == 1280 && restored.resourceSize.y == 720,
                       "Full-output tag retained an earlier letterbox rectangle");
        std::cout << "{\"stage\":\"native-frame-adapter-cpu\",\"passed\":true,\"checks\":" << adapterChecks
                  << ",\"realNonidentityMatricesPreserved\":true,\"jitterPreserved\":true,\"uvToNdcScaleVerified\":true,\"renderTimeZero\":true,\"outputRegionValidated\":true,\"rawUpscalePreserved\":true,\"gpuExecuted\":false,\"sdkCalled\":false}\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
