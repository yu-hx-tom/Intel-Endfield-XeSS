#pragma once

#include "camera_input_probe.hpp"
#include <array>
#include <cstddef>
#include <cstdint>

namespace xess_vk::graph_scope {
using Address = std::uint64_t;
inline constexpr std::size_t maxThreads = 4, maxCallerDepth = 4;
inline constexpr std::size_t maxRegistrations = 8, maxProducers = 8;
inline constexpr std::size_t maxPayloadBytes = 0x140, maxDescriptorBytes = 0x100;
inline constexpr std::uint32_t maxGraphEntries = 8192;

// Pure event data only: no native reads, hooks, target calls, allocation, GPU work,
// or module verification. An adapter must obtain these facts at the reviewed
// boundaries with safe reads. A matching digest here does not authenticate them.
struct Generations {
    std::uint64_t module = 0, load = 0, device = 0, observer = 0;
    bool operator==(const Generations& x) const noexcept;
};
struct Stamp {
    std::uint64_t thread = 0;
    Address moduleBase = 0;
    Generations generations;
    std::array<char, 65> moduleSha256{};
    bool operator==(const Stamp& x) const noexcept;
};
struct Key {
    std::uint64_t engine = 0, id = 0, callerId = 0;
    Stamp stamp;
    explicit operator bool() const noexcept { return engine != 0 && id != 0; }
    bool operator==(const Key& x) const noexcept;
};
enum class Pass { Unknown, FiPrepare, FiReconstruct, FiInpaint, UpscalePrepare };
enum class Producer { Unknown, Fi, Upscaler };
enum class Exit { Normal, Abnormal };
enum class Status {
    Accepted, ActiveAssociation, CompletedAssociation, UnexecutedRegistrations,
    MissingScope, StaleKey, CrossThread, GenerationMismatch, InvalidIdentity,
    UnknownRoute, UnsupportedPass, OutOfOrder, ReentrantScope, ScopeInvalidated,
    InvalidAddress, AddressOverflow, InvalidStackBounds, GraphMismatch,
    TableMismatch, EntryMismatch, SnapshotSizeMismatch, UnknownPropertySet,
    PayloadChanged, CameraUnavailable, CameraChanged, CameraNonFinite,
    CameraProvenanceMismatch, ContextMismatch, DuplicateRegistration,
    AmbiguousMatch, UnmatchedCallback, DuplicateCallback, OddSchedule,
    CapacityExhausted, IdExhausted, AbnormalExit
};
const char* statusName(Status) noexcept;
const char* passName(Pass) noexcept;
struct Summary {
    std::size_t registered = 0, callbacksStarted = 0, callbacksCompleted = 0;
    std::size_t unexecuted = 0;
    bool invalidated = false;
};
struct Outcome {
    Status status = Status::MissingScope;
    Key key;
    Summary summary;
};
struct Limits {
    std::size_t threads = maxThreads, callerDepth = maxCallerDepth;
    std::size_t registrations = maxRegistrations, producers = maxProducers;
};
struct StackBounds { Address low = 0, high = 0; }; // high is exclusive
struct Payload {
    std::size_t size = 0;
    std::array<unsigned char, maxPayloadBytes> bytes{};
};
struct Descriptors {
    std::size_t count = 0, size = 0;
    std::array<unsigned char, maxDescriptorBytes> bytes{};
};
struct GraphState {
    Address graph = 0, arena = 0, table = 0;
    std::uint32_t count = 0, capacity = 0;
};
struct Entry {
    std::uint32_t index = 0;
    Address address = 0, executor = 0, copiedPayload = 0;
};
struct CallerEntry {
    Address entryRva = 0, originalRsp = 0;
    StackBounds stack;
};
struct Construction {
    Address initializerRva = 0, callerReturnRva = 0;
    GraphState graph;
};
struct RendererEntry {
    Address entryRva = 0, callerReturnRva = 0, originalRsp = 0, rbp = 0;
    Address owner = 0, viewContextSlot = 0, viewContext = 0, camera = 0;
};
struct ProducerEntry {
    Producer kind = Producer::Unknown;
    Address entryRva = 0, callerReturnRva = 0, originalRsp = 0, rbp = 0;
    Address savedRendererRbp = 0;
};
struct Provenance {
    Address helperRva = 0, helperCallerReturnRva = 0;
    Address helperOriginalRsp = 0, helperOriginalRbp = 0;
    Address savedRendererRbp = 0, savedProducerReturnRva = 0, savedGraph = 0;
    Address sourcePayload = 0, rendererOwner = 0, viewContext = 0, camera = 0;
};
struct RegistrationBefore {
    Pass pass = Pass::Unknown;
    Provenance provenance;
    GraphState graph;
    std::uint32_t passIndex = 0, parentPassIndex = 0;
    Payload source;
    camera_probe::Snapshot camera;
};
struct RegistrationReturn {
    Address helperReturnRax = 0;
    Provenance provenance; // full recheck of the checked source chain
    GraphState graph;
    Entry entry;
    Payload sourceAfter, copy;
    camera_probe::Snapshot camera;
};
struct Context {
    Address address = 0, deviceContext = 0, ownerContext = 0;
    Address resourceBegin = 0, resourceEnd = 0, graphValue40 = 0;
    Address allocator = 0, arena = 0;
};
struct ExecutorEntry {
    Address entryRva = 0, callerReturnRva = 0, originalRsp = 0;
    GraphState graph;
    // Values read from graph+40/+58, used to cross-check the per-callback context.
    Address graphValue40 = 0, graphValue58 = 0;
};
struct CallbackEntry {
    Address callRva = 0, callerReturnRva = 0;
    std::uint32_t encodedSchedulePass = 0;
    GraphState graph;
    Entry entry;
    Address actualPayload = 0;
    Context context;
    Payload snapshot;
};
struct Association {
    Key caller, construction, renderer, producer, registration, execution, callback;
    Pass pass = Pass::Unknown;
    Provenance provenance;
    GraphState graph;
    Entry entry;
    Context context;
    std::uint32_t parentPassIndex = 0, encodedSchedulePass = 0;
    std::size_t callbackOrdinal = 0;
    Payload payload;
    Descriptors descriptors;
    camera_probe::Snapshot camera;
    bool cpuBuildExecutionAssociated = false;
    bool executionActive = false, callbackActive = false, callbackCompleted = false;
    // Unconditionally false. Address/byte equality and CPU scope cannot prove these.
    bool sameFrameVerified = false, matrixSemanticsVerified = false;
    bool arenaLifetimePinned = false, gpuComplete = false, copyPermitted = false;
    bool xeFGConstantsReady = false;
};

// Caller provides serialization for ALL accesses, including across threads.
// No internal lock or heap allocation. Different caller scopes may nest on the
// same thread in any phase; their state is independent. An active query requires
// the owning caller to be the current top scope (nested scopes suspend it).
// Keys have an engine incarnation plus monotonic IDs, never pointer generations.
// Failed event validation invalidates its owning caller; no silent eviction.
// Outputs are owned historical copies, not transferable live eligibility.
class Engine {
public:
    explicit Engine(Limits = {}) noexcept;
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    Outcome enterCaller(const Stamp&, const CallerEntry&) noexcept;
    Outcome constructGraph(const Stamp&, const Key& caller, const Construction&) noexcept;
    Outcome enterRenderer(const Stamp&, const Key& caller, const RendererEntry&) noexcept;
    Outcome exitRenderer(const Stamp&, const Key& renderer, Exit) noexcept;
    Outcome enterProducer(const Stamp&, const Key& renderer, const ProducerEntry&) noexcept;
    Outcome exitProducer(const Stamp&, const Key& producer, Exit) noexcept;
    Outcome beforeRegistration(const Stamp&, const Key& producer, const RegistrationBefore&) noexcept;
    Outcome returnRegistration(const Stamp&, const Key& registration, const RegistrationReturn&) noexcept;
    Outcome abortRegistration(const Stamp&, const Key& registration) noexcept;
    Outcome enterExecutor(const Stamp&, const Key& caller, const ExecutorEntry&) noexcept;
    Outcome enterCallback(const Stamp&, const Key& execution, const CallbackEntry&) noexcept;
    // Query after matched entry and before original dispatch; never waits for return.
    Outcome activeAssociation(const Stamp&, const Key& callback, Association&) noexcept;
    // The first completed evidence is published only on normal callback return.
    Outcome exitCallback(const Stamp&, const Key& callback, Exit, Association* = nullptr) noexcept;
    Outcome exitExecutor(const Stamp&, const Key& execution, Exit) noexcept;
    Outcome exitCaller(const Stamp&, const Key& caller, Exit) noexcept;
private:
    struct ProducerSlot { Key key; ProducerEntry data; bool active = false; };
    struct RegistrationSlot {
        Key key, producer;
        RegistrationBefore before;
        Entry entry;
        GraphState returnedGraph;
        Descriptors descriptors;
        bool ready = false, started = false, completed = false;
    };
    struct CallerSlot {
        Key key, construction, renderer, execution, callback;
        CallerEntry caller;
        RendererEntry rendererData;
        GraphState constructedGraph;
        ExecutorEntry executorData;
        CallbackEntry callbackData;
        std::array<ProducerSlot, maxProducers> producers{};
        std::array<RegistrationSlot, maxRegistrations> registrations{};
        std::size_t producerCount = 0, registrationCount = 0;
        std::size_t activeProducer = maxProducers, pendingRegistration = maxRegistrations;
        std::size_t callbackRegistration = maxRegistrations, callbackOrdinal = 0;
        bool rendererActive = false, rendererCompleted = false;
        bool executionActive = false, executionCompleted = false;
        bool invalidated = false;
    };
    struct ThreadSlot {
        Stamp stamp;
        std::array<CallerSlot, maxCallerDepth> callers{};
        std::size_t depth = 0;
    };
    std::array<ThreadSlot, maxThreads> threads_{};
    Limits limits_;
    std::uint64_t engineId_ = 0, nextId_ = 1;
    Status configuration_ = Status::Accepted;
    Key allocate(const Stamp&, std::uint64_t callerId) noexcept;
    CallerSlot* locate(const Stamp&, const Key&, Status&, bool allowInvalid = false) noexcept;
    Outcome fail(CallerSlot*, Status) noexcept;
    Outcome accept(const Key&, const CallerSlot*) const noexcept;
    Summary summarize(const CallerSlot&) const noexcept;
    Status graphValid(const GraphState&, std::uint32_t) const noexcept;
    Status provenanceValid(const CallerSlot&, const ProducerSlot&, const RegistrationBefore&) const noexcept;
    Status payloadValid(const Stamp&, Pass, const Payload&, Descriptors&) const noexcept;
    void copyAssociation(const CallerSlot&, bool completed, Association&) const noexcept;
};
} // namespace xess_vk::graph_scope
