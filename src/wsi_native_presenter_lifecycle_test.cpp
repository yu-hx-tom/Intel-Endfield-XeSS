// Self-owned WSI lifecycle experiment. Never loaded into or applied to a game.
// Genuine hidden VkSwapchain presents are forwarded for BOTH synthetic roles;
// only the explicitly labelled synthetic Real source enters the XeFG presenter.
#define XESS_PRESENTER_EMBEDDED
#include "presenter.cpp"
#include "native_source_fixture.hpp"
#include "wsi_retirement.hpp"
#include <memory>
#include <sstream>

namespace {
using xess_vk::testing::check;
constexpr uint64_t WsiTimeoutNs = 3'000'000'000;
enum class SyntheticRole { Real, Generated };
struct ForwardedFrame {
    SyntheticRole role;
    uint64_t sequence;
    VkResult aggregate;
    VkResult perSwapchain;
};
bool presentAccepted(VkResult value) { return value == VK_SUCCESS || value == VK_SUBOPTIMAL_KHR; }

// No Vulkan loader/device, window or SDK. Uses the same bounded multi-generation
// helper as the GPU path; the standalone helper test covers the full negatives.
int selfTestRetirement() {
    using Ledger=xess_vk::testing::WsiRetirement;
    Ledger ledger;unsigned checks=0;
    auto require=[&](bool value){++checks;if(!value)throw std::runtime_error("Retirement self-test assertion failed");};
    auto rejects=[&](auto action){bool failed=false;try{action();}catch(const std::logic_error&){failed=true;}require(failed);};
    auto acquire=[&](uint32_t image){ledger.acquiredAfterFence(ledger.current(),image,ledger.stats().acquires+1,VK_SUCCESS,VK_SUCCESS);};
    auto present=[&](uint32_t image){ledger.presented(ledger.current(),image,ledger.stats().presents+1,VK_SUCCESS,VK_SUCCESS);};
    ledger.created(3);rejects([&]{present(0);});
    for(uint32_t n=0;n<303;++n){acquire(n%3);present(n%3);require(ledger.conserved());}
    require(ledger.stats().retiredByAcquire==300&&ledger.stats().pending==3);
    ledger.retiringForReplacement();
    rejects([&]{ledger.acquiredAfterFence(1,0,304,VK_SUCCESS,VK_SUCCESS);});
    rejects([&]{ledger.destroyed(1);}); // Failed creation would stop right here.
    ledger.created(3);acquire(1);present(1);acquire(0);present(0);require(!ledger.canDestroy(1));
    rejects([&]{ledger.acquiredAfterFence(2,1,306,VK_SUCCESS,VK_TIMEOUT);});require(!ledger.canDestroy(1));
    acquire(1);require(ledger.canDestroy(1));present(1);ledger.destroyed(1);
    require(ledger.stats().retiredBySuccessor==3&&ledger.stats().destroyedGenerations==1&&ledger.conserved());
    ledger.retiringForReplacement();ledger.created(3);ledger.retiringForReplacement();ledger.created(3);
    acquire(2);present(2);require(!ledger.canDestroy(2));acquire(2);present(2);
    require(ledger.canDestroy(2)&&ledger.canDestroy(3));ledger.destroyed(2);ledger.destroyed(3);
    std::cout<<"{\"stage\":\"cpu-only-acquire-retirement-self-test\",\"testPassed\":true,\"checks\":"<<checks
        <<",\"generations\":"<<ledger.stats().generations<<",\"syntheticPresents\":"<<ledger.stats().presents
        <<",\"syntheticRetiredByAcquire\":"<<ledger.stats().retiredByAcquire
        <<",\"syntheticRetiredBySuccessor\":"<<ledger.stats().retiredBySuccessor
        <<",\"syntheticPending\":"<<ledger.stats().pending
        <<",\"createsWindows\":false,\"createsVulkanDevice\":false,\"loadsVulkanLoader\":false,\"gameTouched\":false}\n";
    return 0;
}
bool hasExtension(const std::vector<VkExtensionProperties>& extensions, const char* name) {
    return std::any_of(extensions.begin(), extensions.end(), [name](const auto& extension) {
        return std::strcmp(extension.extensionName, name) == 0;
    });
}

// Read-only capability mode: no HWND/surface, VkDevice, queues, SDK DLLs or
// rendering. A temporary self-owned VkInstance only queries driver properties.
int probeMaintenanceSupport() {
    xess_vk::testing::SourceOwner owner;
    owner.loader = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!owner.loader) throw std::runtime_error("No system Vulkan loader");
    owner.gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(owner.loader, "vkGetInstanceProcAddr"));
    if (!owner.gipa) throw std::runtime_error("Missing vkGetInstanceProcAddr");
    uint32_t count = 0;
    const auto instanceExtensionList = owner.inst<PFN_vkEnumerateInstanceExtensionProperties>("vkEnumerateInstanceExtensionProperties");
    check(instanceExtensionList(nullptr, &count, nullptr), "Probe instance extension count");
    std::vector<VkExtensionProperties> instanceExtensions(count);
    check(instanceExtensionList(nullptr, &count, instanceExtensions.data()), "Probe instance extensions");
    const bool surfaceKhr = hasExtension(instanceExtensions, VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
    const bool surfaceExt = hasExtension(instanceExtensions, VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
    owner.application.apiVersion = VK_API_VERSION_1_3;
    owner.application.pApplicationName = "Read-only WSI maintenance inventory";
    owner.instanceInfo.pApplicationInfo = &owner.application;
    check(owner.inst<PFN_vkCreateInstance>("vkCreateInstance")(&owner.instanceInfo, nullptr, &owner.instance), "Probe vkCreateInstance");
    check(owner.inst<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices")(owner.instance, &count, nullptr), "Probe device count");
    std::vector<VkPhysicalDevice> devices(count);
    check(owner.inst<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices")(owner.instance, &count, devices.data()), "Probe devices");
    std::ostringstream result;
    result << "{\"stage\":\"read-only-wsi-maintenance-probe\",\"createsWindows\":false,\"createsVulkanDevice\":false,\"gameTouched\":false"
        << ",\"surfaceMaintenanceKHR\":" << (surfaceKhr ? "true" : "false")
        << ",\"surfaceMaintenanceEXT\":" << (surfaceExt ? "true" : "false")
        << ",\"getSurfaceCapabilities2\":" << (hasExtension(instanceExtensions, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME) ? "true" : "false")
        << ",\"devices\":[";
    bool first = true;
    for (auto device : devices) {
        VkPhysicalDeviceProperties properties{};
        owner.inst<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties")(device, &properties);
        const auto deviceExtensionList = owner.inst<PFN_vkEnumerateDeviceExtensionProperties>("vkEnumerateDeviceExtensionProperties");
        check(deviceExtensionList(device, nullptr, &count, nullptr), "Probe device extension count");
        std::vector<VkExtensionProperties> extensions(count);
        check(deviceExtensionList(device, nullptr, &count, extensions.data()), "Probe device extensions");
        const bool swapKhr = hasExtension(extensions, VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);
        const bool swapExt = hasExtension(extensions, VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);
        VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR maintenance{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR};
        if (swapKhr || swapExt) {
            VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2}; features.pNext = &maintenance;
            owner.inst<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2")(device, &features);
        }
        if (!first) result << ',';
        first = false;
        result << "{\"name\":" << quote(properties.deviceName) << ",\"vendorId\":" << properties.vendorID
            << ",\"swapchainMaintenanceKHR\":" << (swapKhr ? "true" : "false")
            << ",\"swapchainMaintenanceEXT\":" << (swapExt ? "true" : "false")
            << ",\"swapchainMaintenanceFeature\":" << (maintenance.swapchainMaintenance1 ? "true" : "false")
            << ",\"completeKHRPair\":" << (surfaceKhr && swapKhr && maintenance.swapchainMaintenance1 ? "true" : "false")
            << ",\"completeEXTPair\":" << (surfaceExt && swapExt && maintenance.swapchainMaintenance1 ? "true" : "false") << '}';
    }
    result << "]}";
    std::cout << result.str() << '\n';
    return 0;
}

struct LifecycleWatchdog {
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE thread = event ? CreateThread(nullptr, 0, [](void* value) -> DWORD {
        if (WaitForSingleObject(static_cast<HANDLE>(value), 90000) == WAIT_TIMEOUT)
            TerminateProcess(GetCurrentProcess(), 124);
        return 0;
    }, event, 0, nullptr) : nullptr;
    LifecycleWatchdog() {
        if (!event || !thread) { if (event) CloseHandle(event); throw std::runtime_error("Cannot create test watchdog"); }
    }
    ~LifecycleWatchdog() {
        SetEvent(event);
        if (WaitForSingleObject(thread, 3000) != WAIT_OBJECT_0) TerminateProcess(GetCurrentProcess(), 125);
        CloseHandle(thread); CloseHandle(event);
    }
};

// Reuses SourceOwner's tested finite waits and owned native-image lifetime.
// This independent initializer additionally enables actual Win32 WSI. The
// successful creation records and their referenced arrays stay alive in *this.
struct WsiSource : xess_vk::testing::SourceOwner {
    struct RetiredGeneration {
        uint64_t serial = 0;
        VkSwapchainKHR chain = VK_NULL_HANDLE;
        std::vector<VkSemaphore> semaphores;
        bool renderWorkDrained = false;
    };
    HWND hiddenWindow = nullptr;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkSemaphore acquired = VK_NULL_HANDLE;
    VkFence acquireComplete = VK_NULL_HANDLE;
    VkFence presentationComplete = VK_NULL_HANDLE;
    std::vector<VkImage> swapImages;
    std::vector<VkSemaphore> presentReady;
    std::vector<bool> presentedBefore;
    std::vector<uint32_t> perImageAcquires;
    VkFormat swapFormat = VK_FORMAT_UNDEFINED;
    const char* instanceExtensions[4]{VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
        VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME, VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME};
    const char* wsiDeviceExtensions[4]{VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME, VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME};
    VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT maintenance{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT};
    xess_vk::testing::WsiRetirement retirement;
    std::array<RetiredGeneration,xess_vk::testing::WsiRetirement::maxGenerations> oldGenerations;
    VkExtent2D swapExtent{};
    uint64_t oldGenerationsDestroyed = 0, oldPresentSemaphoresDestroyed = 0, imagesReacquiredPriorGenerations = 0;
    uint64_t sourceImagesDestroyed = 0, sourceAllocationsFreed = 0, retainedPresentSemaphores = 0;
    bool renderResourcesCleanupCompleted = false;
    uint64_t acquiredCount = 0, forwardedCount = 0, realPresents = 0, generatedPresents = 0;
    uint64_t acceptedSuboptimal = 0, realPublished = 0, generatedRejected = 0;
    uint64_t publishedSequence = 0;
    uint64_t presentationFenceCompletions = 0;
    uint64_t acquireFenceCompletions = 0;
    SyntheticRole latestRole = SyntheticRole::Generated;
    bool cleanupCompleted = false;
    bool retireViaAcquire = false;
    bool wsiRetainedUntilProcessExit = false;
    bool renderQueueDrained = false;

    void initializeWsi(const xess_vk::InteropConfig& config, bool useAcquireRetirement) {
        cfg = config;
        retireViaAcquire = useAcquireRetirement;
        // On any partial-init or runtime exception retain the self-owned objects
        // until process exit. Never release an acquired image by faking Present.
        retainForPossibleBorrowedChildren = true;
        WNDCLASSW wc{}; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"XeSSSelfOwnedShadowWsi";
        if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            throw std::runtime_error("Register shadow window class");
        RECT rect{0, 0, LONG(cfg.width), LONG(cfg.height)};
        const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
        if (!AdjustWindowRect(&rect, style, FALSE)) throw std::runtime_error("AdjustWindowRect");
        hiddenWindow = CreateWindowExW(WS_EX_NOACTIVATE, wc.lpszClassName, L"Private Vulkan drain window",
            style, CW_USEDEFAULT, CW_USEDEFAULT, rect.right-rect.left, rect.bottom-rect.top,
            nullptr, nullptr, wc.hInstance, nullptr);
        if (!hiddenWindow || IsWindowVisible(hiddenWindow)) throw std::runtime_error("Hidden HWND unavailable");
        loader = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!loader) throw std::runtime_error("No system Vulkan loader");
        gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader, "vkGetInstanceProcAddr"));
        if (!gipa) throw std::runtime_error("Missing vkGetInstanceProcAddr");
        application.pApplicationName = "Self-owned WSI real/generated XeFG boundary";
        application.apiVersion = VK_API_VERSION_1_3;
        instanceInfo.pApplicationInfo = &application;
        uint32_t instanceExtCount = 0;
        const auto instanceExtensionList = inst<PFN_vkEnumerateInstanceExtensionProperties>("vkEnumerateInstanceExtensionProperties");
        check(instanceExtensionList(nullptr, &instanceExtCount, nullptr), "Instance extension count");
        std::vector<VkExtensionProperties> instanceExts(instanceExtCount);
        check(instanceExtensionList(nullptr, &instanceExtCount, instanceExts.data()), "Instance extensions");
        const uint32_t instanceExtensionCount = retireViaAcquire ? 2 : 4;
        for (uint32_t i = 0; i < instanceExtensionCount; ++i) {
            const char* required = instanceExtensions[i];
            if (std::none_of(instanceExts.begin(), instanceExts.end(), [required](const auto& ext) { return std::strcmp(ext.extensionName, required) == 0; }))
                throw std::runtime_error(std::string("Presentation lifetime test requires instance extension: ") + required);
        }
        instanceInfo.enabledExtensionCount = instanceExtensionCount; instanceInfo.ppEnabledExtensionNames = instanceExtensions;
        check(inst<PFN_vkCreateInstance>("vkCreateInstance")(&instanceInfo, nullptr, &instance), "vkCreateInstance");
        VkWin32SurfaceCreateInfoKHR surfaceInfo{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        surfaceInfo.hinstance = wc.hInstance; surfaceInfo.hwnd = hiddenWindow;
        check(inst<PFN_vkCreateWin32SurfaceKHR>("vkCreateWin32SurfaceKHR")(instance, &surfaceInfo, nullptr, &surface), "vkCreateWin32SurfaceKHR");
        uint32_t count = 0;
        check(inst<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices")(instance, &count, nullptr), "Enumerate devices count");
        std::vector<VkPhysicalDevice> candidates(count);
        check(inst<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices")(instance, &count, candidates.data()), "Enumerate devices");
        for (auto candidate : candidates) {
            VkPhysicalDeviceProperties props{};
            inst<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties")(candidate, &props);
            if (props.vendorID != 0x8086) continue;
            uint32_t n = 0;
            inst<PFN_vkGetPhysicalDeviceQueueFamilyProperties>("vkGetPhysicalDeviceQueueFamilyProperties")(candidate, &n, nullptr);
            std::vector<VkQueueFamilyProperties> families(n);
            inst<PFN_vkGetPhysicalDeviceQueueFamilyProperties>("vkGetPhysicalDeviceQueueFamilyProperties")(candidate, &n, families.data());
            for (uint32_t i = 0; i < n; ++i) {
                VkBool32 present = VK_FALSE;
                check(inst<PFN_vkGetPhysicalDeviceSurfaceSupportKHR>("vkGetPhysicalDeviceSurfaceSupportKHR")(candidate, i, surface, &present), "Surface support");
                if (families[i].queueCount && (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
                    physical = candidate; family = i; break;
                }
            }
            if (physical) break;
        }
        if (!physical) throw std::runtime_error("No Intel graphics/present queue");
        uint32_t deviceExtCount = 0;
        const auto deviceExtensionList = inst<PFN_vkEnumerateDeviceExtensionProperties>("vkEnumerateDeviceExtensionProperties");
        check(deviceExtensionList(physical, nullptr, &deviceExtCount, nullptr), "Device extension count");
        std::vector<VkExtensionProperties> deviceExts(deviceExtCount);
        check(deviceExtensionList(physical, nullptr, &deviceExtCount, deviceExts.data()), "Device extensions");
        const uint32_t deviceExtensionCount = retireViaAcquire ? 3 : 4;
        for (uint32_t i = 0; i < deviceExtensionCount; ++i) {
            const char* required = wsiDeviceExtensions[i];
            if (std::none_of(deviceExts.begin(), deviceExts.end(), [required](const auto& ext) { return std::strcmp(ext.extensionName, required) == 0; }))
                throw std::runtime_error(std::string("Presentation lifetime test requires device extension: ") + required);
        }
        VkPhysicalDeviceFeatures2 available{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        available.pNext = retireViaAcquire ? static_cast<void*>(&timeline) : static_cast<void*>(&maintenance);
        maintenance.pNext = &timeline;
        inst<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2")(physical, &available);
        if ((!retireViaAcquire && !maintenance.swapchainMaintenance1) || !timeline.timelineSemaphore)
            throw std::runtime_error("Presentation fence or timeline semaphore feature unavailable");
        queueInfo.queueFamilyIndex = family; queueInfo.queueCount = 1; queueInfo.pQueuePriorities = &priority;
        timeline.timelineSemaphore = VK_TRUE;
        deviceInfo.pNext = retireViaAcquire ? static_cast<void*>(&timeline) : static_cast<void*>(&maintenance);
        deviceInfo.queueCreateInfoCount = 1; deviceInfo.pQueueCreateInfos = &queueInfo;
        deviceInfo.enabledExtensionCount = deviceExtensionCount; deviceInfo.ppEnabledExtensionNames = wsiDeviceExtensions;
        check(inst<PFN_vkCreateDevice>("vkCreateDevice")(physical, &deviceInfo, nullptr, &device), "vkCreateDevice(WSI/source)");
        gdpa = inst<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");
        dev<PFN_vkGetDeviceQueue>("vkGetDeviceQueue")(device, family, 0, &queue);
        inst<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(physical, &memory);
        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.queueFamilyIndex = family; poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        check(dev<PFN_vkCreateCommandPool>("vkCreateCommandPool")(device, &poolInfo, nullptr, &pool), "vkCreateCommandPool");
        VkCommandBufferAllocateInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cb.commandPool = pool; cb.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cb.commandBufferCount = 1;
        check(dev<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(device, &cb, &command), "vkAllocateCommandBuffers");
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        check(dev<PFN_vkCreateFence>("vkCreateFence")(device, &fi, nullptr, &completion), "vkCreateFence");
        frame.producerQueue = queue; frame.producerCompleteFence = completion;
        for (unsigned i = 0; i < frame.planes.size(); ++i) {
            auto& plane = frame.planes[i]; auto& info = plane.successfulImageCreateInfo;
            plane.device = device; plane.ownerQueueFamily = family; plane.currentLayout = VK_IMAGE_LAYOUT_GENERAL;
            info.imageType = VK_IMAGE_TYPE_2D; info.format = xess_vk::testing::kFormats[i]; info.extent = {cfg.width, cfg.height, 1};
            info.mipLevels = info.arrayLayers = 1; info.samples = VK_SAMPLE_COUNT_1_BIT; info.tiling = VK_IMAGE_TILING_OPTIMAL;
            info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            info.sharingMode = VK_SHARING_MODE_EXCLUSIVE; info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            check(dev<PFN_vkCreateImage>("vkCreateImage")(device, &info, nullptr, &plane.image), "vkCreateImage(source)");
            VkMemoryRequirements req{};
            dev<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(device, plane.image, &req);
            uint32_t type = UINT32_MAX;
            for (uint32_t j = 0; j < memory.memoryTypeCount; ++j)
                if ((req.memoryTypeBits & (1u << j)) && (memory.memoryTypes[j].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { type = j; break; }
            if (type == UINT32_MAX) throw std::runtime_error("No image memory type");
            VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; alloc.allocationSize = req.size; alloc.memoryTypeIndex = type;
            check(dev<PFN_vkAllocateMemory>("vkAllocateMemory")(device, &alloc, nullptr, &allocation[i]), "vkAllocateMemory(source)");
            check(dev<PFN_vkBindImageMemory>("vkBindImageMemory")(device, plane.image, allocation[i], 0), "vkBindImageMemory(source)");
        }
        createSwapchain(cfg.width,cfg.height);
    }

    void createSwapchain(uint32_t width,uint32_t height,bool replacement=false) {
        VkSurfaceCapabilitiesKHR caps{};
        check(inst<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>("vkGetPhysicalDeviceSurfaceCapabilitiesKHR")(physical, surface, &caps), "Surface capabilities");
        constexpr VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if ((caps.supportedUsageFlags & usage) != usage) throw std::runtime_error("Surface lacks transfer src/dst");
        auto extent = caps.currentExtent;
        if (extent.width == UINT32_MAX) extent = {width,height};
        if (extent.width != width || extent.height != height || width<cfg.width || height<cfg.height)
            throw std::runtime_error("Unexpected shadow client extent or undersized fixed copy region");
        uint32_t count = 0;
        check(inst<PFN_vkGetPhysicalDeviceSurfaceFormatsKHR>("vkGetPhysicalDeviceSurfaceFormatsKHR")(physical, surface, &count, nullptr), "Surface format count");
        std::vector<VkSurfaceFormatKHR> formats(count);
        check(inst<PFN_vkGetPhysicalDeviceSurfaceFormatsKHR>("vkGetPhysicalDeviceSurfaceFormatsKHR")(physical, surface, &count, formats.data()), "Surface formats");
        for (auto format : formats) {
            if (format.colorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) continue;
            if (format.format == VK_FORMAT_UNDEFINED || format.format == VK_FORMAT_R8G8B8A8_UNORM) { swapFormat = VK_FORMAT_R8G8B8A8_UNORM; break; }
            if (format.format == VK_FORMAT_B8G8R8A8_UNORM) swapFormat = format.format;
        }
        if (swapFormat == VK_FORMAT_UNDEFINED) throw std::runtime_error("No supported SDR RGBA/BGRA swapchain format");
        if (swapFormat != VK_FORMAT_R8G8B8A8_UNORM) {
            VkFormatProperties src{}, dst{};
            auto get = inst<PFN_vkGetPhysicalDeviceFormatProperties>("vkGetPhysicalDeviceFormatProperties");
            get(physical, swapFormat, &src); get(physical, VK_FORMAT_R8G8B8A8_UNORM, &dst);
            if (!(src.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) || !(dst.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT))
                throw std::runtime_error("BGRA-to-RGBA blit unsupported");
        }
        uint32_t n = caps.minImageCount + 1;
        if (caps.maxImageCount) n = std::min(n, caps.maxImageCount);
        VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        bool alphaFound = false;
        for (auto value : {VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
             VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR})
            if (caps.supportedCompositeAlpha & value) { alpha = value; alphaFound = true; break; }
        if (!alphaFound) throw std::runtime_error("No composite alpha mode");
        VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        ci.surface = surface; ci.minImageCount = n; ci.imageFormat = swapFormat; ci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        ci.imageExtent = extent; ci.imageArrayLayers = 1; ci.imageUsage = usage; ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ci.preTransform = caps.currentTransform; ci.compositeAlpha = alpha; ci.presentMode = VK_PRESENT_MODE_FIFO_KHR; ci.clipped = VK_FALSE;
        if(replacement) {
            // All prior renderer/bridge submissions were explicitly drained by
            // resizeWsi. oldSwapchain retires even if this create now fails.
            const auto serial=retirement.current();
            retirement.retiringForReplacement();
            auto& old=oldGenerations.at(static_cast<size_t>(serial-1));
            old.serial=serial;old.chain=swapchain;old.semaphores=std::move(presentReady);old.renderWorkDrained=true;
            imagesReacquiredPriorGenerations+=uint64_t(std::count_if(perImageAcquires.begin(),perImageAcquires.end(),[](auto count){return count>1;}));
            ci.oldSwapchain=swapchain;swapchain=VK_NULL_HANDLE;
        }
        check(dev<PFN_vkCreateSwapchainKHR>("vkCreateSwapchainKHR")(device, &ci, nullptr, &swapchain), "vkCreateSwapchainKHR");
        check(dev<PFN_vkGetSwapchainImagesKHR>("vkGetSwapchainImagesKHR")(device, swapchain, &n, nullptr), "Swapchain image count");
        if(!n||n>xess_vk::testing::WsiRetirement::maxImages)throw std::runtime_error("Swapchain image budget exceeded");
        swapImages.resize(n);
        check(dev<PFN_vkGetSwapchainImagesKHR>("vkGetSwapchainImagesKHR")(device, swapchain, &n, swapImages.data()), "Swapchain images");
        if(!n||n>swapImages.size())throw std::runtime_error("Swapchain enumeration changed capacity");
        swapImages.resize(n);presentReady.assign(n,VK_NULL_HANDLE);presentedBefore.assign(n,false);perImageAcquires.assign(n,0);
        swapExtent=extent;
        if(retireViaAcquire)retirement.created(n);
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if(!acquired)check(dev<PFN_vkCreateSemaphore>("vkCreateSemaphore")(device, &si, nullptr, &acquired), "Create acquired semaphore");
        for (auto& semaphore : presentReady)
            check(dev<PFN_vkCreateSemaphore>("vkCreateSemaphore")(device, &si, nullptr, &semaphore), "Create per-image present semaphore");
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (retireViaAcquire && !acquireComplete)
            check(dev<PFN_vkCreateFence>("vkCreateFence")(device, &fenceInfo, nullptr, &acquireComplete), "Create acquire completion fence");
        else if(!retireViaAcquire && !presentationComplete)
            check(dev<PFN_vkCreateFence>("vkCreateFence")(device, &fenceInfo, nullptr, &presentationComplete), "Create presentation completion fence");
    }

    void destroyRetiredGenerations() {
        for(auto& old:oldGenerations)if(old.chain && retirement.canDestroy(old.serial)) {
            if(!old.renderWorkDrained)throw std::logic_error("Old renderer work not drained");
            // Proof is the first new-generation present's actual reacquisition
            // fence, including the inherited old-generation list. Not WaitIdle.
            for(auto& semaphore:old.semaphores)if(semaphore){
                dev<PFN_vkDestroySemaphore>("vkDestroySemaphore")(device,semaphore,nullptr);
                semaphore=VK_NULL_HANDLE;++oldPresentSemaphoresDestroyed;
            }
            dev<PFN_vkDestroySwapchainKHR>("vkDestroySwapchainKHR")(device,old.chain,nullptr);
            old.chain=VK_NULL_HANDLE;retirement.destroyed(old.serial);++oldGenerationsDestroyed;
        }
    }

    void resizeWsi(uint32_t width,uint32_t height) {
        if(!retireViaAcquire||!swapchain||retirement.stats().generations>=xess_vk::testing::WsiRetirement::maxGenerations)
            throw std::logic_error("Resize mode or generation budget invalid");
        wait();if(!drainQueue(cfg.timeoutMs))throw std::runtime_error("Cannot drain renderer before native resize");
        RECT rect{0,0,LONG(width),LONG(height)};
        const DWORD style=DWORD(GetWindowLongPtrW(hiddenWindow,GWL_STYLE));
        if(!AdjustWindowRect(&rect,style,FALSE)||!SetWindowPos(hiddenWindow,nullptr,0,0,rect.right-rect.left,rect.bottom-rect.top,
            SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE))throw std::runtime_error("Resize hidden native client");
        createSwapchain(width,height,true);
    }

    void barrier(VkImage image, VkImageLayout before, VkImageLayout after, VkAccessFlags src, VkAccessFlags dst,
                 VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = before; b.newLayout = after; b.srcAccessMask = src; b.dstAccessMask = dst;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        dev<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(command, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
    }

    ForwardedFrame renderAndForward(SyntheticRole role, const std::array<VkClearColorValue, 3>& clear) {
        if (role == SyntheticRole::Real) {
            // Producer fixture prepares depth/motion and initializes the stable
            // images. Overwrite its color with a poison value: only a successful
            // actual swap-image copy can make preflight color readback pass.
            auto auxiliaryClear = clear;
            auxiliaryClear[0].float32[0] = 0.125f; auxiliaryClear[0].float32[1] = 0.375f;
            auxiliaryClear[0].float32[2] = 0.625f; auxiliaryClear[0].float32[3] = 1.0f;
            renderClear(auxiliaryClear);
        }
        wait(); // Also proves the single acquire semaphore's previous wait consumed.
        uint32_t index = UINT32_MAX;
        const auto acquireResult = dev<PFN_vkAcquireNextImageKHR>("vkAcquireNextImageKHR")(
            device, swapchain, WsiTimeoutNs, acquired, acquireComplete, &index);
        if (!presentAccepted(acquireResult)) check(acquireResult, "vkAcquireNextImageKHR");
        if (index >= swapImages.size()) throw std::runtime_error("Acquired image out of range");
        if (retireViaAcquire) {
            // Returning an image index alone is NOT retirement evidence. Wait
            // on this actual acquisition's fence before retiring that image's
            // previous present, then reset only after the record is consumed.
            const auto fenceResult = dev<PFN_vkWaitForFences>("vkWaitForFences")(
                device, 1, &acquireComplete, VK_TRUE, WsiTimeoutNs);
            retirement.acquiredAfterFence(retirement.current(),index, acquiredCount + 1,acquireResult,fenceResult);
            ++acquireFenceCompletions;
            check(dev<PFN_vkResetFences>("vkResetFences")(device, 1, &acquireComplete), "Reset consumed acquire fence");
            destroyRetiredGenerations();
        }
        ++acquiredCount; ++perImageAcquires[index]; latestRole = role;
        check(dev<PFN_vkResetCommandBuffer>("vkResetCommandBuffer")(command, 0), "Reset WSI command");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(dev<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(command, &begin), "Begin WSI command");
        const VkImage image = swapImages[index];
        barrier(image, presentedBefore[index] ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkClearColorValue color = clear[0];
        if (role == SyntheticRole::Generated) { color.float32[0] = 1.f; color.float32[1] = 0.f; color.float32[2] = 1.f; color.float32[3] = 1.f; }
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        dev<PFN_vkCmdClearColorImage>("vkCmdClearColorImage")(command, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
        if (role == SyntheticRole::Real) {
            barrier(image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            barrier(frame.planes[0].image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            if (swapFormat == VK_FORMAT_R8G8B8A8_UNORM) {
                VkImageCopy copy{}; copy.srcSubresource = copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                copy.extent = {cfg.width, cfg.height, 1};
                dev<PFN_vkCmdCopyImage>("vkCmdCopyImage")(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    frame.planes[0].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            } else {
                VkImageBlit blit{}; blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                blit.srcOffsets[1] = blit.dstOffsets[1] = {int32_t(cfg.width), int32_t(cfg.height), 1};
                dev<PFN_vkCmdBlitImage>("vkCmdBlitImage")(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    frame.planes[0].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
            }
            barrier(frame.planes[0].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        }
        barrier(image, role == SyntheticRole::Real ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, role == SyntheticRole::Real ? VK_ACCESS_TRANSFER_READ_BIT : VK_ACCESS_TRANSFER_WRITE_BIT,
            0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        check(dev<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(command), "End WSI command");
        check(dev<PFN_vkResetFences>("vkResetFences")(device, 1, &completion), "Reset WSI completion");
        const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.waitSemaphoreCount = 1; submit.pWaitSemaphores = &acquired; submit.pWaitDstStageMask = &waitStage;
        submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
        submit.signalSemaphoreCount = 1; submit.pSignalSemaphores = &presentReady[index];
        check(dev<PFN_vkQueueSubmit>("vkQueueSubmit")(queue, 1, &submit, completion), "Submit WSI clear/copy");
        VkResult perSwapchain = VK_ERROR_UNKNOWN;
        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.waitSemaphoreCount = 1; present.pWaitSemaphores = &presentReady[index]; present.swapchainCount = 1;
        present.pSwapchains = &swapchain; present.pImageIndices = &index; present.pResults = &perSwapchain;
        VkSwapchainPresentFenceInfoEXT presentFenceInfo{VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT};
        if (!retireViaAcquire) {
            presentFenceInfo.swapchainCount = 1; presentFenceInfo.pFences = &presentationComplete; present.pNext = &presentFenceInfo;
        }
        // Never replaced with VK_SUCCESS. This returns the image to real WSI.
        const VkResult result = dev<PFN_vkQueuePresentKHR>("vkQueuePresentKHR")(queue, &present);
        ++forwardedCount;
        if (!presentAccepted(result) || !presentAccepted(perSwapchain))
            throw std::runtime_error("Actual vkQueuePresentKHR failed: aggregate=" + std::to_string(result) + ", perSwapchain=" + std::to_string(perSwapchain));
        if (result == VK_SUBOPTIMAL_KHR || perSwapchain == VK_SUBOPTIMAL_KHR || acquireResult == VK_SUBOPTIMAL_KHR) ++acceptedSuboptimal;
        if (retireViaAcquire) {
            // Successful forwarding creates a NEW unresolved record. Neither
            // the return value nor the render-submit fence retires this present.
            retirement.presented(retirement.current(),index, forwardedCount, result, perSwapchain);
            if (!retirement.conserved()) throw std::runtime_error("Acquire retirement accounting failed");
        } else {
            check(dev<PFN_vkWaitForFences>("vkWaitForFences")(device, 1, &presentationComplete, VK_TRUE, WsiTimeoutNs), "Native presentation fence completion");
            ++presentationFenceCompletions;
            check(dev<PFN_vkResetFences>("vkResetFences")(device, 1, &presentationComplete), "Reset native presentation fence");
        }
        presentedBefore[index] = true;
        if (role == SyntheticRole::Real) ++realPresents; else ++generatedPresents;
        wait();
        return {role, acquiredCount, result, perSwapchain};
    }

    bool publishReal(xess_vk::InteropBridge& bridge, const ForwardedFrame& token) {
        if (token.sequence != acquiredCount || token.role != latestRole || !presentAccepted(token.aggregate) || !presentAccepted(token.perSwapchain))
            throw std::runtime_error("Stale or invalid forwarded-frame token");
        if (token.role != SyntheticRole::Real) { ++generatedRejected; return false; }
        if (publishedSequence == token.sequence) throw std::runtime_error("Real frame already published");
        bridge.publishNativeFrame(frame);
        publishedSequence = token.sequence; ++realPublished; return true;
    }

    void shutdownWsi() {
        if (retireViaAcquire) throw std::logic_error("Acquire mode must retain unresolved WSI resources until process exit");
        if (presentationFenceCompletions != forwardedCount) throw std::runtime_error("Native presentation lifetime not retired");
        // Every present has its own confirmed maintenance1 fence completion.
        // DeviceWaitIdle here drains rendering work, not a guessed substitute
        // for the distinct presentation-resource lifetime requirement.
        check(dev<PFN_vkDeviceWaitIdle>("vkDeviceWaitIdle")(device), "WSI teardown vkDeviceWaitIdle");
        if (presentationComplete) dev<PFN_vkDestroyFence>("vkDestroyFence")(device, presentationComplete, nullptr);
        presentationComplete = VK_NULL_HANDLE;
        for (auto s : presentReady) if (s) dev<PFN_vkDestroySemaphore>("vkDestroySemaphore")(device, s, nullptr);
        presentReady.clear();
        if (acquired) dev<PFN_vkDestroySemaphore>("vkDestroySemaphore")(device, acquired, nullptr);
        acquired = VK_NULL_HANDLE;
        if (swapchain) dev<PFN_vkDestroySwapchainKHR>("vkDestroySwapchainKHR")(device, swapchain, nullptr);
        swapchain = VK_NULL_HANDLE;
        if (surface) inst<PFN_vkDestroySurfaceKHR>("vkDestroySurfaceKHR")(instance, surface, nullptr);
        surface = VK_NULL_HANDLE;
        if (hiddenWindow) DestroyWindow(hiddenWindow);
        hiddenWindow = nullptr;
        UnregisterClassW(L"XeSSSelfOwnedShadowWsi", GetModuleHandleW(nullptr));
        cleanupCompleted = true;
    }

    void retainWsiUntilProcessExit() {
        const auto& counts=retirement.stats();
        if (!retireViaAcquire || !retirement.conserved() || !counts.pending
            || !counts.retiredByAcquire || counts.presents != forwardedCount
            || acquireFenceCompletions != acquiredCount || presentationFenceCompletions != 0)
            throw std::runtime_error("Unproven acquire-retirement stop boundary");
        // Finite tail-submit fence covers rendering/bridge queue commands only.
        // It is explicitly not evidence of pending presentation completion.
        if (!drainQueue(cfg.timeoutMs)) throw std::runtime_error("Cannot drain rendering before WSI retention");
        renderQueueDrained = true;
        // Called only after normal SDK shutdown, consumed bridge wait AND full
        // bridge destruction. All acquire signals were waited and their submit
        // waits consumed. Free renderer-owned resources, keep unresolved WSI.
        const auto& current=retirement.generation(retirement.current());
        for(uint32_t i=0;i<current.imageCount;++i)if(current.images[i].acquired)
            throw std::runtime_error("Cannot clear renderer with an outstanding acquired image");
        if(pool)dev<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(device,pool,nullptr);
        pool=VK_NULL_HANDLE;command=VK_NULL_HANDLE;
        if(completion)dev<PFN_vkDestroyFence>("vkDestroyFence")(device,completion,nullptr);
        completion=VK_NULL_HANDLE;frame.producerCompleteFence=VK_NULL_HANDLE;
        for(unsigned i=0;i<frame.planes.size();++i){
            if(frame.planes[i].image){dev<PFN_vkDestroyImage>("vkDestroyImage")(device,frame.planes[i].image,nullptr);frame.planes[i].image=VK_NULL_HANDLE;++sourceImagesDestroyed;}
            if(allocation[i]){dev<PFN_vkFreeMemory>("vkFreeMemory")(device,allocation[i],nullptr);allocation[i]=VK_NULL_HANDLE;++sourceAllocationsFreed;}
        }
        if(acquired)dev<PFN_vkDestroySemaphore>("vkDestroySemaphore")(device,acquired,nullptr);
        acquired=VK_NULL_HANDLE;
        if(acquireComplete)dev<PFN_vkDestroyFence>("vkDestroyFence")(device,acquireComplete,nullptr);
        acquireComplete=VK_NULL_HANDLE;
        for(size_t i=0;i<presentReady.size();++i)if(presentReady[i]&&!current.images[i].pendingSerial){
            dev<PFN_vkDestroySemaphore>("vkDestroySemaphore")(device,presentReady[i],nullptr);presentReady[i]=VK_NULL_HANDLE;
        }
        retainedPresentSemaphores=uint64_t(std::count_if(presentReady.begin(),presentReady.end(),[](auto sem){return sem!=VK_NULL_HANDLE;}));
        for(const auto& old:oldGenerations)for(auto sem:old.semaphores)if(sem)++retainedPresentSemaphores;
        renderResourcesCleanupCompleted=sourceImagesDestroyed==3 && sourceAllocationsFreed==3;
        retainForPossibleBorrowedChildren = true;
        wsiRetainedUntilProcessExit = true;
        // Remaining owner contains pending present semaphores, last swapchain,
        // device/instance/surface/HWND/loader and small CPU records. Parents stay.
    }
};

struct WsiSourceLifetime {
    void operator()(WsiSource* source) const noexcept {
        // Retain the object itself as well as all its Win32/Vulkan handles,
        // creation records, vectors and loader reference. No member destructor
        // runs on the process-lifetime path, including partial-init exceptions.
        if (source && !source->retainForPossibleBorrowedChildren) delete source;
    }
};

std::array<VkClearColorValue, 3> pattern(uint32_t index) {
    std::array<VkClearColorValue, 3> clear{};
    clear[0].float32[0] = (index & 1) ? 1.f : 0.f; clear[0].float32[1] = (index & 2) ? 1.f : 0.f;
    clear[0].float32[2] = (index & 4) ? 1.f : 0.f; clear[0].float32[3] = 1.f;
    clear[1].float32[0] = xess_vk::testing::kDepth[index % 4];
    clear[2].float32[0] = xess_vk::testing::kMotion[index % 4]; clear[2].float32[1] = xess_vk::testing::kMotion[(index+1) % 4];
    return clear;
}
uint32_t checkedFrames(const char* input) {
    const std::string value = input;
    if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos) throw std::invalid_argument("Invalid --frames");
    const auto result = std::stoull(value);
    if (result < 16 || result > 600) throw std::invalid_argument("--frames must be 16..600");
    return uint32_t(result);
}
}

int main(int argc, char** argv) {
    bool retireViaAcquire = false;
    bool resizeCycles = false;
    try {
        if (argc == 2 && std::string(argv[1]) == "--probe-wsi-maintenance") return probeMaintenanceSupport();
        if (argc == 2 && std::string(argv[1]) == "--self-test-retirement") return selfTestRetirement();
        std::filesystem::path sdkBin; uint32_t frames = 60; bool enabled = true;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--sdk-bin" && i+1 < argc) sdkBin = std::filesystem::absolute(argv[++i]);
            else if (arg == "--frames" && i+1 < argc) frames = checkedFrames(argv[++i]);
            else if (arg == "--off") enabled = false;
            else if (arg == "--retire-via-acquire") retireViaAcquire = true;
            else if (arg == "--resize-cycles") resizeCycles = true;
            else throw std::invalid_argument("Usage: --sdk-bin PATH [--frames 16..600] [--off] [--retire-via-acquire] [--resize-cycles], or --probe-wsi-maintenance, or --self-test-retirement");
        }
        if (sdkBin.empty()) throw std::invalid_argument("--sdk-bin required");
        if(resizeCycles&&!retireViaAcquire)throw std::invalid_argument("--resize-cycles requires --retire-via-acquire");
        LifecycleWatchdog guard;
        std::unique_ptr<WsiSource, WsiSourceLifetime> owner(new WsiSource);
        auto& source = *owner;
        source.initializeWsi({Width, Height, 10000}, retireViaAcquire);
        uint64_t sdkPresented = 0, interpolated = 0, offPresents = 0, byteCount = 0;
        uint32_t submitted = 0, preflightFrames = 4, resizeCount = 0; bool sdkCleanup = false;
        {
            xess_vk::InteropBridge bridge(source.cfg, source.context());
            for (uint32_t i = 0; i < 4; ++i) {
                auto token = source.renderAndForward(SyntheticRole::Real, pattern(i));
                if (!source.publishReal(bridge, token)) throw std::runtime_error("Real preflight refused");
                // Overwrite native sources after bridge copy; shared GPU result
                // must retain the exact swapchain-derived color and auxiliary data.
                source.render(i + 17);
                bridge.verifyFrame(source.expected(i));
            }
            Presenter presenter(bridge, sdkBin, enabled); presenter.initialize();
            if (presenter.window.hwnd == source.hiddenWindow || IsWindowVisible(source.hiddenWindow)
                || !IsWindowVisible(presenter.window.hwnd)) throw std::runtime_error("Window ownership failed");
            std::array<VkClearColorValue, 3> stationary{};
            stationary[0].float32[0] = .08f; stationary[0].float32[1] = .18f;
            stationary[0].float32[2] = .35f; stationary[0].float32[3] = 1.f; stationary[1].float32[0] = .5f;
            for (uint32_t frameId = 1; frameId <= frames; ++frameId) {
                MSG message{};
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                    if (message.message == WM_QUIT) throw std::runtime_error("Test window closed before completion");
                    TranslateMessage(&message); DispatchMessageW(&message);
                }
                if(resizeCycles&&(frameId==frames/3+1 || frameId==(frames*2)/3+1)) {
                    bridge.waitUntilConsumed();
                    ++resizeCount;
                    // Native WSI resize only. The XeFG output and native source
                    // textures stay fixed; copy the explicitly bounded top-left
                    // Width x Height region from a larger, fully cleared image.
                    source.resizeWsi(Width+64*resizeCount,Height+64*resizeCount);
                    const uint32_t sample=resizeCount+4;
                    auto token=source.renderAndForward(SyntheticRole::Real,pattern(sample));
                    if(!source.publishReal(bridge,token))throw std::runtime_error("Resized WSI preflight refused");
                    source.render(sample+17);bridge.verifyFrame(source.expected(sample));++preflightFrames;
                }
                presenter.beginFrame(frameId);
                presenter.marker(frameId, XELL_SIMULATION_END); presenter.marker(frameId, XELL_RENDERSUBMIT_START);
                auto generated = source.renderAndForward(SyntheticRole::Generated, stationary);
                const auto before = bridge.stats().nativeImageFrames;
                if (source.publishReal(bridge, generated) || bridge.stats().nativeImageFrames != before)
                    throw std::runtime_error("Generated frame leaked into XeFG inputs");
                auto real = source.renderAndForward(SyntheticRole::Real, stationary);
                if (!source.publishReal(bridge, real)) throw std::runtime_error("Real frame refused");
                presenter.renderPublishedFrame(frameId, 33.333f);
            }
            sdkCleanup = presenter.shutdown(); bridge.waitUntilConsumed();
            const auto& stats = bridge.stats();
            byteCount = stats.bytesVerified;
            if (!sdkCleanup || presenter.submitted != frames || presenter.statusQueryFailures != 0
                || stats.cpuUploadFrames != 0 || stats.nativeImageFrames != uint64_t(frames)+preflightFrames || stats.framesVerified != preflightFrames
                || stats.bytesVerified != uint64_t(preflightFrames)*Width*Height*12
                || (enabled ? presenter.postWarmupFailures != 0 || presenter.successfulInterpolations < frames-4
                            : presenter.validOffPresents != frames)) throw std::runtime_error("Presenter/bridge accounting failed");
            sdkPresented = presenter.sdkPresented; interpolated = presenter.successfulInterpolations;
            offPresents = presenter.validOffPresents; submitted = presenter.submitted;
        }
        if (source.acquiredCount != uint64_t(frames)*2+preflightFrames || source.forwardedCount != source.acquiredCount
            || source.realPresents != uint64_t(frames)+preflightFrames || source.generatedPresents != frames
            || source.generatedRejected != frames || source.realPublished != uint64_t(frames)+preflightFrames
            || source.acquiredCount <= source.swapImages.size()*4) throw std::runtime_error("Native WSI lifecycle accounting failed");
        const auto imageCount = source.swapImages.size();
        const auto reacquires = source.imagesReacquiredPriorGenerations+uint64_t(std::count_if(source.perImageAcquires.begin(), source.perImageAcquires.end(), [](auto n) { return n > 1; }));
        if (!reacquires) throw std::runtime_error("No real swapchain image reacquired");
        if(resizeCycles&&(resizeCount!=2||source.oldGenerationsDestroyed!=2||source.retirement.stats().destroyedGenerations!=2))
            throw std::runtime_error("Not all previous native swapchain generations were actually destroyed");
        source.render(23); source.wait(); // Borrowed bridge did not destroy owner.
        if (retireViaAcquire) source.retainWsiUntilProcessExit();
        else {
            source.shutdownWsi();
            source.retainForPossibleBorrowedChildren = false;
        }
        std::cout << "{\"stage\":\"self-owned-wsi-native-xefg-lifecycle\",\"testPassed\":true"
            << ",\"frameGenerationRequested\":" << (enabled ? "true" : "false")
            << ",\"retirementMode\":" << quote(retireViaAcquire ? "acquire-fence-with-minimal-wsi-tail-retention" : "maintenance1-present-fence")
            << ",\"swapchainImages\":" << imageCount << ",\"imagesReacquired\":" << reacquires
            << ",\"actualAcquires\":" << source.acquiredCount << ",\"actualForwardedPresents\":" << source.forwardedCount
            << ",\"syntheticRealNativePresents\":" << source.realPresents << ",\"syntheticGeneratedNativePresents\":" << source.generatedPresents
            << ",\"generatedInputsRefused\":" << source.generatedRejected << ",\"suboptimalNativePresents\":" << source.acceptedSuboptimal
            << ",\"nativePresentationFenceCompletions\":" << source.presentationFenceCompletions
            << ",\"swapchainMaintenance1Enabled\":" << (retireViaAcquire ? "false" : "true")
            << ",\"acquireFenceCompletions\":" << source.acquireFenceCompletions
            << ",\"presentsRetiredByAcquireFence\":" << source.retirement.stats().retiredByAcquire
            << ",\"presentsRetiredBySuccessorFence\":" << source.retirement.stats().retiredBySuccessor
            << ",\"pendingPresentsAtStop\":" << source.retirement.stats().pending
            << ",\"maxPendingPresents\":" << source.retirement.stats().highWaterPending
            << ",\"nativeResizeCount\":" << resizeCount << ",\"oldSwapchainGenerationsActuallyDestroyed\":" << source.oldGenerationsDestroyed
            << ",\"oldPresentSemaphoresActuallyDestroyed\":" << source.oldPresentSemaphoresDestroyed
            << ",\"swapchainClipped\":false,\"nativeSourceCopyRegionWidth\":" << Width << ",\"nativeSourceCopyRegionHeight\":" << Height
            << ",\"lastNativeSwapchainWidth\":" << source.swapExtent.width << ",\"lastNativeSwapchainHeight\":" << source.swapExtent.height
            << ",\"xefgSubmittedRealFrames\":" << submitted << ",\"sdkReportedPresentedFrames\":" << sdkPresented
            << ",\"successfulInterpolationPresents\":" << interpolated << ",\"validOffPresents\":" << offPresents
            << ",\"preflightReadbackBytes\":" << byteCount << ",\"poisonedDestinationColorBeforeSwapchainCopy\":true"
            << ",\"realSwapchainColorCopiedBeforePresent\":true,\"sameBorrowedDevice\":true,\"sameAdapterLuidChecked\":true"
            << ",\"distinctSelfOwnedHwnds\":true,\"nativePresentNeverSuppressed\":true,\"cpuUploadFrames\":0"
            << ",\"cleanupSucceeded\":" << (sdkCleanup && source.cleanupCompleted ? "true" : "false")
            << ",\"xefgShutdownSucceeded\":" << (sdkCleanup ? "true" : "false")
            << ",\"wsiResourcesRetainedUntilProcessExit\":" << (source.wsiRetainedUntilProcessExit ? "true" : "false")
            << ",\"wsiExplicitDestructionVerified\":" << (source.cleanupCompleted ? "true" : "false")
            << ",\"renderQueueDrainedBeforeWsiRetention\":" << (source.renderQueueDrained ? "true" : "false")
            << ",\"renderResourcesCleanupSucceeded\":" << (source.renderResourcesCleanupCompleted ? "true" : "false")
            << ",\"nativeSourceImagesActuallyDestroyed\":" << source.sourceImagesDestroyed << ",\"nativeSourceAllocationsActuallyFreed\":" << source.sourceAllocationsFreed
            << ",\"retainedPresentSemaphores\":" << source.retainedPresentSemaphores
            << ",\"retainedNativeSwapchains\":" << (source.wsiRetainedUntilProcessExit ? 1+std::count_if(source.oldGenerations.begin(),source.oldGenerations.end(),[](const auto& old){return old.chain!=VK_NULL_HANDLE;}) : 0)
            << ",\"retainedSourceImages\":" << std::count_if(source.frame.planes.begin(),source.frame.planes.end(),[](const auto& plane){return plane.image!=VK_NULL_HANDLE;})
            << ",\"retainedRenderCommandPools\":" << (source.pool ? 1 : 0)
            << ",\"portableWsiFinalRetirementVerified\":" << (source.cleanupCompleted ? "true" : "false")
            << ",\"preflightVerifiedFrames\":" << preflightFrames
            << ",\"gameTested\":false,\"gameRealGeneratedClassificationImplemented\":false,\"gameFrameIdentityProven\":false"
            << ",\"virtualSwapchainImplemented\":false,\"originalGameHwndTakeoverTested\":false"
            << ",\"resizeRecoveryTested\":" << (resizeCycles ? "true" : "false")
            << ",\"resizeScope\":\"self_owned_native_wsi_only_fixed_xefg_output\",\"xefgOutputResizeTested\":false"
            << ",\"visualInterpolationVerified\":false,\"dynamicMotionQualityVerified\":false}\n";
    } catch (const std::exception& error) {
        std::cout << "{\"stage\":\"self-owned-wsi-native-xefg-lifecycle\",\"testPassed\":false,\"gameTested\":false"
            << ",\"retireViaAcquireRequested\":" << (retireViaAcquire ? "true" : "false")
            << ",\"resizeCyclesRequested\":" << (resizeCycles ? "true" : "false")
            << ",\"cleanupSucceeded\":false,\"wsiExplicitDestructionVerified\":false,\"error\":" << quote(error.what()) << "}\n";
        return 1;
    }
}
