#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace xess_vk::fsr_probe {
using Address = std::uint64_t;
inline constexpr char requiredUnitySha256[] = "BEE7BE52370ADDDD67BA61E4937CA51B7F272656841D187E95E505496DA798D1";
inline constexpr Address nativeTableRva = 0x1CD62C0;
inline constexpr std::size_t bindingCount = 5;
inline constexpr std::size_t hardMaxGraphEntries = 8192;
inline constexpr std::size_t hardMaxBytes = 1024 * 1024;
inline constexpr std::size_t hardMaxReadCalls = 256;
inline constexpr std::size_t constantsFIBytes = 224;

// The caller must verify the pinned module identity before selecting this layout.
// This pure parser neither finds modules nor reads/hashes files or live processes.
enum class InputLayout { Unknown, UnityPlayerBee7be52PrepareInput, UnityPlayerBee7be52SharedFivePass };
enum class NativeTableLayout { Unknown, DirectArrayOf1024PagePointers_1024ObjectPointers };
enum class PassKind { Unknown, PrepareInput, ReconstructPreviousDepth, FrameInpainting };
const char* passName(PassKind) noexcept;
// Exact unique five-property set, independent of descriptor order. No reads.
PassKind classifyFiveProperties(Address moduleBase, const std::array<Address,bindingCount>&) noexcept;

enum class Status {
    NotAttempted, NotRequested, SnapshotParsed, GraphTextureIdRead, NativeCandidateReadUnverified,
    UnknownInputLayout, UnknownNativeTableLayout, InvalidLimits, MissingReader,
    InvalidAddress, AddressOverflow, ByteBudgetExceeded, ReadCallBudgetExceeded,
    Unreadable, PartialRead, CallbackException, CallbackContractViolation, AllocationFailed,
    InvalidGraphRange, GraphEntryLimitExceeded, GraphKeysUnsorted, DuplicateGraphKey,
    GraphKeyMissing, UnsupportedGraphEntryType, NullResourcePayload, NullTextureId,
    PointerCycleOrOverlap, NativeIdOutOfRange, NullNativePage, NullNativeObject,
    ContextBoundsChanged, ConstantsPrerequisitesMissing, ConstantsSizeMismatch,
    ConstantsDescriptorChanged, ConstantsSnapshotReadUnverified, UnknownPassPropertySet, PassPropertySetChanged
};
const char* statusName(Status) noexcept;

enum class Property {
    Unknown, Velocity, DepthBuffer, CombinedVelocity, InputMotionVectors, InputDepth,
    ReconstructedDepthPreviousFrame, DilatedMotionVectors, DilatedDepth, ConstantsFI,
    ReadDilatedMotionVectors, ReadDilatedDepth, CurrentInterpolationSource, InputDistortionField,
    ReconstructedDepthInterpolatedFrame, OpticalFlowScd, InpaintingPyramid, PresentBackbuffer, Output
};
const char* propertyName(Property) noexcept;
Property propertyAtAddress(Address moduleBase, Address propertyAddress) noexcept;

// Must safely copy at most size bytes into destination and return bytes copied.
// maxBytes is the remaining per-probe budget BEFORE charging this request.
// The parser validates size <= maxBytes and charges all requested bytes, even on
// failure. Callback must not read pixels, run target functions, or retain dst.
// No live-memory implementation is provided. A callback exception is contained.
using ReadCallback = std::size_t (*)(void* user, Address address, void* destination,
                                   std::size_t size, std::size_t maxBytes);
struct Reader { ReadCallback read = nullptr; void* user = nullptr; };
struct Limits {
    std::size_t maxGraphEntries = 2048;
    std::size_t maxBytes = 128 * 1024;
    std::size_t maxReadCalls = 64;
};
struct Request {
    InputLayout layout = InputLayout::Unknown;
    Address moduleBase = 0;
    Address prepareInputPayload = 0;
    Address graphContext = 0;
    // Optional exact RCX base from the saved native lookup: this is the direct
    // first-level array address, NOT a pointer slot to dereference again.
    Address nativeTableBase = 0;
    NativeTableLayout nativeLayout = NativeTableLayout::Unknown;
    Limits limits;
    // Off by default. The pinned PrepareInput layout contains a 24-byte slice
    // descriptor at +B0. This requests raw CPU bytes, not decoded shader meaning.
    bool captureConstantsFI = false;
    // Optional prefilter result. A different parsed set must not be attributed
    // to that prefilter's counter/quota. Unknown disables this equality check.
    PassKind expectedPass = PassKind::Unknown;
};
struct NativeCandidate {
    Status status = Status::NotRequested;
    Address tableBase = 0, pageSlot = 0, page = 0, objectSlot = 0, object = 0;
    std::uint32_t widthCandidate = 0, heightCandidate = 0;
    std::uint64_t imageCandidate = 0; // Opaque field value; never treated as a CPU pointer.
    bool idDomainVerified = false;
    bool vulkanImageVerified = false;
};
struct Binding {
    Address descriptor = 0;
    std::uint64_t graphHandle = 0;
    std::uint32_t graphKey = 0;
    Address propertyAddress = 0;
    Property property = Property::Unknown;
    Status graphStatus = Status::NotAttempted;
    Address graphEntry = 0;
    std::uint32_t entryType = 0;
    Address resourcePayload = 0;
    std::uint32_t textureId = 0;
    NativeCandidate native;
};
struct ConstantsSnapshot {
    Status status = Status::NotRequested;
    Address descriptor = 0, cpuPointer = 0;
    std::uint32_t bufferId = 0, byteOffset = 0, byteSize = 0, unknownBC = 0;
    bool descriptorRead = false, descriptorUnchanged = false, graphBoundsUnchanged = false;
    // Published only after an exact-size read and descriptor/bounds rechecks.
    // Failed/partial snapshots leave bytes zero, even if the reader copied some.
    std::array<unsigned char, constantsFIBytes> bytes{};
    bool captured = false;
    bool snapshotAtomic = false, lifetimePinned = false, shaderSemanticsVerified = false;
};
struct Result {
    Status status = Status::NotAttempted;
    PassKind pass = PassKind::Unknown;
    std::size_t payloadExtent = 0;
    Address graphBegin = 0, graphEnd = 0;
    std::size_t graphEntryCount = 0;
    std::array<Binding, bindingCount> bindings;
    ConstantsSnapshot constantsFI;
    std::size_t graphTextureIdsRead = 0, nativeCandidatesRead = 0;
    std::size_t readCalls = 0, bytesRequested = 0, bytesCopied = 0;
    bool allFivePreparePropertiesPresent = false;
    bool allPassPropertiesPresent = false;
    bool contextBoundsUnchanged = false;
    // Bounds equality is only a consistency check. Concurrent target writes can
    // still tear/reuse data between reads; nothing here proves atomicity/lifetime.
    bool snapshotAtomic = false;
    bool pixelsRead = false;
    bool gameFunctionsCalled = false;
    bool gameFrameInputsVerified = false;
    bool xeFGIntegrated = false;
};

// Fixed-depth metadata reads only; all numeric addresses go through Reader.
// SnapshotParsed means the snapshot format was parsed, not that each binding
// resolved. Inspect each graphStatus/native.status and all verification flags.
// Optional constants failure has its own status and never promotes texture
// candidates. A failed final graph-bounds recheck also invalidates Result.status.
Result readPrepareInput(const Request&, Reader) noexcept;
} // namespace xess_vk::fsr_probe
