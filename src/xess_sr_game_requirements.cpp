#include "xess_sr_game_requirements.hpp"
#include <vulkan/vk_layer.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

namespace xess_vk::sr_game {
namespace {
struct Refusal {RequirementsStatus status;const char* reason;};
[[noreturn]] void refuse(RequirementsStatus status,const char* reason) {throw Refusal{status,reason};}
struct Fields {std::size_t first,last;};
Fields fields(VkStructureType type) {
#define SR_FIELDS(T,first,last) return {offsetof(T,first),offsetof(T,last)+sizeof(VkBool32)}
    switch(type) {
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2:
        return {offsetof(VkPhysicalDeviceFeatures2,features),offsetof(VkPhysicalDeviceFeatures2,features)+sizeof(VkPhysicalDeviceFeatures)};
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES:SR_FIELDS(VkPhysicalDeviceVulkan12Features,samplerMirrorClampToEdge,subgroupBroadcastDynamicId);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES:SR_FIELDS(VkPhysicalDeviceVulkan13Features,robustImageAccess,maintenance4);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT:SR_FIELDS(VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT,mutableDescriptorType,mutableDescriptorType);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES:SR_FIELDS(VkPhysicalDeviceShaderFloat16Int8Features,shaderFloat16,shaderInt8);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES:SR_FIELDS(VkPhysicalDeviceShaderIntegerDotProductFeatures,shaderIntegerDotProduct,shaderIntegerDotProduct);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES:SR_FIELDS(VkPhysicalDeviceScalarBlockLayoutFeatures,scalarBlockLayout,scalarBlockLayout);
    default:return {};
    }
#undef SR_FIELDS
}
struct FeatureBlock {VkStructureType type;std::size_t first;std::vector<VkBool32> values;};
using FeatureBlocks=std::vector<FeatureBlock>;
const FeatureBlock* find(const FeatureBlocks& blocks,VkStructureType type) {
    const auto at=std::find_if(blocks.begin(),blocks.end(),[&](const auto& block){return block.type==type;});
    return at==blocks.end()?nullptr:&*at;
}
bool has(const FeatureBlocks& blocks,VkStructureType type,std::size_t offset) {
    const auto* block=find(blocks,type);
    return block&&offset>=block->first&&(offset-block->first)/sizeof(VkBool32)<block->values.size()
        &&block->values[(offset-block->first)/sizeof(VkBool32)]==VK_TRUE;
}
FeatureBlocks capture(const void* head,bool required,const VkPhysicalDeviceFeatures* legacy=nullptr) {
    FeatureBlocks blocks;
    std::array<const VkBaseInStructure*,128> visited{};
    std::size_t count{};
    VkPhysicalDeviceFeatures2 legacyCore{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    const auto append=[&](const VkBaseInStructure* node) {
        if(find(blocks,node->sType))refuse(RequirementsStatus::InvalidFeatureChain,"Duplicate feature structure");
        const auto range=fields(node->sType);
        FeatureBlock copy{node->sType,range.first,{}};
        copy.values.resize((range.last-range.first)/sizeof(VkBool32));
        std::memcpy(copy.values.data(),reinterpret_cast<const unsigned char*>(node)+range.first,range.last-range.first);
        for(const auto value:copy.values)if(value>VK_TRUE)refuse(RequirementsStatus::InvalidBoolean,"Feature boolean is neither VK_FALSE nor VK_TRUE");
        blocks.push_back(std::move(copy));
    };
    if(legacy) {
        legacyCore.features=*legacy;
        append(reinterpret_cast<const VkBaseInStructure*>(&legacyCore));
    }
    for(auto* node=static_cast<const VkBaseInStructure*>(head);node;node=node->pNext) {
        if(count==visited.size()||std::find(visited.begin(),visited.begin()+count,node)!=visited.begin()+count)
            refuse(RequirementsStatus::InvalidFeatureChain,"Cyclic feature chain or more than 128 nodes");
        visited[count++]=node;
        if(!fields(node->sType).last) {
            if(required)refuse(RequirementsStatus::UnsupportedFeatureStructure,"SDK returned an unhandled feature structure");
            continue; // Unrelated application/loader nodes are borrowed only during validation.
        }
        append(node);
    }
    if(required&&!find(blocks,VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2))
        refuse(RequirementsStatus::InvalidFeatureChain,"SDK feature chain lost Features2");
    if((find(blocks,VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES)
        &&(find(blocks,VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES)
           ||find(blocks,VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES)))
        ||(find(blocks,VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES)
           &&find(blocks,VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES)))
        refuse(RequirementsStatus::InvalidFeatureChain,"Promoted and aggregate feature structures conflict");
    return blocks;
}
struct AlternateField {VkStructureType type{};std::size_t offset{};};
AlternateField alternateField(VkStructureType type,std::size_t offset) {
#define SR_EQ(T,field,U) if(type==T&&offset==offsetof(U,field))
#define SR_ALT(typeName,structName,field) return {typeName,offsetof(structName,field)}
    SR_EQ(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,shaderFloat16,VkPhysicalDeviceVulkan12Features)
        SR_ALT(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES,VkPhysicalDeviceShaderFloat16Int8Features,shaderFloat16);
    SR_EQ(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,shaderInt8,VkPhysicalDeviceVulkan12Features)
        SR_ALT(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES,VkPhysicalDeviceShaderFloat16Int8Features,shaderInt8);
    SR_EQ(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,scalarBlockLayout,VkPhysicalDeviceVulkan12Features)
        SR_ALT(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES,VkPhysicalDeviceScalarBlockLayoutFeatures,scalarBlockLayout);
    SR_EQ(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,shaderIntegerDotProduct,VkPhysicalDeviceVulkan13Features)
        SR_ALT(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES,VkPhysicalDeviceShaderIntegerDotProductFeatures,shaderIntegerDotProduct);
    SR_EQ(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES,shaderFloat16,VkPhysicalDeviceShaderFloat16Int8Features)
        SR_ALT(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,VkPhysicalDeviceVulkan12Features,shaderFloat16);
    SR_EQ(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES,shaderInt8,VkPhysicalDeviceShaderFloat16Int8Features)
        SR_ALT(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,VkPhysicalDeviceVulkan12Features,shaderInt8);
    SR_EQ(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES,scalarBlockLayout,VkPhysicalDeviceScalarBlockLayoutFeatures)
        SR_ALT(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,VkPhysicalDeviceVulkan12Features,scalarBlockLayout);
    SR_EQ(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES,shaderIntegerDotProduct,VkPhysicalDeviceShaderIntegerDotProductFeatures)
        SR_ALT(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,VkPhysicalDeviceVulkan13Features,shaderIntegerDotProduct);
#undef SR_ALT
#undef SR_EQ
    return {};
}
bool enabled(const FeatureBlocks& blocks,VkStructureType type,std::size_t offset) {
    if(find(blocks,type))return has(blocks,type,offset);
    const auto alternate=alternateField(type,offset);
    return alternate.type&&has(blocks,alternate.type,alternate.offset);
}
std::vector<std::string> copyNames(std::uint32_t count,const char* const* names) {
    if(count>512||(count&&!names))refuse(RequirementsStatus::InvalidInput,"Absent or excessive extension name array");
    std::vector<std::string> copied;
    for(std::uint32_t i=0;i<count;++i) {
        if(!names[i])refuse(RequirementsStatus::InvalidInput,"Null extension name");
        std::size_t length{};
        while(length<VK_MAX_EXTENSION_NAME_SIZE&&names[i][length])++length;
        if(!length||length==VK_MAX_EXTENSION_NAME_SIZE)refuse(RequirementsStatus::InvalidInput,"Invalid extension name");
        const std::string name(names[i],length);
        if(std::find(copied.begin(),copied.end(),name)==copied.end())copied.push_back(name);
    }
    return copied;
}
void requireNames(const std::vector<std::string>& required,std::uint32_t count,const char* const* names,RequirementsStatus status) {
    const auto effective=copyNames(count,names);
    for(const auto& name:required)if(std::find(effective.begin(),effective.end(),name)==effective.end())
        refuse(status,status==RequirementsStatus::MissingInstanceExtension?"SDK instance extension was not enabled":"SDK device extension was not enabled");
}
std::vector<std::string> mergedNames(std::uint32_t count,const char* const* original,const std::vector<std::string>& required) {
    auto result=copyNames(count,original);
    for(const auto& name:required)if(std::find(result.begin(),result.end(),name)==result.end())result.push_back(name);
    if(result.size()>512)refuse(RequirementsStatus::InvalidInput,"Merged extension array exceeds 512 names");
    return result;
}
std::vector<const char*> pointers(const std::vector<std::string>& names) {
    std::vector<const char*> result;result.reserve(names.size());
    for(const auto& name:names)result.push_back(name.c_str());
    return result;
}

// Same typed flat-node whitelist and synchronous loader borrowing as the bridge
// planner; unknown/pointer-rich app nodes refuse instead of losing their payload.
#define SR_APP_NODES(X) \
 X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,VkPhysicalDeviceFeatures2,0,0) \
 X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,VkPhysicalDeviceVulkan11Features,11,0) \
 X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,VkPhysicalDeviceVulkan12Features,12,0) \
 X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,VkPhysicalDeviceVulkan13Features,13,0) \
 X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES,VkPhysicalDeviceVulkan14Features,14,0) \
 X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES,VkPhysicalDevice16BitStorageFeatures,0,11) \
 X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES,VkPhysicalDeviceMultiviewFeatures,0,11) \
 X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES,VkPhysicalDeviceVariablePointersFeatures,0,11) \
 X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES,VkPhysicalDeviceProtectedMemoryFeatures,0,11) \
 X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES,VkPhysicalDeviceSamplerYcbcrConversionFeatures,0,11) \
 X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES,VkPhysicalDeviceShaderDrawParametersFeatures,0,11) \
 X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES,VkPhysicalDevice8BitStorageFeatures,0,12) \
 X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_INT64_FEATURES,VkPhysicalDeviceShaderAtomicInt64Features,0,12) \
 X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES,VkPhysicalDeviceShaderFloat16Int8Features,0,12) \
 X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES,VkPhysicalDeviceDescriptorIndexingFeatures,0,12) \
 X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES,VkPhysicalDeviceScalarBlockLayoutFeatures,0,12) \
 X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGELESS_FRAMEBUFFER_FEATURES,VkPhysicalDeviceImagelessFramebufferFeatures,0,12) \
 X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFORM_BUFFER_STANDARD_LAYOUT_FEATURES,VkPhysicalDeviceUniformBufferStandardLayoutFeatures,0,12) \
 X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_EXTENDED_TYPES_FEATURES,VkPhysicalDeviceShaderSubgroupExtendedTypesFeatures,0,12) \
 X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SEPARATE_DEPTH_STENCIL_LAYOUTS_FEATURES,VkPhysicalDeviceSeparateDepthStencilLayoutsFeatures,0,12) \
 X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES,VkPhysicalDeviceHostQueryResetFeatures,0,12) \
 X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES,VkPhysicalDeviceTimelineSemaphoreFeatures,0,12) \
 X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES,VkPhysicalDeviceBufferDeviceAddressFeatures,0,12) \
 X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES,VkPhysicalDeviceVulkanMemoryModelFeatures,0,12) \
 X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES,VkPhysicalDeviceDynamicRenderingFeatures,0,13) \
 X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_ROBUSTNESS_FEATURES,VkPhysicalDeviceImageRobustnessFeatures,0,13) \
 X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INLINE_UNIFORM_BLOCK_FEATURES,VkPhysicalDeviceInlineUniformBlockFeatures,0,13) \
 X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES,VkPhysicalDeviceMaintenance4Features,0,13) \
 X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_CREATION_CACHE_CONTROL_FEATURES,VkPhysicalDevicePipelineCreationCacheControlFeatures,0,13) \
 X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIVATE_DATA_FEATURES,VkPhysicalDevicePrivateDataFeatures,0,13) \
 X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES,VkPhysicalDeviceShaderDemoteToHelperInvocationFeatures,0,13) \
 X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES,VkPhysicalDeviceShaderIntegerDotProductFeatures,0,13) \
 X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_TERMINATE_INVOCATION_FEATURES,VkPhysicalDeviceShaderTerminateInvocationFeatures,0,13) \
 X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES,VkPhysicalDeviceSubgroupSizeControlFeatures,0,13) \
 X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES,VkPhysicalDeviceSynchronization2Features,0,13) \
 X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXTURE_COMPRESSION_ASTC_HDR_FEATURES,VkPhysicalDeviceTextureCompressionASTCHDRFeatures,0,13) \
 X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ZERO_INITIALIZE_WORKGROUP_MEMORY_FEATURES,VkPhysicalDeviceZeroInitializeWorkgroupMemoryFeatures,0,13) \
 X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT,VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT,0,0) \
 X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR,VkPhysicalDeviceFragmentShadingRateFeaturesKHR,0,0) \
 X(VK_STRUCTURE_TYPE_DEVICE_PRIVATE_DATA_CREATE_INFO,VkDevicePrivateDataCreateInfo,0,0)
