// Self-owned WSI smoke; optional no-op compute shader. No game/XeSS calls.
#include <windows.h>
#include <tlhelp32.h>
#include <vulkan/vulkan.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void verifyObserverIfRequested(){
    wchar_t expected[32768]{},selected[32768]{};
    const auto count=GetEnvironmentVariableW(L"XESS_WSI_EXPECT_OBSERVER",expected,std::size(expected));
    if(!count)return;
    if(count>=std::size(expected)||!GetFullPathNameW(expected,std::size(selected),selected,nullptr))throw std::runtime_error("Invalid expected observer path");
    HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPMODULE,GetCurrentProcessId());
    if(snapshot==INVALID_HANDLE_VALUE)throw std::runtime_error("Cannot inspect smoke modules");
    MODULEENTRY32W entry{};entry.dwSize=sizeof(entry);HMODULE module{};unsigned matches{};bool same=false;
    if(Module32FirstW(snapshot,&entry))do{if(_wcsicmp(entry.szModule,L"xess-vulkan-observer.dll")==0){++matches;module=entry.hModule;same=_wcsicmp(entry.szExePath,selected)==0;}}while(Module32NextW(snapshot,&entry));
    CloseHandle(snapshot);
    if(matches!=1||!same)throw std::runtime_error("Smoke did not load exactly the expected candidate observer");
    using Probe=std::uint32_t(VKAPI_PTR*)();auto probe=reinterpret_cast<Probe>(GetProcAddress(module,"xessObserverDiagnosticStateV1"));
    if(!probe)throw std::runtime_error("Observer diagnostic-state probe unavailable");
    wchar_t requested[2]{};const bool enabled=GetEnvironmentVariableW(L"XESS_WSI_EXPECT_DIAGNOSTICS",requested,2)==1&&requested[0]==L'1';
    const auto state=probe();
    if(state!=(enabled?7u:4u))throw std::runtime_error("Observer client/diagnostic writer state differs from requested mode");
    std::printf("OBSERVER_PROBE_PASS: diagnostics=%u writer=%u client_seen=1\n",unsigned(enabled),unsigned((state&2)!=0));
}
constexpr uint32_t kDefaultFrames = 24;
constexpr uint64_t kWaitNs = 3'000'000'000;
constexpr DWORD kProcessTimeoutMs = 25000;
bool stopRequested = false;

void check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) throw std::runtime_error(std::string(operation) + ": VkResult " + std::to_string(result));
}
LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_CLOSE) { stopRequested = true; return 0; }
    return DefWindowProcW(window, message, wp, lp);
}
DWORD WINAPI watchdog(void* argument) {
    if (WaitForSingleObject(static_cast<HANDLE>(argument), kProcessTimeoutMs) == WAIT_TIMEOUT) {
        constexpr char text[] = "WSI smoke exceeded 25 seconds; terminating only this test process.\n";
        DWORD written = 0;
        WriteFile(GetStdHandle(STD_ERROR_HANDLE), text, sizeof(text) - 1, &written, nullptr);
        TerminateProcess(GetCurrentProcess(), 124);
    }
    return 0;
}
struct Watchdog {
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE thread = event ? CreateThread(nullptr, 0, watchdog, event, 0, nullptr) : nullptr;
    Watchdog() {
        if (!event || !thread) {
            if (event) CloseHandle(event);
            throw std::runtime_error("Unable to create bounded test watchdog");
        }
    }
    ~Watchdog() {
        SetEvent(event); WaitForSingleObject(thread, INFINITE); CloseHandle(thread); CloseHandle(event);
    }
};

struct Smoke {
    HMODULE library = nullptr;
    HINSTANCE module = GetModuleHandleW(nullptr);
    HWND window = nullptr;
    ATOM windowClass = 0;
    bool borrowedWindow = false;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkSemaphore acquired = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkPipelineLayout computeLayout = VK_NULL_HANDLE;
    VkPipeline computePipeline = VK_NULL_HANDLE;
    uint32_t family = 0;
    VkExtent2D swapchainExtent{};
    VkFormat swapchainFormat = VK_FORMAT_UNDEFINED;
    std::vector<VkImage> images;
    std::vector<VkSemaphore> rendered;

