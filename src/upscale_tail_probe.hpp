#pragma once
#include "fsr_input_probe.hpp"
#include <array>
#include <span>

namespace xess_vk::upscale_tail_probe {
using Address=fsr_probe::Address;
using Reader=fsr_probe::Reader;
using ReadStatus=fsr_probe::Status;
inline constexpr char requiredUnitySha256[]="9AA9397A6C5EE4FA083EA379B81FF14019EEBA26A76D15446318D5947FB3D2D0";
inline constexpr char prepareShaderSha256[]="0be6b001c712d6094916e86a06e8aa77ad9d41158515e3c80b3060c9180d8575";
// This pinned Prepare writes raw MV * cb(+0x50), with no cb(+0x60)
// jitter-cancellation subtraction. Use its unjittered input convention for SR.
inline constexpr bool motionVectorsJittered=false;
inline constexpr std::size_t maxBindings=12,maxPayloadBytes=0x1C0;
inline constexpr Address nativeTableRva=0x1EA6E00,descriptorOffset=0x28,descriptorStride=0x20;
// Reactive/Transparency are legacy trace labels. EB4090 decodes raw MV used by
// XeSS; callback membership is never proof that its dispatch may be removed.
enum class Pass { Unknown, Accumulate, Rcas, Reactive, Transparency, Pyramid, ShadingChange, PrepareReactivity, LumaInstability, Count };
enum class Property { Unknown, Exposure, InputColor, RcasInput, InternalColor, WriteInternalColor, Output, OtherSupported, LanczosLut, CurrentLuma };
enum class Status { NotAttempted, Parsed, UnknownProfile, InvalidLimits, MissingReader, InvalidAddress,
    Overflow, Overlap, UnknownProperty, DuplicateProperty, ReadRejected, BindingRejected,
    PayloadChanged, TableChanged, EntryChanged, MetadataChanged, ConstantsRejected };
const char* passName(Pass) noexcept;
const char* propertyName(Property) noexcept;
const char* statusName(Status) noexcept;
struct Profile {
    Address thunk{},body{},registration{},registrationReturn{};
    std::size_t payloadBytes{},bindingCount{},constantsOffset{},rcasOffset{};
    Address alternateRegistrationReturn{};
    std::size_t imageOffset=descriptorOffset,groupsOffset=0x10,inlineConstantsOffset{},spdOffset{};
    bool implicitGroupZ{},registrationReturnsGraphArray{};
};
Profile profile(Pass) noexcept;
// Three pinned registration bodies return the graph array, not their payload.
// Resolve its exact node/callback with two stable reads; never alter native RAX.
Address registrationPayload(Pass,Address module,Address graph,std::uint32_t node,Address returned,Reader) noexcept;
// The Pyramid registration has two exact property sets, not their union.
std::span<const Address> propertyRvas(Pass,bool alternate=false) noexcept;
Property propertyAtAddress(Address module,Address property) noexcept;
struct Request {
    // Selecting a profile requires the caller's exact SHA/site verification.
    Pass pass=Pass::Unknown;
    Address moduleBase{},prepareInputPayload{},graphContext{},nativeTableBase{};
    fsr_probe::NativeTableLayout nativeLayout=fsr_probe::NativeTableLayout::Unknown;
    fsr_probe::Limits limits{8192,256*1024,256};
};
struct Binding {
    Address descriptor{},propertyAddress{},graphEntry{},resourcePayload{};
    std::uint64_t graphHandle{};
    std::uint32_t graphKey{},entryType{},textureId{},rawSubresource{};
    std::uint8_t rawDependencyDirection{};
    Property property=Property::Unknown;
    ReadStatus graphStatus=ReadStatus::NotAttempted;
    fsr_probe::NativeCandidate native;
};
template<std::size_t N> struct Constants {
    Address cpuPointer{};
    std::uint32_t bufferId{},byteOffset{},byteSize{};
    std::array<unsigned char,N> bytes{};
    bool captured{},bytesUnchanged{};
};
struct Result {
    Status status=Status::NotAttempted;
    ReadStatus detail=ReadStatus::NotAttempted;
    Pass pass=Pass::Unknown;
    std::size_t payloadExtent{},bindingCount{},failureBindingIndex=maxBindings;
    Address failurePropertyAddress{},failureGraphHandle{},failureGraphEntry{};
    std::uint32_t failureGraphKey{},failureEntryType{},failureTextureId{};
    ReadStatus failureGraphStatus=ReadStatus::NotAttempted,failureNativeStatus=ReadStatus::NotAttempted;
    Address graphBegin{};
    std::size_t graphEntryCount{}; // Unknown for indexed lookup, never a capacity.
    bool graphIndexedEntries=true,allocationBoundsVerified=false;
    std::array<Binding,maxBindings> bindings{};
    std::array<unsigned char,maxPayloadBytes> payload{};
    std::array<std::uint32_t,3> groups{};
    std::uint32_t rawKernelToken{};
    Constants<148> constants;
    Constants<16> rcas;
    Constants<160> reactive;
    Constants<24> spd;
    std::size_t readCalls{},bytesRequested{},bytesCopied{},graphTextureIdsRead{},nativeCandidatesRead{};
    bool snapshotCaptured{},payloadUnchanged{},tableUnchanged{},entriesUnchanged{},metadataUnchanged{};
    bool snapshotAtomic=false,lifetimePinned=false,sameFrameVerified=false,pixelsRead=false;
    bool hdrVerified=false,exposureVerified=false,gpuComplete=false,executePermitted=false;
};
// Fixed per-job property/payload profiles and two bounded consistency sweeps. Failure
// publishes no binding, raw payload or constant bytes. No process/GPU access.
Result read(const Request&,Reader) noexcept;
}
