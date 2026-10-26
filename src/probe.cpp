#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <vulkan/vulkan.h>
#include <xess_fg/xefg_swapchain_d3d12.h>
#include <xell/xell_d3d12.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// Small JSON writer: diagnostics use no third-party serialization runtime.
namespace json {
std::string str(const std::string& s) {
    std::ostringstream o;
    o << '"';
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') o << '\\' << c;
        else if (c < 32) {
            const char* h = "0123456789abcdef";
            o << "\\u00" << h[c >> 4] << h[c & 15];
        } else o << c;
    }
    o << '"';
    return o.str();
}
std::string boolValue(bool v) { return v ? "true" : "false"; }
template<class T> std::string number(T v) { return std::to_string(v); }
using Fields = std::vector<std::pair<std::string, std::string>>;
std::string object(const Fields& fields) {
    std::string out = "{";
    for (const auto& [key, value] : fields) {
        if (out.size() > 1) out += ',';
        out += str(key) + ':' + value;
    }
    return out + '}';
}
std::string array(const std::vector<std::string>& entries) {
    std::string out = "[";
    for (const auto& entry : entries) {
        if (out.size() > 1) out += ',';
        out += entry;
    }
    return out + ']';
}
}

template<class T> struct Com {
    T* ptr = nullptr;
    ~Com() { if (ptr) ptr->Release(); }
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    T* operator->() const { return ptr; }
};
struct Library {
    HMODULE handle = nullptr;
    explicit Library(const std::filesystem::path& path, DWORD flags) {
        handle = LoadLibraryExW(path.c_str(), nullptr, flags);
        if (!handle) throw std::runtime_error("LoadLibrary failed: " + path.string() + " (Win32 " + std::to_string(GetLastError()) + ")");
    }
    ~Library() { if (handle) FreeLibrary(handle); }
    Library(const Library&) = delete;
    template<class T> T get(const char* name) const {
        const auto p = GetProcAddress(handle, name);
        if (!p) throw std::runtime_error(std::string("Missing export: ") + name);
        return reinterpret_cast<T>(p);
    }
};
std::string utf8(const wchar_t* s) {
    int size = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return {};
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), size, nullptr, nullptr);
    out.pop_back();
    return out;
}
std::string hexBytes(const void* data, size_t n) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    const char* h = "0123456789abcdef";
    std::string out;
    for (size_t i = 0; i < n; ++i) { out += h[bytes[i] >> 4]; out += h[bytes[i] & 15]; }
    return out;
}
std::string version(uint32_t v) {
    return std::to_string(VK_API_VERSION_MAJOR(v)) + "." + std::to_string(VK_API_VERSION_MINOR(v)) + "." + std::to_string(VK_API_VERSION_PATCH(v));
}
template<class T> std::string sdkVersion(const T& v) {
    return std::to_string(v.major) + "." + std::to_string(v.minor) + "." + std::to_string(v.patch);
}
void vkCheck(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) throw std::runtime_error(std::string(operation) + " failed: " + std::to_string(result));
}

struct Vulkan {
    Library loader{L"vulkan-1.dll", LOAD_LIBRARY_SEARCH_SYSTEM32};
    PFN_vkGetInstanceProcAddr get = loader.get<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
    VkInstance instance = VK_NULL_HANDLE;
    uint32_t loaderVersion = VK_API_VERSION_1_0;
    template<class T> T fn(const char* name) const {
        const auto p = get(instance, name);
        if (!p) throw std::runtime_error(std::string("Missing Vulkan function: ") + name);
        return reinterpret_cast<T>(p);
    }
    Vulkan() {
        auto enumVersion = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(get(nullptr, "vkEnumerateInstanceVersion"));
        if (enumVersion) vkCheck(enumVersion(&loaderVersion), "vkEnumerateInstanceVersion");
        if (loaderVersion < VK_API_VERSION_1_2) throw std::runtime_error("Prototype requires Vulkan 1.2 timeline semaphore support.");
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "Endfield XeSS Vulkan capability probe";
        app.apiVersion = VK_API_VERSION_1_2;
        VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ci.pApplicationInfo = &app;
        vkCheck(fn<PFN_vkCreateInstance>("vkCreateInstance")(&ci, nullptr, &instance), "vkCreateInstance");
    }
    ~Vulkan() {
        if (instance) reinterpret_cast<PFN_vkDestroyInstance>(get(instance, "vkDestroyInstance"))(instance, nullptr);
    }
};