    PFN_vkGetInstanceProcAddr gipa = nullptr;
    PFN_vkGetDeviceProcAddr gdpa = nullptr;
#define DECLARE(name) PFN_##name name = nullptr
    DECLARE(vkCreateInstance); DECLARE(vkDestroyInstance); DECLARE(vkEnumeratePhysicalDevices);
    DECLARE(vkGetPhysicalDeviceProperties); DECLARE(vkGetPhysicalDeviceQueueFamilyProperties);
    DECLARE(vkCreateWin32SurfaceKHR); DECLARE(vkDestroySurfaceKHR); DECLARE(vkGetPhysicalDeviceSurfaceSupportKHR);
    DECLARE(vkGetPhysicalDeviceSurfaceCapabilitiesKHR); DECLARE(vkGetPhysicalDeviceSurfaceFormatsKHR);
    DECLARE(vkEnumerateDeviceExtensionProperties); DECLARE(vkCreateDevice); DECLARE(vkDestroyDevice);
    DECLARE(vkGetDeviceQueue); DECLARE(vkCreateSwapchainKHR); DECLARE(vkDestroySwapchainKHR);
    DECLARE(vkGetSwapchainImagesKHR); DECLARE(vkAcquireNextImageKHR); DECLARE(vkQueuePresentKHR);
    DECLARE(vkCreateCommandPool); DECLARE(vkDestroyCommandPool); DECLARE(vkAllocateCommandBuffers);
    DECLARE(vkBeginCommandBuffer); DECLARE(vkEndCommandBuffer); DECLARE(vkResetCommandBuffer);
    DECLARE(vkCmdPipelineBarrier); DECLARE(vkCmdClearColorImage); DECLARE(vkCreateSemaphore);
    DECLARE(vkDestroySemaphore); DECLARE(vkCreateFence); DECLARE(vkDestroyFence);
    DECLARE(vkWaitForFences); DECLARE(vkResetFences); DECLARE(vkQueueSubmit); DECLARE(vkDeviceWaitIdle);
    DECLARE(vkCreateShaderModule); DECLARE(vkDestroyShaderModule); DECLARE(vkCreatePipelineLayout);
    DECLARE(vkDestroyPipelineLayout); DECLARE(vkCreateComputePipelines); DECLARE(vkDestroyPipeline);
    DECLARE(vkCmdBindPipeline); DECLARE(vkCmdDispatch);
#undef DECLARE

    template<class T> void instanceProc(T& target, const char* name) {
        target = reinterpret_cast<T>(gipa(instance, name));
        if (!target) throw std::runtime_error(std::string("Missing instance command: ") + name);
    }
    template<class T> void deviceProc(T& target, const char* name) {
        target = reinterpret_cast<T>(gdpa(device, name));
        if (!target) throw std::runtime_error(std::string("Missing device command: ") + name);
    }
    ~Smoke() {
        // The process watchdog bounds drivers that never return from idle/teardown.
        if (device && vkDeviceWaitIdle) vkDeviceWaitIdle(device);
        if (device) {
            if (fence && vkDestroyFence) vkDestroyFence(device, fence, nullptr);
            if (acquired && vkDestroySemaphore) vkDestroySemaphore(device, acquired, nullptr);
            if (vkDestroySemaphore) for (const auto semaphore : rendered) if (semaphore) vkDestroySemaphore(device, semaphore, nullptr);
            if (commandPool && vkDestroyCommandPool) vkDestroyCommandPool(device, commandPool, nullptr);
            if (computePipeline && vkDestroyPipeline) vkDestroyPipeline(device, computePipeline, nullptr);
            if (computeLayout && vkDestroyPipelineLayout) vkDestroyPipelineLayout(device, computeLayout, nullptr);
            if (swapchain && vkDestroySwapchainKHR) vkDestroySwapchainKHR(device, swapchain, nullptr);
            if (vkDestroyDevice) vkDestroyDevice(device, nullptr);
        }
        if (surface && vkDestroySurfaceKHR) vkDestroySurfaceKHR(instance, surface, nullptr);
        if (instance && vkDestroyInstance) vkDestroyInstance(instance, nullptr);
        if (window && !borrowedWindow) DestroyWindow(window);
        if (windowClass) UnregisterClassW(L"XeSSVulkanObserverSmoke", module);
        if (library) FreeLibrary(library);
    }

