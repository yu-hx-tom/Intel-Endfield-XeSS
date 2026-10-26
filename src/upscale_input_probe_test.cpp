#include "upscale_input_probe.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <random>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using namespace xess_vk::upscale_probe;
std::atomic<unsigned long long> cases{0},checks{0};
void require(bool ok,const char* reason) {++checks;if(!ok) throw std::runtime_error(reason);}
struct Read {Address address;std::size_t bytes,remaining;};
enum class Fault {None,Zero,Partial,Throw,OverReport,Change};
struct Memory {
    std::map<Address,std::vector<unsigned char>> regions;
    std::vector<Read> calls;
    Fault fault=Fault::None;
    std::size_t faultCall=0,changeOffset=0;
    std::function<void()> firstRead;
    void region(Address base,std::size_t size) {regions.emplace(base,std::vector<unsigned char>(size));}
    unsigned char* at(Address address,std::size_t bytes) {
        auto found=regions.upper_bound(address);if(found==regions.begin())return nullptr;--found;
        const auto offset=address-found->first;
        if(offset>found->second.size()||bytes>found->second.size()-offset)return nullptr;
        return found->second.data()+static_cast<std::size_t>(offset);
    }
    void word(Address address,std::uint32_t value) {
        auto* data=at(address,4);require(data!=nullptr,"fixture DWORD bounds");
        for(unsigned i=0;i<4;++i)data[i]=static_cast<unsigned char>(value>>(i*8));
    }
    void pointer(Address address,Address value) {
        auto* data=at(address,8);require(data!=nullptr,"fixture pointer bounds");
        for(unsigned i=0;i<8;++i)data[i]=static_cast<unsigned char>(value>>(i*8));
    }
    static std::size_t read(void* user,Address address,void* destination,std::size_t bytes,std::size_t remaining) {
        auto& m=*static_cast<Memory*>(user);
        require(bytes<=remaining,"callback exceeded remaining byte budget");
        m.calls.push_back({address,bytes,remaining});
        if(m.calls.size()==1&&m.firstRead)m.firstRead();
        const bool fail=m.faultCall==m.calls.size();
        if(fail&&m.fault==Fault::Throw)throw std::runtime_error("synthetic callback exception");
        if(fail&&m.fault==Fault::OverReport)return bytes+1;
        if(fail&&m.fault==Fault::Zero)return 0;
        const auto* source=m.at(address,bytes);if(!source)return 0;
        const auto copied=fail&&m.fault==Fault::Partial?bytes-1:bytes;
        std::memcpy(destination,source,copied);
        if(fail&&m.fault==Fault::Change) {
            require(m.changeOffset<bytes,"mutation bounds");
            static_cast<unsigned char*>(destination)[m.changeOffset]^=1;
        }
        return copied;
    }
    bool touched(Address first,Address size=1) const {
        return std::any_of(calls.begin(),calls.end(),[&](const auto& r){
            return r.address<first+size&&first<r.address+r.bytes;
        });
    }
};
struct Fixture {
    static constexpr Address payload=0x10000,context=0x20000,graph=0x30000,resources=0x40000;
    static constexpr Address table=0x100000,objects=0x20000000;
    static constexpr std::array<std::uint32_t,8> ids{1,0x400,0x801,0x10001,0x80001,0xF0001,0xFF801,0xFFFFF};
    static constexpr std::array<unsigned,8> order{3,6,0,7,1,5,2,4};
    static constexpr std::array<Address,8> properties{0x1D10628,0x1D1062C,0x1D10630,0x1D10684,0x1D10688,0x1D1068C,0x1D10690,0x1D10694};
    Memory memory;Request request;
    static Address page(std::uint32_t id) {return 0x200000+Address(id>>10)*0x4000;}
    Fixture(bool native=true) {
        request.layout=InputLayout::UnityPlayerBee7be52UpscalerPrepareEight;
        request.moduleBase=0x700000000;request.prepareInputPayload=payload;request.graphContext=context;
        if(native) {request.nativeTableBase=table;request.nativeLayout=NativeTableLayout::DirectArrayOf1024PagePointers_1024ObjectPointers;}
        memory.region(payload,payloadBytes);memory.region(context,0x20);memory.region(graph,8*0x20);
        memory.region(resources,0x100);memory.region(table,0x2000);
        memory.pointer(context+0x10,graph);memory.pointer(context+0x18,graph+8*0x20);
        for(unsigned i=0;i<8;++i) {
            memory.word(graph+i*0x20,(i+1)*10);memory.word(graph+i*0x20+8,i%2?3:2);
            memory.pointer(graph+i*0x20+0x10,resources+i*0x10);memory.word(resources+i*0x10,ids[i]);
            memory.pointer(payload+0x28+i*0x20,0xCAFE00000000ull|(order[i]+1)*10);
            memory.pointer(payload+0x38+i*0x20,request.moduleBase+properties[i]);
            *memory.at(payload+0x30+i*0x20,1)=static_cast<unsigned char>(i<3?0:1);
            memory.word(payload+0x40+i*0x20,0x12340000+i);
            memory.region(page(ids[i]),0x2000);memory.pointer(table+Address(ids[i]>>10)*8,page(ids[i]));
            memory.pointer(page(ids[i])+Address(ids[i]&0x3FF)*8,objects+i*0x100);
            memory.region(objects+i*0x100,0x40);memory.word(objects+i*0x100+0x14,1128+i);
            memory.word(objects+i*0x100+0x18,634+i);
            memory.pointer(objects+i*0x100+0x38,0xF123000000000000ull+i);
        }
    }
    Result run() {
        memory.calls.clear();
        const auto configuredLimits=request.limits;
        auto result=readUpscalerPrepare(request,{Memory::read,&memory});
        require(!result.snapshotAtomic&&!result.lifetimePinned&&!result.pixelsRead&&!result.gameFunctionsCalled&&
            !result.gpuCompletionVerified&&!result.copyPermitted&&!result.undilatedVerified&&
            !result.motionSemanticsVerified&&!result.sameFrameVerified&&!result.xeFGIntegrated,"metadata gained semantic permissions");
        require(result.readCalls==memory.calls.size()&&result.bytesCopied<=result.bytesRequested,"read accounting");
        require(result.readCalls<=configuredLimits.maxReadCalls&&result.bytesRequested<=configuredLimits.maxBytes,"limit exceeded");
        std::size_t charged=0;
        for(const auto& read:memory.calls) {require(read.remaining==configuredLimits.maxBytes-charged,"remaining budget");charged+=read.bytes;}
        require(charged==result.bytesRequested,"all requests including failures charged");
        for(const auto& b:result.bindings)
            require(!b.native.idDomainVerified&&!b.native.vulkanImageVerified,"native metadata certified");
        if(!result.snapshotCaptured) {
            require(result.status!=Status::SnapshotParsed&&!result.allPropertiesPresent&&result.graphTextureIdsRead==0&&result.nativeCandidatesRead==0,"partial summary publication");
            for(const auto& b:result.bindings)require(b.descriptor==0&&b.graphHandle==0&&b.graphKey==0&&b.propertyAddress==0&&
                b.property==Property::Unknown&&b.rawDependencyDirection==0&&b.rawSubresource==0&&b.graphEntry==0&&
                b.entryType==0&&b.resourcePayload==0&&b.textureId==0&&b.native.tableBase==0&&b.native.page==0&&
                b.native.object==0&&b.native.imageCandidate==0,"partial binding publication");
        }
        return result;
    }
};
void validSnapshots() {
    ++cases;Fixture f;auto r=f.run();
    require(r.status==Status::SnapshotParsed&&r.snapshotCaptured&&r.allPropertiesPresent&&r.graphTextureIdsRead==8&&r.nativeCandidatesRead==8,"native success");
    require(r.descriptorsUnchanged&&r.graphEntriesUnchanged&&r.bindingMetadataUnchanged&&r.contextBoundsUnchanged,"repeat checks");
    require(r.readCalls==86&&r.bytesRequested==1632,"expected bounded native work");
    for(unsigned i=0;i<8;++i) {
        const auto& b=r.bindings[i];const auto j=Fixture::order[i];
        require(b.property==static_cast<Property>(i+1)&&b.descriptor==Fixture::payload+0x28+i*0x20,"slot/property layout");
        require(b.textureId==Fixture::ids[j]&&b.graphKey==(j+1)*10&&b.graphHandle>0xFFFFFFFF,"exact graph key mapping");
        require(b.native.widthCandidate==1128+j&&b.native.heightCandidate==634+j&&b.native.imageCandidate==0xF123000000000000ull+j,"native candidate mapping");
        require(!f.memory.touched(b.native.imageCandidate)&&!f.memory.touched(b.propertyAddress,4),"opaque image/property dereferenced");
        require(b.rawSubresource==0x12340000+i&&b.rawDependencyDirection==(i<3?0:1),"raw graph fields changed");
    }
    require(!f.memory.touched(Fixture::payload,0x28)&&!f.memory.touched(Fixture::payload+0x128,0x18),"header/constants touched");
    ++cases;Fixture noNative(false);r=noNative.run();
    require(r.snapshotCaptured&&r.graphTextureIdsRead==8&&r.nativeCandidatesRead==0&&r.readCalls==22&&r.bytesRequested==1120,"disabled native");
    require(!noNative.memory.touched(Fixture::table,0x2000),"disabled native was read");
    ++cases;Fixture opaque;*opaque.memory.at(Fixture::payload+0x30,1)=255;r=opaque.run();
    require(r.snapshotCaptured&&r.bindings[0].rawDependencyDirection==255,"raw dependency interpreted as enum/access permission");
    std::mt19937 random(0x7143);
    for(unsigned round=0;round<256;++round) {
        ++cases;Fixture p(false);std::array<unsigned,8> permutation{0,1,2,3,4,5,6,7};
        std::shuffle(permutation.begin(),permutation.end(),random);
        std::array<unsigned char,256> records{};std::memcpy(records.data(),p.memory.at(Fixture::payload+0x28,256),256);
        for(unsigned i=0;i<8;++i)std::memcpy(p.memory.at(Fixture::payload+0x28+i*0x20,0x20),records.data()+permutation[i]*0x20,0x20);
        r=p.run();require(r.snapshotCaptured,"property order assumed");
        for(unsigned i=0;i<8;++i)require(r.bindings[i].property==static_cast<Property>(permutation[i]+1)&&
            r.bindings[i].textureId==Fixture::ids[Fixture::order[permutation[i]]],"permutation metadata association");
    }
}
void classificationAndPreflight() {
    for(unsigned missing=0;missing<8;++missing) {
        ++cases;Fixture f;f.memory.pointer(Fixture::payload+0x38+missing*0x20,0);auto r=f.run();
        require(r.status==Status::UnknownProperty&&r.readCalls==1,"missing property not rejected before graph");
        ++cases;Fixture unknown;unknown.memory.pointer(Fixture::payload+0x38+missing*0x20,unknown.request.moduleBase+0x1D10784);
        require(unknown.run().status==Status::UnknownProperty,"FI property accepted in upscale layout");
        for(unsigned replacement=0;replacement<8;++replacement)if(replacement!=missing) {
            ++cases;Fixture duplicate;duplicate.memory.pointer(Fixture::payload+0x38+missing*0x20,duplicate.request.moduleBase+Fixture::properties[replacement]);
            auto d=duplicate.run();require(d.status==Status::DuplicateProperty&&d.readCalls==1,"duplicate/missing member accepted");
        }
    }
    const auto max=std::numeric_limits<Address>::max();
    std::vector<std::function<void(Request&)>> invalid{
        [](auto& q){q.layout=InputLayout::Unknown;},[](auto& q){q.moduleBase=0;},[](auto& q){q.prepareInputPayload=0;},
        [](auto& q){q.graphContext=0;},[](auto& q){q.limits.maxBytes=0;},[](auto& q){q.limits.maxBytes=1024*1024+1;},
        [](auto& q){q.limits.maxReadCalls=0;},[](auto& q){q.limits.maxReadCalls=257;},
        [](auto& q){q.limits.maxGraphEntries=0;},[](auto& q){q.limits.maxGraphEntries=8193;},
        [=](auto& q){q.moduleBase=max-0x1D10697;},[=](auto& q){q.prepareInputPayload=max-0x13F;},
        [=](auto& q){q.graphContext=max-0x1F;},[](auto& q){q.graphContext=q.prepareInputPayload+0x130;},
        [](auto& q){q.nativeLayout=NativeTableLayout::Unknown;}};
    for(auto& change:invalid) {++cases;Fixture f;change(f.request);auto r=f.run();require(!r.snapshotCaptured&&r.readCalls==0,"preflight read memory");}
    ++cases;Fixture f;auto r=readUpscalerPrepare(f.request,{});require(r.status==Status::MissingReader&&r.readCalls==0,"missing callback");
    require(propertyAtAddress(0,Fixture::properties[0])==Property::Unknown&&propertyAtAddress(0x700000000,3)==Property::Unknown,"property underflow");
    require(std::strcmp(statusName(static_cast<Status>(999)),"UnknownStatus")==0&&std::strcmp(propertyName(static_cast<Property>(999)),"unknown")==0,"unknown names");
}
void failureAtEveryRead() {
    Fixture reference;require(reference.run().snapshotCaptured,"baseline");const auto trace=reference.memory.calls;
    for(const auto fault:{Fault::Zero,Fault::Partial,Fault::Throw,Fault::OverReport})
        for(std::size_t at=1;at<=trace.size();++at) {
            ++cases;Fixture f;f.memory.fault=fault;f.memory.faultCall=at;auto r=f.run();
            require(!r.snapshotCaptured&&r.readCalls==at,"failed read continued or published");
            const auto expected=fault==Fault::Zero?ReadStatus::Unreadable:fault==Fault::Partial?ReadStatus::PartialRead:
                fault==Fault::Throw?ReadStatus::CallbackException:ReadStatus::CallbackContractViolation;
            require(r.detailStatus==expected,"read error detail lost");
        }
    for(const auto recordIndex:{std::size_t(43),std::size_t(84),std::size_t(85)})
        for(std::size_t byte=0;byte<trace[recordIndex].bytes;++byte) {
            ++cases;Fixture f;f.memory.fault=Fault::Change;f.memory.faultCall=recordIndex+1;f.memory.changeOffset=byte;
            auto r=f.run();require(!r.snapshotCaptured,"changed graph/bounds/descriptor published");
            require(r.status==(recordIndex==43?Status::GraphEntriesChanged:recordIndex==85?Status::DescriptorsChanged:Status::ReadOrGraphRejected),"changed structure status");
        }
    // Every byte read in the second binding/native sweep must affect acceptance
    // (either a mismatch or a refusal of the newly returned pointer/ID).
    for(std::size_t index=44;index<84;++index)
        for(std::size_t byte=0;byte<trace[index].bytes;++byte) {
            ++cases;Fixture f;f.memory.fault=Fault::Change;f.memory.faultCall=index+1;f.memory.changeOffset=byte;
            require(!f.run().snapshotCaptured,"changed native/texture metadata published");
        }
}
void budgetsAndGraphFailures() {
    ++cases;Fixture baseline;const auto normal=baseline.run();
    for(std::size_t limit=1;limit<=normal.readCalls;++limit) {
        ++cases;Fixture f;f.request.limits.maxReadCalls=limit;auto r=f.run();
        require(r.snapshotCaptured==(limit==normal.readCalls),"call budget boundary");
    }
    for(std::size_t limit=1;limit<=normal.bytesRequested;++limit) {
        ++cases;Fixture f;f.request.limits.maxBytes=limit;auto r=f.run();
        require(r.snapshotCaptured==(limit==normal.bytesRequested),"byte budget boundary");
    }
    std::vector<std::function<void(Fixture&)>> failures{
        [](auto& f){f.memory.pointer(Fixture::context+0x18,Fixture::graph-1);},
        [](auto& f){f.memory.pointer(Fixture::context+0x18,Fixture::graph+1);},
        [](auto& f){f.memory.pointer(Fixture::context+0x10,0);},
        [](auto& f){f.memory.pointer(Fixture::context+0x10,0);f.memory.pointer(Fixture::context+0x18,0);},
        [](auto& f){f.memory.pointer(Fixture::context+0x18,Fixture::graph+8193*0x20);},
        [](auto& f){f.memory.pointer(Fixture::context+0x10,Fixture::payload+0x128);f.memory.pointer(Fixture::context+0x18,Fixture::payload+0x148);},
        [](auto& f){f.memory.word(Fixture::graph+0x20,10);},[](auto& f){f.memory.word(Fixture::graph+0x20,1);},
        [](auto& f){f.memory.pointer(Fixture::payload+0x28,999);},[](auto& f){f.memory.pointer(Fixture::payload+0x28,25);},
        [](auto& f){f.memory.word(Fixture::graph+3*0x20+8,1);},
        [](auto& f){f.memory.pointer(Fixture::graph+3*0x20+0x10,0);},
        [](auto& f){f.memory.pointer(Fixture::graph+3*0x20+0x10,Fixture::payload+0x130);},
        [](auto& f){f.memory.word(Fixture::resources+3*0x10,0);},
        [](auto& f){f.memory.word(Fixture::resources+3*0x10,0x100000);},
        [](auto& f){f.request.nativeTableBase=std::numeric_limits<Address>::max()-0x1FFF;},
        [](auto& f){f.request.nativeTableBase=Fixture::payload;},
        [](auto& f){f.memory.pointer(Fixture::table+Address(Fixture::ids[3]>>10)*8,0);},
        [](auto& f){f.memory.pointer(Fixture::table+Address(Fixture::ids[3]>>10)*8,Fixture::table);},
        [](auto& f){f.memory.pointer(Fixture::page(Fixture::ids[3])+Address(Fixture::ids[3]&0x3FF)*8,0);},
        [](auto& f){f.memory.pointer(Fixture::page(Fixture::ids[3])+Address(Fixture::ids[3]&0x3FF)*8,Fixture::graph);}};
    for(auto& change:failures) {++cases;Fixture f;change(f);require(!f.run().snapshotCaptured,"invalid graph/native accepted");}
}
void mutationReentryAndThreads() {
    ++cases;Fixture f;f.memory.firstRead=[&]{f.request={};};
    require(f.run().snapshotCaptured,"callback changed frozen request");
    ++cases;Fixture outer;bool nested=false;
    outer.memory.firstRead=[&]{Fixture inner(false);nested=inner.run().snapshotCaptured;};
    require(outer.run().snapshotCaptured&&nested,"reentrant invocation failed");
    std::array<std::thread,8> workers;std::atomic<bool> failed=false;
    for(auto& worker:workers)worker=std::thread([&]{try {
        for(unsigned i=0;i<500;++i) {++cases;Fixture local(i%2==0);require(local.run().snapshotCaptured,"concurrent pure parse");}
    }catch(...){failed=true;}});
    for(auto& worker:workers)worker.join();
    require(!failed,"concurrent parser shared mutable state");
}