struct Node {
    virtual ~Node()=default;
    virtual void* data() noexcept=0;
    virtual void next(const void*) noexcept=0;
    unsigned aggregate{},member{};
    VkStructureType type() noexcept {return static_cast<VkBaseOutStructure*>(data())->sType;}
};
template<class T> struct TypedNode final:Node {
    T value{};
    TypedNode(VkStructureType type,const VkBaseInStructure* original) {
        if(original)value=*reinterpret_cast<const T*>(original);
        else value.sType=type;
        value.pNext=nullptr;
    }
    void* data() noexcept override {return &value;}
    void next(const void* pointer) noexcept override {
        if constexpr(std::is_same_v<decltype(value.pNext),void*>)value.pNext=const_cast<void*>(pointer);
        else value.pNext=pointer;
    }
};
using Chain=std::vector<std::unique_ptr<Node>>;
std::unique_ptr<Node> makeNode(VkStructureType type,const VkBaseInStructure* original=nullptr) {
    std::unique_ptr<Node> result;
    switch(type) {
#define SR_NODE(stype,T,agg,mem) case stype:result=std::make_unique<TypedNode<T>>(type,original);result->aggregate=agg;result->member=mem;break;
    SR_APP_NODES(SR_NODE)
#undef SR_NODE
    case VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO:result=std::make_unique<TypedNode<VkLayerDeviceCreateInfo>>(type,original);break;
    default:refuse(RequirementsStatus::UnsupportedApplicationChain,"Application pNext node is outside the typed flat whitelist");
    }
    return result;
}
Node* findNode(Chain& chain,VkStructureType type) {
    for(const auto& node:chain)if(node->type()==type)return node.get();
    return nullptr;
}
bool conflicts(const Chain& chain,const Node& next) {
    for(const auto& node:chain)if((next.aggregate&&node->member==next.aggregate)||(next.member&&node->aggregate==next.member))return true;
    return false;
}
Chain cloneChain(const void* head,AllowBorrowedLoaderChain allowLoader,bool& borrowed) {
    Chain result;
    std::array<const VkBaseInStructure*,128> visited{};std::size_t count{};
    std::vector<VkLayerFunction> loaderFunctions;
    for(auto* source=static_cast<const VkBaseInStructure*>(head);source;source=source->pNext) {
        if(count==visited.size()||std::find(visited.begin(),visited.begin()+count,source)!=visited.begin()+count)
            refuse(RequirementsStatus::InvalidFeatureChain,"Cyclic application pNext chain or more than 128 nodes");
        visited[count++]=source;
        if(source->sType==VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO) {
            if(allowLoader!=AllowBorrowedLoaderChain::SynchronousCreateDeviceCallOnly)
                refuse(RequirementsStatus::BorrowedLoaderChainNotAllowed,"Loader payload requires explicit synchronous borrowing");
            const auto function=reinterpret_cast<const VkLayerDeviceCreateInfo*>(source)->function;
            if((function!=VK_LAYER_LINK_INFO&&function!=VK_LOADER_DATA_CALLBACK)
                ||std::find(loaderFunctions.begin(),loaderFunctions.end(),function)!=loaderFunctions.end())
                refuse(RequirementsStatus::UnsupportedApplicationChain,"Unknown or repeated loader device-create function payload");
            loaderFunctions.push_back(function);borrowed=true;
        } else if(findNode(result,source->sType))refuse(RequirementsStatus::InvalidFeatureChain,"Duplicate application pNext type");
        auto copy=makeNode(source->sType,source);
        if(conflicts(result,*copy))refuse(RequirementsStatus::InvalidFeatureChain,"Application aggregate and promoted feature structures conflict");
        result.push_back(std::move(copy));
    }
    return result;
}
void setTrue(Node& node,std::size_t offset) {
    const VkBool32 enabled=VK_TRUE;
    std::memcpy(static_cast<unsigned char*>(node.data())+offset,&enabled,sizeof(enabled));
}
void mergeFeatures(Chain& chain,const FeatureBlocks& required) {
    for(const auto& block:required) {
        auto* destination=findNode(chain,block.type);
        if(!destination) {
            auto candidate=makeNode(block.type);
            if(!conflicts(chain,*candidate)) {destination=candidate.get();chain.push_back(std::move(candidate));}
        }
        for(std::size_t i=0;i<block.values.size();++i)if(block.values[i]) {
            const auto offset=block.first+i*sizeof(VkBool32);
            if(destination) {setTrue(*destination,offset);continue;}
            const auto alternate=alternateField(block.type,offset);
            if(!alternate.type)refuse(RequirementsStatus::UnsupportedFeatureStructure,"Required aggregate field has no supported promoted merge mapping");
            auto* mapped=findNode(chain,alternate.type);
            if(!mapped) {
                auto candidate=makeNode(alternate.type);
                if(conflicts(chain,*candidate))refuse(RequirementsStatus::InvalidFeatureChain,"Required feature merge would duplicate a promoted structure");
                mapped=candidate.get();chain.push_back(std::move(candidate));
            }
            setTrue(*mapped,alternate.offset);
        }
    }
}
const void* link(Chain& chain) noexcept {
    const void* head=nullptr;
    for(auto at=chain.rbegin();at!=chain.rend();++at) {(*at)->next(head);head=(*at)->data();}
    return head;
}
void validateDeviceSupport(VkPhysicalDevice physical,const FeatureBlocks& required,
    const std::vector<std::string>& requiredExtensions,const VkDeviceCreateInfo& original,DeviceSupportQueries queries) {
    if(!queries.features)refuse(RequirementsStatus::CapabilityQueryMissing,"A physical-device Features2 support query is required before changing creation parameters");
    const auto originalExtensions=copyNames(original.enabledExtensionCount,original.ppEnabledExtensionNames);
    std::vector<std::string> added;
    for(const auto& name:requiredExtensions)
        if(std::find(originalExtensions.begin(),originalExtensions.end(),name)==originalExtensions.end())added.push_back(name);
    if(!added.empty()) {
        if(!queries.extensions)refuse(RequirementsStatus::CapabilityQueryMissing,"A device-extension support query is required for newly enabled SDK extensions");
        std::vector<VkExtensionProperties> available;
        bool complete=false;
        for(unsigned attempt=0;attempt<3;++attempt) {
            std::uint32_t count{};
            if(queries.extensions(physical,nullptr,&count,nullptr)!=VK_SUCCESS||count>4096)
                refuse(RequirementsStatus::CapabilityQueryFailed,"Device-extension count query failed or exceeded 4096 names");
            available.resize(count);
            if(!count) {complete=true;break;}
            const auto capacity=count;
            const auto result=queries.extensions(physical,nullptr,&count,available.data());
            if(result==VK_INCOMPLETE)continue;
            if(result!=VK_SUCCESS||count>capacity)
                refuse(RequirementsStatus::CapabilityQueryFailed,"Device-extension enumeration failed");
            available.resize(count);complete=true;break;
        }
        if(!complete)refuse(RequirementsStatus::CapabilityQueryFailed,"Device-extension enumeration remained incomplete after three attempts");
        for(const auto& name:added) {
            const bool present=std::any_of(available.begin(),available.end(),[&](const auto& extension) {
                return std::memchr(extension.extensionName,0,VK_MAX_EXTENSION_NAME_SIZE)&&name==extension.extensionName;
            });
            if(!present)refuse(RequirementsStatus::RequiredExtensionUnsupported,"A newly enabled SDK required extension is unsupported");
        }
    }
    Chain supported;
    supported.push_back(makeNode(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2));
    for(const auto& block:required)if(block.type!=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2)supported.push_back(makeNode(block.type));
    link(supported);
    queries.features(physical,static_cast<VkPhysicalDeviceFeatures2*>(supported.front()->data()));
    const auto captured=capture(supported.front()->data(),true);
    for(const auto& block:required)for(std::size_t i=0;i<block.values.size();++i)
        if(block.values[i]&&!enabled(captured,block.type,block.first+i*sizeof(VkBool32)))
            refuse(RequirementsStatus::RequiredFeatureUnsupported,"A runtime SDK required feature is unsupported on the queried physical device");
}
#undef SR_APP_NODES
} // namespace

