// Owns every input object and an independent UI thread. This exercises the
// explicit layer's OFF output and teardown, not Unity's asynchronous source.
#define XESS_WSI_EMBEDDED
#include "vulkan_wsi_smoke.cpp"

namespace {
constexpr DWORD kUiDeadlineMs = 3000;
constexpr uint32_t kFramesPerMode = 12;
PFN_vkQueuePresentKHR sourceNextPresent{};
unsigned sourcePresentCalls{};
bool sourceInstanceEntry{};
VKAPI_ATTR VkResult VKAPI_CALL observeOwnedPresent(VkQueue queue, const VkPresentInfoKHR* info) {
    const auto originalError = GetLastError();
    const auto call = ++sourcePresentCalls;
    if (call <= 4) {
        HMODULE module{};
        char path[MAX_PATH]{};
        const auto callee = reinterpret_cast<std::uintptr_t>(sourceNextPresent);
        const char* name = "unknown";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCSTR>(callee), &module)) {
            const auto length = GetModuleFileNameA(module, path, MAX_PATH);
            if (length && length < MAX_PATH) {
                const auto* leaf = std::strrchr(path, '\\');
                name = leaf ? leaf + 1 : path;
            }
        }
        std::printf("{\"stage\":\"owned-present-source\",\"entryMode\":\"%s\",\"call\":%u,\"thread\":%lu,"
                    "\"queue\":\"%p\",\"info\":\"%p\",\"pNext\":\"%p\","
                    "\"swapchainCount\":%u,\"chain\":\"%p\",\"image\":%u,"
                    "\"waitCount\":%u,\"wait0\":\"%p\",\"callee\":\"%p\","
                    "\"calleeModule\":\"%s\",\"calleeRva\":\"0x%llx\"}\n",
                    sourceInstanceEntry ? "instance_proc_addr" : "device_proc_addr", call, GetCurrentThreadId(),
                    static_cast<void*>(queue), static_cast<const void*>(info), info->pNext,
                    info->swapchainCount, static_cast<void*>(info->pSwapchains[0]), info->pImageIndices[0],
                    info->waitSemaphoreCount, info->waitSemaphoreCount ? static_cast<void*>(info->pWaitSemaphores[0]) : nullptr,
                    reinterpret_cast<void*>(callee), name,
                    static_cast<unsigned long long>(module ? callee - reinterpret_cast<std::uintptr_t>(module) : 0));
        std::fflush(stdout);
    }
    SetLastError(originalError);
    return sourceNextPresent(queue, info);
}
struct BorrowedWindow {
    static constexpr UINT configureMessage = WM_APP + 28;
    HANDLE ready = nullptr;
    HANDLE thread = nullptr;
    HWND window = nullptr;
    DWORD startupError = ERROR_SUCCESS;
    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wp, LPARAM lp) {
        if (message == configureMessage) {
            if (wp == 2) { ShowWindow(window, SW_MINIMIZE); return IsIconic(window); }
            if (wp == 3) { ShowWindow(window, SW_RESTORE); return !IsIconic(window); }
            const DWORD style = (wp ? WS_POPUP : WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX) |
                                (GetWindowLongPtrW(window, GWL_STYLE) & WS_VISIBLE);
            RECT rect{0, 0, LOWORD(lp), HIWORD(lp)};
            if (!AdjustWindowRect(&rect, style, FALSE)) return 0;
            SetWindowLongPtrW(window, GWL_STYLE, style);
            return SetWindowPos(window, nullptr, 0, 0, rect.right - rect.left, rect.bottom - rect.top,
                                SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        }
        if (message == WM_CLOSE) {
            DestroyWindow(window);
            return 0;
        }
        if (message == WM_DESTROY) {
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(window, message, wp, lp);
    }
    static DWORD WINAPI entry(void* argument) {
        auto& owner = *static_cast<BorrowedWindow*>(argument);
        const auto module = GetModuleHandleW(nullptr);
        constexpr auto className = L"XeSSPresentationRuntimeBorrowedFixture";
        WNDCLASSW wc{};
        wc.lpfnWndProc = procedure;
        wc.hInstance = module;
        wc.lpszClassName = className;
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        if (!RegisterClassW(&wc)) {
            owner.startupError = GetLastError();
            SetEvent(owner.ready);
            return 1;
        }
        owner.window = CreateWindowExW(0, className, L"Presentation OFF teardown fixture (auto closes)",
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT,
            640, 360, nullptr, nullptr, module, nullptr);
        if (!owner.window) {
            owner.startupError = GetLastError();
            SetEvent(owner.ready);
            UnregisterClassW(className, module);
            return 1;
        }
        ShowWindow(owner.window, SW_SHOWNOACTIVATE);
        ShowWindow(owner.window, SW_SHOWNOACTIVATE);
        SetEvent(owner.ready);
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (IsWindow(owner.window)) DestroyWindow(owner.window);
        UnregisterClassW(className, module);
        return 0;
    }
    void open() {
        ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ready) throw std::runtime_error("Borrowed UI ready event creation failed");
        thread = CreateThread(nullptr, 0, entry, this, 0, nullptr);
        if (!thread) throw std::runtime_error("Borrowed UI thread creation failed");
        if (WaitForSingleObject(ready, kUiDeadlineMs) != WAIT_OBJECT_0)
            throw std::runtime_error("Borrowed UI thread startup exceeded its deadline");
        if (startupError || !IsWindow(window))
            throw std::runtime_error("Borrowed UI window creation failed");
    }
    bool close() {
        // Completion, not a posted request, permits releasing this owner.
        if (IsWindow(window) && !PostMessageW(window, WM_CLOSE, 0, 0)) return false;
        if (WaitForSingleObject(thread, kUiDeadlineMs) != WAIT_OBJECT_0) return false;
        CloseHandle(thread);
        CloseHandle(ready);
        thread = ready = nullptr;
        return true;
    }
    void configure(std::uint32_t width, std::uint32_t height, bool borderless = false) {
        DWORD_PTR result{};
        if (!SendMessageTimeoutW(window, configureMessage, borderless, MAKELPARAM(width, height),
                                 SMTO_ABORTIFHUNG | SMTO_BLOCK, kUiDeadlineMs, &result) || !result)
            throw std::runtime_error("Owned window extent/style transition failed");
    }
    void minimized(bool value) {
        DWORD_PTR result{};
        if (!SendMessageTimeoutW(window, configureMessage, value ? 2 : 3, 0,
                                 SMTO_ABORTIFHUNG | SMTO_BLOCK, kUiDeadlineMs, &result) || !result)
            throw std::runtime_error("Owned window minimize/restore failed");
    }
};
void checkpoint(const char* stage) {
    std::printf("{\"stage\":\"%s\"}\n", stage);
    std::fflush(stdout);
}
void requireWindow(HWND window, const char* stage) {
    if (!IsWindow(window) || !IsWindowVisible(window))
        throw std::runtime_error(std::string(stage) + ": borrowed HWND was lost or hidden");
}
} // namespace

