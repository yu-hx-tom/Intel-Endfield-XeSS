// Read-only WSI capability inventory. No logical device, queue, surface, window,
// swapchain, SDK context, game module, or GPU workload is created.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <vulkan/vulkan_core.h>
#include <algorithm>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void check(VkResult value, const char* operation) {
    if (value != VK_SUCCESS) throw std::runtime_error(std::string(operation) + ": " + std::to_string(value));
}
std::string quoted(const char* value) {
    std::ostringstream out; out << '"';
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(value); *p; ++p) {
        if (*p == '"' || *p == '\\') out << '\\' << char(*p);
        else if (*p < 32) out << "\\u00" << std::hex << std::setw(2) << std::setfill('0') << unsigned(*p) << std::dec;
        else out << char(*p);
    }
    out << '"'; return out.str();
}
template<class T, class Enumerate> std::vector<T> enumerate(Enumerate operation, uint32_t limit, const char* name) {
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        uint32_t count{}; check(operation(&count, nullptr), name);
        if (count > limit) throw std::runtime_error(std::string(name) + ": count exceeds bound");
        std::vector<T> values(count);
        if (!count) return values;
        uint32_t written = count;
        const auto result = operation(&written, values.data());
        if (written > count) throw std::runtime_error(std::string(name) + ": invalid returned count");
        if (result == VK_INCOMPLETE) continue;
        check(result, name); values.resize(written); return values;
    }
    throw std::runtime_error(std::string(name) + ": unstable enumeration");
}
bool has(const std::vector<VkExtensionProperties>& values, const char* name) {
    return std::any_of(values.begin(), values.end(), [name](const auto& value) {
        return std::strncmp(value.extensionName, name, VK_MAX_EXTENSION_NAME_SIZE) == 0;
    });
}
void extensionList(std::ostream& out, const std::vector<VkExtensionProperties>& values) {
    out << '['; bool first = true;
    for (const auto& value : values) {
        if (!std::memchr(value.extensionName, 0, sizeof(value.extensionName))) throw std::runtime_error("Unterminated extension name");
        if (!first) out << ',';
        first = false; out << quoted(value.extensionName);
    }
    out << ']';
}
struct Owner {
    HMODULE loader{}; PFN_vkGetInstanceProcAddr gipa{};
    VkInstance instance{}; PFN_vkDestroyInstance destroy{};
    ~Owner() { if (instance && destroy) destroy(instance, nullptr); if (loader) FreeLibrary(loader); }
    template<class T> T get(const char* name) const {
        const auto pointer = gipa(instance, name);
        if (!pointer) throw std::runtime_error(std::string("Missing Vulkan entry: ") + name);
        return reinterpret_cast<T>(pointer);
    }
};
template<class T> T structure(VkStructureType type) { T value{}; value.sType = type; return value; }
void feature(std::ostream& out, const char* name, bool advertised, VkBool32 value) {
    out << ',' << quoted(name) << ":{\"advertised\":" << (advertised ? "true" : "false")
        << ",\"feature_queried\":" << (advertised ? "true" : "false") << ",\"feature_supported\":";
    if (advertised) out << (value ? "true" : "false"); else out << "null";
    out << ",\"enabled\":false}";
}
int probe() {
    Owner owner;
    owner.loader = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!owner.loader) throw std::runtime_error("Cannot load system Vulkan loader");
    owner.gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(owner.loader, "vkGetInstanceProcAddr"));
    if (!owner.gipa) throw std::runtime_error("Missing vkGetInstanceProcAddr");
    const auto getVersion = owner.get<PFN_vkEnumerateInstanceVersion>("vkEnumerateInstanceVersion");
    uint32_t version{}; check(getVersion(&version), "vkEnumerateInstanceVersion");
    if (version < VK_API_VERSION_1_1) throw std::runtime_error("Probe requires Vulkan 1.1 property/feature queries");
    const auto enumInstance = owner.get<PFN_vkEnumerateInstanceExtensionProperties>("vkEnumerateInstanceExtensionProperties");
    const auto instanceExtensions = enumerate<VkExtensionProperties>([&](auto count, auto values) { return enumInstance(nullptr, count, values); }, 1024, "Instance extensions");
    auto app = structure<VkApplicationInfo>(VK_STRUCTURE_TYPE_APPLICATION_INFO);
    app.pApplicationName = "Read-only present capability inventory"; app.apiVersion = VK_API_VERSION_1_1;
    auto create = structure<VkInstanceCreateInfo>(VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO); create.pApplicationInfo = &app;
    const auto createInstance = owner.get<PFN_vkCreateInstance>("vkCreateInstance");
    check(createInstance(&create, nullptr, &owner.instance), "vkCreateInstance");
    owner.destroy = owner.get<PFN_vkDestroyInstance>("vkDestroyInstance");
    const auto enumDevices = owner.get<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices");
    const auto devices = enumerate<VkPhysicalDevice>([&](auto count, auto values) { return enumDevices(owner.instance, count, values); }, 16, "Physical devices");
    const auto enumDevice = owner.get<PFN_vkEnumerateDeviceExtensionProperties>("vkEnumerateDeviceExtensionProperties");
    const auto getProperties = owner.get<PFN_vkGetPhysicalDeviceProperties2>("vkGetPhysicalDeviceProperties2");
    const auto getFeatures = owner.get<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2");
    std::ostringstream out;
    out << "{\"stage\":\"read-only-wsi-present-capability-probe\",\"schema\":1,\"loader_api_version\":" << version
        << ",\"header_version\":" << VK_HEADER_VERSION << ",\"created_instance\":true,\"created_logical_device\":false"
        << ",\"created_queue\":false,\"created_surface\":false,\"created_window\":false,\"submitted_gpu_work\":false"
        << ",\"game_touched\":false,\"enabled_instance_extensions\":[],\"instance_extensions\":";
    extensionList(out, instanceExtensions);
    out << ",\"surface_maintenance_khr\":" << (has(instanceExtensions, VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME) ? "true" : "false")
        << ",\"surface_maintenance_ext\":" << (has(instanceExtensions, VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME) ? "true" : "false")
        << ",\"devices\":[";
    bool first = true;
    for (auto device : devices) {
        auto properties = structure<VkPhysicalDeviceProperties2>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2);
        auto identity = structure<VkPhysicalDeviceIDProperties>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES);
        properties.pNext = &identity; getProperties(device, &properties);
        if (!std::memchr(properties.properties.deviceName, 0, sizeof(properties.properties.deviceName))) throw std::runtime_error("Unterminated device name");
        const auto extensions = enumerate<VkExtensionProperties>([&](auto count, auto values) { return enumDevice(device, nullptr, count, values); }, 1024, "Device extensions");
        auto id = structure<VkPhysicalDevicePresentIdFeaturesKHR>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR);
        auto wait = structure<VkPhysicalDevicePresentWaitFeaturesKHR>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR);
        auto id2 = structure<VkPhysicalDevicePresentId2FeaturesKHR>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR);
        auto wait2 = structure<VkPhysicalDevicePresentWait2FeaturesKHR>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_2_FEATURES_KHR);
        auto maintenance = structure<VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR);
        const bool hasId = has(extensions, VK_KHR_PRESENT_ID_EXTENSION_NAME), hasWait = has(extensions, VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
        const bool hasId2 = has(extensions, VK_KHR_PRESENT_ID_2_EXTENSION_NAME), hasWait2 = has(extensions, VK_KHR_PRESENT_WAIT_2_EXTENSION_NAME);
        const bool maintenanceKhr = has(extensions, VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);
        const bool maintenanceExt = has(extensions, VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);
        auto features = structure<VkPhysicalDeviceFeatures2>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2);
        const auto add = [&](auto& value, bool advertised) { if (advertised) { value.pNext = features.pNext; features.pNext = &value; } };
        add(id, hasId); add(wait, hasWait); add(id2, hasId2); add(wait2, hasWait2); add(maintenance, maintenanceKhr || maintenanceExt);
        getFeatures(device, &features);
        if (!first) out << ',';
        first = false;
        out << "{\"name\":" << quoted(properties.properties.deviceName) << ",\"vendor_id\":" << properties.properties.vendorID
            << ",\"device_id\":" << properties.properties.deviceID << ",\"api_version\":" << properties.properties.apiVersion
            << ",\"driver_version_raw\":" << properties.properties.driverVersion
            << ",\"device_luid_valid\":" << (identity.deviceLUIDValid ? "true" : "false") << ",\"device_luid_hex\":\"";
        for (auto byte : identity.deviceLUID) out << std::hex << std::setw(2) << std::setfill('0') << unsigned(byte);
        out << std::dec << "\",\"device_node_mask\":" << identity.deviceNodeMask << ",\"device_extensions\":";
        extensionList(out, extensions);
        feature(out, "present_id", hasId, id.presentId); feature(out, "present_wait", hasWait, wait.presentWait);
        feature(out, "present_id2", hasId2, id2.presentId2); feature(out, "present_wait2", hasWait2, wait2.presentWait2);
        feature(out, "swapchain_maintenance1", maintenanceKhr || maintenanceExt, maintenance.swapchainMaintenance1);
        out << ",\"swapchain_maintenance_khr\":" << (maintenanceKhr ? "true" : "false")
            << ",\"swapchain_maintenance_ext\":" << (maintenanceExt ? "true" : "false")
            << ",\"google_display_timing\":{\"advertised\":" << (has(extensions, VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME) ? "true" : "false")
            << ",\"feature_struct_required\":false,\"enabled\":false}"
            << ",\"surface_present_id2_support\":null,\"surface_present_wait2_support\":null"
            << ",\"surface_specific_capabilities_queried\":false,\"presentation_retirement_tested\":false}";
    }
    out << "],\"probe_succeeded\":true}";
    // Finish all own Vulkan cleanup before publishing successful probe output.
    owner.destroy(owner.instance, nullptr); owner.instance = VK_NULL_HANDLE;
    std::cout << out.str() << '\n';
    return 0;
}
} // namespace
int main() {
    try { return probe(); }
    catch (const std::exception& error) {
        std::cerr << "WSI capability probe failed: " << error.what() << '\n'; return 1;
    }
}