struct RuntimeRequirements::Impl {
    RequiredQueries queries;
    VkInstance instance{};
    VkPhysicalDevice physical{};
    std::uint32_t minimumApi{};
    std::vector<std::string> instanceExtensions,deviceExtensions;
    FeatureBlocks features;
};
RuntimeRequirements::RuntimeRequirements() noexcept=default;
RuntimeRequirements::~RuntimeRequirements()=default;
RuntimeRequirements::RuntimeRequirements(RuntimeRequirements&&) noexcept=default;
RuntimeRequirements& RuntimeRequirements::operator=(RuntimeRequirements&&) noexcept=default;
bool RuntimeRequirements::queried() const noexcept {return status_==RequirementsStatus::Ready&&impl_;}
bool RuntimeRequirements::instanceQueried() const noexcept {return (status_==RequirementsStatus::InstanceReady||status_==RequirementsStatus::Ready)&&impl_;}
RequirementsStatus RuntimeRequirements::status() const noexcept {return (status_==RequirementsStatus::Ready||status_==RequirementsStatus::InstanceReady)&&!impl_?RequirementsStatus::Unqueried:status_;}
const char* RuntimeRequirements::reason() const noexcept {return (status_==RequirementsStatus::Ready||status_==RequirementsStatus::InstanceReady)&&!impl_?"Requirements storage was moved":reason_;}
std::uint32_t RuntimeRequirements::minimumApiVersion() const noexcept {return impl_?impl_->minimumApi:0;}
std::span<const std::string> RuntimeRequirements::instanceExtensions() const noexcept {return impl_?std::span<const std::string>(impl_->instanceExtensions):std::span<const std::string>{};}
std::span<const std::string> RuntimeRequirements::deviceExtensions() const noexcept {return impl_?std::span<const std::string>(impl_->deviceExtensions):std::span<const std::string>{};}

