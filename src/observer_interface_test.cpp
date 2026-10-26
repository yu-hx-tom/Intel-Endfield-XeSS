// Tests the DLL's loader-facing protocol only. No driver, instance, or device is
// created, and no Vulkan loader or game is involved.
#include <windows.h>
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using Negotiate = VkResult (VKAPI_PTR*)(VkNegotiateLayerInterface*);

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Library {
    HMODULE module{};
    explicit Library(const std::filesystem::path& path) {
        module = LoadLibraryExW(path.c_str(), nullptr,
                               LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) throw std::runtime_error("LoadLibraryExW failed: " + std::to_string(GetLastError()));
    }
    ~Library() { if (module) FreeLibrary(module); }
    Library(const Library&) = delete;
    Library& operator=(const Library&) = delete;
    template<class T> T symbol(const char* name) const {
        const auto address = GetProcAddress(module, name);
        if (!address) throw std::runtime_error(std::string("Missing DLL export: ") + name);
        return reinterpret_cast<T>(address);
    }
};

void testInterface(const Library& library) {
    const auto negotiate = library.symbol<Negotiate>("vkNegotiateLoaderLayerInterfaceVersion");
    const auto exportedGipa = library.symbol<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
    const auto exportedGdpa = library.symbol<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");
    const auto exportedGpdpa = library.symbol<PFN_GetPhysicalDeviceProcAddr>("vk_layerGetPhysicalDeviceProcAddr");
    const auto exportedCreate = library.symbol<PFN_vkCreateInstance>("vkCreateInstance");

    VkNegotiateLayerInterface info{};
    info.sType = LAYER_NEGOTIATE_INTERFACE_STRUCT;
    info.loaderLayerInterfaceVersion = 2;
    require(negotiate(&info) == VK_SUCCESS, "Interface v2 negotiation failed");
    require(info.loaderLayerInterfaceVersion == 2, "Negotiated interface version is not 2");
    require(info.pfnGetInstanceProcAddr == exportedGipa, "Negotiated GIPA differs from export");
    require(info.pfnGetDeviceProcAddr == exportedGdpa, "Negotiated GDPA differs from export");
    require(info.pfnGetPhysicalDeviceProcAddr == exportedGpdpa, "Negotiated GPDPA differs from export");

    struct Entry { const char* name; PFN_vkVoidFunction expected; };
    const std::array<Entry, 4> entries{{
        {"vkCreateInstance", reinterpret_cast<PFN_vkVoidFunction>(exportedCreate)},
        {"vkGetInstanceProcAddr", reinterpret_cast<PFN_vkVoidFunction>(exportedGipa)},
        {"vkGetDeviceProcAddr", reinterpret_cast<PFN_vkVoidFunction>(exportedGdpa)},
        {"vk_layerGetPhysicalDeviceProcAddr", reinterpret_cast<PFN_vkVoidFunction>(exportedGpdpa)},
    }};

    // Reserved inaccessible memory makes an accidental dispatch-key read fail.
    // Only the loader-global names above are queried with this not-yet-created
    // instance. Unknown commands with an invalid non-null instance are invalid
    // Vulkan use and intentionally are not tested that way.
    void* inaccessible = VirtualAlloc(nullptr, 4096, MEM_RESERVE, PAGE_NOACCESS);
    require(inaccessible != nullptr, "Cannot reserve inaccessible test handle");
    struct Reservation { void* address; ~Reservation() { VirtualFree(address, 0, MEM_RELEASE); } } guard{inaccessible};
    const std::array<VkInstance, 2> handles{VK_NULL_HANDLE, reinterpret_cast<VkInstance>(inaccessible)};
    for (const auto handle : handles) {
        for (const auto& entry : entries)
            require(info.pfnGetInstanceProcAddr(handle, entry.name) == entry.expected,
                    "GIPA loader-global lookup differs from DLL/negotiated callback");
    }

    require(info.pfnGetInstanceProcAddr(VK_NULL_HANDLE, "vkObserverDefinitelyUnknown") == nullptr,
            "Unknown global GIPA lookup must return null");
    require(info.pfnGetInstanceProcAddr(VK_NULL_HANDLE, nullptr) == nullptr,
            "Null-name GIPA lookup must return null");
    require(info.pfnGetDeviceProcAddr(VK_NULL_HANDLE, "vkObserverDefinitelyUnknown") == nullptr,
            "Unknown null-device GDPA lookup must return null");
    require(info.pfnGetPhysicalDeviceProcAddr(VK_NULL_HANDLE, "vkObserverDefinitelyUnknown") == nullptr,
            "Unknown null-instance GPDPA lookup must return null");

    for (const uint32_t version : {0u, 1u}) {
        VkNegotiateLayerInterface unsupported{};
        unsupported.sType = LAYER_NEGOTIATE_INTERFACE_STRUCT;
        unsupported.loaderLayerInterfaceVersion = version;
        require(negotiate(&unsupported) == VK_ERROR_INITIALIZATION_FAILED,
                "Unsupported old interface version was not rejected");
    }
    VkNegotiateLayerInterface newer{};
    newer.sType = LAYER_NEGOTIATE_INTERFACE_STRUCT;
    newer.loaderLayerInterfaceVersion = 3;
    require(negotiate(&newer) == VK_SUCCESS && newer.loaderLayerInterfaceVersion == 2,
            "Higher maximum interface version did not negotiate down to 2");
    require(negotiate(nullptr) == VK_ERROR_INITIALIZATION_FAILED, "Null negotiation input was not rejected");
    VkNegotiateLayerInterface malformed{};
    malformed.loaderLayerInterfaceVersion = 2;
    require(negotiate(&malformed) == VK_ERROR_INITIALIZATION_FAILED, "Invalid negotiation sType was not rejected");
}

} // namespace

int main(int argc, char** argv) {
    try {
        const bool minimal = argc == 4 && std::strcmp(argv[3], "--release-minimal") == 0;
        if ((argc != 3 && !minimal) || std::strcmp(argv[1], "--layer") != 0)
            throw std::runtime_error("Usage: xess-observer-interface-test --layer <absolute DLL path> [--release-minimal]");
        const auto path = std::filesystem::path(reinterpret_cast<const char8_t*>(argv[2]));
        require(path.is_absolute(), "--layer must be an absolute DLL path");
        const Library library(path);
        testInterface(library);
        if (minimal) {
            using Capability = uint32_t(VKAPI_PTR*)();
            require(library.symbol<Capability>("xessObserverMinimalReleaseV1")() == 1,"Expected minimal release DLL");
            require(library.symbol<Capability>("xessObserverCpuDiagnosticsV1")() == 0,"CPU diagnostics must be compiled out");
            require(library.symbol<Capability>("xessObserverResearchDiagnosticsV1")() == 0,"Research diagnostics must be compiled out");
        }
        std::cout << "{\"passed\":true,\"negotiated_interface_version\":2,"
                     "\"global_lookup_checks\":8,\"game_integration_tested\":false}\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Observer interface test failed: " << error.what() << '\n';
        return 1;
    }
}
