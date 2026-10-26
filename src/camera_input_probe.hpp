#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace xess_vk::camera_probe {
using Address = std::uint64_t;
// Historical graph-frame research retains its original binary identity.
inline constexpr char requiredUnitySha256[] = "BEE7BE52370ADDDD67BA61E4937CA51B7F272656841D187E95E505496DA798D1";
inline constexpr char currentUnitySha256[] = "9AA9397A6C5EE4FA083EA379B81FF14019EEBA26A76D15446318D5947FB3D2D0";
inline constexpr std::size_t prefixBytes = 0x100;
inline constexpr Address projectionSourceOffset = 0xC0;
inline constexpr Address clipToPrevSourceOffset = 0x480;
inline constexpr std::size_t clipToPrevBytes = 64;
inline constexpr std::size_t hardMaxBytes = 2 * (prefixBytes + clipToPrevBytes);
inline constexpr std::size_t hardMaxReadCalls = 4;
// Historical static evidence only; this component does not verify or hook an image.
inline constexpr Address prospectiveEntryRva = 0x16B9F1C;
inline constexpr Address knownCallerReturnRva = 0x28D024;

enum class InputLayout { Unknown, UnityPlayerBee7be52CameraRaw, UnityPlayer9aa9397aCameraRaw };
enum class Status {
    NotAttempted, SnapshotReadUnverified, UnknownInputLayout, InvalidLimits,
    MissingReader, InvalidAddress, AddressOverflow, ByteBudgetExceeded,
    ReadCallBudgetExceeded, Unreadable, PartialRead, CallbackException,
    CallbackContractViolation, PrefixChanged, ClipToPrevChanged,
    NonFinitePrefix, NonFiniteClipToPrev, UnverifiedClipToPrevLayout
};
enum class Phase { Preflight, PrefixBefore, ClipToPrevBefore, ClipToPrevAfter, PrefixAfter, Validate, Complete };
const char* statusName(Status) noexcept;
const char* phaseName(Phase) noexcept;

// The caller supplies fault-safe reads of its chosen address space. Copy at most
// size bytes; return actual bytes copied, never keep destination. remainingBytes
// is the remaining per-probe budget BEFORE charging this request. Exceptions are
// contained, but portable C++ cannot make an unsafe callback memory-safe.
// The callback must not execute target functions or access image pixels.
using ReadCallback = std::size_t (*)(void* user, Address address, void* destination,
                                   std::size_t size, std::size_t remainingBytes);
struct Reader { ReadCallback read = nullptr; void* user = nullptr; };
struct Limits { std::size_t maxBytes = hardMaxBytes, maxReadCalls = hardMaxReadCalls; };
struct Request {
    // Selecting a layout is a caller declaration, not module verification.
    InputLayout layout = InputLayout::Unknown;
    Address camera = 0;
    bool captureClipToPrev = false;
    Limits limits;
};
struct Snapshot {
    // Little-endian raw IEEE754 candidate bits. No transpose, inverse, sign
    // adjustment, normalization, camera-space label or float arithmetic occurs.
    // The middle +40/+80 blocks are retained without semantic identification.
    std::array<std::uint32_t, prefixBytes / 4> prefixFloatBits{};
    std::array<std::uint32_t, clipToPrevBytes / 4> clipToPrevFloatBits{};
    bool captured = false;
    bool clipToPrevCaptured = false;
};
struct Result {
    Status status = Status::NotAttempted;
    Phase phase = Phase::Preflight;
    Address camera = 0;
    bool clipToPrevRequested = false;
    Snapshot snapshot;
    std::size_t readCalls = 0, bytesRequested = 0, bytesCopied = 0;
    Address failedReadAddress = 0;
    std::size_t failedReadBytes = 0;
    bool prefixReadingsEqual = false, clipToPrevReadingsEqual = false;
    bool requestedReadingsEqual = false, allRetainedFloatCandidatesFinite = false;
    bool nonFiniteOffsetAvailable = false;
    // Byte offset relative to camera, meaningful only when the flag is true.
    Address firstNonFiniteByteOffset = 0;
    bool snapshotAtomic = false, lifetimePinned = false, sourceIdentityVerified = false;
    bool matrixSemanticsVerified = false, sameFrameVerified = false;
    bool pixelsRead = false, gameFunctionsCalled = false, xeFGConstantsReady = false;
};

// Fixed 2 or 4 reads, stack storage only. All ranges and the full planned budget
// are validated before the first read. Any failure leaves BOTH arrays zero and
// captured flags false. Equality of repeated reads does not prove atomicity,
// object identity/lifetime, a stable frame, or absence of intervening writes.
Result readCamera(const Request&, Reader) noexcept;
} // namespace xess_vk::camera_probe
