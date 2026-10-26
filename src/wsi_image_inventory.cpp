#include "wsi_image_inventory.hpp"
#include <limits>

namespace xess_vk::wsi_image_inventory {
namespace {
bool supported(const CreateDescription& d) noexcept {
    return d.copied && !d.pNextPresent && !d.flags && d.minImageCount && d.minImageCount<=maximumImages &&
        d.format && d.width && d.height && d.arrayLayers==1 && d.sharingMode==0 && d.usage && !(d.usage&~0x9fu);
}
image_inventory::Description implied(const CreateDescription& d) noexcept {
    // Khronos WSI's implied VkImageCreateInfo, within our flags=0/pNext=null
    // scope. initialLayout is a creation parameter, NOT an observed live layout.
    return {d.format,d.width,d.height,1,1,d.arrayLayers,1,d.usage,0,0,d.sharingMode,0};
}
bool healthy(image_inventory::Status status) noexcept {
    return status==image_inventory::Status::NotObserved || status==image_inventory::Status::LiveHandleObserved;
}
} // namespace
const char* name(Status status) noexcept {
    switch(status) {
#define WSI_STATUS(value,text) case Status::value: return text
    WSI_STATUS(NotAttempted,"not_attempted"); WSI_STATUS(CreatePending,"create_pending");
    WSI_STATUS(ChainObserved,"chain_observed"); WSI_STATUS(CountObserved,"count_observed");
    WSI_STATUS(IncompleteIgnored,"incomplete_ignored"); WSI_STATUS(ImagesRegistered,"images_registered");
    WSI_STATUS(ImagesRepeated,"images_repeated"); WSI_STATUS(OldChainRetired,"old_chain_retired");
    WSI_STATUS(ChainDestroyed,"chain_destroyed"); WSI_STATUS(UnknownChain,"unknown_chain");
    WSI_STATUS(RetiredChain,"retired_chain"); WSI_STATUS(StaleToken,"stale_token");
    WSI_STATUS(InvalidToken,"invalid_token"); WSI_STATUS(UnsupportedParameters,"unsupported_parameters");
    WSI_STATUS(InvalidEnumeration,"invalid_enumeration"); WSI_STATUS(EnumerationChanged,"enumeration_changed");
    WSI_STATUS(DuplicateImage,"duplicate_image"); WSI_STATUS(ConflictingImage,"conflicting_image");
    WSI_STATUS(OwnershipLost,"ownership_lost"); WSI_STATUS(DuplicateChain,"duplicate_chain");
    WSI_STATUS(CapacityExceeded,"capacity_exceeded"); WSI_STATUS(GenerationExhausted,"generation_exhausted");
    WSI_STATUS(InventoryFault,"inventory_fault"); WSI_STATUS(DriverFailure,"driver_failure");
    WSI_STATUS(Stopped,"stopped"); WSI_STATUS(Exception,"exception");
#undef WSI_STATUS
    }
    return "unknown";
}
Inventory::Chain* Inventory::find(std::uint64_t handle) noexcept {
    if(handle)for(auto& chain:chains_)if(chain.life!=Life::Empty && chain.token.chain==handle)return &chain;
    return nullptr;
}
const Inventory::Chain* Inventory::find(std::uint64_t handle) const noexcept {
    if(handle)for(const auto& chain:chains_)if(chain.life!=Life::Empty && chain.token.chain==handle)return &chain;
    return nullptr;
}
void Inventory::remove(Chain& chain,Changes& changes) {
    for(std::size_t i=0;i<chain.images.size();++i) {
        const auto record=chain.images[i];
        if(!record.image)continue;
        const auto live=common_.lookup(record.image);
        if(live.status==image_inventory::Status::LiveHandleObserved && live.record.generation==record.generation) {
            // Outer device lock serializes lookup -> remove with all other
            // inventory writers, so a later reused handle cannot be erased.
            common_.destroying(record.image);
            if(changes.removedCount<changes.removed.size())
                changes.removed[changes.removedCount++]={record,chain.token,static_cast<std::uint32_t>(i)};
        } else {
            ++changes.ownershipMismatches;
            if(!healthy(live.status))changes.inventoryStatus=live.status;
        }
        chain.images[i]={};
    }
    chain.complete=false;
}
void Inventory::retire(Chain& chain,Changes& changes) {
    chain.life=Life::Retired;
    remove(chain,changes);
}
void Inventory::fault(Status status,Changes& changes) {
    fault_=status;
    for(auto& chain:chains_)if(chain.life!=Life::Empty)retire(chain,changes);
    pending_={};changes.added={};changes.addedCount=0;changes.status=status;
}
void Inventory::exception(Changes& changes) noexcept {
    fault_=Status::Exception;
    // A mutex/system failure during a partially committed batch cannot safely
    // leave anonymous records eligible for downstream matching. Retire the
    // common namespace as well; this is solely the exceptional failure path.
    try {common_.stop();}catch(...){}
    pending_={};
    for(auto& chain:chains_) {chain.life=Life::Retired;chain.images={};chain.complete=false;}
    changes.added={};changes.addedCount=0;changes.status=Status::Exception;
    changes.inventoryStatus=image_inventory::Status::Stopped;
}
bool Inventory::nextSerial(std::uint64_t& serial,Changes& changes) {
    if(serial_==std::numeric_limits<std::uint64_t>::max()) {fault(Status::GenerationExhausted,changes);return false;}
    serial=++serial_;return true;
}
Changes Inventory::beforeCreate(std::uint64_t oldSwapchain,const CreateDescription& supplied) noexcept {
    Changes changes;const auto description=supplied;changes.description=description;
    try {
        if(fault_!=Status::NotAttempted) {changes.status=fault_;return changes;}
        if(oldSwapchain) {
            changes.oldSwapchainRetired=true;
            if(auto* old=find(oldSwapchain))retire(*old,changes);
            else {
                // Remember an unobserved retired handle too: a later completion
                // cannot silently claim that same handle without a destroy.
                Chain* tombstone{};
                for(auto& chain:chains_)if(chain.life==Life::Empty){tombstone=&chain;break;}
                if(!tombstone){fault(Status::CapacityExceeded,changes);return changes;}
                std::uint64_t generation{};
                if(!nextSerial(generation,changes))return changes;
                tombstone->life=Life::Retired;tombstone->token={oldSwapchain,generation};
            }
        }
        if(!supported(description)){changes.status=Status::UnsupportedParameters;return changes;}
        const auto common=common_.lookup(0);changes.inventoryStatus=common.status;
        if(!healthy(common.status)){fault(Status::InventoryFault,changes);return changes;}
        Pending* available{};
        for(auto& pending:pending_)if(!pending.serial){available=&pending;break;}
        if(!available){fault(Status::CapacityExceeded,changes);return changes;}
        std::uint64_t serial{};
        if(!nextSerial(serial,changes))return changes;
        *available={serial,description};changes.createTicket={serial};changes.status=Status::CreatePending;
    }catch(...){exception(changes);}
    return changes;
}
Changes Inventory::afterCreate(CreateTicket ticket,std::int32_t driverResult,std::uint64_t newSwapchain) noexcept {
    Changes changes;
    try {
        if(fault_!=Status::NotAttempted){changes.status=fault_;return changes;}
        Pending* pending{};
        if(ticket.serial)for(auto& item:pending_)if(item.serial==ticket.serial){pending=&item;break;}
        if(!pending){changes.status=Status::InvalidToken;return changes;}
        const auto description=pending->description;*pending={};changes.description=description;
        if(driverResult!=0){changes.status=Status::DriverFailure;return changes;}
        if(!newSwapchain){changes.status=Status::InvalidToken;return changes;}
        if(find(newSwapchain)){fault(Status::DuplicateChain,changes);return changes;}
        Chain* available{};
        for(auto& chain:chains_)if(chain.life==Life::Empty){available=&chain;break;}
        if(!available){fault(Status::CapacityExceeded,changes);return changes;}
        available->life=Life::Ready;available->token={newSwapchain,ticket.serial};available->description=description;
        changes.chain=available->token;changes.status=Status::ChainObserved;
    }catch(...){exception(changes);}
    return changes;
}
EnumerationTicket Inventory::beforeGetImages(std::uint64_t handle,bool hasArray,std::uint32_t capacity) const noexcept {
    EnumerationTicket ticket;ticket.inputCapacity=capacity;ticket.hasImageArray=hasArray;
    if(fault_!=Status::NotAttempted){ticket.status=fault_;return ticket;}
    const auto* chain=find(handle);
    if(!chain)return ticket;
    ticket.chain=chain->token;
    ticket.status=chain->life==Life::Ready?Status::ChainObserved:Status::RetiredChain;
    return ticket;
}
Changes Inventory::afterGetImages(const EnumerationTicket& supplied,const Enumeration& suppliedResult) noexcept {
    Changes changes;const auto ticket=supplied;const auto result=suppliedResult;
    try {
        if(fault_!=Status::NotAttempted){changes.status=fault_;return changes;}
        auto* chain=find(ticket.chain.chain);
        if(!ticket.chain.chain || !ticket.chain.generation || !chain){changes.status=Status::UnknownChain;return changes;}
        if(chain->token.generation!=ticket.chain.generation){changes.status=Status::StaleToken;return changes;}
        changes.chain=chain->token;changes.description=chain->description;
        if(chain->life!=Life::Ready || ticket.status!=Status::ChainObserved){changes.status=Status::RetiredChain;return changes;}
        const auto reject=[&](Status status){retire(*chain,changes);changes.status=status;};
        if(result.driverResult!=0 && result.driverResult!=5){changes.status=Status::DriverFailure;return changes;}
        if(!result.countCopied){reject(Status::InvalidEnumeration);return changes;}
        if(!ticket.hasImageArray) {
            if(result.driverResult!=0 || !result.images.empty() || !result.returnedCount || result.returnedCount>maximumImages ||
                result.returnedCount<chain->description.minImageCount){reject(Status::InvalidEnumeration);return changes;}
            if(chain->knownCount && chain->knownCount!=result.returnedCount){reject(Status::EnumerationChanged);return changes;}
            chain->knownCount=result.returnedCount;changes.status=Status::CountObserved;return changes;
        }
        if(result.returnedCount>ticket.inputCapacity || result.returnedCount>maximumImages || result.images.size()!=result.returnedCount) {
            reject(Status::InvalidEnumeration);return changes;
        }
        // Freeze the bounded copied handles before entering the common inventory.
        std::array<std::uint64_t,maximumImages> images{};
        for(std::size_t i=0;i<result.images.size();++i) {
            images[i]=result.images[i];
            if(!images[i]){reject(Status::InvalidEnumeration);return changes;}
            for(std::size_t j=0;j<i;++j)if(images[i]==images[j]){reject(Status::DuplicateImage);return changes;}
        }
        if(result.driverResult==5) {
            if(chain->knownCount && (result.returnedCount>=chain->knownCount || ticket.inputCapacity>=chain->knownCount)) {
                reject(Status::EnumerationChanged);return changes;
            }
            // INCOMPLETE does not publish a partial identity set. If a full list
            // is already known, its visible prefix still has to match exactly.
            if(chain->complete)for(std::size_t i=0;i<result.returnedCount;++i)
                if(chain->images[i].image!=images[i]){reject(Status::EnumerationChanged);return changes;}
            changes.status=Status::IncompleteIgnored;return changes;
        }
        if(!result.returnedCount || result.returnedCount<chain->description.minImageCount) {
            reject(Status::InvalidEnumeration);return changes;
        }
        if(chain->knownCount && chain->knownCount!=result.returnedCount){reject(Status::EnumerationChanged);return changes;}
        if(chain->complete) {
            for(std::size_t i=0;i<result.returnedCount;++i) {
                if(chain->images[i].image!=images[i]){reject(Status::EnumerationChanged);return changes;}
                const auto current=common_.lookup(images[i]);changes.inventoryStatus=current.status;
                if(!healthy(current.status)){fault(Status::InventoryFault,changes);return changes;}
                if(current.status!=image_inventory::Status::LiveHandleObserved || current.record.generation!=chain->images[i].generation) {
                    reject(Status::OwnershipLost);return changes;
                }
            }
            changes.status=Status::ImagesRepeated;return changes;
        }
        const auto common=common_.lookup(0);changes.inventoryStatus=common.status;
        if(!healthy(common.status)){fault(Status::InventoryFault,changes);return changes;}
        if(result.returnedCount>CommonInventory::maximumActive-common.active) {
            reject(Status::CapacityExceeded);return changes;
        }
        // Validate the ENTIRE proposed list before any created() mutation. This
        // avoids poisoning the common inventory via a later conflicting handle.
        for(std::size_t i=0;i<result.returnedCount;++i) {
            const auto existing=common_.lookup(images[i]);changes.inventoryStatus=existing.status;
            if(!healthy(existing.status)){fault(Status::InventoryFault,changes);return changes;}
            if(existing.status==image_inventory::Status::LiveHandleObserved){reject(Status::ConflictingImage);return changes;}
            for(const auto& other:chains_)if(&other!=chain && other.life!=Life::Empty)
                for(const auto& owned:other.images)if(owned.image==images[i]){reject(Status::ConflictingImage);return changes;}
        }
        const auto description=implied(chain->description);
        for(std::size_t i=0;i<result.returnedCount;++i) {
            common_.created(images[i],description);
            const auto created=common_.lookup(images[i]);changes.inventoryStatus=created.status;
            if(created.status!=image_inventory::Status::LiveHandleObserved || !created.record.generation) {
                fault(Status::InventoryFault,changes);return changes;
            }
            chain->images[i]=created.record;
            changes.added[changes.addedCount++]={created.record,chain->token,static_cast<std::uint32_t>(i)};
        }
        chain->knownCount=result.returnedCount;chain->complete=true;changes.status=Status::ImagesRegistered;
    }catch(...){exception(changes);}
    return changes;
}
Changes Inventory::beforeDestroy(std::uint64_t handle) noexcept {
    Changes changes;
    try {
        if(fault_!=Status::NotAttempted){changes.status=fault_;return changes;}
        auto* chain=find(handle);
        if(!chain){changes.status=Status::UnknownChain;return changes;}
        changes.chain=chain->token;changes.description=chain->description;
        remove(*chain,changes);*chain={};changes.status=Status::ChainDestroyed;
    }catch(...){exception(changes);}
    return changes;
}
Changes Inventory::stop() noexcept {
    Changes changes;
    try {fault(Status::Stopped,changes);}catch(...){exception(changes);}
    return changes;
}
#ifdef XESS_WSI_INVENTORY_TEST
bool Inventory::setNextSerialForTest(std::uint64_t current) noexcept {
    if(fault_!=Status::NotAttempted)return false;
    for(const auto& chain:chains_)if(chain.life!=Life::Empty)return false;
    for(const auto& pending:pending_)if(pending.serial)return false;
    serial_=current;return true;
}
#endif
} // namespace xess_vk::wsi_image_inventory