std::string probeXeFG(ID3D12Device* device, const std::filesystem::path& sdkBin, bool& accepted) {
    using namespace json;
    Fields fields;
    accepted = false;
    try {
        // An absolute directory and restricted dependency search avoid loading game/local DLLs.
        const DWORD flags = LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32;
        Library xell(sdkBin / "libxell.dll", flags);
        Library fg(sdkBin / "libxess_fg.dll", flags);
        auto create = fg.get<decltype(&xefgSwapChainD3D12CreateContext)>("xefgSwapChainD3D12CreateContext");
        auto properties = fg.get<decltype(&xefgSwapChainGetProperties)>("xefgSwapChainGetProperties");
        auto destroy = fg.get<decltype(&xefgSwapChainDestroy)>("xefgSwapChainDestroy");
        auto getVersion = fg.get<decltype(&xefgSwapChainGetVersion)>("xefgSwapChainGetVersion");
        auto createLL = xell.get<decltype(&xellD3D12CreateContext)>("xellD3D12CreateContext");
        auto destroyLL = xell.get<decltype(&xellDestroyContext)>("xellDestroyContext");
        auto versionLL = xell.get<decltype(&xellGetVersion)>("xellGetVersion");
        xefg_swapchain_version_t fv{};
        xell_version_t lv{};
        const auto fvResult = getVersion(&fv);
        const auto lvResult = versionLL(&lv);
        fields.emplace_back("versionResult", number(fvResult));
        fields.emplace_back("runtimeVersion", str(sdkVersion(fv)));
        fields.emplace_back("xellVersionResult", number(lvResult));
        fields.emplace_back("xellRuntimeVersion", str(sdkVersion(lv)));
        xefg_swapchain_handle_t ctx = nullptr;
        const auto cr = create(device, &ctx);
        fields.emplace_back("createContextResult", number(cr));
        if (cr >= 0 && ctx) {
            xefg_swapchain_properties_t props{};
            const auto pr = properties(ctx, &props);
            fields.emplace_back("getPropertiesResult", number(pr));
            fields.emplace_back("maxSupportedInterpolations", pr >= 0 ? number(props.maxSupportedInterpolations) : "null");
            fields.emplace_back("maxOutputMultiplier", pr >= 0 ? number(props.maxSupportedInterpolations + 1) : "null");
            fields.emplace_back("hasWarning", boolValue(cr > 0 || pr > 0));
            xell_context_handle_t ll = nullptr;
            const auto lr = createLL(device, &ll);
            fields.emplace_back("xellCreateContextResult", number(lr));
            // Probe does not initialize a swapchain or bind XeLL; full integration is a later stage.
            accepted = pr >= 0 && props.maxSupportedInterpolations >= 1 && lr == XELL_RESULT_SUCCESS && ll;
            const auto dr = destroy(ctx);
            fields.emplace_back("destroyContextResult", number(dr));
            const auto ldr = ll ? destroyLL(ll) : XELL_RESULT_SUCCESS;
            if (ll) fields.emplace_back("xellDestroyContextResult", number(ldr));
            const bool cleanupSucceeded = dr >= 0 && ldr == XELL_RESULT_SUCCESS;
            fields.emplace_back("cleanupSucceeded", boolValue(cleanupSucceeded));
            accepted = accepted && cleanupSucceeded;
        }
    } catch (const std::exception& e) { accepted = false; fields.emplace_back("error", str(e.what())); }
    fields.emplace_back("contextCapabilityPassed", boolValue(accepted));
    fields.emplace_back("generatedFramesVerified", "false");
    return object(fields);
}