RuntimeRequirements queryInstanceRequirements(const RequiredQueries& queries) noexcept {
    RuntimeRequirements result;
    try {
        if(!queries.instanceExtensions||!queries.deviceExtensions||!queries.deviceFeatures)
            refuse(RequirementsStatus::InvalidInput,"Requirements queries need nonnull SDK exports");
        auto owned=std::make_unique<RuntimeRequirements::Impl>();
        owned->queries=queries;
        std::uint32_t count{};const char* const* names{};
        if(queries.instanceExtensions(&count,&names,&owned->minimumApi)<0)
            refuse(RequirementsStatus::QueryFailed,"SDK instance requirements query failed");
        if(VK_API_VERSION_VARIANT(owned->minimumApi)||owned->minimumApi<VK_API_VERSION_1_1)
            refuse(RequirementsStatus::InvalidInput,"SDK returned an invalid minimum Vulkan API version");
        owned->instanceExtensions=copyNames(count,names);
        result.impl_=std::move(owned);result.status_=RequirementsStatus::InstanceReady;result.reason_="SDK instance requirements queried and copied";
    } catch(const Refusal& error) {result.status_=error.status;result.reason_=error.reason;}
    catch(const std::bad_alloc&) {result.status_=RequirementsStatus::OutOfMemory;result.reason_="Requirements snapshot allocation failed";}
    catch(...) {result.status_=RequirementsStatus::QueryFailed;result.reason_="Requirements query threw an exception";}
    return result;
}
RuntimeRequirements queryDeviceRequirements(RuntimeRequirements&& instanceRequirements,VkInstance instance,VkPhysicalDevice physical) noexcept {
    RuntimeRequirements result=std::move(instanceRequirements);
    try {
        if(!result.instanceQueried())refuse(result.status(),result.reason());
        if(result.queried())refuse(RequirementsStatus::InvalidInput,"Device requirements were already queried");
        if(!instance||!physical)refuse(RequirementsStatus::InvalidInput,"Device requirements need nonnull instance/physical-device identity");
        auto& owned=*result.impl_;
        owned.instance=instance;owned.physical=physical;
        std::uint32_t count{};const char* const* names{};
        if(owned.queries.deviceExtensions(instance,physical,&count,&names)<0)
            refuse(RequirementsStatus::QueryFailed,"SDK device extension requirements query failed");
        owned.deviceExtensions=copyNames(count,names);
        VkPhysicalDeviceFeatures2 seed{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};void* head=&seed;
        if(owned.queries.deviceFeatures(instance,physical,&head)<0)
            refuse(RequirementsStatus::QueryFailed,"SDK device feature requirements query failed");
        // Same immediate owned capture as the standalone SR fixture: retain no SDK pNext pointer.
        owned.features=capture(head,true);
        result.status_=RequirementsStatus::Ready;result.reason_="SDK requirements queried and copied";
    } catch(const Refusal& error) {result.status_=error.status;result.reason_=error.reason;}
    catch(const std::bad_alloc&) {result.status_=RequirementsStatus::OutOfMemory;result.reason_="Requirements snapshot allocation failed";}
    catch(...) {result.status_=RequirementsStatus::QueryFailed;result.reason_="Requirements query threw an exception";}
    return result;
}
RuntimeRequirements queryRuntimeRequirements(const RequiredQueries& queries,VkInstance instance,VkPhysicalDevice physical) noexcept {
    return queryDeviceRequirements(queryInstanceRequirements(queries),instance,physical);
}
RuntimeRequirements queryDeviceRequirements(const RuntimeRequirements& source,VkInstance instance,VkPhysicalDevice physical) noexcept {
    RuntimeRequirements copy;
    if(!source.instanceQueried()){copy.status_=source.status();copy.reason_=source.reason();return copy;}
    try {
        copy.impl_=std::make_unique<RuntimeRequirements::Impl>();
        copy.impl_->queries=source.impl_->queries;
        copy.impl_->minimumApi=source.impl_->minimumApi;
        copy.impl_->instanceExtensions=source.impl_->instanceExtensions;
        copy.status_=RequirementsStatus::InstanceReady;
    }catch(...){copy.status_=RequirementsStatus::OutOfMemory;copy.reason_="Instance requirements copy failed";return copy;}
    return queryDeviceRequirements(std::move(copy),instance,physical);
}

