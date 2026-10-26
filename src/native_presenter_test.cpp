// Only this test process owns the Vulkan images, device and DXGI/XeFG window.
// The shared Presenter implementation consumes already-published GPU copies.
#define XESS_PRESENTER_EMBEDDED
#include "presenter.cpp"
#include "native_source_fixture.hpp"
#include <sstream>

namespace {
struct NativePresenterWatchdog {
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE thread = event ? CreateThread(nullptr, 0, [](void* value) -> DWORD {
        if (WaitForSingleObject(static_cast<HANDLE>(value), 90000) == WAIT_TIMEOUT)
            TerminateProcess(GetCurrentProcess(), 124);
        return 0;
    }, event, 0, nullptr) : nullptr;
    NativePresenterWatchdog() {
        if (!event || !thread) {
            if (event) CloseHandle(event);
            throw std::runtime_error("Cannot create self-test watchdog");
        }
    }
    ~NativePresenterWatchdog() {
        SetEvent(event);
        if (WaitForSingleObject(thread, 3000) != WAIT_OBJECT_0) TerminateProcess(GetCurrentProcess(), 125);
        CloseHandle(thread); CloseHandle(event);
    }
};
uint32_t frameArgument(const char* text) {
    const std::string value = text;
    if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos)
        throw std::invalid_argument("--frames requires an unsigned integer");
    const auto result = std::stoull(value);
    if (result < 8 || result > 600) throw std::invalid_argument("--frames must be 8..600 (90-second watchdog)");
    return static_cast<uint32_t>(result);
}
uint32_t extentArgument(const char* text) {
    const std::string value = text;
    if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos)
        throw std::invalid_argument("Extent requires an unsigned integer");
    const auto result = std::stoull(value);
    if (result < 1 || result > 16384) throw std::invalid_argument("Extent must be 1..16384");
    return static_cast<uint32_t>(result);
}
}