void directIndexSnapshots() {
    const auto prepare=[](Fixture& f) {
        f.request.layout=InputLayout::UnityPlayer9aa9397aUpscalerDirectIndexEight;
        f.memory.regions.erase(Fixture::graph);
        f.memory.pointer(Fixture::context+0x18,0xBADBADBADBADull); // Not an end pointer.
        constexpr std::array<Address,8> properties{0x1EDE538,0x1EDE53C,0x1EDE540,0x1EDE594,0x1EDE598,0x1EDE59C,0x1EDE5A0,0x1EDE5A4};
        for(unsigned i=0;i<8;++i) {
            const auto entry=Fixture::graph+Address((i+1)*10)*16;
            f.memory.region(entry,16); f.memory.pointer(entry,Fixture::resources+i*0x10);
            f.memory.word(entry+12,i%2?3:2);
            f.memory.pointer(Fixture::payload+0x38+i*0x20,f.request.moduleBase+properties[i]);
        }
    };
    ++cases;Fixture f;prepare(f);auto r=f.run();
    require(r.snapshotCaptured&&r.graphIndexedEntries&&r.contextTableUnchanged&&r.graphEntriesObserved==8&&
            !r.contextBoundsUnchanged&&!r.graphEnd&&!r.graphEntryCount&&!r.allocationBoundsVerified,
            "direct index reports observations without inventing allocation bounds/count");
    require(r.readCalls==100&&r.bytesRequested==1360&&!f.memory.touched(Fixture::context+0x18,8),
            "direct index reads eight 16-byte entries and never treats context+18 as end");
    for(unsigned i=0;i<8;++i) require(r.bindings[i].textureId==Fixture::ids[Fixture::order[i]]&&
            r.bindings[i].graphEntry==Fixture::graph+Address((Fixture::order[i]+1)*10)*16,
            "direct low32 index preserves exact native candidate provenance");
    const auto trace=f.memory.calls;
    for(const auto fault:{Fault::Zero,Fault::Partial,Fault::Throw,Fault::OverReport})
        for(std::size_t call=1;call<=trace.size();++call) {
            ++cases;Fixture failed;prepare(failed);failed.memory.fault=fault;failed.memory.faultCall=call;
            require(!failed.run().snapshotCaptured,"failed/partial direct read published candidates");
        }
    // Every repeated descriptor/base/entry/native-chain byte is compared.
    for(std::size_t call=51;call<=trace.size();++call) {
        ++cases;Fixture changed;prepare(changed);changed.memory.fault=Fault::Change;changed.memory.faultCall=call;
        require(!changed.run().snapshotCaptured,"changed direct-index/native snapshot published candidates");
    }
    for(unsigned kind=0;kind<8;++kind) {
        ++cases;Fixture bad;prepare(bad);
        const auto entry=Fixture::graph+Address(40)*16;
        if(kind==0) bad.memory.pointer(Fixture::payload+0x28,8192);
        if(kind==1) bad.memory.pointer(Fixture::context+0x10,std::numeric_limits<Address>::max()-32);
        if(kind==2) bad.memory.word(entry+12,1);
        if(kind==3) bad.memory.pointer(entry,Fixture::payload+0x128);
        if(kind==4) bad.memory.pointer(entry,entry+4);
        if(kind==5) bad.memory.pointer(Fixture::context+0x10,Fixture::payload);
        if(kind==6) bad.request.limits.maxReadCalls=99;
        if(kind==7) bad.request.limits.maxBytes=1359;
        require(!bad.run().snapshotCaptured,"unsupported index/type/alias/overflow/budget accepted");
    }
    ++cases;Fixture noNative(false);prepare(noNative);r=noNative.run();
    require(r.snapshotCaptured&&r.readCalls==36&&r.bytesRequested==848&&r.nativeCandidatesRead==0,
            "direct graph-only bounded sweep does not touch native table");
    ++cases;Fixture high;prepare(high);
    const auto highEntry=Fixture::graph+8191*Address(16);
    high.memory.region(highEntry,16);
    high.memory.pointer(highEntry,Fixture::resources+Fixture::order[0]*0x10);
    high.memory.word(highEntry+12,2);high.memory.pointer(Fixture::payload+0x28,8191);
    r=high.run();
    require(r.snapshotCaptured&&r.bindings[0].graphKey==8191&&r.readCalls==100&&r.graphEntriesObserved==8,
            "highest supported index reads only eight entries, never an 8192-entry array");
    ++cases;Fixture overflow;prepare(overflow);
    overflow.request.graphContext=std::numeric_limits<Address>::max()-0x17;
    require(overflow.run().status==Status::AddressOverflow&&overflow.memory.calls.empty(),
            "direct context span near MAX rejects before overlap arithmetic or reading");
}

} // namespace
int main() {
    try {
        validSnapshots();classificationAndPreflight();failureAtEveryRead();budgetsAndGraphFailures();mutationReentryAndThreads();directIndexSnapshots();
        std::cout<<"{\"passed\":true,\"cases\":"<<cases.load()<<",\"checks\":"<<checks.load()
            <<",\"threads\":8,\"concurrentInvocations\":4000,\"legacyNativeReadCalls\":86,\"legacyNativeRequestedBytes\":1632,"
              "\"graphOnlyReadCalls\":22,\"graphOnlyRequestedBytes\":1120,\"liveProcessRead\":false,\"pixelsRead\":false,"
              "\"directNativeReadCalls\":100,\"directNativeRequestedBytes\":1360,\"supportedIndexCeiling\":8192,"
              "\"hookInstalled\":false,\"undilatedVerified\":false,\"xeFGIntegrated\":false}\n";
        return 0;
    }catch(const std::exception& e) {std::cerr<<"test failure: "<<e.what()<<"; cases="<<cases.load()<<" checks="<<checks.load()<<'\n';return 1;}
}