struct InstanceRequirementsPlan::Impl {
    VkInstanceCreateInfo info{};
    VkApplicationInfo application{};
    std::vector<std::string> extensions;
    std::vector<const char*> extensionPointers;
};
InstanceRequirementsPlan::InstanceRequirementsPlan() noexcept=default;
InstanceRequirementsPlan::~InstanceRequirementsPlan()=default;
InstanceRequirementsPlan::InstanceRequirementsPlan(InstanceRequirementsPlan&&) noexcept=default;
InstanceRequirementsPlan& InstanceRequirementsPlan::operator=(InstanceRequirementsPlan&&) noexcept=default;
bool InstanceRequirementsPlan::ready() const noexcept {return status_==RequirementsStatus::Ready&&impl_;}
RequirementsStatus InstanceRequirementsPlan::status() const noexcept {return status_==RequirementsStatus::Ready&&!impl_?RequirementsStatus::NotRequested:status_;}
const char* InstanceRequirementsPlan::reason() const noexcept {return status_==RequirementsStatus::Ready&&!impl_?"Plan storage was moved":reason_;}
const VkInstanceCreateInfo* InstanceRequirementsPlan::createInfo() const noexcept {return ready()?&impl_->info:nullptr;}
InstanceRequirementsPlan planInstanceRequirements(const RuntimeRequirements& requirements,const VkInstanceCreateInfo& original,bool optIn) noexcept {
    InstanceRequirementsPlan result;
    if(!optIn)return result;
    try {
        if(!requirements.instanceQueried())refuse(requirements.status(),requirements.reason());
        if(original.sType!=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO
            ||(original.pApplicationInfo&&original.pApplicationInfo->sType!=VK_STRUCTURE_TYPE_APPLICATION_INFO))
            refuse(RequirementsStatus::InvalidInput,"Invalid original instance creation header");
        auto owned=std::make_unique<InstanceRequirementsPlan::Impl>();owned->info=original;
        owned->application.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO;
        if(original.pApplicationInfo)owned->application=*original.pApplicationInfo;
        const auto api=owned->application.apiVersion?owned->application.apiVersion:VK_API_VERSION_1_0;
        if(VK_API_VERSION_VARIANT(api))refuse(RequirementsStatus::InvalidInput,"Unsupported Vulkan API version variant");
        owned->application.apiVersion=std::max(api,requirements.impl_->minimumApi);
        owned->info.pApplicationInfo=&owned->application;
        owned->extensions=mergedNames(original.enabledExtensionCount,original.ppEnabledExtensionNames,requirements.impl_->instanceExtensions);
        owned->extensionPointers=pointers(owned->extensions);
        owned->info.enabledExtensionCount=static_cast<std::uint32_t>(owned->extensionPointers.size());
        owned->info.ppEnabledExtensionNames=owned->extensionPointers.data();
        result.impl_=std::move(owned);result.status_=RequirementsStatus::Ready;result.reason_="Owned instance API/extensions plan ready for one synchronous create call";
    } catch(const Refusal& error) {result.status_=error.status;result.reason_=error.reason;}
    catch(const std::bad_alloc&) {result.status_=RequirementsStatus::OutOfMemory;result.reason_="Instance requirements plan allocation failed";}
    catch(...) {result.status_=RequirementsStatus::InvalidInput;result.reason_="Instance requirements planning threw an exception";}
    return result;
}

