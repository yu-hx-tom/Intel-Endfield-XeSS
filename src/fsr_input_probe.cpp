#include "fsr_input_probe.hpp"
#include "research_build.hpp"
#include "fsr_probe_internal.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>
#include <vector>

namespace xess_vk::fsr_probe {
#if XESS_RESEARCH_DIAGNOSTICS
namespace {
using namespace detail;
constexpr Address descriptorOffset = 0x10, descriptorStride = 0x20;
constexpr std::size_t descriptorsBytes = bindingCount * descriptorStride;
constexpr Address constantsDescriptorOffset = 0xB0, fullPayloadBytes = 0xD0;
constexpr std::size_t constantsDescriptorBytes = 24;
Address initialPayloadBytes(const Request& request) noexcept {
    if(request.layout==InputLayout::UnityPlayerBee7be52SharedFivePass) return 0xC8;
    return request.captureConstantsFI ? fullPayloadBytes : descriptorOffset + descriptorsBytes;
}

struct PropertyMapping { Address rva; Property property; };
constexpr PropertyMapping propertyMappings[]{
    {0x1D10610, Property::Velocity}, {0x1D10614, Property::DepthBuffer},
    {0x1D10618, Property::CombinedVelocity}, {0x1D10784, Property::InputMotionVectors},
    {0x1D10788, Property::InputDepth}, {0x1D107E4, Property::ReconstructedDepthPreviousFrame},
    {0x1D107E8, Property::DilatedMotionVectors}, {0x1D107EC, Property::DilatedDepth},
    {0x1D107F0, Property::ConstantsFI},
    {0x1D1073C, Property::ReadDilatedMotionVectors}, {0x1D10740, Property::ReadDilatedDepth},
    {0x1D10744, Property::CurrentInterpolationSource}, {0x1D10748, Property::InputDistortionField},
    {0x1D107A8, Property::ReconstructedDepthInterpolatedFrame}, {0x1D10738, Property::OpticalFlowScd},
    {0x1D1076C, Property::InpaintingPyramid}, {0x1D1077C, Property::PresentBackbuffer}, {0x1D107E0, Property::Output}
};

bool overlapsObservedMetadata(const Request& request, const Result& result, Address address, Address bytes) noexcept {
    if (overlapsInput(request,result,address,bytes)) return true;
    if (validRange(request.nativeTableBase,pointerPageBytes) &&
        overlaps(address,bytes,request.nativeTableBase,pointerPageBytes)) return true;
    for (const auto& binding : result.bindings) {
        if (validRange(binding.resourcePayload,4) && overlaps(address,bytes,binding.resourcePayload,4)) return true;
        if (validRange(binding.native.page,pointerPageBytes) && overlaps(address,bytes,binding.native.page,pointerPageBytes)) return true;
        if (validRange(binding.native.object,0x40) && overlaps(address,bytes,binding.native.object,0x40)) return true;
    }
    return false;
}

void captureConstants(const Request& request, Result& result, BoundedReader<Result>& reader,
                      const std::array<unsigned char,16>& expectedBounds) noexcept {
    auto& snapshot=result.constantsFI;
    if (!request.captureConstantsFI) return;
    if (!result.allPassPropertiesPresent || !result.contextBoundsUnchanged ||
        result.graphTextureIdsRead != bindingCount) { snapshot.status=Status::ConstantsPrerequisitesMissing; return; }
    // The classified complete D0 (Prepare) or C8 (other pass) range was checked. BC is
    // not initialized by the identified allocator; keep it opaque, without
    // assigning padding/flags meaning or requiring zero.
    snapshot.descriptor=request.prepareInputPayload+constantsDescriptorOffset;
    std::array<unsigned char,constantsDescriptorBytes> descriptor{},after{};
    auto status=reader.read(snapshot.descriptor,descriptor.data(),descriptor.size());
    if (status!=Status::SnapshotParsed) { snapshot.status=status; return; }
    snapshot.descriptorRead=true;
    snapshot.bufferId=u32(descriptor.data()); snapshot.byteOffset=u32(descriptor.data()+4);
    snapshot.byteSize=u32(descriptor.data()+8); snapshot.unknownBC=u32(descriptor.data()+12);
    snapshot.cpuPointer=u64(descriptor.data()+16);
    if (snapshot.byteSize!=constantsFIBytes) { snapshot.status=Status::ConstantsSizeMismatch; return; }
    if (!snapshot.cpuPointer) { snapshot.status=Status::InvalidAddress; return; }
    if (!validRange(snapshot.cpuPointer,constantsFIBytes)) { snapshot.status=Status::AddressOverflow; return; }
    if (overlapsObservedMetadata(request,result,snapshot.cpuPointer,constantsFIBytes)) {
        snapshot.status=Status::PointerCycleOrOverlap; return;
    }
    std::array<unsigned char,constantsFIBytes> bytes{};
    status=reader.read(snapshot.cpuPointer,bytes.data(),bytes.size());
    if (status!=Status::SnapshotParsed) { snapshot.status=status; return; }
    status=reader.read(snapshot.descriptor,after.data(),after.size());
    if (status!=Status::SnapshotParsed) { snapshot.status=status; return; }
    // BC has unknown/uninitialized meaning, but a change is still evidence of
    // an inconsistent descriptor snapshot. Reject conservatively, not infer why.
    snapshot.descriptorUnchanged=descriptor==after;
    if (!snapshot.descriptorUnchanged) { snapshot.status=Status::ConstantsDescriptorChanged; return; }
    std::array<unsigned char,16> finalBounds{};
    status=reader.read(request.graphContext+contextRangeOffset,finalBounds.data(),finalBounds.size());
    if (status!=Status::SnapshotParsed) {
        result.contextBoundsUnchanged=false; result.status=status; snapshot.status=status; return;
    }
    snapshot.graphBoundsUnchanged=expectedBounds==finalBounds;
    if (!snapshot.graphBoundsUnchanged) {
        result.contextBoundsUnchanged=false; result.status=Status::ContextBoundsChanged;
        snapshot.status=Status::ContextBoundsChanged; return;
    }
    snapshot.bytes=bytes; snapshot.captured=true; snapshot.status=Status::ConstantsSnapshotReadUnverified;
}


Result parse(const Request& request, Reader callback) {
    Result result;
    result.payloadExtent=initialPayloadBytes(request);
    if (request.captureConstantsFI) result.constantsFI.status=Status::NotAttempted;
    auto fail = [&](Status status) { result.status = status; return result; };
    if (request.layout != InputLayout::UnityPlayerBee7be52PrepareInput &&
        request.layout != InputLayout::UnityPlayerBee7be52SharedFivePass) return fail(Status::UnknownInputLayout);
    if (!callback.read) return fail(Status::MissingReader);
    if (!request.limits.maxGraphEntries || request.limits.maxGraphEntries > hardMaxGraphEntries
        || !request.limits.maxBytes || request.limits.maxBytes > hardMaxBytes
        || !request.limits.maxReadCalls || request.limits.maxReadCalls > hardMaxReadCalls) return fail(Status::InvalidLimits);
    if (!request.moduleBase || !request.prepareInputPayload || !request.graphContext) return fail(Status::InvalidAddress);
    if (!validRange(request.moduleBase, 0x1D107F4)
        || !validRange(request.prepareInputPayload, result.payloadExtent)
        || !validRange(request.graphContext, contextRangeOffset + 16)) return fail(Status::AddressOverflow);
    if (overlaps(request.prepareInputPayload, result.payloadExtent, request.graphContext, contextRangeOffset + 16))
        return fail(Status::PointerCycleOrOverlap);
    BoundedReader reader(callback, request.limits, result);
    Address descriptorStart = 0, contextRange = 0;
    if (!add(request.prepareInputPayload, descriptorOffset, descriptorStart)
        || !add(request.graphContext, contextRangeOffset, contextRange)) return fail(Status::AddressOverflow);
    std::array<unsigned char, descriptorsBytes> descriptors{};
    auto status = reader.read(descriptorStart, descriptors.data(), descriptors.size());
    if (status != Status::SnapshotParsed) return fail(status);
    std::array<Address,bindingCount> propertyAddresses{};
    for(std::size_t i=0;i<bindingCount;++i) propertyAddresses[i]=u64(descriptors.data()+i*descriptorStride+0x10);
    result.pass=classifyFiveProperties(request.moduleBase,propertyAddresses);
    if(request.expectedPass!=PassKind::Unknown&&request.expectedPass!=result.pass) return fail(Status::PassPropertySetChanged);
    if(request.layout==InputLayout::UnityPlayerBee7be52SharedFivePass) {
        if(result.pass==PassKind::Unknown) return fail(Status::UnknownPassPropertySet);
        result.payloadExtent=result.pass==PassKind::PrepareInput?0xD0:0xC8;
        if(!validRange(request.prepareInputPayload,result.payloadExtent)) return fail(Status::AddressOverflow);
        if(overlaps(request.prepareInputPayload,result.payloadExtent,request.graphContext,contextRangeOffset+16))
            return fail(Status::PointerCycleOrOverlap);
    } else if(result.pass!=PassKind::PrepareInput) result.pass=PassKind::Unknown;
    result.allPassPropertiesPresent=result.pass!=PassKind::Unknown;
    result.allFivePreparePropertiesPresent=result.pass==PassKind::PrepareInput;
    std::array<unsigned char,16> bounds{};
    std::vector<unsigned char> graph;
    status=detail::readGraphSnapshot(request,result,contextRange,reader,bounds,graph);
    if(status!=Status::SnapshotParsed) return fail(status);
    for (std::size_t i = 0; i < bindingCount; ++i) {
        auto& binding = result.bindings[i];
        binding.descriptor = descriptorStart + i * descriptorStride;
        const auto* source = descriptors.data() + i * descriptorStride;
        binding.graphHandle = u64(source);
        binding.graphKey = static_cast<std::uint32_t>(binding.graphHandle);
        binding.propertyAddress = u64(source + 0x10);
        binding.property = propertyAtAddress(request.moduleBase, binding.propertyAddress);
        detail::resolveGraphBinding(request,result,binding,reader,graph);
    }
    std::array<unsigned char, 16> finalBounds{};
    status = reader.read(contextRange, finalBounds.data(), finalBounds.size());
    if (status != Status::SnapshotParsed) return fail(status);
    result.contextBoundsUnchanged = bounds == finalBounds;
    result.status=result.contextBoundsUnchanged ? Status::SnapshotParsed : Status::ContextBoundsChanged;
    captureConstants(request,result,reader,bounds);
    return result;
}
} // namespace

Property propertyAtAddress(Address moduleBase, Address address) noexcept {
    if (!moduleBase || address < moduleBase) return Property::Unknown;
    for (const auto& entry : propertyMappings) if (address - moduleBase == entry.rva) return entry.property;
    return Property::Unknown;
}
PassKind classifyFiveProperties(Address moduleBase,const std::array<Address,bindingCount>& addresses) noexcept {
    constexpr std::array<std::array<Property,bindingCount>,3> sets{{
        {Property::InputMotionVectors,Property::InputDepth,Property::ReconstructedDepthPreviousFrame,Property::DilatedMotionVectors,Property::DilatedDepth},
        {Property::ReadDilatedMotionVectors,Property::ReadDilatedDepth,Property::CurrentInterpolationSource,Property::InputDistortionField,Property::ReconstructedDepthInterpolatedFrame},
        {Property::OpticalFlowScd,Property::InpaintingPyramid,Property::PresentBackbuffer,Property::CurrentInterpolationSource,Property::Output}}};
    for(std::size_t pass=0;pass<sets.size();++pass) {
        unsigned mask=0; bool valid=true;
        for(const auto address:addresses) {
            const auto property=propertyAtAddress(moduleBase,address);
            std::size_t index=0; while(index<bindingCount&&sets[pass][index]!=property) ++index;
            if(index==bindingCount||(mask&(1u<<index))) {valid=false;break;}
            mask|=1u<<index;
        }
        if(valid&&mask==31) return static_cast<PassKind>(pass+1);
    }
    return PassKind::Unknown;
}
const char* passName(PassKind pass) noexcept {
    switch(pass) {
    case PassKind::PrepareInput:return "prepare_input";
    case PassKind::ReconstructPreviousDepth:return "reconstruct_previous_depth";
    case PassKind::FrameInpainting:return "frame_inpainting";
    default:return "unknown";
    }
}
const char* propertyName(Property value) noexcept {
    switch (value) {
    case Property::Unknown: return "unknown";
    case Property::Velocity: return "_Velocity";
    case Property::DepthBuffer: return "_DepthBuffer";
    case Property::CombinedVelocity: return "_CombinedVelocity";
    case Property::InputMotionVectors: return "r_input_motion_vectors";
    case Property::InputDepth: return "r_input_depth";
    case Property::ReconstructedDepthPreviousFrame: return "rw_reconstructed_depth_previous_frame";
    case Property::DilatedMotionVectors: return "rw_dilated_motion_vectors";
    case Property::DilatedDepth: return "rw_dilated_depth";
    case Property::ConstantsFI: return "cbFI";
    case Property::ReadDilatedMotionVectors:return "r_dilated_motion_vectors";
    case Property::ReadDilatedDepth:return "r_dilated_depth";
    case Property::CurrentInterpolationSource:return "r_current_interpolation_source";
    case Property::InputDistortionField:return "r_input_distortion_field";
    case Property::ReconstructedDepthInterpolatedFrame:return "rw_reconstructed_depth_interpolated_frame";
    case Property::OpticalFlowScd:return "r_optical_flow_scd";
    case Property::InpaintingPyramid:return "r_inpainting_pyramid";
    case Property::PresentBackbuffer:return "r_present_backbuffer";
    case Property::Output:return "rw_output";
    }
    return "unknown";
}
#endif
const char* statusName(Status value) noexcept {
#define STATUS_NAME(name) case Status::name: return #name
    switch (value) {
    STATUS_NAME(NotAttempted); STATUS_NAME(NotRequested); STATUS_NAME(SnapshotParsed); STATUS_NAME(GraphTextureIdRead);
    STATUS_NAME(NativeCandidateReadUnverified); STATUS_NAME(UnknownInputLayout); STATUS_NAME(UnknownNativeTableLayout);
    STATUS_NAME(InvalidLimits); STATUS_NAME(MissingReader); STATUS_NAME(InvalidAddress); STATUS_NAME(AddressOverflow);
    STATUS_NAME(ByteBudgetExceeded); STATUS_NAME(ReadCallBudgetExceeded); STATUS_NAME(Unreadable); STATUS_NAME(PartialRead);
    STATUS_NAME(CallbackException); STATUS_NAME(CallbackContractViolation); STATUS_NAME(AllocationFailed);
    STATUS_NAME(InvalidGraphRange); STATUS_NAME(GraphEntryLimitExceeded); STATUS_NAME(GraphKeysUnsorted);
    STATUS_NAME(DuplicateGraphKey); STATUS_NAME(GraphKeyMissing); STATUS_NAME(UnsupportedGraphEntryType);
    STATUS_NAME(NullResourcePayload); STATUS_NAME(NullTextureId); STATUS_NAME(PointerCycleOrOverlap);
    STATUS_NAME(NativeIdOutOfRange); STATUS_NAME(NullNativePage); STATUS_NAME(NullNativeObject); STATUS_NAME(ContextBoundsChanged);
    STATUS_NAME(ConstantsPrerequisitesMissing); STATUS_NAME(ConstantsSizeMismatch);
    STATUS_NAME(ConstantsDescriptorChanged); STATUS_NAME(ConstantsSnapshotReadUnverified);
    STATUS_NAME(UnknownPassPropertySet); STATUS_NAME(PassPropertySetChanged);
    }
#undef STATUS_NAME
    return "UnknownStatus";
}
#if XESS_RESEARCH_DIAGNOSTICS
Result readPrepareInput(const Request& request, Reader callback) noexcept {
    try { return parse(request, callback); }
    catch (const std::bad_alloc&) { Result result; result.status = Status::AllocationFailed; return result; }
    catch (...) { Result result; result.status = Status::CallbackException; return result; }
}
#endif
} // namespace xess_vk::fsr_probe
