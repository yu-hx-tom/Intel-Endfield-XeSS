#include "upscale_tail_probe.hpp"
#include "fsr_probe_internal.hpp"
#include <algorithm>
#include <cstring>
#include <tuple>

namespace xess_vk::upscale_tail_probe {
namespace {
namespace core=fsr_probe::detail;
constexpr std::array<Address,3> rcasProperties{0x1EDE550,0x1EDE590,0x1EDE5E8};
constexpr std::array<Address,12> accumulateProperties{
    0x1EDE550,0x1EDE574,0x1EDE54C,0x1EDE584,0x1EDE588,0x1EDE580,
    0x1EDE544,0x1EDE58C,0x1EDE540,0x1EDE5E4,0x1EDE5E8,0x1EDE5D0};
constexpr std::array<Address,3> reactiveProperties{0x1EDE520,0x1EDE524,0x1EDE528};
constexpr std::array<Address,4> transparencyProperties{0x1EDE520,0x1EDE524,0x1EDE530,0x1EDE534};
constexpr std::array<Address,11> pyramidProperties{
    0x1EDE544,0x1EDE554,0x1EDE5A8,0x1EDE5AC,0x1EDE5B0,0x1EDE5B4,
    0x1EDE5B8,0x1EDE5BC,0x1EDE5C0,0x1EDE5C4,0x1EDE5C8};
constexpr std::array<Address,11> pyramidAlternateProperties{
    0x1EDE544,0x1EDE548,0x1EDE54C,0x1EDE550,0x1EDE5A8,0x1EDE5B0,
    0x1EDE5B4,0x1EDE5B8,0x1EDE5BC,0x1EDE5C0,0x1EDE5C4};
constexpr std::array<Address,2> shadingChangeProperties{0x1EDE558,0x1EDE5D8};
constexpr std::array<Address,12> reactivityProperties{
    0x1EDE55C,0x1EDE54C,0x1EDE560,0x1EDE564,0x1EDE568,0x1EDE56C,
    0x1EDE570,0x1EDE544,0x1EDE550,0x1EDE5CC,0x1EDE5D0,0x1EDE5D4};
constexpr std::array<Address,9> instabilityProperties{
    0x1EDE550,0x1EDE574,0x1EDE54C,0x1EDE578,0x1EDE57C,0x1EDE580,
    0x1EDE544,0x1EDE5DC,0x1EDE5E0};
bool same(const Binding& a,const Binding& b) noexcept {
    const auto& x=a.native;const auto& y=b.native;
    return std::tie(a.graphStatus,a.graphEntry,a.entryType,a.resourcePayload,a.textureId,x.status,x.pageSlot,x.page,x.objectSlot,x.object,x.widthCandidate,x.heightCandidate,x.imageCandidate)==
           std::tie(b.graphStatus,b.graphEntry,b.entryType,b.resourcePayload,b.textureId,y.status,y.pageSlot,y.page,y.objectSlot,y.object,y.widthCandidate,y.heightCandidate,y.imageCandidate);
}
void resolve(const Request& request,Result& result,Binding& b,core::BoundedReader<Result>& reader,const std::array<unsigned char,16>& entry) noexcept {
    b.entryType=core::u32(entry.data()+12);b.resourcePayload=core::u64(entry.data());
    if(b.entryType!=2&&b.entryType!=3){b.graphStatus=ReadStatus::UnsupportedGraphEntryType;return;}
    if(!b.resourcePayload){b.graphStatus=ReadStatus::NullResourcePayload;return;}
    if(!core::validRange(b.resourcePayload,4)){b.graphStatus=ReadStatus::AddressOverflow;return;}
    if(core::overlapsInput(request,result,b.resourcePayload,4)){b.graphStatus=ReadStatus::PointerCycleOrOverlap;return;}
    b.graphStatus=reader.word(b.resourcePayload,b.textureId);
    if(b.graphStatus!=ReadStatus::SnapshotParsed)return;
    if(!b.textureId){b.graphStatus=ReadStatus::NullTextureId;return;}
    b.graphStatus=ReadStatus::GraphTextureIdRead;core::resolveNative(request,result,b,reader);
}
template<std::size_t N>
ReadStatus constants(const Request& request,Result& result,core::BoundedReader<Result>& reader,std::size_t offset,Constants<N>& out) noexcept {
    const auto* slice=result.payload.data()+offset;
    out.bufferId=core::u32(slice);out.byteOffset=core::u32(slice+4);out.byteSize=core::u32(slice+8);out.cpuPointer=core::u64(slice+16);
    if(out.byteSize!=N||out.byteOffset>UINT32_MAX-N)return ReadStatus::ConstantsSizeMismatch;
    if(!core::validRange(out.cpuPointer,N))return ReadStatus::InvalidAddress;
    if(core::overlapsInput(request,result,out.cpuPointer,N)||
       (request.nativeTableBase&&core::overlaps(out.cpuPointer,N,request.nativeTableBase,core::pointerPageBytes)))return ReadStatus::PointerCycleOrOverlap;
    std::array<unsigned char,N> after{};
    auto status=reader.read(out.cpuPointer,out.bytes.data(),out.bytes.size());
    if(status!=ReadStatus::SnapshotParsed)return status;
    status=reader.read(out.cpuPointer,after.data(),after.size());
    if(status!=ReadStatus::SnapshotParsed)return status;
    if(out.bytes!=after)return ReadStatus::ConstantsDescriptorChanged;
    out.captured=out.bytesUnchanged=true;return ReadStatus::SnapshotParsed;
}
}
Profile profile(Pass pass) noexcept {
    if(pass==Pass::Rcas)return {0xEB2210,0xEB3880,0xEB2870,0x1836FAC,0xB8,3,0x88,0xA0};
    if(pass==Pass::Accumulate)return {0xEB23A0,0xEB5E10,0xEB36A0,0x1836D0C,0x1C0,12,0x1A8,0};
    if(pass==Pass::Reactive)return {0xEB2270,0xEB4090,0xEB2BD0,0x1834B1B,0x128,3,0,0,0,0x28,4,0x88,0,false,true};
    if(pass==Pass::Transparency)return {0xEB2290,0xEB4370,0xEB2D90,0x1834F03,0x148,4,0,0,0,0x28,4,0xA8};
    if(pass==Pass::Pyramid)return {0xEB2300,0xEB4BF0,0xEB30C0,0x1835826,0x1A0,11,0x170,0,0x1835D5A,0x10,4,0,0x188,true,true};
    if(pass==Pass::ShadingChange)return {0xEB2340,0xEB5130,0xEB3390,0x1835EF8,0x80,2,0x68,0,0,0x28,0x1C,0,0,false,true};
    if(pass==Pass::PrepareReactivity)return {0xEB2360,0xEB5980,0xEB34D0,0x18363E8,0x1C0,12,0x1A8,0,0,0x28,4};
    if(pass==Pass::LumaInstability)return {0xEB2380,0xEB5BC0,0xEB3570,0x18367FE,0x160,9,0x148,0,0,0x28,4};
    return {};
}
Address registrationPayload(Pass pass,Address module,Address graph,std::uint32_t node,Address returned,Reader callback) noexcept {
    const auto spec=profile(pass);
    if(!spec.payloadBytes||!core::validRange(returned,spec.registrationReturnsGraphArray?24:spec.payloadBytes))return 0;
    if(!spec.registrationReturnsGraphArray)return returned;
    constexpr std::uint32_t nodeLimit=8192;
    if(!callback.read||node>=nodeLimit||!core::validRange(graph,16)||!core::validRange(module,spec.thunk+1))return 0;
    struct Counts {std::size_t readCalls{},bytesRequested{},bytesCopied{};} counts;
    core::BoundedReader<Counts> reader(callback,{nodeLimit,96,8},counts);
    Address table{},slot{};
    if(reader.pointer(graph+8,table)!=ReadStatus::SnapshotParsed||table!=returned||
       !core::add(table,Address(node)*48+8,slot)||!core::validRange(slot,16)||core::overlaps(graph,16,slot,16))return 0;
    std::array<unsigned char,16> entry{},again{};
    if(reader.read(slot,entry.data(),entry.size())!=ReadStatus::SnapshotParsed||core::u64(entry.data())!=module+spec.thunk)return 0;
    const auto payload=core::u64(entry.data()+8);
    if(!core::validRange(payload,spec.payloadBytes)||core::overlaps(payload,spec.payloadBytes,graph,16)||core::overlaps(payload,spec.payloadBytes,slot,16))return 0;
    if(reader.pointer(graph+8,table)!=ReadStatus::SnapshotParsed||table!=returned||
       reader.read(slot,again.data(),again.size())!=ReadStatus::SnapshotParsed||entry!=again)return 0;
    return payload;
}
std::span<const Address> propertyRvas(Pass pass,bool alternate) noexcept {
    switch(pass){case Pass::Accumulate:return accumulateProperties;case Pass::Rcas:return rcasProperties;
    case Pass::Reactive:return reactiveProperties;case Pass::Transparency:return transparencyProperties;
    case Pass::Pyramid:return alternate?std::span<const Address>(pyramidAlternateProperties):std::span<const Address>(pyramidProperties);
    case Pass::ShadingChange:return shadingChangeProperties;case Pass::PrepareReactivity:return reactivityProperties;
    case Pass::LumaInstability:return instabilityProperties;default:return {};}
}
Property propertyAtAddress(Address module,Address address) noexcept {
    if(!module||address<module)return Property::Unknown;
    switch(address-module){
    case 0x1EDE550:return Property::Exposure;case 0x1EDE540:return Property::InputColor;
    case 0x1EDE590:return Property::RcasInput;case 0x1EDE584:return Property::InternalColor;
    case 0x1EDE5E4:return Property::WriteInternalColor;case 0x1EDE5E8:return Property::Output;
    case 0x1EDE588:return Property::LanczosLut;case 0x1EDE544:return Property::CurrentLuma;
    default:for(unsigned pass=1;pass<unsigned(Pass::Count);++pass)for(const bool alternate:{false,true})
            for(const auto rva:propertyRvas(Pass(pass),alternate))if(address-module==rva)return Property::OtherSupported;
        return address-module==0x1EDE538?Property::OtherSupported:Property::Unknown;
    }
}
const char* passName(Pass pass) noexcept {
    switch(pass){case Pass::Accumulate:return "accumulate";case Pass::Rcas:return "rcas";
    case Pass::Reactive:return "reactive_eb4090";case Pass::Transparency:return "transparency_eb4370";
    case Pass::Pyramid:return "pyramid_eb4bf0";case Pass::ShadingChange:return "shading_change_eb5130";
    case Pass::PrepareReactivity:return "reactivity_eb5980";case Pass::LumaInstability:return "instability_eb5bc0";
    default:return "unknown";}
}
const char* propertyName(Property property) noexcept {
    switch(property){case Property::Exposure:return "r_input_exposure";case Property::InputColor:return "r_input_color_jittered";
    case Property::RcasInput:return "r_rcas_input";case Property::InternalColor:return "r_internal_upscaled_color";
    case Property::WriteInternalColor:return "rw_internal_upscaled_color";case Property::Output:return "rw_upscaled_output";
    case Property::LanczosLut:return "r_lanczos_lut";case Property::CurrentLuma:return "r_current_luma";
    case Property::OtherSupported:return "supported_other";default:return "unknown";}
}
const char* statusName(Status status) noexcept {
    switch(status){
#define TAIL_STATUS(value) case Status::value:return #value
    TAIL_STATUS(NotAttempted);TAIL_STATUS(Parsed);TAIL_STATUS(UnknownProfile);TAIL_STATUS(InvalidLimits);
    TAIL_STATUS(MissingReader);TAIL_STATUS(InvalidAddress);TAIL_STATUS(Overflow);TAIL_STATUS(Overlap);
    TAIL_STATUS(UnknownProperty);TAIL_STATUS(DuplicateProperty);TAIL_STATUS(ReadRejected);TAIL_STATUS(BindingRejected);
    TAIL_STATUS(PayloadChanged);TAIL_STATUS(TableChanged);TAIL_STATUS(EntryChanged);TAIL_STATUS(MetadataChanged);TAIL_STATUS(ConstantsRejected);
#undef TAIL_STATUS
    }return "unknown";
}
Result read(const Request& request,Reader callback) noexcept {
    Result result;result.pass=request.pass;const auto spec=profile(request.pass);result.payloadExtent=spec.payloadBytes;
    const auto fail=[&](Status status,ReadStatus detail=ReadStatus::NotAttempted,std::size_t index=maxBindings){
        if(index<result.bindings.size()){const auto& b=result.bindings[index];result.failurePropertyAddress=b.propertyAddress;result.failureGraphHandle=b.graphHandle;
            result.failureGraphKey=b.graphKey;result.failureGraphEntry=b.graphEntry;result.failureEntryType=b.entryType;result.failureTextureId=b.textureId;
            result.failureGraphStatus=b.graphStatus;result.failureNativeStatus=b.native.status;}
        result.status=status;result.detail=detail;result.failureBindingIndex=index;result.bindings={};result.payload={};
        result.constants={};result.rcas={};result.reactive={};result.spd={};result.groups={};result.rawKernelToken=0;result.bindingCount=result.graphTextureIdsRead=result.nativeCandidatesRead=0;
        result.snapshotCaptured=false;return result;
    };
    if(!spec.payloadBytes)return fail(Status::UnknownProfile);
    if(!callback.read)return fail(Status::MissingReader);
    if(!request.limits.maxGraphEntries||request.limits.maxGraphEntries>fsr_probe::hardMaxGraphEntries||
       !request.limits.maxBytes||request.limits.maxBytes>fsr_probe::hardMaxBytes||
       !request.limits.maxReadCalls||request.limits.maxReadCalls>fsr_probe::hardMaxReadCalls)return fail(Status::InvalidLimits);
    if(!request.moduleBase||!request.prepareInputPayload||!request.graphContext)return fail(Status::InvalidAddress);
    if(!core::validRange(request.moduleBase,0x1EDE5EC)||!core::validRange(request.prepareInputPayload,spec.payloadBytes)||
       !core::validRange(request.graphContext,0x18))return fail(Status::Overflow);
    if(core::overlaps(request.prepareInputPayload,spec.payloadBytes,request.graphContext,0x18))return fail(Status::Overlap);
    if(request.nativeTableBase&&request.nativeLayout!=fsr_probe::NativeTableLayout::DirectArrayOf1024PagePointers_1024ObjectPointers)
        return fail(Status::BindingRejected,ReadStatus::UnknownNativeTableLayout);
    core::BoundedReader<Result> reader(callback,request.limits,result);
    auto status=reader.read(request.prepareInputPayload,result.payload.data(),spec.payloadBytes);
    if(status!=ReadStatus::SnapshotParsed)return fail(Status::ReadRejected,status);
    const auto expected=[&](Address property,bool alternate)->std::size_t{
        if(property<request.moduleBase)return maxBindings;const auto rva=property-request.moduleBase;
        const auto properties=propertyRvas(request.pass,alternate);
        for(std::size_t n=0;n<properties.size();++n)
            if(rva==properties[n]||(request.pass==Pass::Accumulate&&n==2&&rva==0x1EDE538))return n;
        return maxBindings;
    };
    std::uint32_t seen{},alternateSeen{};
    for(std::size_t n=0;n<spec.bindingCount;++n){auto& b=result.bindings[n];const auto* raw=result.payload.data()+spec.imageOffset+n*descriptorStride;
        b.descriptor=request.prepareInputPayload+spec.imageOffset+n*descriptorStride;b.propertyAddress=core::u64(raw+16);
        const auto property=expected(b.propertyAddress,false),alternate=expected(b.propertyAddress,true);
        if(property==maxBindings&&alternate==maxBindings)return fail(Status::UnknownProperty,ReadStatus::NotAttempted,n);
        for(std::size_t before=0;before<n;++before)if(result.bindings[before].propertyAddress==b.propertyAddress)return fail(Status::DuplicateProperty,ReadStatus::NotAttempted,n);
        if(property<maxBindings)seen|=1u<<property;if(alternate<maxBindings)alternateSeen|=1u<<alternate;
        b.property=propertyAtAddress(request.moduleBase,b.propertyAddress);b.graphHandle=core::u64(raw);b.graphKey=std::uint32_t(b.graphHandle);
        b.rawDependencyDirection=raw[8];b.rawSubresource=core::u32(raw+24);
    }
    const auto required=(1u<<spec.bindingCount)-1;
    if(seen!=required&&alternateSeen!=required)return fail(Status::UnknownProperty);
    status=reader.pointer(request.graphContext+0x10,result.graphBegin);
    if(status!=ReadStatus::SnapshotParsed)return fail(Status::ReadRejected,status);
    std::array<std::array<unsigned char,16>,maxBindings> entries{};
    for(std::size_t n=0;n<spec.bindingCount;++n){auto& b=result.bindings[n];
        status=core::indexedGraphEntry(result.graphBegin,b.graphKey,request.limits.maxGraphEntries,b.graphEntry);
        if(status!=ReadStatus::SnapshotParsed)return fail(Status::BindingRejected,status,n);
        if(core::overlaps(b.graphEntry,16,request.prepareInputPayload,spec.payloadBytes)||core::overlaps(b.graphEntry,16,request.graphContext,0x18)||
           (request.nativeTableBase&&core::validRange(request.nativeTableBase,core::pointerPageBytes)&&core::overlaps(b.graphEntry,16,request.nativeTableBase,core::pointerPageBytes)))
            return fail(Status::BindingRejected,ReadStatus::PointerCycleOrOverlap,n);
    }
    for(std::size_t n=0;n<spec.bindingCount;++n){status=reader.read(result.bindings[n].graphEntry,entries[n].data(),16);
        if(status!=ReadStatus::SnapshotParsed)return fail(Status::ReadRejected,status,n);}
    for(std::size_t n=0;n<spec.bindingCount;++n){auto& b=result.bindings[n];resolve(request,result,b,reader,entries[n]);
        if(b.graphStatus!=ReadStatus::GraphTextureIdRead)return fail(Status::BindingRejected,b.graphStatus,n);
        if(request.nativeTableBase&&b.native.status!=ReadStatus::NativeCandidateReadUnverified)return fail(Status::BindingRejected,b.native.status,n);}
    if(spec.constantsOffset){status=constants(request,result,reader,spec.constantsOffset,result.constants);
        if(status!=ReadStatus::SnapshotParsed)return fail(Status::ConstantsRejected,status);}
    if(spec.inlineConstantsOffset){std::copy_n(result.payload.data()+spec.inlineConstantsOffset,result.reactive.bytes.size(),result.reactive.bytes.data());
        result.reactive.byteSize=result.reactive.bytes.size();result.reactive.captured=true;}
    if(spec.spdOffset){status=constants(request,result,reader,spec.spdOffset,result.spd);
        if(status!=ReadStatus::SnapshotParsed)return fail(Status::ConstantsRejected,status);
        if(core::overlaps(result.constants.cpuPointer,148,result.spd.cpuPointer,24))return fail(Status::ConstantsRejected,ReadStatus::PointerCycleOrOverlap);}
    if(spec.rcasOffset){status=constants(request,result,reader,spec.rcasOffset,result.rcas);
        if(status!=ReadStatus::SnapshotParsed)return fail(Status::ConstantsRejected,status);
        if(core::overlaps(result.constants.cpuPointer,148,result.rcas.cpuPointer,16))return fail(Status::ConstantsRejected,ReadStatus::PointerCycleOrOverlap);}
    std::array<unsigned char,maxPayloadBytes> after{};status=reader.read(request.prepareInputPayload,after.data(),spec.payloadBytes);
    if(status!=ReadStatus::SnapshotParsed)return fail(Status::ReadRejected,status);
    if(result.payload!=after)return fail(Status::PayloadChanged);
    result.payloadUnchanged=true;result.reactive.bytesUnchanged=result.reactive.captured;Address table{};status=reader.pointer(request.graphContext+0x10,table);
    if(status!=ReadStatus::SnapshotParsed)return fail(Status::ReadRejected,status);
    if(table!=result.graphBegin)return fail(Status::TableChanged);result.tableUnchanged=true;
    for(std::size_t n=0;n<spec.bindingCount;++n){std::array<unsigned char,16> entry{};status=reader.read(result.bindings[n].graphEntry,entry.data(),16);
        if(status!=ReadStatus::SnapshotParsed)return fail(Status::ReadRejected,status,n);
        if(entry!=entries[n])return fail(Status::EntryChanged,ReadStatus::NotAttempted,n);}
    result.entriesUnchanged=true;
    for(std::size_t n=0;n<spec.bindingCount;++n){auto again=result.bindings[n];again.native={};resolve(request,result,again,reader,entries[n]);
        if(!same(result.bindings[n],again))return fail(Status::MetadataChanged,again.graphStatus,n);}
    result.metadataUnchanged=result.snapshotCaptured=true;result.bindingCount=spec.bindingCount;
    result.graphTextureIdsRead=spec.bindingCount;result.nativeCandidatesRead=request.nativeTableBase?spec.bindingCount:0;
    result.rawKernelToken=core::u32(result.payload.data());for(std::size_t n=0;n<3;++n)result.groups[n]=spec.implicitGroupZ&&n==2?1:core::u32(result.payload.data()+spec.groupsOffset+4*n);
    result.status=Status::Parsed;return result;
}
}