struct DeviceRequirementsPlan::Impl {
    VkDeviceCreateInfo info{};
    std::vector<std::string> extensions;
    std::vector<const char*> extensionPointers;
    Chain chain;
    bool borrowedLoader{};
};
DeviceRequirementsPlan::DeviceRequirementsPlan() noexcept=default;
DeviceRequirementsPlan::~DeviceRequirementsPlan()=default;
DeviceRequirementsPlan::DeviceRequirementsPlan(DeviceRequirementsPlan&&) noexcept=default;
DeviceRequirementsPlan& DeviceRequirementsPlan::operator=(DeviceRequirementsPlan&&) noexcept=default;
bool DeviceRequirementsPlan::ready() const noexcept {return status_==RequirementsStatus::Ready&&impl_;}
RequirementsStatus DeviceRequirementsPlan::status() const noexcept {return status_==RequirementsStatus::Ready&&!impl_?RequirementsStatus::NotRequested:status_;}
const char* DeviceRequirementsPlan::reason() const noexcept {return status_==RequirementsStatus::Ready&&!impl_?"Plan storage was moved":reason_;}
const VkDeviceCreateInfo* DeviceRequirementsPlan::createInfo() const noexcept {return ready()?&impl_->info:nullptr;}
bool DeviceRequirementsPlan::requiresSynchronousLoaderCall() const noexcept {return impl_&&impl_->borrowedLoader;}
DeviceRequirementsPlan planDeviceRequirements(const RuntimeRequirements& requirements,const VkDeviceCreateInfo& original,
    bool optIn,AllowBorrowedLoaderChain allowLoader,DeviceSupportQueries supportQueries) noexcept {
    DeviceRequirementsPlan result;
    if(!optIn)return result;
    try {
        if(!requirements.queried())refuse(requirements.status(),requirements.reason());
        if(original.sType!=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO)refuse(RequirementsStatus::InvalidInput,"Invalid original device creation header");
        (void)capture(original.pNext,false,original.pEnabledFeatures);
        auto owned=std::make_unique<DeviceRequirementsPlan::Impl>();owned->info=original;
        owned->chain=cloneChain(original.pNext,allowLoader,owned->borrowedLoader);
        if(original.pEnabledFeatures) {
            auto core=makeNode(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2);
            static_cast<VkPhysicalDeviceFeatures2*>(core->data())->features=*original.pEnabledFeatures;
            owned->chain.push_back(std::move(core));
        }
        mergeFeatures(owned->chain,requirements.impl_->features);
        owned->info.pEnabledFeatures=nullptr;owned->info.pNext=link(owned->chain);
        const auto effective=capture(owned->info.pNext,false);
        for(const auto& block:requirements.impl_->features)for(std::size_t i=0;i<block.values.size();++i)
            if(block.values[i]&&!enabled(effective,block.type,block.first+i*sizeof(VkBool32)))
                refuse(RequirementsStatus::MissingEnabledFeature,"Merged plan did not retain every runtime SDK required feature");
        owned->extensions=mergedNames(original.enabledExtensionCount,original.ppEnabledExtensionNames,requirements.impl_->deviceExtensions);
        owned->extensionPointers=pointers(owned->extensions);
        owned->info.enabledExtensionCount=static_cast<std::uint32_t>(owned->extensionPointers.size());
        owned->info.ppEnabledExtensionNames=owned->extensionPointers.data();
        validateDeviceSupport(requirements.impl_->physical,requirements.impl_->features,requirements.impl_->deviceExtensions,original,supportQueries);
        result.impl_=std::move(owned);result.status_=RequirementsStatus::Ready;result.reason_="Owned device extensions/features plan ready for one synchronous create call";
    } catch(const Refusal& error) {result.status_=error.status;result.reason_=error.reason;}
    catch(const std::bad_alloc&) {result.status_=RequirementsStatus::OutOfMemory;result.reason_="Device requirements plan allocation failed";}
    catch(...) {result.status_=RequirementsStatus::InvalidInput;result.reason_="Device requirements planning threw an exception";}
    return result;
}
RuntimeRequirementsAdmission admitRuntimeRequirements(const RuntimeRequirements& requirements,const RuntimeIdentity& identity,
    const VkInstanceCreateInfo& instance,const VkDeviceCreateInfo& device,VkResult instanceResult,VkResult deviceResult) noexcept {
    RuntimeRequirementsAdmission result;
    try {
        if(!requirements.queried())refuse(requirements.status(),requirements.reason());
        const auto& required=*requirements.impl_;
        if(!identity.instance||!identity.physicalDevice||!identity.device||!identity.incarnation
            ||identity.instance!=required.instance||identity.physicalDevice!=required.physical)
            refuse(RequirementsStatus::IdentityMismatch,"Creation identity differs from the SDK queried instance/physical device");
        if(instanceResult!=VK_SUCCESS||deviceResult!=VK_SUCCESS)
            refuse(RequirementsStatus::CreationFailed,"Successful instance and device creation evidence is required");
        if(instance.sType!=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO||device.sType!=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO
            ||(instance.pApplicationInfo&&instance.pApplicationInfo->sType!=VK_STRUCTURE_TYPE_APPLICATION_INFO))
            refuse(RequirementsStatus::InvalidInput,"Invalid effective Vulkan creation structure");
        const auto api=instance.pApplicationInfo&&instance.pApplicationInfo->apiVersion?instance.pApplicationInfo->apiVersion:VK_API_VERSION_1_0;
        if(VK_API_VERSION_VARIANT(api)||api<required.minimumApi)
            refuse(RequirementsStatus::InstanceApiTooLow,"Effective instance API version is below the SDK minimum");
        requireNames(required.instanceExtensions,instance.enabledExtensionCount,instance.ppEnabledExtensionNames,RequirementsStatus::MissingInstanceExtension);
        requireNames(required.deviceExtensions,device.enabledExtensionCount,device.ppEnabledExtensionNames,RequirementsStatus::MissingDeviceExtension);
        const auto effective=capture(device.pNext,false,device.pEnabledFeatures);
        for(const auto& block:required.features)for(std::size_t i=0;i<block.values.size();++i)
            if(block.values[i]&&!enabled(effective,block.type,block.first+i*sizeof(VkBool32)))
                refuse(RequirementsStatus::MissingEnabledFeature,"A runtime SDK required feature was not enabled at device creation");
        result.status_=RequirementsStatus::Ready;result.reason_="Runtime SDK requirements match successful effective creation parameters";
        result.identity_=identity;result.queries_=required.queries;
    } catch(const Refusal& error) {result.status_=error.status;result.reason_=error.reason;}
    catch(const std::bad_alloc&) {result.status_=RequirementsStatus::OutOfMemory;result.reason_="Requirements validation allocation failed";}
    catch(...) {result.status_=RequirementsStatus::InvalidInput;result.reason_="Requirements validation threw an exception";}
    return result;
}
} // namespace xess_vk::sr_game
