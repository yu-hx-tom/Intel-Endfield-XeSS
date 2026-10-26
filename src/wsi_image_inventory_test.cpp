#include "wsi_image_inventory.hpp"
#include <array>
#include <atomic>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {
namespace wsi=xess_vk::wsi_image_inventory;
namespace images=xess_vk::image_inventory;
using Status=wsi::Status;
std::uint64_t checks{},cases{};
void require(bool okay,const char* reason){++checks;if(!okay)throw std::runtime_error(reason);}
wsi::CreateDescription description(){return {true,false,0,2,37,1920,1080,1,0x13,0};}
const images::Description ordinary{37,17,19,1,1,1,1,0x13,0,0,0,0};
struct Fixture {
    std::unique_ptr<wsi::CommonInventory> common=std::make_unique<wsi::CommonInventory>();
    wsi::Inventory inventory{*common};
    wsi::ChainToken create(std::uint64_t handle,wsi::CreateDescription d=description()) {
        const auto begin=inventory.beforeCreate(0,d);require(begin.status==Status::CreatePending && begin.createTicket.serial,"begin create");
        const auto end=inventory.afterCreate(begin.createTicket,0,handle);require(end.status==Status::ChainObserved,"end create");
        return end.chain;
    }
    wsi::Changes enumerate(std::uint64_t chain,std::span<const std::uint64_t> values,std::int32_t result=0,
                          std::uint32_t capacity=32) {
        return inventory.afterGetImages(inventory.beforeGetImages(chain,true,capacity),{result,true,static_cast<std::uint32_t>(values.size()),values});
    }
};
void conservative(const wsi::Changes& changes) {
    require(!changes.acquiredVerified&&!changes.presentedVerified&&!changes.lifetimePinned&&!changes.gpuCompletionVerified&&!changes.copyPermitted,"metadata became GPU permission");
    require(changes.addedCount<=wsi::maximumImages&&changes.removedCount<=wsi::maximumRemoved,"output capacity exceeded");
    for(std::size_t i=0;i<changes.addedCount;++i)require(changes.added[i].record.image&&changes.added[i].record.generation&&changes.added[i].owner.chain&&changes.added[i].owner.generation,"partial added identity");
}
void basicAndSharedGeneration() {
    ++cases;Fixture f;f.common->created(10,ordinary);const auto normal=f.common->lookup(10).record;
    const auto chain=f.create(100);constexpr std::array<std::uint64_t,3> handles{20,21,22};
    auto change=f.enumerate(100,handles);conservative(change);
    require(change.status==Status::ImagesRegistered&&change.addedCount==3&&change.removedCount==0,"initial registration");
    for(unsigned i=0;i<3;++i) {
        const auto& added=change.added[i];
        require(added.owner.chain==100&&added.owner.generation==chain.generation&&added.index==i,"chain/index association");
        require(added.record.image==handles[i]&&added.record.generation==normal.generation+i+1,"not in shared image-generation sequence");
        const auto& d=added.record.description;
        require(d.format==37&&d.width==1920&&d.height==1080&&d.depth==1&&d.mipLevels==1&&d.arrayLayers==1&&d.samples==1&&
                d.flags==0&&d.tiling==0&&d.sharingMode==0&&d.initialLayout==0&&d.usage==0x13,"wrong implied image description");
    }
    const auto firstGeneration=change.added[0].record.generation;
    change=f.enumerate(100,handles);require(change.status==Status::ImagesRepeated&&!change.addedCount&&!change.removedCount,"repeat is new create");
    require(f.common->lookup(20).record.generation==firstGeneration,"repeat changed generation");
    f.common->created(11,ordinary);require(f.common->lookup(11).record.generation==normal.generation+4,"repeat advanced shared generation");
    change=f.inventory.beforeDestroy(100);conservative(change);require(change.removedCount==3,"destroy failed to remove images");
    require(f.common->lookup(10).status==images::Status::LiveHandleObserved&&f.common->lookup(11).status==images::Status::LiveHandleObserved,"removed ordinary images");
    require(f.common->lookup(20).status==images::Status::NotObserved,"destroy kept old image");
    const auto replacement=f.create(100);require(replacement.generation>chain.generation,"chain reuse kept generation");
    change=f.enumerate(100,handles);require(change.added[0].record.generation>normal.generation+4,"image reuse kept generation");
}
void retirementAndTokens() {
    ++cases;Fixture f;f.create(100);constexpr std::array<std::uint64_t,3> handles{20,21,22};f.enumerate(100,handles);
    const auto get=f.inventory.beforeGetImages(100,true,3);
    auto replacement=f.inventory.beforeCreate(100,description());conservative(replacement);
    require(replacement.oldSwapchainRetired&&replacement.removedCount==3&&replacement.status==Status::CreatePending,"old not retired before driver");
    require(f.common->lookup(20).status==images::Status::NotObserved,"old image visible during create call");
    const auto failure=f.inventory.afterCreate(replacement.createTicket,-3,0);require(failure.status==Status::DriverFailure,"driver failure ignored");
    require(f.inventory.afterGetImages(get,{0,true,3,handles}).status==Status::RetiredChain,"failure revived retired old chain");
    require(f.inventory.afterCreate(replacement.createTicket,0,200).status==Status::InvalidToken,"create ticket reusable");

    ++cases;Fixture stale;stale.create(100);stale.enumerate(100,handles);
    const auto oldGet=stale.inventory.beforeGetImages(100,true,3);stale.inventory.beforeDestroy(100);stale.create(100);
    constexpr std::array<std::uint64_t,3> newer{30,31,32};stale.enumerate(100,newer);
    const auto late=stale.inventory.afterGetImages(oldGet,{0,true,3,handles});
    require(late.status==Status::StaleToken&&!late.addedCount&&!late.removedCount,"late GetImages associated reused chain");
    require(stale.common->lookup(30).status==images::Status::LiveHandleObserved&&stale.common->lookup(20).status==images::Status::NotObserved,"late GetImages touched new generation");

    ++cases;Fixture unknown;auto begin=unknown.inventory.beforeCreate(900,description());
    require(begin.oldSwapchainRetired&&unknown.inventory.beforeGetImages(900,true,3).status==Status::RetiredChain,"unobserved old handle revived");
    require(unknown.inventory.afterCreate(begin.createTicket,0,901).status==Status::ChainObserved,"valid replacement after unobserved old");
    unknown.inventory.beforeDestroy(900);require(unknown.create(900).generation>begin.createTicket.serial,"destroy did not permit new chain generation");

    ++cases;Fixture concurrent;const auto a=concurrent.inventory.beforeCreate(0,description());
    const auto b=concurrent.inventory.beforeCreate(0,description());
    require(concurrent.inventory.afterCreate(b.createTicket,0,102).chain.generation==b.createTicket.serial,"out-of-order completion changed ticket");
    require(concurrent.inventory.afterCreate(a.createTicket,0,101).chain.generation==a.createTicket.serial,"first pending operation lost");
}
void unsupportedDescriptions() {
    for(unsigned kind=0;kind<13;++kind) {
        ++cases;Fixture f;f.create(100);constexpr std::array<std::uint64_t,3> handles{1,2,3};f.enumerate(100,handles);
        auto d=description();
        switch(kind){
        case 0:d.copied=false;break;case 1:d.pNextPresent=true;break;case 2:d.flags=2;break;case 3:d.flags=0x80000000;break;
        case 4:d.arrayLayers=2;break;case 5:d.sharingMode=1;break;case 6:d.minImageCount=0;break;case 7:d.minImageCount=33;break;
        case 8:d.width=0;break;case 9:d.height=0;break;case 10:d.format=0;break;case 11:d.usage=0;break;case 12:d.usage=0x100;break;
        }
        auto changes=f.inventory.beforeCreate(100,d);conservative(changes);
        require(changes.status==Status::UnsupportedParameters&&!changes.createTicket.serial&&changes.removedCount==3&&changes.oldSwapchainRetired,"unsupported metadata did not retire old first");
        require(f.common->lookup(1).status==images::Status::NotObserved,"unsupported recreation left old identity");
    }
}
void partialAndCountQueries() {
    ++cases;Fixture f;f.create(100);constexpr std::array<std::uint64_t,3> full{1,2,3};
    auto change=f.enumerate(100,std::span(full).first(1),5,1);
    require(change.status==Status::IncompleteIgnored&&change.addedCount==0&&f.common->lookup(1).status==images::Status::NotObserved,"incomplete published partial identity");
    change=f.inventory.afterGetImages(f.inventory.beforeGetImages(100,false,999),{0,true,3,{}});
    require(change.status==Status::CountObserved&&!change.addedCount,"count-only created images");
    change=f.enumerate(100,full);require(change.status==Status::ImagesRegistered&&change.addedCount==3,"full result after incomplete failed");
    const auto generation=f.common->lookup(1).record.generation;
    change=f.enumerate(100,std::span(full).first(2),5,2);
    require(change.status==Status::IncompleteIgnored&&!change.addedCount&&!change.removedCount&&f.common->lookup(1).record.generation==generation,"known prefix incomplete mutated identity");
    constexpr std::array<std::uint64_t,1> wrong{9};change=f.enumerate(100,wrong,5,1);
    require(change.status==Status::EnumerationChanged&&change.removedCount==3,"changed prefix not retired");

    ++cases;Fixture changed;changed.create(100);changed.inventory.afterGetImages(changed.inventory.beforeGetImages(100,false,0),{0,true,3,{}});
    constexpr std::array<std::uint64_t,2> two{1,2};change=changed.enumerate(100,two);
    require(change.status==Status::EnumerationChanged&&!change.addedCount,"count query contradiction accepted");
    ++cases;Fixture failed;failed.create(100);failed.enumerate(100,full);
    const auto error=failed.inventory.afterGetImages(failed.inventory.beforeGetImages(100,true,3),{-1,false,0,{}});
    require(error.status==Status::DriverFailure&&!error.removedCount&&failed.common->lookup(1).status==images::Status::LiveHandleObserved,"driver error fabricated retirement");
}
void malformedEnumeration() {
    for(unsigned kind=0;kind<11;++kind) {
        ++cases;Fixture f;f.create(100);std::array<std::uint64_t,3> handles{1,2,3};
        auto token=f.inventory.beforeGetImages(100,true,3);wsi::Enumeration e{0,true,3,handles};
        switch(kind){
        case 0:e.countCopied=false;break;case 1:token.inputCapacity=2;break;case 2:e.returnedCount=33;break;
        case 3:e.images=std::span(handles).first(2);break;case 4:handles[2]=0;break;case 5:handles[2]=handles[0];break;
        case 6:e.returnedCount=0;e.images={};break;case 7:e.returnedCount=1;e.images=std::span(handles).first(1);break;
        case 8:token.hasImageArray=false;break;case 9:token.hasImageArray=false;e.driverResult=5;e.images={};break;
        case 10:e.returnedCount=UINT32_MAX;e.images={};break;
        }
        const auto change=f.inventory.afterGetImages(token,e);conservative(change);
        require((change.status==Status::InvalidEnumeration||change.status==Status::DuplicateImage)&&!change.addedCount,"malformed result accepted");
        require(f.common->lookup(0).active==0,"malformed result partially inserted");
    }
    ++cases;Fixture f;constexpr std::array<std::uint64_t,3> handles{1,2,3};
    require(f.enumerate(999,handles).status==Status::UnknownChain,"unobserved chain gained identity");
}
void conflictsAndOwnership() {
    ++cases;Fixture f;f.common->created(3,ordinary);const auto existing=f.common->lookup(3).record;f.create(100);
    constexpr std::array<std::uint64_t,3> handles{1,2,3};const auto conflict=f.enumerate(100,handles);
    require(conflict.status==Status::ConflictingImage&&!conflict.addedCount&&f.common->lookup(0).status==images::Status::NotObserved,"conflict poisoned common inventory");
    require(f.common->lookup(1).status==images::Status::NotObserved&&f.common->lookup(2).status==images::Status::NotObserved&&f.common->lookup(3).record.generation==existing.generation,"whole-list preflight mutated ordinary images");

    ++cases;Fixture other;other.create(100);other.enumerate(100,handles);other.create(200);
    constexpr std::array<std::uint64_t,3> overlapping{4,5,3};const auto otherConflict=other.enumerate(200,overlapping);
    require(otherConflict.status==Status::ConflictingImage&&!otherConflict.addedCount&&other.common->lookup(4).status==images::Status::NotObserved,"other chain conflict inserted a prefix");
    require(other.common->lookup(3).status==images::Status::LiveHandleObserved,"other chain identity erased");

    ++cases;Fixture reused;reused.create(100);reused.enumerate(100,handles);
    reused.common->destroying(2);reused.common->created(2,ordinary);const auto newer=reused.common->lookup(2).record;
    const auto retired=reused.inventory.beforeDestroy(100);conservative(retired);
    require(retired.removedCount==2&&retired.ownershipMismatches==1,"removal generation mismatch unreported");
    require(reused.common->lookup(2).record.generation==newer.generation&&reused.common->lookup(2).record.description.width==17,"destroy erased reused ordinary handle");
    for(std::size_t i=0;i<retired.removedCount;++i)require(retired.removed[i].record.image!=2,"reported new generation as removed");

    ++cases;Fixture lost;lost.create(100);lost.enumerate(100,handles);lost.common->destroying(1);lost.common->created(1,ordinary);
    const auto repeated=lost.enumerate(100,handles);
    require(repeated.status==Status::OwnershipLost&&repeated.removedCount==2&&repeated.ownershipMismatches==1,"repeat did not reject changed generation");
    require(lost.common->lookup(1).record.description.width==17,"ownership failure erased new image");
}
void capacitiesAndStop() {
    ++cases;Fixture f;
    for(std::uint64_t chain=1;chain<=16;++chain) {
        f.create(chain);std::array<std::uint64_t,32> handles{};
        for(std::size_t i=0;i<handles.size();++i)handles[i]=1000+chain*32+i;
        const auto added=f.enumerate(chain,handles);require(added.addedCount==32,"maximum chain batch rejected");
    }
    require(f.common->lookup(0).active==512,"maximum active WSI count");
    const auto stopped=f.inventory.stop();conservative(stopped);
    require(stopped.removedCount==512&&stopped.status==Status::Stopped&&f.common->lookup(0).active==0,"bounded stop did not remove all owned images");
    require(f.inventory.beforeCreate(0,description()).status==Status::Stopped,"stop revived");

    ++cases;Fixture full;
    for(std::uint64_t i=1;i<=16;++i)full.create(i);
    const auto ticket=full.inventory.beforeCreate(0,description()).createTicket;
    require(full.inventory.afterCreate(ticket,0,17).status==Status::CapacityExceeded,"chain capacity overflow accepted");
    require(full.inventory.beforeGetImages(1,true,3).status==Status::CapacityExceeded,"capacity failure recovered implicitly");
    ++cases;Fixture pending;
    for(unsigned i=0;i<16;++i)require(pending.inventory.beforeCreate(0,description()).status==Status::CreatePending,"pending slot rejected early");
    require(pending.inventory.beforeCreate(0,description()).status==Status::CapacityExceeded,"pending capacity exceeded");

    ++cases;Fixture commonFull;
    for(std::uint64_t i=1;i<wsi::CommonInventory::maximumActive;++i)commonFull.common->created(i,ordinary);
    commonFull.create(100);constexpr std::array<std::uint64_t,3> values{100000,100001,100002};
    const auto refused=commonFull.enumerate(100,values);
    require(refused.status==Status::CapacityExceeded&&!refused.addedCount&&commonFull.common->lookup(0).status==images::Status::NotObserved,"common capacity preflight poisoned inventory");
    require(commonFull.common->lookup(100000).status==images::Status::NotObserved,"common capacity inserted prefix");

    ++cases;Fixture exhausted;require(exhausted.inventory.setNextSerialForTest(UINT64_MAX),"serial fixture setup");
    require(exhausted.inventory.beforeCreate(0,description()).status==Status::GenerationExhausted,"serial wrapped");
    ++cases;Fixture fault;fault.create(100);fault.common->stop();
    require(fault.enumerate(100,values).status==Status::InventoryFault,"common fault accepted");
    ++cases;Fixture duplicate;duplicate.create(100);const auto duplicateTicket=duplicate.inventory.beforeCreate(0,description()).createTicket;
    require(duplicate.inventory.afterCreate(duplicateTicket,0,100).status==Status::DuplicateChain,"duplicate live chain accepted");
}
void externallySerializedThreads() {
    ++cases;Fixture f;std::mutex outer;std::atomic<unsigned> errors{};std::uint64_t lastImageGeneration{};
    std::array<std::thread,8> workers;
    for(unsigned t=0;t<workers.size();++t)workers[t]=std::thread([&,t]{
        for(unsigned i=0;i<200;++i) {
            const std::uint64_t chain=1+std::uint64_t(t)*100000+i;
            const std::array<std::uint64_t,3> handles{chain*4,chain*4+1,chain*4+2};
            wsi::CreateTicket create;wsi::EnumerationTicket enumeration;
            {std::lock_guard lock(outer);const auto begin=f.inventory.beforeCreate(0,description());create=begin.createTicket;if(begin.status!=Status::CreatePending)++errors;}
            std::this_thread::yield();
            {std::lock_guard lock(outer);if(f.inventory.afterCreate(create,0,chain).status!=Status::ChainObserved)++errors;enumeration=f.inventory.beforeGetImages(chain,true,3);}
            std::this_thread::yield();
            {std::lock_guard lock(outer);const auto added=f.inventory.afterGetImages(enumeration,{0,true,3,handles});
                if(added.status!=Status::ImagesRegistered||added.addedCount!=3)++errors;
                for(std::size_t j=0;j<added.addedCount;++j){if(added.added[j].record.generation<=lastImageGeneration)++errors;lastImageGeneration=added.added[j].record.generation;}
            }
            {std::lock_guard lock(outer);if(f.inventory.beforeDestroy(chain).removedCount!=3)++errors;}
        }
    });
    for(auto& worker:workers)worker.join();
    require(errors==0&&f.common->lookup(0).active==0&&lastImageGeneration==4800,"external-lock concurrent lifecycle/generation failure");
}
} // namespace
int main() {
    try {
        basicAndSharedGeneration();retirementAndTokens();unsupportedDescriptions();partialAndCountQueries();malformedEnumeration();conflictsAndOwnership();capacitiesAndStop();externallySerializedThreads();
        std::cout<<"{\"passed\":true,\"cases\":"<<cases<<",\"checks\":"<<checks<<",\"threadedCycles\":1600,\"threads\":8"
                 <<",\"sharedGeneration\":true,\"gameTouched\":false,\"gpuTouched\":false}\n";return 0;
    }catch(const std::exception& error){std::cerr<<"FAIL: "<<error.what()<<" cases="<<cases<<" checks="<<checks<<'\n';return 1;}
}