int main(int argc, char** argv) {
    bool fakeSwapchain{};
    for (int n = 1; n < argc; ++n) {
        if (!std::strcmp(argv[n], "--instance-present-entry") && !sourceInstanceEntry)
            sourceInstanceEntry = true;
        else if (!std::strcmp(argv[n], "--fake-swapchain") && !fakeSwapchain)
            fakeSwapchain = true;
        else {
            std::fputs("Usage: xess-presentation-runtime-gpu-test [--instance-present-entry] [--fake-swapchain]\n", stderr);
            return 2;
        }
    }
    try {
        wchar_t option[8]{};
        if (GetEnvironmentVariableW(L"XESS_PRESENTATION_OFF", option, 8) != 1 || option[0] != L'1')
            throw std::runtime_error("Self-owned fixture requires presentation OFF opt-in");
        const bool fakeEnvironment = GetEnvironmentVariableW(L"XESS_FAKE_SWAPCHAIN", option, 8) == 1 && option[0] == L'1';
        if (fakeEnvironment != fakeSwapchain)
            throw std::runtime_error("Fake swapchain fixture option must match exact XESS_FAKE_SWAPCHAIN=1");
        // Heap owners survive every unknown GPU failure. Exception handling
        // never unwinds Smoke into vkDeviceWaitIdle or vendor destruction.
        // This watchdog bounds only the test child, not a kernel GPU hang.
        auto* watchdogGuard = new Watchdog;
        (void)watchdogGuard;
        auto* ui = new BorrowedWindow;
        ui->open();
        ui->configure(320, 180);
        auto* smoke = new Smoke;
        checkpoint("initializing-borrowed-off");
        smoke->initialize(VK_API_VERSION_1_3, false, false, true, ui->window);
        sourceNextPresent = smoke->vkQueuePresentKHR;
        if (sourceInstanceEntry) {
            const auto present = smoke->gipa(smoke->instance, "vkQueuePresentKHR");
            if (!present) throw std::runtime_error("Vulkan instance resolver has no vkQueuePresentKHR entry");
            sourceNextPresent = reinterpret_cast<PFN_vkQueuePresentKHR>(present);
        }
        smoke->vkQueuePresentKHR = observeOwnedPresent;
        const auto original = ui->window;
        const auto initial = smoke->swapchain;
        // Keep the initial BGRA8 format throughout. Native prepare deliberately
        // does not admit changing SDK format as part of this mode-only test.
        smoke->createSwapchain(initial, VK_FORMAT_B8G8R8A8_UNORM);
        smoke->vkDestroySwapchainKHR(smoke->device, initial, nullptr);
        if (smoke->swapchainFormat != VK_FORMAT_B8G8R8A8_UNORM)
            throw std::runtime_error("Fixture did not obtain its required BGRA8 format");
        const auto extent = smoke->swapchainExtent;
        if (!extent.width || !extent.height || extent.width > 640 || extent.height > 360)
            throw std::runtime_error("Fixture surface extent exceeded its fixed-size limit");
        checkpoint("fifo-off-frames");
        smoke->run(kFramesPerMode, nullptr, nullptr, false);
        requireWindow(original, "FIFO output");

        const auto previous = smoke->swapchain;
        checkpoint("fifo-to-immediate-recreate");
        smoke->createSwapchain(previous, VK_FORMAT_B8G8R8A8_UNORM, VK_PRESENT_MODE_IMMEDIATE_KHR);
        if (smoke->swapchainExtent.width != extent.width || smoke->swapchainExtent.height != extent.height ||
            smoke->swapchainFormat != VK_FORMAT_B8G8R8A8_UNORM || smoke->images.size() != smoke->rendered.size())
            throw std::runtime_error("Mode replacement changed the fixture extent/format/image count");
        checkpoint("destroy-old-fifo-chain");
        smoke->vkDestroySwapchainKHR(smoke->device, previous, nullptr);
        checkpoint("immediate-off-frames");
        smoke->run(kFramesPerMode, nullptr, nullptr, false);
        requireWindow(original, "IMMEDIATE output after old-chain destruction");

        std::uint32_t resizeCount{}, minimizedFrames{};
        bool minimizedAcquireOutOfDate{};
        for (unsigned cycle = 0; cycle != 12; ++cycle) {
            const auto width = cycle % 2 ? 320u : 384u;
            const auto height = cycle % 2 ? 180u : 216u;
            ui->configure(width, height, cycle % 2 == 0);
            const auto old = smoke->swapchain;
            checkpoint("small-extent-borderless-style-recreate");
            const bool destroyFirst = cycle == 7;
            if (destroyFirst) smoke->vkDestroySwapchainKHR(smoke->device, old, nullptr);
            smoke->createSwapchain(destroyFirst ? VK_NULL_HANDLE : old, VK_FORMAT_B8G8R8A8_UNORM);
            if (smoke->swapchainExtent.width != width || smoke->swapchainExtent.height != height ||
                smoke->images.size() != smoke->rendered.size())
                throw std::runtime_error("Positive original HWND query failed to reach replacement native WSI");
            if (!destroyFirst) smoke->vkDestroySwapchainKHR(smoke->device, old, nullptr);
            smoke->run(kFramesPerMode, nullptr, nullptr, false);
            ++resizeCount;
            if (cycle == 5) {
                checkpoint("original-window-minimized");
                ui->minimized(true);
                // Only the original output is minimized. Its private native WSI
                // must retain the last positive extent and consume real waits.
                VkSurfaceCapabilitiesKHR caps{};
                check(smoke->vkGetPhysicalDeviceSurfaceCapabilitiesKHR(smoke->physical, smoke->surface, &caps), "minimized capabilities");
                if (caps.currentExtent.width != width || caps.currentExtent.height != height)
                    throw std::runtime_error("Minimize replaced the last positive hidden extent");
                if (fakeSwapchain) {
                    std::uint32_t imageIndex = UINT32_MAX;
                    const auto acquired = smoke->vkAcquireNextImageKHR(smoke->device, smoke->swapchain,
                        0, smoke->acquired, VK_NULL_HANDLE, &imageIndex);
                    if (acquired != VK_ERROR_OUT_OF_DATE_KHR || imageIndex != UINT32_MAX)
                        throw std::runtime_error("Minimized fake acquire must return OUT_OF_DATE without signaling or writing an index");
                    minimizedAcquireOutOfDate = true;
                } else {
                    smoke->run(4, nullptr, nullptr, false);
                    minimizedFrames = 4;
                }
                ui->minimized(false);
                checkpoint("original-window-restored");
                smoke->run(kFramesPerMode, nullptr, nullptr, false);
            }
        }
        ui->configure(320, 180);
        requireWindow(original, "repeated extent/minimize/borderless transitions");

        // The layer must prove private queues complete before SDK/bridge
        // destruction and the native device tail. External log validation
        // distinguishes these void destroy calls from retained quarantine.
        checkpoint("normal-device-cleanup");
        smoke->vkDestroySwapchainKHR(smoke->device, smoke->swapchain, nullptr);
        smoke->swapchain = VK_NULL_HANDLE;
        // Present can now return before our rendered semaphore's private wait
        // completes. Own queue-tail completion makes its destruction legal.
        check(smoke->vkResetFences(smoke->device,1,&smoke->fence),"reset owned queued tail fence");
        VkSubmitInfo queuedTail{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        check(smoke->vkQueueSubmit(smoke->queue,1,&queuedTail,smoke->fence),"submit owned queued tail");
        check(smoke->vkWaitForFences(smoke->device,1,&smoke->fence,VK_TRUE,10'000'000'000ull),"complete owned queued tail");
        smoke->vkDestroyFence(smoke->device, smoke->fence, nullptr);
        smoke->fence = VK_NULL_HANDLE;
        smoke->vkDestroySemaphore(smoke->device, smoke->acquired, nullptr);
        smoke->acquired = VK_NULL_HANDLE;
        for (const auto semaphore : smoke->rendered)
            smoke->vkDestroySemaphore(smoke->device, semaphore, nullptr);
        smoke->rendered.clear();
        smoke->vkDestroyCommandPool(smoke->device, smoke->commandPool, nullptr);
        smoke->commandPool = VK_NULL_HANDLE;
        smoke->vkDestroyDevice(smoke->device, nullptr);
        smoke->device = VK_NULL_HANDLE;
        requireWindow(original, "device destruction");
        checkpoint("native-surface-instance-tail");
        smoke->vkDestroySurfaceKHR(smoke->instance, smoke->surface, nullptr);
        smoke->surface = VK_NULL_HANDLE;
        smoke->vkDestroyInstance(smoke->instance, nullptr);
        smoke->instance = VK_NULL_HANDLE;
        requireWindow(original, "surface/instance destruction");
        if (!ui->close()) throw std::runtime_error("Borrowed UI close exceeded deadline; owner retained");
        delete ui;
        // Retain Smoke and the loader/module through process exit. There is no
        // FreeLibrary or unproven second cleanup after the native tail.
        std::printf(
            "{\"stage\":\"presentation-runtime-self-owned-gpu\",\"fixtureCallsCompleted\":true,"
            "\"frames\":%u,\"fifoFrames\":%u,\"immediateFrames\":%u,"
            "\"frameCountsArePresentCalls\":true,"
            "\"startupReplacement\":true,\"sameExtentModeReplacement\":true,"
            "\"positiveExtentReplacements\":%u,\"minimizedFrameCalls\":%u,\"restoredOutputCalls\":true,"
            "\"fakeSwapchainRequested\":%s,\"minimizedAcquireOutOfDate\":%s,"
            "\"destroyBeforeNullOldReplacement\":true,"
            "\"borderlessStyleTransitions\":true,\"exclusiveFullscreenTested\":false,\"monitorSizedFullscreenTested\":false,"
            "\"oldChainDestroyedBeforeMoreOutput\":true,\"independentUiThread\":true,"
            "\"normalDeviceDestroyReturned\":true,\"nativeSurfaceInstanceTailReturned\":true,"
            "\"originalHwndRetainedUntilUiClose\":true,\"moduleRetained\":true,"
            "\"shutdownLogValidationRequired\":true,\"xefgRequested\":false,"
            "\"queuedOffLogValidationRequired\":true,\"applicationSemaphoreDestructionAfterQueuedTail\":true,"
            "\"gameTested\":false,\"unityAsyncHookTested\":false,"
            "\"visualInterpolationVerified\":false}\n",
            15 * kFramesPerMode + minimizedFrames, 14 * kFramesPerMode + minimizedFrames,
            kFramesPerMode, resizeCount, minimizedFrames, fakeSwapchain ? "true" : "false",
            minimizedAcquireOutOfDate ? "true" : "false");
        std::fflush(stdout);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "PRESENTATION_RUNTIME_GPU_FAIL: %s; retained GPU owners, no destructor cleanup\n", error.what());
        std::fflush(stderr);
        // Only this self-owned test process. No DLL detach or SDK retries
        // follow an unknown GPU failure.
        TerminateProcess(GetCurrentProcess(), 1);
        std::_Exit(1);
    }
}
