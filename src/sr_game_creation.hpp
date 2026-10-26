#pragma once
#include "xess_sr_game_requirements.hpp"
#include <vector>

namespace xess_vk::sr_game {
// Stores only the successful instance API/extension facts required by admission;
// application/loader pNext pointers never survive their synchronous create call.
class Creation {
    RuntimeRequirements instanceRequirements_;
    InstanceRequirementsPlan instancePlan_;
    VkApplicationInfo application_{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    std::vector<std::string> extensions_;
    std::vector<const char*> names_;
    VkInstance instance_{};
    bool prepared_{},created_{};
public:
    const VkInstanceCreateInfo* prepareInstance(bool requested,const RequiredQueries&,
                                              const VkInstanceCreateInfo&) noexcept;
    bool instanceSupported(PFN_vkEnumerateInstanceVersion,PFN_vkEnumerateInstanceExtensionProperties) const noexcept;
    void instanceCreated(VkInstance,VkResult) noexcept;
    RuntimeRequirements deviceRequirements(VkPhysicalDevice) const noexcept;
    RuntimeRequirementsAdmission admit(const RuntimeRequirements&,const RuntimeIdentity&,
                                      const VkDeviceCreateInfo&,VkResult) const noexcept;
    bool available() const noexcept{return created_;}
    const char* reason() const noexcept{return instancePlan_.ready()?instanceRequirements_.reason():instancePlan_.reason();}
};
}