    void initialize(uint32_t apiVersion, bool compute, bool hidden = false, bool deviceBeforeSurface = false,
                    HWND borrowed = nullptr) {
        if (borrowed) {
            if (!IsWindow(borrowed)) throw std::runtime_error("Borrowed fixture window is invalid");
            borrowedWindow = true;
            window = borrowed;
        } else {
            WNDCLASSW wc{}; wc.lpfnWndProc = windowProc; wc.hInstance = module;
            wc.lpszClassName = L"XeSSVulkanObserverSmoke"; wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
            windowClass = RegisterClassW(&wc);
            if (!windowClass) throw std::runtime_error("RegisterClassW failed");
            window = CreateWindowExW(0, wc.lpszClassName, L"Vulkan observer WSI test (auto closes)",
                                    WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT,
                                    640, 360, nullptr, nullptr, module, nullptr);
            if (!window) throw std::runtime_error("CreateWindowExW failed");
            if (!hidden) ShowWindow(window, SW_SHOWNOACTIVATE);
        }

        library = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!library) throw std::runtime_error("Unable to load system Vulkan runtime");
        gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(library, "vkGetInstanceProcAddr"));
        if (!gipa) throw std::runtime_error("Missing vkGetInstanceProcAddr");
        instanceProc(vkCreateInstance, "vkCreateInstance");
        const char* instanceExtensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
        VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        application.pApplicationName = "XeSS observer self-owned WSI smoke"; application.apiVersion = apiVersion;
        VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        create.pApplicationInfo = &application; create.enabledExtensionCount = 2; create.ppEnabledExtensionNames = instanceExtensions;
        check(vkCreateInstance(&create, nullptr, &instance), "vkCreateInstance");
        verifyObserverIfRequested();
