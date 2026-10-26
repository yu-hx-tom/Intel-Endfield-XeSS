#include "upscale_tail_probe.hpp"
#include <algorithm>
#include <cstring>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
namespace p=xess_vk::upscale_tail_probe;
unsigned checks{};
void require(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
struct Memory {
    struct Region {std::uint64_t base;std::vector<unsigned char> bytes;};
    std::vector<Region> regions;
    std::function<void(std::uint64_t,std::size_t)> before;
    std::uint64_t partial{},throwAt{};
    void add(std::uint64_t base,std::size_t size){regions.push_back({base,std::vector<unsigned char>(size)});}
    unsigned char* at(std::uint64_t address,std::size_t size){
        for(auto& r:regions)if(address>=r.base&&size<=r.bytes.size()&&address-r.base<=r.bytes.size()-size)return r.bytes.data()+address-r.base;
        throw std::runtime_error("test address absent");
    }
    template<class T>void put(std::uint64_t address,T value){std::memcpy(at(address,sizeof(value)),&value,sizeof(value));}
    static std::size_t read(void* user,std::uint64_t address,void* output,std::size_t size,std::size_t budget){
        auto& m=*static_cast<Memory*>(user);require(size<=budget,"reader obeys remaining budget");
        if(m.before)m.before(address,size);if(address==m.throwAt)throw std::runtime_error("test callback throw");
        try{const auto copied=address==m.partial?size/2:size;std::memcpy(output,m.at(address,size),copied);return copied;}catch(...){return 0;}
    }
};
struct Fixture {
    static constexpr std::uint64_t module=0x180000000,payload=0x100000,context=0x200000,table=0x300000,resources=0x400000,native=0x500000,page=0x600000,objects=0x700000,cb=0x800000,rcasCb=0x900000,spdCb=0xA00000;
    Memory memory;p::Request request;
    explicit Fixture(p::Pass pass=p::Pass::Rcas){
        request.pass=pass;request.moduleBase=module;request.prepareInputPayload=payload;request.graphContext=context;
        request.nativeTableBase=native;request.nativeLayout=xess_vk::fsr_probe::NativeTableLayout::DirectArrayOf1024PagePointers_1024ObjectPointers;
        const auto s=p::profile(pass);memory.add(payload,s.payloadBytes);memory.add(context,24);memory.add(table,8192*16);
        memory.add(resources,4096);memory.add(native,8192);memory.add(page,8192);memory.add(objects,4096);memory.add(cb,148);memory.add(rcasCb,16);memory.add(spdCb,24);
        memory.put(context+16,table);memory.put(native,page);memory.put<std::uint32_t>(payload,1234);
        for(unsigned n=0;n<(s.implicitGroupZ?2u:3u);++n)memory.put<std::uint32_t>(payload+s.groupsOffset+4*n,3+n);
        const auto props=p::propertyRvas(pass);
        for(std::size_t n=0;n<s.bindingCount;++n){const auto d=payload+s.imageOffset+n*32;const auto key=n+1,id=n+11;
            memory.put<std::uint64_t>(d,0xCAFEBABE00000000ull|key);memory.put(d+16,module+props[n]);
            memory.put<std::uint8_t>(d+8,n==s.bindingCount-1);memory.put<std::uint32_t>(d+24,static_cast<unsigned>(n));
            memory.put<std::uint64_t>(table+16*key,resources+8*n);memory.put<std::uint32_t>(table+16*key+12,2+n%2);
            memory.put<std::uint32_t>(resources+8*n,static_cast<unsigned>(id));memory.put(page+8*id,objects+64*n);
            memory.put<std::uint32_t>(objects+64*n+20,320);memory.put<std::uint32_t>(objects+64*n+24,180);memory.put<std::uint64_t>(objects+64*n+56,0xA000+n);
        }
        if(s.constantsOffset)slice(s.constantsOffset,cb,148);if(s.rcasOffset)slice(s.rcasOffset,rcasCb,16);if(s.spdOffset)slice(s.spdOffset,spdCb,24);
        if(s.inlineConstantsOffset)for(unsigned n=0;n<160;++n)*memory.at(payload+s.inlineConstantsOffset+n,1)=static_cast<unsigned char>(159-n);
        for(unsigned n=0;n<148;++n)*memory.at(cb+n,1)=static_cast<unsigned char>(n);
        for(unsigned n=0;n<16;++n)*memory.at(rcasCb+n,1)=static_cast<unsigned char>(15-n);
    }
    void slice(std::size_t offset,std::uint64_t pointer,unsigned bytes){memory.put<std::uint32_t>(payload+offset,100);memory.put<std::uint32_t>(payload+offset+4,256);memory.put(payload+offset+8,bytes);memory.put(payload+offset+16,pointer);}
    p::Result run(){return p::read(request,{Memory::read,&memory});}
};
void rejected(const p::Result& r,p::Status expected){
    require(r.status==expected,"expected refusal status");require(!r.snapshotCaptured&&!r.bindingCount&&!r.graphTextureIdsRead&&!r.nativeCandidatesRead,"failure publishes no counts");
    require(!r.constants.captured&&!r.rcas.captured&&!r.reactive.captured&&!r.spd.captured,"failure publishes no constants");
    require(std::all_of(r.payload.begin(),r.payload.end(),[](auto x){return x==0;}),"failure clears payload");
    require(std::all_of(r.bindings.begin(),r.bindings.end(),[](const auto& x){return !x.textureId&&!x.propertyAddress&&!x.native.imageCandidate;}),"failure clears bindings");
}
void auxiliaryJobs(){
    struct Expected{p::Pass pass;std::size_t bytes,bindings,images,groups;};
    const Expected expected[]{
        {p::Pass::Reactive,296,3,40,4},{p::Pass::Transparency,328,4,40,4},
        {p::Pass::Pyramid,416,11,16,4},{p::Pass::ShadingChange,128,2,40,28},
        {p::Pass::PrepareReactivity,448,12,40,4},{p::Pass::LumaInstability,352,9,40,4}};
    for(const auto& e:expected){Fixture f(e.pass);const auto s=p::profile(e.pass);const auto r=f.run();
        require(s.payloadBytes==e.bytes&&s.bindingCount==e.bindings&&s.imageOffset==e.images&&s.groupsOffset==e.groups,"audited native callback layouts remain distinct");
        require(r.status==p::Status::Parsed&&r.bindingCount==e.bindings&&r.payloadUnchanged&&r.metadataUnchanged,"all auxiliary jobs parse with two stable sweeps");
        require(r.groups==std::array<std::uint32_t,3>{3,4,e.pass==p::Pass::Pyramid?1u:5u},"each native callback uses its actual dispatch header; Pyramid fixes Z to one");
        require(r.reactive.captured==(e.pass==p::Pass::Reactive||e.pass==p::Pass::Transparency)&&r.constants.captured==bool(s.constantsOffset),"inline reactive parameters do not impersonate a 148-byte descriptor");
        if(r.reactive.captured)require(r.reactive.bytes[0]==159&&r.reactive.bytes[159]==0&&r.reactive.bytesUnchanged,"full inline 160-byte constants validated with payload stability");
        require(r.spd.captured==(e.pass==p::Pass::Pyramid),"Pyramid's second descriptor reads the separate 24-byte SPD constants");
        {Fixture unknown(e.pass);unknown.memory.put(Fixture::payload+s.imageOffset+16,Fixture::module+0x1EDEFFF);rejected(unknown.run(),p::Status::UnknownProperty);}
        {Fixture duplicate(e.pass);duplicate.memory.put(Fixture::payload+s.imageOffset+32+16,Fixture::module+p::propertyRvas(e.pass)[0]);rejected(duplicate.run(),p::Status::DuplicateProperty);}
        {Fixture mutation(e.pass);unsigned n{};mutation.memory.before=[&](auto a,auto){if(a==Fixture::payload&&++n==2)mutation.memory.put<std::uint32_t>(Fixture::payload+s.groupsOffset,99);};rejected(mutation.run(),p::Status::PayloadChanged);}
    }
    {Fixture alternate(p::Pass::Pyramid);const auto s=p::profile(p::Pass::Pyramid);
        for(std::size_t n=0;n<s.bindingCount;++n)alternate.memory.put(Fixture::payload+s.imageOffset+32*n+16,Fixture::module+p::propertyRvas(p::Pass::Pyramid,true)[n]);
        require(alternate.run().status==p::Status::Parsed,"both exact Pyramid branches supported");}
    {Fixture hybrid(p::Pass::Pyramid);hybrid.memory.put(Fixture::payload+16+32+16,Fixture::module+0x1EDE548);
        rejected(hybrid.run(),p::Status::UnknownProperty);}
    {Fixture spd(p::Pass::Pyramid);spd.slice(0x188,Fixture::spdCb,16);rejected(spd.run(),p::Status::ConstantsRejected);}
    {Fixture alias(p::Pass::Pyramid);alias.slice(0x188,Fixture::cb,24);rejected(alias.run(),p::Status::ConstantsRejected);}
    {Fixture mutation(p::Pass::Pyramid);unsigned n{};mutation.memory.before=[&](auto a,auto){if(a==Fixture::spdCb&&++n==2)*mutation.memory.at(a,1)=99;};rejected(mutation.run(),p::Status::ConstantsRejected);}
    {Fixture invalid;invalid.request.pass=p::Pass::Count;rejected(invalid.run(),p::Status::UnknownProfile);}
    require(p::propertyRvas(p::Pass::ShadingChange)[1]==0x1EDE5D8,"retained game failure and native builder identify rw_shading_change, not its separate sampled property");
    {Fixture oldProperty(p::Pass::ShadingChange);oldProperty.memory.put(Fixture::payload+40+32+16,Fixture::module+0x1EDE55C);rejected(oldProperty.run(),p::Status::UnknownProperty);}
}
void registrationReturns(){
    constexpr std::uint64_t graph=0xB00000,nodes=0xC00000;
    constexpr std::uint32_t node=2;
    const auto slot=nodes+node*48+8;
    const auto setup=[&](Fixture& f){f.memory.add(graph,16);f.memory.add(nodes,3*48);f.memory.put(graph+8,nodes);
        f.memory.put(slot,Fixture::module+p::profile(f.request.pass).thunk);f.memory.put(slot+8,Fixture::payload);};
    const auto resolve=[&](Fixture& f,std::uint64_t returned=nodes){return p::registrationPayload(f.request.pass,Fixture::module,graph,node,returned,{Memory::read,&f.memory});};
    for(unsigned value=1;value<unsigned(p::Pass::Count);++value){const auto pass=p::Pass(value);
        const bool graphReturn=pass==p::Pass::Reactive||pass==p::Pass::Pyramid||pass==p::Pass::ShadingChange;
        require(p::profile(pass).registrationReturnsGraphArray==graphReturn,"only three audited inline-copy native registrations return their graph array");
        Fixture f(pass);setup(f);require(resolve(f,graphReturn?nodes:Fixture::payload)==Fixture::payload,"both real native return contracts recover exact payload");
        if(!graphReturn)require(p::registrationPayload(pass,0,0,0,Fixture::payload,{})==Fixture::payload,"existing payload-returning routes keep zero extra reads");
    }
    {Fixture f(p::Pass::Reactive);setup(f);require(resolve(f,Fixture::payload)==0,"graph-returning route never accepts a guessed payload RAX");}
    {Fixture f(p::Pass::Reactive);setup(f);f.memory.put(slot,Fixture::module+p::profile(p::Pass::Pyramid).thunk);require(resolve(f)==0,"node must carry this exact pinned callback");}
    {Fixture f(p::Pass::Reactive);setup(f);f.memory.put<std::uint64_t>(slot+8,0);require(resolve(f)==0,"null copied payload refused");}
    {Fixture f(p::Pass::Reactive);setup(f);f.memory.put(slot+8,slot);require(resolve(f)==0,"copied payload cannot alias its node entry");}
    {Fixture f(p::Pass::Reactive);setup(f);f.memory.put(slot+8,graph);require(resolve(f)==0,"copied payload cannot alias graph header");}
    {Fixture f(p::Pass::Reactive);setup(f);unsigned n{};f.memory.before=[&](auto a,auto){if(a==graph+8&&++n==2)f.memory.put(graph+8,nodes+48);};require(resolve(f)==0,"graph-array relocation between sweeps refused");}
    {Fixture f(p::Pass::Reactive);setup(f);unsigned n{};f.memory.before=[&](auto a,auto){if(a==slot&&++n==2)f.memory.put(slot+8,Fixture::payload+16);};require(resolve(f)==0,"node reuse between sweeps refused");}
    {Fixture f(p::Pass::Reactive);setup(f);f.memory.partial=slot;require(resolve(f)==0,"partial native node read refused");}
    {Fixture f(p::Pass::Reactive);setup(f);f.memory.throwAt=slot;require(resolve(f)==0,"native node reader exception contained");}
    {Fixture f(p::Pass::Reactive);setup(f);
        require(p::registrationPayload(f.request.pass,Fixture::module,graph,8192,nodes,{Memory::read,&f.memory})==0,"node index support bound is exclusive");
        require(p::registrationPayload(f.request.pass,Fixture::module,UINT64_MAX-8,node,nodes,{Memory::read,&f.memory})==0,"graph-header overflow refused");
        require(p::registrationPayload(f.request.pass,UINT64_MAX-8,graph,node,nodes,{Memory::read,&f.memory})==0,"callback-address overflow refused");
        require(p::registrationPayload(f.request.pass,Fixture::module,graph,node,nodes,{})==0,"graph-returning route requires reader");
        f.memory.put<std::uint64_t>(graph+8,UINT64_MAX-64);
        require(p::registrationPayload(f.request.pass,Fixture::module,graph,node,UINT64_MAX-64,{Memory::read,&f.memory})==0,"node-stride overflow refused");
    }
    require(!p::registrationPayload(p::Pass::Unknown,0,0,0,Fixture::payload,{}),"unknown return contract cannot publish payload");
}
void valid(){
    for(auto pass:{p::Pass::Rcas,p::Pass::Accumulate}){Fixture f(pass);const auto r=f.run();const auto s=p::profile(pass);
        require(r.status==p::Status::Parsed&&r.bindingCount==s.bindingCount&&r.payloadExtent==s.payloadBytes,"both exact profiles parse");
        require(r.payloadUnchanged&&r.tableUnchanged&&r.entriesUnchanged&&r.metadataUnchanged,"all repeated metadata stable");
        require(r.constants.captured&&r.constants.bytesUnchanged&&r.constants.bytes[147]==147,"148 raw constant bytes copied");
        require(r.rcas.captured==(pass==p::Pass::Rcas),"RCAS coefficients separate from exposure image");
        require(r.rawKernelToken==1234&&r.groups==std::array<std::uint32_t,3>{3,4,5},"header fields retained as raw values");
        require(r.bindings[0].graphKey==1&&r.bindings[0].graphHandle==0xCAFEBABE00000001ull&&r.bindings[0].textureId==11,"low32 graph index distinct from full graph handle and ID");
        require(r.bindings[0].native.imageCandidate==0xA000&&!r.bindings[0].native.vulkanImageVerified,"native value remains candidate");
        if(pass==p::Pass::Accumulate)require(r.bindings[4].property==p::Property::LanczosLut&&r.bindings[6].property==p::Property::CurrentLuma&&
            std::strcmp(p::propertyName(r.bindings[4].property),"r_lanczos_lut")==0&&std::strcmp(p::propertyName(r.bindings[6].property),"r_current_luma")==0,
            "fixed Accumulate optional-dispatch properties retain exact names and RVAs");
        require(!r.allocationBoundsVerified&&!r.snapshotAtomic&&!r.sameFrameVerified&&!r.exposureVerified&&!r.hdrVerified&&!r.executePermitted,"parser does not invent semantic or lifetime proof");
        require(r.readCalls<=f.request.limits.maxReadCalls&&r.bytesRequested<=f.request.limits.maxBytes,"bounded complete profile reads");
    }
    Fixture alt(p::Pass::Accumulate);alt.memory.put(Fixture::payload+40+2*32+16,Fixture::module+0x1EDE538);require(alt.run().status==p::Status::Parsed,"Accumulate alternate motion property accepted");
    Fixture reorder;unsigned char temp[32];auto* a=reorder.memory.at(Fixture::payload+40,32);auto* b=reorder.memory.at(Fixture::payload+72,32);
    std::memcpy(temp,a,32);std::memcpy(a,b,32);std::memcpy(b,temp,32);require(reorder.run().status==p::Status::Parsed,"exact set independent of descriptor order");
    Fixture noNative;noNative.request.nativeTableBase=0;const auto r=noNative.run();require(r.status==p::Status::Parsed&&!r.nativeCandidatesRead,"native lookup optional and explicit");
}
void failures(){
    {Fixture f;f.request.pass=p::Pass::Unknown;rejected(f.run(),p::Status::UnknownProfile);}
    {Fixture f;rejected(p::read(f.request,{}),p::Status::MissingReader);}
    {Fixture f;f.request.limits.maxBytes=0;rejected(f.run(),p::Status::InvalidLimits);}
    {Fixture f;f.request.prepareInputPayload=UINT64_MAX-16;rejected(f.run(),p::Status::Overflow);}
    {Fixture f;f.request.graphContext=Fixture::payload;rejected(f.run(),p::Status::Overlap);}
    {Fixture f;f.memory.put(Fixture::payload+56,Fixture::module+0x1EDEFFF);const auto r=f.run();rejected(r,p::Status::UnknownProperty);require(r.failureBindingIndex==0&&r.failurePropertyAddress==Fixture::module+0x1EDEFFF,"failed property retained only in explicit failure metadata");}
    {Fixture f;f.memory.put(Fixture::payload+88,Fixture::module+0x1EDE550);rejected(f.run(),p::Status::DuplicateProperty);}
    {Fixture f;f.memory.put<std::uint64_t>(Fixture::payload+40,8192);const auto r=f.run();rejected(r,p::Status::BindingRejected);require(r.detail==p::ReadStatus::GraphEntryLimitExceeded,"support ceiling is not array count");}
    {Fixture f;f.memory.put<std::uint64_t>(Fixture::table+16,Fixture::payload+48);const auto r=f.run();rejected(r,p::Status::BindingRejected);require(r.detail==p::ReadStatus::PointerCycleOrOverlap,"resource cannot alias payload");}
    {Fixture f;f.memory.put<std::uint32_t>(Fixture::resources,0x100001);const auto r=f.run();rejected(r,p::Status::BindingRejected);require(r.detail==p::ReadStatus::NativeIdOutOfRange,"native ID never masked into domain");}
    {Fixture f;f.memory.put<std::uint32_t>(Fixture::table+28,1);rejected(f.run(),p::Status::BindingRejected);}
    {Fixture f;f.request.limits.maxBytes=10;const auto r=f.run();rejected(r,p::Status::ReadRejected);require(r.detail==p::ReadStatus::ByteBudgetExceeded&&!r.readCalls,"insufficient budget performs no read");}
    {Fixture f;f.request.limits.maxReadCalls=1;rejected(f.run(),p::Status::ReadRejected);}
    {Fixture f;f.memory.partial=Fixture::payload;rejected(f.run(),p::Status::ReadRejected);}
    {Fixture f;f.memory.throwAt=Fixture::payload;const auto r=f.run();rejected(r,p::Status::ReadRejected);require(r.detail==p::ReadStatus::CallbackException,"reader exception contained");}
    {Fixture f;f.slice(0x88,Fixture::cb,147);rejected(f.run(),p::Status::ConstantsRejected);}
    {Fixture f;f.slice(0x88,Fixture::payload+40,148);rejected(f.run(),p::Status::ConstantsRejected);}
    {Fixture f;unsigned n{};f.memory.before=[&](auto address,auto){if(address==Fixture::payload&&++n==2)f.memory.put<std::uint32_t>(Fixture::payload,999);};rejected(f.run(),p::Status::PayloadChanged);}
    {Fixture f;unsigned n{};f.memory.before=[&](auto address,auto){if(address==Fixture::context+16&&++n==2)f.memory.put(Fixture::context+16,Fixture::table+16);};rejected(f.run(),p::Status::TableChanged);}
    {Fixture f;unsigned n{};f.memory.before=[&](auto address,auto){if(address==Fixture::table+16&&++n==2)f.memory.put<std::uint32_t>(Fixture::table+28,3);};rejected(f.run(),p::Status::EntryChanged);}
    {Fixture f;unsigned n{};f.memory.before=[&](auto address,auto){if(address==Fixture::objects+56&&++n==2)f.memory.put<std::uint64_t>(Fixture::objects+56,0xDEAD);};rejected(f.run(),p::Status::MetadataChanged);}
    {Fixture f;unsigned n{};f.memory.before=[&](auto address,auto){if(address==Fixture::cb&&++n==2)*f.memory.at(Fixture::cb,1)=255;};rejected(f.run(),p::Status::ConstantsRejected);}
}
}
int main(){try{valid();auxiliaryJobs();registrationReturns();failures();std::cout<<"upscale tail parser: "<<checks<<" checks passed\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
