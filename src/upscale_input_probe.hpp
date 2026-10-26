#pragma once

#include "fsr_input_probe.hpp"
#include <array>
#include <cstddef>
#include <cstdint>

namespace xess_vk::upscale_probe {
using Address=fsr_probe::Address;
using Reader=fsr_probe::Reader;
using Limits=fsr_probe::Limits;
using ReadStatus=fsr_probe::Status;
using NativeCandidate=fsr_probe::NativeCandidate;
using NativeTableLayout=fsr_probe::NativeTableLayout;
inline constexpr std::size_t bindingCount=8;
inline constexpr std::size_t payloadBytes=0x140, descriptorOffset=0x28, descriptorStride=0x20;
inline constexpr Address callbackThunkRva=0xEB22E0, callbackBodyRva=0xEB49D0;
inline constexpr Address nativeTableRva=0x1EA6E00;
inline constexpr char requiredUnitySha256[]="9AA9397A6C5EE4FA083EA379B81FF14019EEBA26A76D15446318D5947FB3D2D0";

// Current-layout selection requires the caller to verify requiredUnitySha256 and the
// fixed callback identity. This module does not discover, hook, or read a process.
enum class InputLayout { Unknown, UnityPlayerBee7be52UpscalerPrepareEight,
                         UnityPlayer9aa9397aUpscalerDirectIndexEight };
// Bee7 is retained only for historical offline fixtures; no runtime detection.
enum class Property {
    Unknown, InputMotionVectors, InputDepth, InputColorJittered,
    WriteDilatedMotionVectors, WriteDilatedDepth, WriteReconstructedPreviousNearestDepth,
    WriteFarthestDepth, WriteCurrentLuma
};
enum class Status {
    NotAttempted, SnapshotParsed, UnknownInputLayout, MissingReader, InvalidLimits,
    InvalidAddress, AddressOverflow, PointerCycleOrOverlap, UnknownNativeTableLayout,
    UnknownProperty, DuplicateProperty, ReadOrGraphRejected, BindingRejected,
    DescriptorsChanged, GraphEntriesChanged, BindingMetadataChanged, AllocationFailed,
    CallbackException, GraphTableChanged
};
const char* statusName(Status) noexcept;
const char* propertyName(Property) noexcept;
Property propertyAtAddress(Address moduleBase,Address propertyAddress) noexcept;

struct Request {
    InputLayout layout=InputLayout::Unknown;
    Address moduleBase=0, prepareInputPayload=0, graphContext=0;
    // Optional, direct first-level table array (no extra pointer indirection).
    Address nativeTableBase=0;
    NativeTableLayout nativeLayout=NativeTableLayout::Unknown;
    // Two graph/ID/native sweeps fit within these per-invocation ceilings.
    // For direct indexing, maxGraphEntries is a supported-index exclusive cap,
    // not a read array count, capacity or allocation extent.
    Limits limits{8192,256*1024,128};
};
struct Binding {
    Address descriptor=0;
    std::uint64_t graphHandle=0;
    std::uint32_t graphKey=0;
    Address propertyAddress=0;
    Property property=Property::Unknown;
    // Raw CPU graph fields only; no VkAccessFlags/layout meaning is inferred.
    std::uint8_t rawDependencyDirection=0;
    std::uint32_t rawSubresource=0;
    ReadStatus graphStatus=ReadStatus::NotAttempted;
    Address graphEntry=0;
    std::uint32_t entryType=0;
    Address resourcePayload=0;
    std::uint32_t textureId=0;
    NativeCandidate native;
};
struct Result {
    Status status=Status::NotAttempted;
    ReadStatus detailStatus=ReadStatus::NotAttempted;
    // bindingCount means the failure was not localized to one binding.
    std::size_t failureBindingIndex=bindingCount;
    std::size_t payloadExtent=payloadBytes;
    Address graphBegin=0, graphEnd=0;
    std::size_t graphEntryCount=0;
    // Direct-index observations have no certified array count/end. The index
    // ceiling is an observer support limit, never an allocation bound.
    std::size_t graphEntriesObserved=0;
    bool graphIndexedEntries=false, allocationBoundsVerified=false;
    std::array<Binding,bindingCount> bindings{};
    std::size_t graphTextureIdsRead=0, nativeCandidatesRead=0;
    std::size_t readCalls=0, bytesRequested=0, bytesCopied=0;
    bool snapshotCaptured=false, allPropertiesPresent=false;
    bool descriptorsUnchanged=false, graphEntriesUnchanged=false;
    bool bindingMetadataUnchanged=false, contextBoundsUnchanged=false, contextTableUnchanged=false;
    bool snapshotAtomic=false, lifetimePinned=false, pixelsRead=false;
    bool gameFunctionsCalled=false, gpuCompletionVerified=false, copyPermitted=false;
    bool undilatedVerified=false, motionSemanticsVerified=false;
    bool sameFrameVerified=false, xeFGIntegrated=false;
};

// Fixed eight-property layout only. Order is retained, set membership is exact.
// A successful capture requires all graph IDs (and all optional native candidates)
// plus equal repeated descriptors, base, selected entries and binding metadata.
// The current direct-index layout has no verified allocation bounds or count.
// Any failure publishes zero bindings/counts; diagnostics/read accounting remain.
// Repeated equality is not an atomic snapshot, lifetime pin or semantic proof.
// Constants, camera fields, dispatch header and frame IDs are never read here.
Result readUpscalerPrepare(const Request&,Reader) noexcept;
} // namespace xess_vk::upscale_probe