#define LOAD_INSTANCE(name) instanceProc(name, #name)
        LOAD_INSTANCE(vkDestroyInstance); LOAD_INSTANCE(vkEnumeratePhysicalDevices); LOAD_INSTANCE(vkGetPhysicalDeviceProperties);
        LOAD_INSTANCE(vkGetPhysicalDeviceQueueFamilyProperties); LOAD_INSTANCE(vkCreateWin32SurfaceKHR); LOAD_INSTANCE(vkDestroySurfaceKHR);
        LOAD_INSTANCE(vkGetPhysicalDeviceSurfaceSupportKHR); LOAD_INSTANCE(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
        LOAD_INSTANCE(vkGetPhysicalDeviceSurfaceFormatsKHR); LOAD_INSTANCE(vkEnumerateDeviceExtensionProperties);
        LOAD_INSTANCE(vkCreateDevice); instanceProc(gdpa, "vkGetDeviceProcAddr");
#undef LOAD_INSTANCE
        VkWin32SurfaceCreateInfoKHR surfaceInfo{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        surfaceInfo.hinstance = module; surfaceInfo.hwnd = window;
        if (!deviceBeforeSurface)
            check(vkCreateWin32SurfaceKHR(instance, &surfaceInfo, nullptr, &surface), "vkCreateWin32SurfaceKHR");
        uint32_t physicalCount = 0;
        check(vkEnumeratePhysicalDevices(instance, &physicalCount, nullptr), "vkEnumeratePhysicalDevices(count)");
        std::vector<VkPhysicalDevice> physicals(physicalCount);
        check(vkEnumeratePhysicalDevices(instance, &physicalCount, physicals.data()), "vkEnumeratePhysicalDevices");
        for (const auto candidate : physicals) {
            VkPhysicalDeviceProperties props{}; vkGetPhysicalDeviceProperties(candidate, &props);
            // The research target is Intel. Do not silently validate another GPU.
            if (props.vendorID != 0x8086) continue;
            uint32_t familyCount = 0; vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, nullptr);
            std::vector<VkQueueFamilyProperties> families(familyCount);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, families.data());
            for (uint32_t index = 0; index < familyCount; ++index) {
                VkBool32 present = VK_FALSE;
                if (deviceBeforeSurface) {
                    PFN_vkGetPhysicalDeviceWin32PresentationSupportKHR win32Support{};
                    instanceProc(win32Support, "vkGetPhysicalDeviceWin32PresentationSupportKHR");
                    present = win32Support(candidate, index);
                } else {
                    check(vkGetPhysicalDeviceSurfaceSupportKHR(candidate, index, surface, &present), "vkGetPhysicalDeviceSurfaceSupportKHR");
                }
                if ((families[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (!compute || (families[index].queueFlags & VK_QUEUE_COMPUTE_BIT)) && families[index].queueCount && present) {
                    physical = candidate; family = index;
                    std::printf("GPU: %s, graphics/present family %u\n", props.deviceName, family); break;
                }
            }
            if (physical) break;
        }
        if (!physical) throw std::runtime_error("No Intel graphics queue with Win32 presentation support");
        float priority = 1.0f;
        VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queueInfo.queueFamilyIndex = family; queueInfo.queueCount = 1; queueInfo.pQueuePriorities = &priority;
        const char* deviceExtensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        deviceInfo.queueCreateInfoCount = 1; deviceInfo.pQueueCreateInfos = &queueInfo;
        deviceInfo.enabledExtensionCount = 1; deviceInfo.ppEnabledExtensionNames = deviceExtensions;
        check(vkCreateDevice(physical, &deviceInfo, nullptr, &device), "vkCreateDevice");
#define LOAD_DEVICE(name) deviceProc(name, #name)
        LOAD_DEVICE(vkDestroyDevice); LOAD_DEVICE(vkDeviceWaitIdle); LOAD_DEVICE(vkGetDeviceQueue);
        LOAD_DEVICE(vkCreateSwapchainKHR); LOAD_DEVICE(vkDestroySwapchainKHR); LOAD_DEVICE(vkGetSwapchainImagesKHR);
        LOAD_DEVICE(vkAcquireNextImageKHR); LOAD_DEVICE(vkQueuePresentKHR); LOAD_DEVICE(vkCreateCommandPool);
        LOAD_DEVICE(vkDestroyCommandPool); LOAD_DEVICE(vkAllocateCommandBuffers); LOAD_DEVICE(vkBeginCommandBuffer);
        LOAD_DEVICE(vkEndCommandBuffer); LOAD_DEVICE(vkResetCommandBuffer); LOAD_DEVICE(vkCmdPipelineBarrier);
        LOAD_DEVICE(vkCmdClearColorImage); LOAD_DEVICE(vkCreateSemaphore); LOAD_DEVICE(vkDestroySemaphore);
        LOAD_DEVICE(vkCreateFence); LOAD_DEVICE(vkDestroyFence); LOAD_DEVICE(vkWaitForFences); LOAD_DEVICE(vkResetFences); LOAD_DEVICE(vkQueueSubmit);
        LOAD_DEVICE(vkCreateShaderModule); LOAD_DEVICE(vkDestroyShaderModule); LOAD_DEVICE(vkCreatePipelineLayout);
        LOAD_DEVICE(vkDestroyPipelineLayout); LOAD_DEVICE(vkCreateComputePipelines); LOAD_DEVICE(vkDestroyPipeline);
        LOAD_DEVICE(vkCmdBindPipeline); LOAD_DEVICE(vkCmdDispatch);
#undef LOAD_DEVICE
        if (compute) {
            // Minimal self-owned SPIR-V GLCompute entry, local size 1x1x1,
            // no resources or writes. Tests observation across module teardown.
            constexpr uint32_t code[] = {
                0x07230203,0x00010000,0,5,0,
                (2u<<16)|17,1, (3u<<16)|14,0,1,
                (5u<<16)|15,5,3,0x6e69616d,0,
                (6u<<16)|16,3,17,1,1,1,
                (2u<<16)|19,1, (3u<<16)|33,2,1,
                (5u<<16)|54,1,3,0,2, (2u<<16)|248,4, (1u<<16)|253, (1u<<16)|56
            };
            VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            check(vkCreatePipelineLayout(device, &layout, nullptr, &computeLayout), "vkCreatePipelineLayout");
            VkShaderModuleCreateInfo shaderInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            shaderInfo.codeSize=sizeof(code); shaderInfo.pCode=code;
            VkShaderModule shader{};
            check(vkCreateShaderModule(device, &shaderInfo, nullptr, &shader), "vkCreateShaderModule");
            VkComputePipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            pipeline.stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            pipeline.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT; pipeline.stage.module=shader; pipeline.stage.pName="main";
            pipeline.layout=computeLayout;
            const auto result=vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &computePipeline);
            vkDestroyShaderModule(device, shader, nullptr);
            check(result, "vkCreateComputePipelines");
        }
        vkGetDeviceQueue(device, family, 0, &queue);
        if (deviceBeforeSurface)
            check(vkCreateWin32SurfaceKHR(instance, &surfaceInfo, nullptr, &surface), "vkCreateWin32SurfaceKHR(after device)");
        createSwapchain();
        VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pool.queueFamilyIndex = family;
        check(vkCreateCommandPool(device, &pool, nullptr, &commandPool), "vkCreateCommandPool");
        VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocation.commandPool = commandPool; allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; allocation.commandBufferCount = 1;
        check(vkAllocateCommandBuffers(device, &allocation, &command), "vkAllocateCommandBuffers");
        VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        check(vkCreateSemaphore(device, &semaphore, nullptr, &acquired), "vkCreateSemaphore(acquired)");
        rendered.resize(images.size(), VK_NULL_HANDLE);
        for (auto& item : rendered) check(vkCreateSemaphore(device, &semaphore, nullptr, &item), "vkCreateSemaphore(rendered)");
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        check(vkCreateFence(device, &fenceInfo, nullptr, &fence), "vkCreateFence");
    }

    void createSwapchain(VkSwapchainKHR previous = VK_NULL_HANDLE, VkFormat preferred = VK_FORMAT_B8G8R8A8_UNORM,
                         VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR) {
        VkSurfaceCapabilitiesKHR capabilities{};
        check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &capabilities), "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
        if (!(capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT))
            throw std::runtime_error("Surface does not support transfer-destination images");
        uint32_t formatCount = 0;
        check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &formatCount, nullptr), "vkGetPhysicalDeviceSurfaceFormatsKHR(count)");
        std::vector<VkSurfaceFormatKHR> formats(formatCount);
        check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &formatCount, formats.data()), "vkGetPhysicalDeviceSurfaceFormatsKHR");
        if (formats.empty()) throw std::runtime_error("Surface has no formats");
        auto format = formats.front();
        if (format.format == VK_FORMAT_UNDEFINED) format.format = VK_FORMAT_B8G8R8A8_UNORM;
        for (const auto candidate : formats) if (candidate.format == preferred && candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) format = candidate;
        auto extent = capabilities.currentExtent;
        if (extent.width == UINT32_MAX) {
            RECT rect{}; GetClientRect(window, &rect);
            extent.width = std::clamp(static_cast<uint32_t>(rect.right), capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
            extent.height = std::clamp(static_cast<uint32_t>(rect.bottom), capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
        }
        uint32_t imageCount = capabilities.minImageCount + 1;
        if (capabilities.maxImageCount) imageCount = std::min(imageCount, capabilities.maxImageCount);
        VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        for (const auto candidate : {VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                                    VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR})
            if (capabilities.supportedCompositeAlpha & candidate) { alpha = candidate; break; }
        VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        info.surface = surface; info.minImageCount = imageCount; info.imageFormat = format.format; info.imageColorSpace = format.colorSpace;
        info.imageExtent = extent; info.imageArrayLayers = 1; info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE; info.preTransform = capabilities.currentTransform;
        info.compositeAlpha = alpha; info.presentMode = presentMode; info.clipped = VK_TRUE;
        info.oldSwapchain = previous;
        check(vkCreateSwapchainKHR(device, &info, nullptr, &swapchain), "vkCreateSwapchainKHR");
        check(vkGetSwapchainImagesKHR(device, swapchain, &imageCount, nullptr), "vkGetSwapchainImagesKHR(count)");
        images.resize(imageCount);
        check(vkGetSwapchainImagesKHR(device, swapchain, &imageCount, images.data()), "vkGetSwapchainImagesKHR");
        swapchainExtent = extent;
        swapchainFormat = format.format;
        std::printf("Swapchain: %u x %u, format %d, images %u\n", extent.width, extent.height, format.format, imageCount);
    }

    void run(uint32_t frames, void (*afterPresent)(void*,uint32_t) = nullptr, void* user = nullptr,
             bool waitIdle = true) {
        for (uint32_t frame = 0; frame < frames; ++frame) {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
            if (stopRequested) throw std::runtime_error("Test window closed before all frames completed");
            check(vkWaitForFences(device, 1, &fence, VK_TRUE, kWaitNs), "vkWaitForFences");
            uint32_t index = 0;
            const auto acquireResult = vkAcquireNextImageKHR(device, swapchain, kWaitNs, acquired, VK_NULL_HANDLE, &index);
            if (acquireResult != VK_SUCCESS && acquireResult != VK_SUBOPTIMAL_KHR) check(acquireResult, "vkAcquireNextImageKHR");
            check(vkResetCommandBuffer(command, 0), "vkResetCommandBuffer");
            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            check(vkBeginCommandBuffer(command, &begin), "vkBeginCommandBuffer");
            if (computePipeline) {
                vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, computePipeline);
                vkCmdDispatch(command, 1, 1, 1);
            }
            VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.image = images[index];
            barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            // Match the acquire semaphore wait stage so the layout transition is
            // ordered after the presentation engine releases this image.
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
            VkClearColorValue clear{};
            clear.float32[0] = 0.05f + 0.5f * static_cast<float>(frame) / frames;
            clear.float32[1] = 0.18f; clear.float32[2] = 0.35f; clear.float32[3] = 1.0f;
            vkCmdClearColorImage(command, images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &barrier.subresourceRange);
            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = 0;
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
            check(vkEndCommandBuffer(command), "vkEndCommandBuffer");
            check(vkResetFences(device, 1, &fence), "vkResetFences");
            const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.waitSemaphoreCount = 1; submit.pWaitSemaphores = &acquired;
            submit.pWaitDstStageMask = &waitStage; submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
            submit.signalSemaphoreCount = 1; submit.pSignalSemaphores = &rendered[index];
            check(vkQueueSubmit(queue, 1, &submit, fence), "vkQueueSubmit");
            VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR}; present.waitSemaphoreCount = 1; present.pWaitSemaphores = &rendered[index];
            present.swapchainCount = 1; present.pSwapchains = &swapchain; present.pImageIndices = &index;
            const auto presentResult = vkQueuePresentKHR(queue, &present);
            if (presentResult != VK_SUCCESS && presentResult != VK_SUBOPTIMAL_KHR) check(presentResult, "vkQueuePresentKHR");
            if (afterPresent) afterPresent(user,frame);
        }
        check(vkWaitForFences(device, 1, &fence, VK_TRUE, kWaitNs), "vkWaitForFences(final)");
        if (waitIdle) check(vkDeviceWaitIdle(device), "vkDeviceWaitIdle(final)");
        std::printf("WSI_SMOKE_PASS: %u acquire/clear/present calls completed; this is not frame generation.\n", frames);
    }
};
} // namespace

