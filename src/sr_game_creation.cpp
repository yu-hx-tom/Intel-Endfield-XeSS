#include "sr_game_creation.hpp"
#include <algorithm>
#include <cstring>

namespace xess_vk::sr_game {
const VkInstanceCreateInfo* Creation::prepareInstance(bool requested,const RequiredQueries& queries,
                                                     const VkInstanceCreateInfo& original) noexcept {
    prepared_=created_=false;instance_={};
    if(!requested)return &original;
    instanceRequirements_=queryInstanceRequirements(queries);
    instancePlan_=planInstanceRequirements(instanceRequirements_,original,true);
    if(!instancePlan_.ready())return &original;
    try {
        const auto* effective=instancePlan_.createInfo();
        application_.apiVersion=effective->pApplicationInfo->apiVersion;
        extensions_.clear();names_.clear();
        for(unsigned i=0;i<effective->enabledExtensionCount;++i)extensions_.emplace_back(effective->ppEnabledExtensionNames[i]);
        for(const auto& name:extensions_)names_.push_back(name.c_str());
        prepared_=true;return effective;
    }catch(...){return &original;}
}
void Creation::instanceCreated(VkInstance instance,VkResult result) noexcept {
    created_=prepared_&&result==VK_SUCCESS&&instance;instance_=created_?instance:VK_NULL_HANDLE;
    // Both the loader chain and application strings can now be dead stack data.
    instancePlan_={};
}
bool Creation::instanceSupported(PFN_vkEnumerateInstanceVersion version,PFN_vkEnumerateInstanceExtensionProperties enumerate) const noexcept {
    if(!prepared_||!version)return false;
    try {
        std::uint32_t api{};
        if(version(&api)!=VK_SUCCESS||api<application_.apiVersion)return false;
        const auto required=instanceRequirements_.instanceExtensions();
        if(required.empty())return true;
        if(!enumerate)return false;
        for(unsigned attempt=0;attempt<3;++attempt) {
            std::uint32_t count{};
            if(enumerate(nullptr,&count,nullptr)!=VK_SUCCESS||count>4096)return false;
            std::vector<VkExtensionProperties> extensions(count);
            const auto result=enumerate(nullptr,&count,extensions.data());
            if(result==VK_INCOMPLETE)continue;
            if(result!=VK_SUCCESS||count>extensions.size())return false;
            extensions.resize(count);
            return std::all_of(required.begin(),required.end(),[&](const auto& name){
                return std::any_of(extensions.begin(),extensions.end(),[&](const auto& value){
                    return std::memchr(value.extensionName,0,sizeof(value.extensionName))&&name==value.extensionName;});});
        }
    }catch(...){}
    return false;
}
RuntimeRequirements Creation::deviceRequirements(VkPhysicalDevice physical) const noexcept {
    if(!created_)return {};
    return queryDeviceRequirements(instanceRequirements_,instance_,physical);
}
RuntimeRequirementsAdmission Creation::admit(const RuntimeRequirements& requirements,const RuntimeIdentity& identity,
                                            const VkDeviceCreateInfo& device,VkResult result) const noexcept {
    if(!created_||identity.instance!=instance_)return {};
    VkInstanceCreateInfo instance{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instance.pApplicationInfo=&application_;instance.enabledExtensionCount=static_cast<unsigned>(names_.size());
    instance.ppEnabledExtensionNames=names_.data();
    return admitRuntimeRequirements(requirements,identity,instance,device,VK_SUCCESS,result);
}
}
