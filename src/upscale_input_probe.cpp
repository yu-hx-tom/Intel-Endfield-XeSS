#include "upscale_input_probe.hpp"
#include "fsr_probe_internal.hpp"

#include <new>
#include <tuple>
#include <vector>

namespace xess_vk::upscale_probe {
namespace {
namespace core=fsr_probe::detail;
constexpr std::array<Address,bindingCount> propertyRvas{
    0x1D10628,0x1D1062C,0x1D10630,0x1D10684,
    0x1D10688,0x1D1068C,0x1D10690,0x1D10694};
constexpr std::array<Address,bindingCount> currentPropertyRvas{
    0x1EDE538,0x1EDE53C,0x1EDE540,0x1EDE594,0x1EDE598,0x1EDE59C,0x1EDE5A0,0x1EDE5A4};
using DescriptorBytes=std::array<unsigned char,bindingCount*descriptorStride>;

bool sameMetadata(const Binding& a,const Binding& b) noexcept {
    const auto& x=a.native; const auto& y=b.native;
    return std::tie(a.graphStatus,a.graphEntry,a.entryType,a.resourcePayload,a.textureId,
                    x.status,x.tableBase,x.pageSlot,x.page,x.objectSlot,x.object,
                    x.widthCandidate,x.heightCandidate,x.imageCandidate)==
           std::tie(b.graphStatus,b.graphEntry,b.entryType,b.resourcePayload,b.textureId,
                    y.status,y.tableBase,y.pageSlot,y.page,y.objectSlot,y.object,
                    y.widthCandidate,y.heightCandidate,y.imageCandidate);
}
Binding descriptorBinding(const Request& request,const DescriptorBytes& bytes,std::size_t i) noexcept {
    Binding binding;
    const auto* data=bytes.data()+i*descriptorStride;
    binding.descriptor=request.prepareInputPayload+descriptorOffset+i*descriptorStride;
    binding.graphHandle=core::u64(data);
    binding.graphKey=static_cast<std::uint32_t>(binding.graphHandle);
    binding.propertyAddress=core::u64(data+0x10);
    const auto& properties=request.layout==InputLayout::UnityPlayer9aa9397aUpscalerDirectIndexEight ? currentPropertyRvas : propertyRvas;
    for(std::size_t property=0;property<properties.size();++property)
        if(binding.propertyAddress>=request.moduleBase && binding.propertyAddress-request.moduleBase==properties[property])
            binding.property=static_cast<Property>(property+1);
    binding.rawDependencyDirection=data[8]; binding.rawSubresource=core::u32(data+0x18);
    return binding;
}

void parse(const Request& request,Reader callback,Result& result) {
    auto fail=[&](Status status,ReadStatus detail=ReadStatus::NotAttempted,std::size_t index=bindingCount) {
        result.status=status; result.detailStatus=detail; result.failureBindingIndex=index;
        result.bindings={}; result.graphTextureIdsRead=0; result.nativeCandidatesRead=0;
        result.snapshotCaptured=false; result.allPropertiesPresent=false;
    };
    if(request.layout!=InputLayout::UnityPlayerBee7be52UpscalerPrepareEight) {fail(Status::UnknownInputLayout);return;}
    if(!callback.read) {fail(Status::MissingReader);return;}
    if(!request.limits.maxGraphEntries||request.limits.maxGraphEntries>fsr_probe::hardMaxGraphEntries||
       !request.limits.maxBytes||request.limits.maxBytes>fsr_probe::hardMaxBytes||
       !request.limits.maxReadCalls||request.limits.maxReadCalls>fsr_probe::hardMaxReadCalls) {
        fail(Status::InvalidLimits);return;
    }
    if(!request.moduleBase||!request.prepareInputPayload||!request.graphContext) {fail(Status::InvalidAddress);return;}
    if(!core::validRange(request.moduleBase,propertyRvas.back()+4)||
       !core::validRange(request.prepareInputPayload,payloadBytes)||
       !core::validRange(request.graphContext,0x20)) {fail(Status::AddressOverflow);return;}
    if(core::overlaps(request.prepareInputPayload,payloadBytes,request.graphContext,0x20)) {
        fail(Status::PointerCycleOrOverlap);return;
    }
    if(request.nativeTableBase&&request.nativeLayout!=NativeTableLayout::DirectArrayOf1024PagePointers_1024ObjectPointers) {
        fail(Status::UnknownNativeTableLayout);return;
    }
    core::BoundedReader<Result> reader(callback,request.limits,result);
    DescriptorBytes descriptors{},afterDescriptors{};
    auto readStatus=reader.read(request.prepareInputPayload+descriptorOffset,descriptors.data(),descriptors.size());
    if(readStatus!=ReadStatus::SnapshotParsed) {fail(Status::ReadOrGraphRejected,readStatus);return;}
    unsigned seen=0;
    for(std::size_t i=0;i<bindingCount;++i) {
        auto& binding=result.bindings[i]; binding=descriptorBinding(request,descriptors,i);
        if(binding.property==Property::Unknown) {fail(Status::UnknownProperty,ReadStatus::NotAttempted,i);return;}
        const auto mask=1u<<(static_cast<unsigned>(binding.property)-1);
        if(seen&mask) {fail(Status::DuplicateProperty,ReadStatus::NotAttempted,i);return;}
        seen|=mask;
    }
    // Eight unique members from an eight-element set imply completeness.
    std::array<unsigned char,16> bounds{},afterBounds{};
    std::vector<unsigned char> graph;
    readStatus=core::readGraphSnapshot(request,result,request.graphContext+core::contextRangeOffset,reader,bounds,graph);
    if(readStatus!=ReadStatus::SnapshotParsed) {fail(Status::ReadOrGraphRejected,readStatus);return;}
    for(std::size_t i=0;i<bindingCount;++i) {
        auto& binding=result.bindings[i];
        core::resolveGraphBinding(request,result,binding,reader,graph);
        if(binding.graphStatus!=ReadStatus::GraphTextureIdRead) {fail(Status::BindingRejected,binding.graphStatus,i);return;}
        if(request.nativeTableBase&&binding.native.status!=ReadStatus::NativeCandidateReadUnverified) {
            fail(Status::BindingRejected,binding.native.status,i);return;
        }
    }
    // Do not allocate or read a second graph buffer beyond the remaining budget.
    if(graph.size()>request.limits.maxBytes-result.bytesRequested) {
        fail(Status::ReadOrGraphRejected,ReadStatus::ByteBudgetExceeded);return;
    }
    std::vector<unsigned char> afterGraph(graph.size());
    if(!afterGraph.empty()) {
        readStatus=reader.read(result.graphBegin,afterGraph.data(),afterGraph.size());
        if(readStatus!=ReadStatus::SnapshotParsed) {fail(Status::ReadOrGraphRejected,readStatus);return;}
    }
    result.graphEntriesUnchanged=graph==afterGraph;
    if(!result.graphEntriesUnchanged) {fail(Status::GraphEntriesChanged);return;}
    // Reader accounting remains on result. Scratch counters are not published;
    // each reported graph/native count represents eight bindings, not two sweeps.
    Result verificationScope;
    verificationScope.graphBegin=result.graphBegin; verificationScope.graphEnd=result.graphEnd;
    verificationScope.graphEntryCount=result.graphEntryCount;
    for(std::size_t i=0;i<bindingCount;++i) {
        auto again=descriptorBinding(request,descriptors,i);
        core::resolveGraphBinding(request,verificationScope,again,reader,afterGraph);
        if(again.graphStatus!=ReadStatus::GraphTextureIdRead) {fail(Status::BindingRejected,again.graphStatus,i);return;}
        if(request.nativeTableBase&&again.native.status!=ReadStatus::NativeCandidateReadUnverified) {
            fail(Status::BindingRejected,again.native.status,i);return;
        }
        if(!sameMetadata(result.bindings[i],again)) {fail(Status::BindingMetadataChanged,ReadStatus::NotAttempted,i);return;}
    }
    result.bindingMetadataUnchanged=true;
    readStatus=reader.read(request.graphContext+core::contextRangeOffset,afterBounds.data(),afterBounds.size());
    if(readStatus!=ReadStatus::SnapshotParsed) {fail(Status::ReadOrGraphRejected,readStatus);return;}
    result.contextBoundsUnchanged=bounds==afterBounds;
    if(!result.contextBoundsUnchanged) {fail(Status::ReadOrGraphRejected,ReadStatus::ContextBoundsChanged);return;}
    readStatus=reader.read(request.prepareInputPayload+descriptorOffset,afterDescriptors.data(),afterDescriptors.size());
    if(readStatus!=ReadStatus::SnapshotParsed) {fail(Status::ReadOrGraphRejected,readStatus);return;}
    result.descriptorsUnchanged=descriptors==afterDescriptors;
    if(!result.descriptorsUnchanged) {fail(Status::DescriptorsChanged);return;}
    result.status=Status::SnapshotParsed; result.detailStatus=ReadStatus::SnapshotParsed;
    result.snapshotCaptured=true; result.allPropertiesPresent=true;
}

void resolveDirectBinding(const Request& request, Result& scope, Binding& binding,
                          core::BoundedReader<Result>& reader, const std::array<unsigned char,16>& entry) {
    binding.entryType=core::u32(entry.data()+12);
    if(binding.entryType!=2 && binding.entryType!=3) { binding.graphStatus=ReadStatus::UnsupportedGraphEntryType; return; }
    binding.resourcePayload=core::u64(entry.data());
    if(!binding.resourcePayload) { binding.graphStatus=ReadStatus::NullResourcePayload; return; }
    if(!core::validRange(binding.resourcePayload,4)) { binding.graphStatus=ReadStatus::AddressOverflow; return; }
    if(core::overlapsInput(request,scope,binding.resourcePayload,4)) { binding.graphStatus=ReadStatus::PointerCycleOrOverlap; return; }
    binding.graphStatus=reader.word(binding.resourcePayload,binding.textureId);
    if(binding.graphStatus!=ReadStatus::SnapshotParsed) return;
    if(!binding.textureId) { binding.graphStatus=ReadStatus::NullTextureId; return; }
    binding.graphStatus=ReadStatus::GraphTextureIdRead;
    ++scope.graphTextureIdsRead;
    core::resolveNative(request,scope,binding,reader);
    if(binding.native.status==ReadStatus::NativeCandidateReadUnverified) ++scope.nativeCandidatesRead;
}
void parseIndexed(const Request& request,Reader callback,Result& result) {
    auto fail=[&](Status status,ReadStatus detail=ReadStatus::NotAttempted,std::size_t index=bindingCount) {
        result.status=status; result.detailStatus=detail; result.failureBindingIndex=index;
        result.bindings={}; result.graphTextureIdsRead=result.nativeCandidatesRead=result.graphEntriesObserved=0;
        result.snapshotCaptured=result.allPropertiesPresent=false;
    };
    result.graphIndexedEntries=true;
    if(!callback.read) {fail(Status::MissingReader);return;}
    if(!request.limits.maxGraphEntries||request.limits.maxGraphEntries>fsr_probe::hardMaxGraphEntries||
       !request.limits.maxBytes||request.limits.maxBytes>fsr_probe::hardMaxBytes||
       !request.limits.maxReadCalls||request.limits.maxReadCalls>fsr_probe::hardMaxReadCalls) {fail(Status::InvalidLimits);return;}
    if(!request.moduleBase||!request.prepareInputPayload||!request.graphContext) {fail(Status::InvalidAddress);return;}
    if(!core::validRange(request.moduleBase,currentPropertyRvas.back()+4)||
       !core::validRange(request.prepareInputPayload,payloadBytes)||!core::validRange(request.graphContext,0x18)) {
        fail(Status::AddressOverflow);return;
    }
    if(core::overlaps(request.prepareInputPayload,payloadBytes,request.graphContext,0x18)) {fail(Status::PointerCycleOrOverlap);return;}
    if(request.nativeTableBase&&request.nativeLayout!=NativeTableLayout::DirectArrayOf1024PagePointers_1024ObjectPointers) {
        fail(Status::UnknownNativeTableLayout);return;
    }
    core::BoundedReader<Result> reader(callback,request.limits,result);
    DescriptorBytes descriptors{},afterDescriptors{};
    auto status=reader.read(request.prepareInputPayload+descriptorOffset,descriptors.data(),descriptors.size());
    if(status!=ReadStatus::SnapshotParsed) {fail(Status::ReadOrGraphRejected,status);return;}
    unsigned seen=0;
    for(std::size_t i=0;i<bindingCount;++i) {
        auto& binding=result.bindings[i]; binding=descriptorBinding(request,descriptors,i);
        if(binding.property==Property::Unknown) {fail(Status::UnknownProperty,ReadStatus::NotAttempted,i);return;}
        const auto mask=1u<<(static_cast<unsigned>(binding.property)-1);
        if(seen&mask) {fail(Status::DuplicateProperty,ReadStatus::NotAttempted,i);return;}
        seen|=mask;
    }
    Address table{},afterTable{};
    status=reader.pointer(request.graphContext+0x10,table);
    if(status!=ReadStatus::SnapshotParsed) {fail(Status::ReadOrGraphRejected,status);return;}
    result.graphBegin=table; // Observed base only; graphEnd/count remain unknown.
    for(std::size_t i=0;i<bindingCount;++i) {
        auto& binding=result.bindings[i];
        status=core::indexedGraphEntry(table,binding.graphKey,request.limits.maxGraphEntries,binding.graphEntry);
        if(status!=ReadStatus::SnapshotParsed) {fail(Status::ReadOrGraphRejected,status,i);return;}
        if(core::overlaps(binding.graphEntry,16,request.prepareInputPayload,payloadBytes)||
           core::overlaps(binding.graphEntry,16,request.graphContext,0x18)||
           (core::validRange(request.nativeTableBase,core::pointerPageBytes)&&
            core::overlaps(binding.graphEntry,16,request.nativeTableBase,core::pointerPageBytes))) {
            fail(Status::ReadOrGraphRejected,ReadStatus::PointerCycleOrOverlap,i);return;
        }
    }
    std::array<std::array<unsigned char,16>,bindingCount> entries{};
    for(std::size_t i=0;i<bindingCount;++i) {
        status=reader.read(result.bindings[i].graphEntry,entries[i].data(),16);
        if(status!=ReadStatus::SnapshotParsed) {fail(Status::ReadOrGraphRejected,status,i);return;}
    }
    for(std::size_t i=0;i<bindingCount;++i) {
        auto& binding=result.bindings[i]; resolveDirectBinding(request,result,binding,reader,entries[i]);
        if(binding.graphStatus!=ReadStatus::GraphTextureIdRead) {fail(Status::BindingRejected,binding.graphStatus,i);return;}
        if(request.nativeTableBase&&binding.native.status!=ReadStatus::NativeCandidateReadUnverified) {
            fail(Status::BindingRejected,binding.native.status,i);return;
        }
    }
    // The second sweep rereads the actual selected entries, not an invented
    // array span or an alleged context+18 end pointer.
    status=reader.read(request.prepareInputPayload+descriptorOffset,afterDescriptors.data(),afterDescriptors.size());
    if(status!=ReadStatus::SnapshotParsed) {fail(Status::ReadOrGraphRejected,status);return;}
    result.descriptorsUnchanged=descriptors==afterDescriptors;
    if(!result.descriptorsUnchanged) {fail(Status::DescriptorsChanged);return;}
    status=reader.pointer(request.graphContext+0x10,afterTable);
    if(status!=ReadStatus::SnapshotParsed) {fail(Status::ReadOrGraphRejected,status);return;}
    result.contextTableUnchanged=table==afterTable;
    if(!result.contextTableUnchanged) {fail(Status::GraphTableChanged);return;}
    for(std::size_t i=0;i<bindingCount;++i) {
        std::array<unsigned char,16> after{};
        status=reader.read(result.bindings[i].graphEntry,after.data(),after.size());
        if(status!=ReadStatus::SnapshotParsed) {fail(Status::ReadOrGraphRejected,status,i);return;}
        if(entries[i]!=after) {fail(Status::GraphEntriesChanged,ReadStatus::NotAttempted,i);return;}
    }
    result.graphEntriesUnchanged=true;
    Result verification=result; verification.graphTextureIdsRead=verification.nativeCandidatesRead=0;
    for(std::size_t i=0;i<bindingCount;++i) {
        auto again=descriptorBinding(request,descriptors,i); again.graphEntry=result.bindings[i].graphEntry;
        resolveDirectBinding(request,verification,again,reader,entries[i]);
        if(again.graphStatus!=ReadStatus::GraphTextureIdRead) {fail(Status::BindingRejected,again.graphStatus,i);return;}
        if(request.nativeTableBase&&again.native.status!=ReadStatus::NativeCandidateReadUnverified) {
            fail(Status::BindingRejected,again.native.status,i);return;
        }
        if(!sameMetadata(result.bindings[i],again)) {fail(Status::BindingMetadataChanged,ReadStatus::NotAttempted,i);return;}
    }
    result.bindingMetadataUnchanged=true; result.graphEntriesObserved=bindingCount;
    result.status=Status::SnapshotParsed; result.detailStatus=ReadStatus::SnapshotParsed;
    result.snapshotCaptured=result.allPropertiesPresent=true;
}

} // namespace

Property propertyAtAddress(Address moduleBase,Address address) noexcept {
    if(!moduleBase||address<moduleBase) return Property::Unknown;
    for(std::size_t i=0;i<currentPropertyRvas.size();++i)
        if(address-moduleBase==currentPropertyRvas[i]) return static_cast<Property>(i+1);
    return Property::Unknown;
}
const char* propertyName(Property property) noexcept {
    switch(property) {
    case Property::InputMotionVectors:return "r_input_motion_vectors";
    case Property::InputDepth:return "r_input_depth";
    case Property::InputColorJittered:return "r_input_color_jittered";
    case Property::WriteDilatedMotionVectors:return "rw_dilated_motion_vectors";
    case Property::WriteDilatedDepth:return "rw_dilated_depth";
    case Property::WriteReconstructedPreviousNearestDepth:return "rw_reconstructed_previous_nearest_depth";
    case Property::WriteFarthestDepth:return "rw_farthest_depth";
    case Property::WriteCurrentLuma:return "rw_current_luma";
    default:return "unknown";
    }
}
const char* statusName(Status status) noexcept {
#define CASE(name) case Status::name:return #name
    switch(status) {
    CASE(NotAttempted);CASE(SnapshotParsed);CASE(UnknownInputLayout);CASE(MissingReader);
    CASE(InvalidLimits);CASE(InvalidAddress);CASE(AddressOverflow);CASE(PointerCycleOrOverlap);
    CASE(UnknownNativeTableLayout);CASE(UnknownProperty);CASE(DuplicateProperty);CASE(ReadOrGraphRejected);
    CASE(BindingRejected);CASE(DescriptorsChanged);CASE(GraphEntriesChanged);CASE(BindingMetadataChanged);
    CASE(AllocationFailed);CASE(CallbackException);CASE(GraphTableChanged);
    }
#undef CASE
    return "UnknownStatus";
}
Result readUpscalerPrepare(const Request& source,Reader reader) noexcept {
    // A caller callback may mutate source; freeze the complete request first.
    const Request request=source;
    Result result;
    try {
        if(request.layout==InputLayout::UnityPlayer9aa9397aUpscalerDirectIndexEight) parseIndexed(request,reader,result);
        else parse(request,reader,result);
    }
    catch(const std::bad_alloc&) {
        result.status=Status::AllocationFailed; result.detailStatus=ReadStatus::AllocationFailed;
        result.bindings={};result.graphTextureIdsRead=0;result.nativeCandidatesRead=0;
        result.snapshotCaptured=false;result.allPropertiesPresent=false;
    }
    catch(...) {
        result.status=Status::CallbackException;result.detailStatus=ReadStatus::CallbackException;
        result.bindings={};result.graphTextureIdsRead=0;result.nativeCandidatesRead=0;
        result.snapshotCaptured=false;result.allPropertiesPresent=false;
    }
    return result;
}
} // namespace xess_vk::upscale_probe