#ifndef XESS_WSI_EMBEDDED
int main(int argc, char** argv) {
    try {
        uint32_t frames = kDefaultFrames;
        bool framesSet = false, api13 = false, compute = false;
        for (int argument = 1; argument < argc; ++argument) {
            if (std::strcmp(argv[argument], "--api-1.3") == 0 && !api13) {
                api13 = true;
            } else if (std::strcmp(argv[argument], "--compute-smoke") == 0 && !compute) {
                compute = true;
            } else if (std::strcmp(argv[argument], "--frames") == 0 && !framesSet && argument + 1 < argc) {
                const char* number = argv[++argument];
                char* end = nullptr;
                const unsigned long value = std::strtoul(number, &end, 10);
                if (!end || end == number || *end || value < 10 || value > 600)
                    throw std::runtime_error("--frames must be an integer from 10 to 600");
                frames = static_cast<uint32_t>(value); framesSet = true;
            } else {
                throw std::runtime_error("Usage: xess-vulkan-wsi-smoke [--frames 10..600] [--api-1.3] [--compute-smoke]");
            }
        }
        Watchdog watchdogGuard;
        { Smoke smoke; smoke.initialize(api13 ? VK_API_VERSION_1_3 : VK_API_VERSION_1_1, compute); smoke.run(frames); }
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "WSI_SMOKE_FAIL: %s\n", error.what());
        return 1;
    }
}
#endif