int main(int argc, char** argv) {
    try {
        std::filesystem::path sdkBin;
        uint32_t frameLimit = 120;
        xess_vk::InteropConfig cfg{Width, Height, 10000};
        bool enabled = true, injectSubmissionFailure = false;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--sdk-bin" && i + 1 < argc) sdkBin = std::filesystem::absolute(argv[++i]);
            else if (arg == "--frames" && i + 1 < argc) frameLimit = frameArgument(argv[++i]);
            else if (arg == "--width" && i + 1 < argc) cfg.width = extentArgument(argv[++i]);
            else if (arg == "--height" && i + 1 < argc) cfg.height = extentArgument(argv[++i]);
            else if (arg == "--input-width" && i + 1 < argc) cfg.inputWidth = extentArgument(argv[++i]);
            else if (arg == "--input-height" && i + 1 < argc) cfg.inputHeight = extentArgument(argv[++i]);
            else if (arg == "--off") enabled = false;
            else if (arg == "--fail-after-submit") injectSubmissionFailure = true;
            else throw std::invalid_argument("Usage: --sdk-bin PATH [--frames 120] [--width 960 --height 540] [--input-width W --input-height H] [--off] [--fail-after-submit]");
        }
        if (sdkBin.empty()) throw std::invalid_argument("--sdk-bin is required");
        cfg.validate();
        const auto auxiliaryExtent = cfg.extent(xess_vk::Format::MotionRG16F);
        if (auxiliaryExtent.width > cfg.width || auxiliaryExtent.height > cfg.height)
            throw std::invalid_argument("Synthetic low-resolution input must not exceed output extent");
        NativePresenterWatchdog watchdog;
        xess_vk::testing::SourceOwner owner;
        owner.initialize(cfg);
        std::string report;
        bool passed = false;
        owner.retainForPossibleBorrowedChildren = true;
        {
            xess_vk::InteropBridge bridge(cfg, owner.context());
            // Read back four actual native image copies before initializing XeFG.
            // expected() creates comparison bytes only; publishFrame is never used.
            constexpr uint32_t preflightFrames = 4;
            for (uint32_t index = 0; index < preflightFrames; ++index) {
                owner.render(index);
                bridge.publishNativeFrame(owner.frame);
                owner.render(index + 997); // Prove source reuse after copy completion.
                bridge.verifyFrame(owner.expected(index));
            }
            Presenter presenter(bridge, sdkBin, enabled);
            presenter.injectSubmissionFailure = injectSubmissionFailure;
            presenter.initialize();
            // A stationary constant scene avoids claiming meaningful dynamic
            // interpolation quality from arbitrary clear-generated motion fields.
            std::array<VkClearColorValue, 3> stationary{};
            stationary[0].float32[0] = 0.08f; stationary[0].float32[1] = 0.18f;
            stationary[0].float32[2] = 0.35f; stationary[0].float32[3] = 1.0f;
            stationary[1].float32[0] = 0.5f; // Nonlinear synthetic depth, identity camera.
            // RG16F motion vectors are exactly zero for every pixel.
            const auto started = std::chrono::steady_clock::now();
            auto previous = started;
            bool closed = false;
            for (uint32_t frame = 1; frame <= frameLimit && !closed; ++frame) {
                MSG message{};
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                    if (message.message == WM_QUIT) { closed = true; break; }
                    TranslateMessage(&message); DispatchMessageW(&message);
                }
                if (closed) break;
                const auto now = std::chrono::steady_clock::now();
                const float elapsedMs = frame == 1 ? 33.333f : std::chrono::duration<float, std::milli>(now - previous).count();
                previous = now;
                presenter.beginFrame(frame);
                presenter.marker(frame, XELL_SIMULATION_END);
                presenter.marker(frame, XELL_RENDERSUBMIT_START);
                owner.renderClear(stationary);
                bridge.publishNativeFrame(owner.frame);
                presenter.renderPublishedFrame(frame, elapsedMs);
            }
            const bool cleanup = presenter.shutdown();
            bridge.waitUntilConsumed();
            const auto& stats = bridge.stats();
            passed = !closed && cleanup && presenter.submitted == frameLimit && presenter.statusQueryFailures == 0
                && stats.cpuUploadFrames == 0 && stats.nativeImageFrames == frameLimit + preflightFrames
                && stats.framesVerified == preflightFrames
                && stats.bytesVerified == uint64_t(preflightFrames) * (cfg.byteSize(xess_vk::Format::ColorRGBA8)
                    + cfg.byteSize(xess_vk::Format::DepthR32F) + cfg.byteSize(xess_vk::Format::MotionRG16F))
                && (enabled ? presenter.postWarmupFailures == 0 && presenter.successfulInterpolations >= frameLimit - 4
                            : presenter.validOffPresents == frameLimit);
            std::ostringstream result;
            result << "{\"stage\":\"native-vulkan-image-xefg-presenter\",\"inputSource\":\"Three device-local VkImages produced by Vulkan GPU clear; stationary synthetic scene\","
                << "\"gameIntegrationTested\":false,\"vulkanWsiTested\":false,\"visualInterpolationVerified\":false,\"dynamicMotionQualityVerified\":false,"
                << "\"frameGenerationRequested\":" << (enabled ? "true" : "false")
                << ",\"outputWidth\":" << cfg.width << ",\"outputHeight\":" << cfg.height
                << ",\"inputWidth\":" << auxiliaryExtent.width << ",\"inputHeight\":" << auxiliaryExtent.height
                << ",\"motionSemantics\":\"synthetic_zero_undilated\",\"gameMotionSemanticsVerified\":false"
                << ",\"submittedFrames\":" << presenter.submitted << ",\"sdkReportedPresentedFrames\":" << presenter.sdkPresented
                << ",\"successfulInterpolationPresents\":" << presenter.successfulInterpolations
                << ",\"presentsWithoutVerifiedInterpolation\":" << presenter.noInterpolation
                << ",\"validOffPresents\":" << presenter.validOffPresents << ",\"statusQueryFailures\":" << presenter.statusQueryFailures
                << ",\"postWarmupInterpolationFailures\":" << presenter.postWarmupFailures
                << ",\"nativeImageFrames\":" << stats.nativeImageFrames << ",\"cpuUploadFrames\":" << stats.cpuUploadFrames
                << ",\"preflightReadbackFrames\":" << stats.framesVerified << ",\"preflightBytesCompared\":" << stats.bytesVerified
                << ",\"preflightEveryPixelCompared\":true,\"preflightSourcesReusedBeforeReadback\":true"
                << ",\"sameAdapterLuidChecked\":true,\"borrowedVulkanDevice\":true,\"borrowedDeviceAliveAfterBridgeDestruction\":true"
                << ",\"elapsedSeconds\":" << std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count()
                << ",\"cleanupSucceeded\":" << (cleanup ? "true" : "false") << ",\"testPassed\":" << (passed ? "true" : "false") << "}";
            report = result.str();
        }
        owner.retainForPossibleBorrowedChildren = false;
        // The report is emitted only after this real submission proves that the
        // bridge did not destroy caller-owned device/images/queue/fence objects.
        owner.render(7); owner.wait();
        std::cout << report << '\n';
        return passed ? 0 : 2;
    } catch (const std::exception& error) {
        std::cout << "{\"stage\":\"native-vulkan-image-xefg-presenter\",\"testPassed\":false,\"gameIntegrationTested\":false,\"error\":"
            << quote(error.what()) << "}\n";
        return 1;
    }
}