int main(int argc, char** argv) {
    using namespace json;
    if (argc != 3 || std::string(argv[1]) != "--sdk-bin") {
        std::cerr << "Usage: xess-vulkan-probe --sdk-bin <absolute path to trusted XeSS SDK bin>\n";
        return 1;
    }
    Fields report{{"schemaVersion", "1"}, {"stage", str("capability-probe")}, {"gameIntegrationTested", "false"}, {"generatedFramesVerified", "false"}};
    try {
        const auto sdkBin = std::filesystem::absolute(argv[2]);
        Vulkan vk;
        report.emplace_back("vulkanLoaderVersion", str(version(vk.loaderVersion)));
        Com<IDXGIFactory1> factory;
        HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory.ptr));
        if (FAILED(hr)) throw std::runtime_error("CreateDXGIFactory1 failed: " + std::to_string(hr));
        auto enumPhysical = vk.fn<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices");
        uint32_t count = 0;
        vkCheck(enumPhysical(vk.instance, &count, nullptr), "vkEnumeratePhysicalDevices(count)");
        std::vector<VkPhysicalDevice> devices(count);
        vkCheck(enumPhysical(vk.instance, &count, devices.data()), "vkEnumeratePhysicalDevices");
        std::vector<std::string> deviceReports;
        bool anyPassed = false;
        for (auto device : devices) {
            VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
            VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
            id.pNext = &driver;
            VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id};
            vk.fn<PFN_vkGetPhysicalDeviceProperties2>("vkGetPhysicalDeviceProperties2")(device, &props);
            Fields entry{{"name", str(props.properties.deviceName)}, {"vendorId", number(props.properties.vendorID)}, {"deviceId", number(props.properties.deviceID)}, {"apiVersion", str(version(props.properties.apiVersion))}, {"driverInfo", str(driver.driverInfo)}, {"deviceLuidValid", boolValue(id.deviceLUIDValid)}, {"deviceLuidHex", str(hexBytes(id.deviceLUID, VK_LUID_SIZE))}, {"deviceNodeMask", number(id.deviceNodeMask)}};
            VkPhysicalDeviceTimelineSemaphoreFeatures timeline{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
            VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &timeline};
            vk.fn<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2")(device, &features);
            entry.emplace_back("timelineSemaphore", boolValue(timeline.timelineSemaphore));
            auto enumExt = vk.fn<PFN_vkEnumerateDeviceExtensionProperties>("vkEnumerateDeviceExtensionProperties");
            uint32_t ec = 0;
            vkCheck(enumExt(device, nullptr, &ec, nullptr), "vkEnumerateDeviceExtensionProperties(count)");
            std::vector<VkExtensionProperties> extensions(ec);
            vkCheck(enumExt(device, nullptr, &ec, extensions.data()), "vkEnumerateDeviceExtensionProperties");
            auto has = [&](const char* name) { return std::any_of(extensions.begin(), extensions.end(), [&](const auto& e) { return std::strcmp(e.extensionName, name) == 0; }); };
            const bool memoryExt = has(VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME);
            const bool semaphoreExt = has(VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME);
            entry.emplace_back("externalMemoryWin32", boolValue(memoryExt));
            entry.emplace_back("externalSemaphoreWin32", boolValue(semaphoreExt));
            std::vector<std::string> formats;
            bool formatsPassed = true;
            for (const auto& [name, format] : std::vector<std::pair<const char*, VkFormat>>{{"color-rgba8", VK_FORMAT_R8G8B8A8_UNORM}, {"depth-r32f", VK_FORMAT_R32_SFLOAT}, {"motion-rg16f", VK_FORMAT_R16G16_SFLOAT}}) {
                VkPhysicalDeviceExternalImageFormatInfo extInfo{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
                extInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
                VkPhysicalDeviceImageFormatInfo2 info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, &extInfo};
                info.format = format; info.type = VK_IMAGE_TYPE_2D; info.tiling = VK_IMAGE_TILING_OPTIMAL;
                info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
                VkExternalImageFormatProperties extProps{VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
                VkImageFormatProperties2 ip{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, &extProps};
                const auto result = vk.fn<PFN_vkGetPhysicalDeviceImageFormatProperties2>("vkGetPhysicalDeviceImageFormatProperties2")(device, &info, &ip);
                const auto bits = extProps.externalMemoryProperties.externalMemoryFeatures;
                bool importable = result == VK_SUCCESS && (bits & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT);
                formatsPassed = formatsPassed && importable;
                formats.push_back(object({{"role", str(name)}, {"vkFormat", number(format)}, {"usage", number(info.usage)}, {"result", number(result)}, {"importable", boolValue(importable)}, {"exportable", boolValue(bits & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT)}, {"dedicatedOnly", boolValue(bits & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT)}, {"compatibleHandleTypes", number(extProps.externalMemoryProperties.compatibleHandleTypes)}}));
            }
            entry.emplace_back("d3d12ResourceFormats", array(formats));
            VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
            type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
            VkPhysicalDeviceExternalSemaphoreInfo si{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO, &type};
            si.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
            VkExternalSemaphoreProperties sp{VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
            vk.fn<PFN_vkGetPhysicalDeviceExternalSemaphoreProperties>("vkGetPhysicalDeviceExternalSemaphoreProperties")(device, &si, &sp);
            const bool fenceImport = sp.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT;
            entry.emplace_back("d3d12Fence", object({{"timeline", "true"}, {"importable", boolValue(fenceImport)}, {"exportable", boolValue(sp.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT)}, {"compatibleHandleTypes", number(sp.compatibleHandleTypes)}}));
            bool matched = false, fgAccepted = false;
            if (id.deviceLUIDValid) {
                for (UINT index = 0;; ++index) {
                    Com<IDXGIAdapter1> adapter;
                    hr = factory->EnumAdapters1(index, &adapter.ptr);
                    if (hr == DXGI_ERROR_NOT_FOUND) break;
                    if (FAILED(hr)) throw std::runtime_error("EnumAdapters1 failed");
                    DXGI_ADAPTER_DESC1 desc{};
                    hr = adapter->GetDesc1(&desc);
                    if (FAILED(hr)) throw std::runtime_error("GetDesc1 failed");
                    if (std::memcmp(&desc.AdapterLuid, id.deviceLUID, VK_LUID_SIZE) != 0) continue;
                    matched = true;
                    entry.emplace_back("dxgiAdapter", str(utf8(desc.Description)));
                    Com<ID3D12Device> d3d;
                    hr = D3D12CreateDevice(adapter.ptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&d3d.ptr));
                    entry.emplace_back("d3d12CreateDeviceResult", number(hr));
                    if (SUCCEEDED(hr)) entry.emplace_back("xefg", probeXeFG(d3d.ptr, sdkBin, fgAccepted));
                    break;
                }
            }
            const bool passed = matched && memoryExt && semaphoreExt && timeline.timelineSemaphore && formatsPassed && fenceImport && fgAccepted;
            entry.emplace_back("sameAdapterMatched", boolValue(matched));
            entry.emplace_back("capabilityGatePassed", boolValue(passed));
            entry.emplace_back("sharedResourceTransferTested", "false");
            anyPassed = anyPassed || passed;
            deviceReports.push_back(object(entry));
        }
        report.emplace_back("devices", array(deviceReports));
        report.emplace_back("capabilityGatePassed", boolValue(anyPassed));
        report.emplace_back("nextStage", str("Import shared textures and fences, then verify GPU-produced pixel data before implementing a XeFG presenter."));
        std::cout << object(report) << '\n';
        return anyPassed ? 0 : 2;
    } catch (const std::exception& e) {
        report.emplace_back("error", str(e.what()));
        report.emplace_back("capabilityGatePassed", "false");
        std::cout << object(report) << '\n';
        return 1;
    }
}
