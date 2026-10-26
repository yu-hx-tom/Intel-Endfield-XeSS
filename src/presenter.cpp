#include "interop.hpp"
#include "launcher_preferences.hpp"
#include "fence_wait.hpp"
#include "presenter_shutdown.hpp"
#include "d3d12_gpu_timestamps.hpp"
#include "upscale_frame_constants.hpp"
#include <windows.h>
#include <dxgi1_6.h>
#include <xess_fg/xefg_swapchain_d3d12.h>
#include <xess_fg/xefg_swapchain_debug.h>
#include <xell/xell_d3d12.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr uint32_t Width = 960, Height = 540;
template<class T> struct Com {
    T* p = nullptr;
    ~Com() { reset(); }
    void reset() { if (p) p->Release(); p = nullptr; }
    T* operator->() const { return p; }
    Com() = default;
    Com(const Com&) = delete;
};
struct D3D12TimestampResources {
    ID3D12Device* device{};
    ID3D12CommandQueue* queue{};
    ID3D12GraphicsCommandList* list{};
    Com<ID3D12QueryHeap> heap;
    Com<ID3D12Resource> readback;
    std::uint64_t frequencyHz{}, qpcFrequency{};
    std::uint64_t calibrationCalls{};
    xess_vk::GpuTimestampCalibration calibration;
    bool initialize() noexcept {
        if (FAILED(queue->GetTimestampFrequency(&frequencyHz)) || !frequencyHz) return false;
        LARGE_INTEGER frequency{};
        if (QueryPerformanceFrequency(&frequency)) qpcFrequency = frequency.QuadPart;
        D3D12_QUERY_HEAP_DESC query{};
        query.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        query.Count = 2;
        if (FAILED(device->CreateQueryHeap(&query, IID_PPV_ARGS(&heap.p)))) return false;
        D3D12_HEAP_PROPERTIES properties{};
        properties.Type = D3D12_HEAP_TYPE_READBACK;
        properties.CreationNodeMask = properties.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = 2 * sizeof(std::uint64_t);
        buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        return SUCCEEDED(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &buffer,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback.p)));
    }
    static std::uint64_t qpcNow() noexcept {
        LARGE_INTEGER value{};
        return QueryPerformanceCounter(&value) ? static_cast<std::uint64_t>(value.QuadPart) : 0;
    }
    xess_vk::GpuTimestampCalibration calibrate() noexcept {
        if (calibrationCalls++ % 300) return calibration;
        calibration = {};
        calibration.captureQpcTicks = qpcNow();
        calibration.qpcFrequency = qpcFrequency;
        calibration.available = qpcFrequency && calibration.captureQpcTicks &&
            SUCCEEDED(queue->GetClockCalibration(&calibration.gpuTicks, &calibration.qpcTicks));
        // D3D12 supplies no maximum calibration deviation.
        return calibration;
    }
    void begin() noexcept { list->EndQuery(heap.p, D3D12_QUERY_TYPE_TIMESTAMP, 0); }
    void endAndResolve() noexcept {
        list->EndQuery(heap.p, D3D12_QUERY_TYPE_TIMESTAMP, 1);
        list->ResolveQueryData(heap.p, D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, readback.p, 0);
    }
    bool read(std::uint64_t& beginTicks, std::uint64_t& endTicks) noexcept {
        void* data{};
        const D3D12_RANGE readRange{0, 2 * sizeof(std::uint64_t)};
        if (FAILED(readback->Map(0, &readRange, &data))) return false;
        if (!data) {
            const D3D12_RANGE noWrites{0, 0};
            readback->Unmap(0, &noWrites);
            return false;
        }
        std::uint64_t ticks[2];
        std::memcpy(ticks, data, sizeof(ticks));
        const D3D12_RANGE noWrites{0, 0};
        readback->Unmap(0, &noWrites);
        beginTicks = ticks[0];
        endTicks = ticks[1];
        return true;
    }
    void release() noexcept { readback.reset(); heap.reset(); }
    void retain() noexcept { readback.p = nullptr; heap.p = nullptr; }
};
void hrCheck(HRESULT hr, const char* name) {
    if(hr==DXGI_ERROR_DEVICE_REMOVED||hr==DXGI_ERROR_DEVICE_RESET||hr==DXGI_ERROR_DEVICE_HUNG)
        xess_vk::measurement::increment(xess_vk::measurement::Counter::DeviceLost);
    if (FAILED(hr)) throw std::runtime_error(std::string(name) + " HRESULT=" + std::to_string(hr));
}
void sdkCheck(int result, const char* name) {
    if(result<0)xess_vk::measurement::increment(xess_vk::measurement::Counter::SdkErrors);
    if(result>0)xess_vk::measurement::increment(xess_vk::measurement::Counter::SdkWarnings);
    if (result < 0) throw std::runtime_error(std::string(name) + " result=" + std::to_string(result));
    if (result > 0) std::cerr << name << " warning=" << result << '\n';
}
std::string quote(const std::string& value) {
    std::string out = "\"";
    for (unsigned char c : value) {
        if (c == '\\' || c == '"') { out += '\\'; out += static_cast<char>(c); }
        else if (c >= 32) out += static_cast<char>(c);
        else out += ' ';
    }
    return out + '"';
}
struct Dll {
    HMODULE h;
    explicit Dll(const std::filesystem::path& path) : h(LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        if (!h) throw std::runtime_error("LoadLibrary " + path.string() + ": " + std::to_string(GetLastError()));
    }
    ~Dll() { if (h) FreeLibrary(h); }
    template<class F> F fn(const char* name) {
        auto p = GetProcAddress(h, name);
        if (!p) throw std::runtime_error(std::string("Missing SDK export ") + name);
        return reinterpret_cast<F>(p);
    }
};
#define FG_FN(name) decltype(&::name) name = fg.fn<decltype(&::name)>(#name)
#define LL_FN(name) decltype(&::name) name = ll.fn<decltype(&::name)>(#name)
struct Sdk {
    Dll ll, fg;
    explicit Sdk(const std::filesystem::path& dir) : ll(dir / "libxell.dll"), fg(dir / "libxess_fg.dll") {}
    FG_FN(xefgSwapChainD3D12CreateContext);
    FG_FN(xefgSwapChainD3D12InitFromSwapChainDesc);
    FG_FN(xefgSwapChainD3D12GetInitializationParameters);
    FG_FN(xefgSwapChainD3D12GetSwapChainPtr);
    FG_FN(xefgSwapChainD3D12TagFrameResource);
    FG_FN(xefgSwapChainSetLatencyReduction);
    FG_FN(xefgSwapChainSetEnabled);
    FG_FN(xefgSwapChainGetProperties);
    FG_FN(xefgSwapChainSetPresentId);
    FG_FN(xefgSwapChainTagFrameConstants);
    FG_FN(xefgSwapChainGetLastPresentStatus);
    FG_FN(xefgSwapChainSetLoggingCallback);
    FG_FN(xefgSwapChainEnableDebugFeature);
    FG_FN(xefgSwapChainSetSceneChangeThreshold);
    FG_FN(xefgSwapChainDestroy);
    LL_FN(xellD3D12CreateContext);
    LL_FN(xellSetSleepMode);
    LL_FN(xellGetSleepMode);
    LL_FN(xellSetLoggingCallback);
    LL_FN(xellSleep);
    LL_FN(xellAddMarkerData);
    LL_FN(xellDestroyContext);
};
#undef FG_FN
#undef LL_FN
void logSdk(const char* message, xefg_swapchain_logging_level_t level, void*) {
    std::cerr << "XeFG[" << int(level) << "]: " << message << '\n';
}
void logXeLL(const char* message, xell_logging_level_t level) {
    std::cerr << "XeLL[" << int(level) << "]: " << message << '\n';
}
LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_CLOSE) { PostQuitMessage(0); return 0; }
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
struct Window {
    HWND hwnd = nullptr;
    bool owned = true;
    explicit Window(uint32_t width = Width, uint32_t height = Height, HWND borrowed = nullptr) {
        if (borrowed) { hwnd = borrowed; owned = false; return; }
        WNDCLASSW wc{};
        wc.lpfnWndProc = windowProc; wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"XeSSVulkanPrototypeWindow"; wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) throw std::runtime_error("RegisterClass failed");
        RECT size{0, 0, LONG(width), LONG(height)};
        DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
        AdjustWindowRect(&size, style, FALSE);
        hwnd = CreateWindowExW(WS_EX_NOACTIVATE, wc.lpszClassName, L"Vulkan transfer -> XeSS-FG | independent synthetic test", style,
            CW_USEDEFAULT, CW_USEDEFAULT, size.right-size.left, size.bottom-size.top, nullptr, nullptr, wc.hInstance, nullptr);
        if (!hwnd) throw std::runtime_error("CreateWindow failed");
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    }
    ~Window() { if (owned && hwnd) DestroyWindow(hwnd); }
};
template<class List>
void transition(List* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before; barrier.Transition.StateAfter = after;
    list->ResourceBarrier(1, &barrier);
}
#ifndef XESS_PRESENTER_EMBEDDED
// Exact finite, small integer velocities suffice for this diagnostic scene.
uint16_t halfInteger(int value) {
    if (!value) return 0;
    unsigned int magnitude = static_cast<unsigned int>(std::abs(value));
    unsigned int exponent = 0;
    while ((1u << (exponent + 1)) <= magnitude) ++exponent;
    return static_cast<uint16_t>((value < 0 ? 0x8000 : 0) | ((exponent + 15) << 10) | ((magnitude - (1u << exponent)) << (10 - exponent)));
}
// CPU-authored test data is uploaded by Vulkan, then transferred to D3D12.
// This deliberately does not claim a Vulkan rasterized game scene.
std::array<std::vector<uint8_t>, 3> scene(uint32_t frame) {
    std::array<std::vector<uint8_t>, 3> planes;
    for (auto& plane : planes) plane.resize(size_t(Width) * Height * 4);
    int phase = int(frame % 240), previousPhase = int((frame-1) % 240);
    auto position = [](int p) { return 140 + (p < 120 ? p : 240-p) * 4; };
    int left = position(phase), previousLeft = position(previousPhase);
    const uint16_t vx = halfInteger(previousLeft-left);
    for (uint32_t y = 0; y < Height; ++y) for (uint32_t x = 0; x < Width; ++x) {
        const size_t offset = (size_t(y)*Width+x)*4;
        const bool box = int(x) >= left && int(x) < left+160 && y >= 170 && y < 350;
        const bool grid = x % 48 < 2 || y % 48 < 2;
        planes[0][offset] = box ? 235 : (grid ? 44 : 16);
        planes[0][offset+1] = box ? 110 : (grid ? 55 : 25);
        planes[0][offset+2] = box ? 32 : (grid ? 70 : 40);
        planes[0][offset+3] = 255;
        float depth = box ? 0.25f : 0.75f;
        std::memcpy(&planes[1][offset], &depth, 4);
        uint16_t motion[2]{box ? vx : uint16_t(0), 0};
        std::memcpy(&planes[2][offset], motion, 4);
    }
    return planes;
}
#endif

constexpr std::uint32_t continuousInitFlags = XEFG_SWAPCHAIN_INIT_FLAG_INVERTED_DEPTH |
    XEFG_SWAPCHAIN_INIT_FLAG_USE_NDC_VELOCITY | XEFG_SWAPCHAIN_INIT_FLAG_JITTERED_MV;
[[maybe_unused]] xefg_swapchain_frame_constant_data_t nativeFrameConstants(
    const xess_vk::frame_input::FrameConstantsSample& sample, bool resetHistory) {
    namespace fi = xess_vk::frame_input;
    constexpr auto required = fi::View | fi::Projection | fi::Jitter | fi::MotionScale | fi::Extents;
    if ((sample.validityMask & required) != required)
        throw std::invalid_argument("Native XeFG constants require verified view/projection, jitter, motion scale and extents");
    for (const auto* matrix : {&sample.view, &sample.projection})
        if (!std::all_of(matrix->begin(), matrix->end(), [](float value) { return std::isfinite(value); }))
            throw std::invalid_argument("Native XeFG matrix contains nonfinite values");
    for (float jitter : sample.jitter)
        if (!std::isfinite(jitter) || jitter < -0.5f || jitter > 0.5f)
            throw std::invalid_argument("Native XeFG jitter must be within [-0.5,0.5]");
    if (sample.renderSize[0] <= 0 || sample.renderSize[1] <= 0 ||
        sample.upscaleSize[0] <= 0 || sample.upscaleSize[1] <= 0)
        throw std::invalid_argument("Native XeFG constants have an invalid image extent");
    xefg_swapchain_frame_constant_data_t result{};
    std::copy(sample.view.begin(), sample.view.end(), result.viewMatrix);
    std::copy(sample.projection.begin(), sample.projection.end(), result.projectionMatrix);
    result.jitterOffsetX = sample.jitter[0]; result.jitterOffsetY = sample.jitter[1];
    // The Prepare constants scale source vectors to texture UV displacement.
    // XeFG NDC has a two-unit axis and Y opposite to texture UV.
    result.motionVectorScaleX = 2.0f * sample.motionVectorScale[0];
    result.motionVectorScaleY = -2.0f * sample.motionVectorScale[1];
    if (!std::isfinite(result.motionVectorScaleX) || !std::isfinite(result.motionVectorScaleY) ||
        result.motionVectorScaleX == 0 || result.motionVectorScaleY == 0)
        throw std::invalid_argument("Native XeFG NDC motion scale must be finite and nonzero");
    result.resetHistory = resetHistory ? 1 : 0;
    // No measured game render duration is available at this presentation entry.
    result.frameRenderTime = 0.0f;
    return result;
}
[[maybe_unused]] xefg_swapchain_d3d12_resource_data_t backbufferRegionTag(VkExtent2D output, VkRect2D rect) {
    if (!rect.offset.x && !rect.offset.y && !rect.extent.width && !rect.extent.height)
        rect.extent = output;
    if (rect.offset.x < 0 || rect.offset.y < 0 || !rect.extent.width || !rect.extent.height ||
        static_cast<std::uint32_t>(rect.offset.x) > output.width ||
        static_cast<std::uint32_t>(rect.offset.y) > output.height ||
        rect.extent.width > output.width - static_cast<std::uint32_t>(rect.offset.x) ||
        rect.extent.height > output.height - static_cast<std::uint32_t>(rect.offset.y))
        throw std::invalid_argument("Native interpolation rectangle is outside the output");
    xefg_swapchain_d3d12_resource_data_t tag{};
    tag.type = XEFG_SWAPCHAIN_RES_BACKBUFFER;
    tag.validity = XEFG_SWAPCHAIN_RV_UNTIL_NEXT_PRESENT;
    tag.resourceBase = {static_cast<std::uint32_t>(rect.offset.x), static_cast<std::uint32_t>(rect.offset.y)};
    tag.resourceSize = {rect.extent.width, rect.extent.height};
    return tag;
}
// This exact recording path also runs with an inert command list in the CPU fixture.
template<class List, class Bridge, class Tag>
void recordPublishedResources(List* list, Bridge& bridge, ID3D12Resource* backbuffer, bool enabled,
                              const xefg_swapchain_d3d12_resource_data_t& region, Tag&& tagResource) {
    auto* color = bridge.texture(xess_vk::Format::ColorRGBA8);
    transition(list, color, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(list, backbuffer, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyResource(backbuffer, color);
    xess_vk::measurement::command(xess_vk::measurement::Command::D3dCopy);
    transition(list, color, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    transition(list, backbuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    if (!enabled) return;
    tagResource(region);
    for (unsigned i = 0; i < 2; ++i) {
        const auto format = i == 0 ? xess_vk::Format::DepthR32F : xess_vk::Format::MotionRG16F;
        auto* source = bridge.texture(format);
        const auto extent = bridge.config().extent(format);
        transition(list, source, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        xefg_swapchain_d3d12_resource_data_t tag{};
        tag.type = i == 0 ? XEFG_SWAPCHAIN_RES_DEPTH : XEFG_SWAPCHAIN_RES_MOTION_VECTOR;
        tag.validity = XEFG_SWAPCHAIN_RV_ONLY_NOW; tag.resourceSize = {extent.width, extent.height};
        tag.pResource = source; tag.incomingState = D3D12_RESOURCE_STATE_COPY_SOURCE;
        // SDK records its own copy and restores incomingState in this same list.
        tagResource(tag);
        transition(list, source, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    }
}
template<class List, class Bridge>
void recordSharedPublishedResources(List* list, Bridge& bridge, xess_vk::SharedPlaneKey key,
                                    ID3D12Resource* backbuffer, bool enabled) {
    auto* color = bridge.texture(xess_vk::Format::ColorRGBA8, key);
    transition(list, color, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(list, backbuffer, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyResource(backbuffer, color);
    xess_vk::measurement::command(xess_vk::measurement::Command::D3dCopy);
    transition(list, color, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    transition(list, backbuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    if (enabled) for (auto format : {xess_vk::Format::DepthR32F, xess_vk::Format::MotionRG16F})
        transition(list, bridge.texture(format, key), D3D12_RESOURCE_STATE_COMMON,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}
template<class Bridge, class Tag>
void tagSharedPublishedResources(Bridge& bridge, xess_vk::SharedPlaneKey key,
                                 const xefg_swapchain_d3d12_resource_data_t& region, Tag&& tagResource) {
    tagResource(nullptr, region);
    const auto config = bridge.config(key);
    for (unsigned i = 0; i < 2; ++i) {
        const auto format = i == 0 ? xess_vk::Format::DepthR32F : xess_vk::Format::MotionRG16F;
        const auto extent = config.extent(format);
        xefg_swapchain_d3d12_resource_data_t tag{};
        tag.type = i == 0 ? XEFG_SWAPCHAIN_RES_DEPTH : XEFG_SWAPCHAIN_RES_MOTION_VECTOR;
        tag.validity = XEFG_SWAPCHAIN_RV_UNTIL_NEXT_PRESENT; tag.resourceSize = {extent.width, extent.height};
        tag.pResource = bridge.texture(format, key); tag.incomingState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        tagResource(nullptr, tag);
    }
}
template<class List, class Bridge>
void restoreSharedPublishedResources(List* list, Bridge& bridge, xess_vk::SharedPlaneKey key) {
    for (auto format : {xess_vk::Format::DepthR32F, xess_vk::Format::MotionRG16F})
        transition(list, bridge.texture(format, key), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_COMMON);
}
[[maybe_unused]] VkRect2D nativeOutputInterpolationRect(
    const xess_vk::frame_input::FrameConstantsSample& sample, VkExtent2D output,
    VkExtent2D depth, VkExtent2D motion, VkRect2D rect) {
    const bool explicitRect = rect.offset.x || rect.offset.y || rect.extent.width || rect.extent.height;
    const auto tag = backbufferRegionTag(output, rect);
    if (!depth.width || !depth.height || depth.width > tag.resourceSize.x || depth.height > tag.resourceSize.y ||
        motion.width != depth.width || motion.height != depth.height ||
        sample.renderSize[0] != static_cast<std::int32_t>(depth.width) ||
        sample.renderSize[1] != static_cast<std::int32_t>(depth.height) ||
        sample.upscaleSize[0] <= 0 || sample.upscaleSize[1] <= 0 ||
        (!explicitRect && (sample.upscaleSize[0] != static_cast<std::int32_t>(output.width) ||
                           sample.upscaleSize[1] != static_cast<std::int32_t>(output.height))))
        throw std::invalid_argument("Native bundle constants and image extents disagree");
    return {{static_cast<std::int32_t>(tag.resourceBase.x), static_cast<std::int32_t>(tag.resourceBase.y)},
            {tag.resourceSize.x, tag.resourceSize.y}};
}
bool interpolationRectChanged(VkRect2D previous, VkRect2D current) {
    return previous.offset.x != current.offset.x || previous.offset.y != current.offset.y ||
           previous.extent.width != current.extent.width || previous.extent.height != current.extent.height;
}
template<class Bridge>
void waitForCallerCommands(Bridge& bridge, xess_vk::CpuWallTime& cpu) {
    xess_vk::CpuWallScope elapsed(cpu);
    bridge.waitUntilConsumed();
}
// This entry is intentionally late: the borrowed game's input and simulation
// already happened. Sleep still has to precede every XeLL marker for this ID.
// It is a partial XeLL experiment, not complete game latency instrumentation.
template<class Wait, class Sleep>
int beginBorrowedPresentTiming(std::uint32_t frame, Wait&& wait, Sleep&& sleep) {
    wait();
    return sleep(frame);
}
template<class Fence>
xess_vk::FrameCompletionState completedFrameValue(Fence* fence, std::uint64_t target) noexcept {
    using State = xess_vk::FrameCompletionState;
    if (!fence || !target || target == UINT64_MAX) return State::Invalid;
    const auto done = fence->GetCompletedValue();
    if (done == UINT64_MAX) return State::DeviceLost;
    return done >= target ? State::Complete : State::Pending;
}
bool sameFrameReceipt(const xess_vk::FrameCompletion& a, const xess_vk::FrameCompletion& b) noexcept {
    return a.key == b.key && a.sharedKey == b.sharedKey && a.sharedAuxiliary == b.sharedAuxiliary &&
        a.producedValue == b.producedValue && a.consumedValue == b.consumedValue &&
        a.sdkTailValue == b.sdkTailValue && a.producedValid == b.producedValid &&
        a.consumedValid == b.consumedValid && a.sdkTailValid == b.sdkTailValid &&
        a.d3dWaitQueued == b.d3dWaitQueued && a.callerSubmitted == b.callerSubmitted &&
        a.postPresentSubmitted == b.postPresentSubmitted && a.poisoned == b.poisoned;
}
void poisonPreparedFrameReceipt(xess_vk::FrameCompletion& receipt, xess_vk::FrameCompletion* stored,
                                bool prepared) noexcept {
    if (!prepared) return;
    receipt.poisoned = true;
    if (stored && stored->key == receipt.key) *stored = receipt;
}
std::uint64_t nextSdkTailValue(std::uint64_t previous) {
    if (previous >= UINT64_MAX - 1) throw std::overflow_error("Presenter SDK-tail fence value exhausted");
    return previous + 1;
}
template<class Fence>
xess_vk::FrameCompletionState frameReceiptState(Fence* fence, const xess_vk::FrameCompletion& expected,
                                                const xess_vk::FrameCompletion& receipt) noexcept {
    using State = xess_vk::FrameCompletionState;
    if (!receipt.key.valid() || receipt.poisoned || !receipt.producedValid || !receipt.consumedValid ||
        !receipt.sdkTailValid || !receipt.d3dWaitQueued || !receipt.callerSubmitted ||
        !receipt.producedValue || receipt.producedValue == UINT64_MAX ||
        receipt.consumedValue != receipt.producedValue ||
        (receipt.sharedKey.valid() && !receipt.postPresentSubmitted) ||
        (!receipt.sharedKey.valid() && (receipt.sharedKey != xess_vk::SharedPlaneKey{} ||
                                     receipt.sharedAuxiliary || receipt.postPresentSubmitted)) ||
        !sameFrameReceipt(receipt, expected)) return State::Invalid;
    return completedFrameValue(fence, receipt.sdkTailValue);
}

struct Presenter {
    xess_vk::InteropBridge& bridge;
    Sdk sdk;
    Window window;
    xefg_swapchain_handle_t context = nullptr;
    xell_context_handle_t latency = nullptr;
    Com<IDXGIFactory2> factory;
    Com<IDXGISwapChain4> swapchain;
    Com<ID3D12CommandAllocator> allocator;
    Com<ID3D12GraphicsCommandList> list;
    Com<ID3D12Fence> completed;
    D3D12TimestampResources gpuTimestampResources;
    xess_vk::detail::CallerGpuTimestamps callerGpuTimestamps;
    struct FrameSlot {
        Com<ID3D12CommandAllocator> allocator;
        Com<ID3D12GraphicsCommandList> list;
        Com<ID3D12CommandAllocator> postAllocator;
        Com<ID3D12GraphicsCommandList> postList;
        D3D12TimestampResources timestampResources;
        xess_vk::detail::CallerGpuTimestamps timestamps;
        xess_vk::FrameCompletion receipt;
        bool initialized{};
    };
    std::array<FrameSlot, xess_vk::frameSlotCount> frameSlots;
    std::uint64_t callerTimestampSequence{};
    std::uint64_t callerTimestampGeneration{1};
    std::uint64_t measurementContext{};
    xess_vk::GpuTimestampSample latestCallerTimestamp;
    HANDLE event = nullptr;
    uint64_t completedValue = 0;
    bool fgEnabled;
    bool lastRequestedEnabled{};
    bool continuousMode{};
    bool initiallyQuiescent{};
    bool gpuTimestampsRequested{};
    bool injectSubmissionFailure = false;
    bool stopped = false;
    bool shutdownFailed = false;
    UINT syncInterval = 1;
    // Flip-model presents with interval 0 and no tearing replace each other
    // within one refresh, which dropped most generated frames. When the OS
    // supports it, the proxy owns an ALLOW_TEARING chain (as in the SDK guide)
    // and interval-0 presents tear instead, leaving pacing to the Intel driver.
    UINT swapChainFlags = 0;
    // Requested maximum generated frames per application frame, and the value
    // the SDK applied (capped by hardware and the Intel Graphics Software override).
    uint32_t interpolatedFramesRequest = 1, appliedInterpolatedFrames = 1;
    unsigned multiplierRequested{2};
    // Counts caller-list consumption during real-frame reuse, and lifecycle full-tail waits.
    xess_vk::CpuWallTime previousD3DWaitCpu, dxgiPresentCpu;
    uint32_t submitted = 0, successfulInterpolations = 0, noInterpolation = 0;
    uint32_t validOffPresents = 0, statusQueryFailures = 0, postWarmupFailures = 0;
    uint64_t sdkPresented = 0;
    xefg_swapchain_present_status_t lastStatus{};
    std::uint32_t lastStatusPresentId{};
    int lastStatusQueryResult{};
    bool lastStatusValid{}, lastStatusQueryAttempted{};
    xess_vk::fg_tuning::Applied tuning;
    xess_vk::fg_tuning::Totals tuningTotals;
    VkRect2D previousInterpolationRect{};
    bool interpolationHistoryValid{};
    std::uint32_t borrowedSleepFrame{}, borrowedSleepCalls{}, presentMarkerCalls{}, enabledRunFrames{};
    int borrowedSleepResult{}, presentStartResult{}, presentEndResult{};
    bool borrowedSleepReturned{};
    std::map<int, uint32_t> frameResults;
    explicit Presenter(xess_vk::InteropBridge& b, const std::filesystem::path& bin, bool enabled, HWND borrowed = nullptr, bool continuous = false, bool quiescent = false, bool gpuTimestamps = false)
        : bridge(b), sdk(bin), window(b.config().width, b.config().height, borrowed), fgEnabled(enabled), continuousMode(continuous), initiallyQuiescent(quiescent), gpuTimestampsRequested(gpuTimestamps) {
        if (b.config().colorOnly && enabled) throw std::invalid_argument("Color-only presentation requires XeFG OFF");
    }
    void initialize() {
        const auto input = bridge.config().extent(xess_vk::Format::MotionRG16F);
        if (input.width > bridge.config().width || input.height > bridge.config().height)
            throw std::invalid_argument("Synthetic low-resolution motion must not exceed the output extent");
        auto* device = bridge.device();
        hrCheck(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory.p)), "CreateDXGIFactory2");
        hrCheck(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator.p)), "CreateCommandAllocator");
        hrCheck(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.p, nullptr, IID_PPV_ARGS(&list.p)), "CreateCommandList");
        hrCheck(list->Close(), "Close initial list");
        hrCheck(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&completed.p)), "CreateFence");
        gpuTimestampResources.device = device;
        gpuTimestampResources.queue = bridge.queue();
        gpuTimestampResources.list = list.p;
        callerGpuTimestamps.initialize(gpuTimestampsRequested, gpuTimestampResources);
        latestCallerTimestamp.requested = gpuTimestampsRequested;
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event) throw std::runtime_error("CreateEvent failed");
        sdkCheck(sdk.xellD3D12CreateContext(device, &latency), "xellD3D12CreateContext");
        sdkCheck(sdk.xellSetLoggingCallback(latency, XELL_LOGGING_LEVEL_INFO, logXeLL), "xellSetLoggingCallback");
        xell_sleep_params_t sleep{};
        sleep.bLowLatencyMode = window.owned || (continuousMode && initiallyQuiescent) ? 1 : 0;
        sleep.minimumIntervalUs = window.owned ? 33333 : 0;
        sdkCheck(sdk.xellSetSleepMode(latency, &sleep), "xellSetSleepMode");
        sdkCheck(sdk.xefgSwapChainD3D12CreateContext(device, &context), "xefgSwapChainD3D12CreateContext");
        sdkCheck(sdk.xefgSwapChainSetLoggingCallback(context, XEFG_SWAPCHAIN_LOGGING_LEVEL_INFO, logSdk, nullptr), "SetLoggingCallback");
        sdkCheck(sdk.xefgSwapChainSetLatencyReduction(context, latency), "SetLatencyReduction");
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = bridge.config().width; desc.Height = bridge.config().height; desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1; desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 3; desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.Scaling = DXGI_SCALING_STRETCH; desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        {
            Com<IDXGIFactory5> factory5;
            BOOL tearing = FALSE;
            if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&factory5.p))) &&
                SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing))) && tearing)
                swapChainFlags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
        }
        desc.Flags = swapChainFlags;
        xefg_swapchain_d3d12_init_params_t init{};
        // This independent fixture uses un-dilated synthetic motion (zero in
        // native-clear tests). Do not treat FSR's dilated low-res field as an
        // equivalent game input merely because its format and extent match.
        init.maxInterpolatedFrames = interpolatedFramesRequest; init.uiMode = XEFG_SWAPCHAIN_UI_MODE_NONE;
        multiplierRequested=interpolatedFramesRequest==XEFG_SWAPCHAIN_USE_MAX_SUPPORTED_INTERPOLATED_FRAMES?0:interpolatedFramesRequest+1;
        if(multiplierRequested){
            xefg_swapchain_properties_t supported{};
            sdkCheck(sdk.xefgSwapChainGetProperties(context,&supported),"GetProperties before initialization");
            if(!supported.maxSupportedInterpolations)throw std::runtime_error("XeFG reports no supported interpolation capacity");
            init.maxInterpolatedFrames=xess_vk::launcher_preferences::generatedCapacity(multiplierRequested,supported.maxSupportedInterpolations);
        }
        if (continuousMode) init.initFlags = continuousInitFlags;
        std::cerr << "Initializing XeFG swapchain...\n";
        sdkCheck(sdk.xefgSwapChainD3D12InitFromSwapChainDesc(context, window.hwnd, &desc, nullptr, bridge.queue(), factory.p, &init), "InitFromSwapChainDesc");
        sdkCheck(sdk.xefgSwapChainD3D12GetSwapChainPtr(context, IID_PPV_ARGS(&swapchain.p)), "GetSwapChainPtr");
        {
            xefg_swapchain_d3d12_init_params_t applied{};
            sdkCheck(sdk.xefgSwapChainD3D12GetInitializationParameters(context, &applied), "GetInitializationParameters");
            if (!applied.maxInterpolatedFrames || applied.maxInterpolatedFrames > 3)
                throw std::runtime_error("XeFG applied an unsupported number of interpolated frames");
            appliedInterpolatedFrames = applied.maxInterpolatedFrames;
            if(!multiplierRequested)multiplierRequested=appliedInterpolatedFrames+1;
        }
        if(continuousMode){
            tuning=xess_vk::fg_tuning::apply(xess_vk::launcher_preferences::startupFgTuning(),
                [&](float value){return int(sdk.xefgSwapChainSetSceneChangeThreshold(context,value));},
                [&](unsigned mode){const xefg_swapchain_debug_feature_t features[]{XEFG_SWAPCHAIN_DEBUG_FEATURE_TAG_INTERPOLATED_FRAMES,
                    XEFG_SWAPCHAIN_DEBUG_FEATURE_PRESENT_FAILED_INTERPOLATION,XEFG_SWAPCHAIN_DEBUG_FEATURE_SHOW_ONLY_INTERPOLATION};
                    return int(sdk.xefgSwapChainEnableDebugFeature(context,features[mode-1],1,nullptr));});
        }
        sdkCheck(sdk.xefgSwapChainSetEnabled(context, fgEnabled), "SetEnabled");
        if(!continuousMode)sdkCheck(sdk.xefgSwapChainEnableDebugFeature(context, XEFG_SWAPCHAIN_DEBUG_FEATURE_TAG_INTERPOLATED_FRAMES, fgEnabled, nullptr), "EnableDebugFeature");
        std::cerr << "XeFG presenter initialized.\n";
    }
    void resizeOutput(std::uint32_t width, std::uint32_t height) {
        waitFrame();
        bridge.waitUntilConsumed();
        // The SDK requires every submitted tagging list to complete and all
        // back-buffer references to be released before proxy ResizeBuffers.
        // GetBuffer references are local to renderPublishedFrame; reset the
        // completed list before releasing its size-dependent resources.
        hrCheck(allocator->Reset(), "Resize allocator Reset");
        hrCheck(list->Reset(allocator.p, nullptr), "Resize list Reset");
        hrCheck(list->Close(), "Resize list Close");
        for (auto& slot : frameSlots) if (slot.initialized) {
            hrCheck(slot.allocator->Reset(), "Resize slot allocator Reset");
            hrCheck(slot.list->Reset(slot.allocator.p, nullptr), "Resize slot list Reset");
            hrCheck(slot.list->Close(), "Resize slot list Close");
        }
        setEnabled(false);
        hrCheck(swapchain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, swapChainFlags), "ResizeBuffers");
        bridge.resizeOutput(width, height);
        enabledRunFrames = 0;
        callerGpuTimestamps.rebuilt();
        for (auto& slot : frameSlots) if (slot.initialized) slot.timestamps.rebuilt();
        if (callerTimestampGeneration != UINT64_MAX) ++callerTimestampGeneration;
    }
    void setEnabled(bool enabled) {
        lastRequestedEnabled = enabled;
        if (enabled && continuousMode) {
            xell_sleep_params_t mode{};
            sdkCheck(sdk.xellGetSleepMode(latency, &mode), "xellGetSleepMode");
            if (!mode.bLowLatencyMode)
                throw std::logic_error("XeLL latency reduction must be enabled before XeFG");
        }
        sdkCheck(sdk.xefgSwapChainSetEnabled(context, enabled), "SetEnabled");
        if (!enabled || enabled != fgEnabled) enabledRunFrames = 0;
        if (!enabled) interpolationHistoryValid = false;
        fgEnabled = enabled;
    }
    void waitFrame() {
        if (!completed.p || !completedValue) return;
        {
            xess_vk::CpuWallScope elapsed(previousD3DWaitCpu);
            xess_vk::detail::waitForD3D12Fence(completed.p, completedValue, event, 10000, "Presenter GPU completion");
        }
        callerGpuTimestamps.completed(gpuTimestampResources);
        acceptCallerTimestamp(callerGpuTimestamps.latest());
        pollCallerTimestamps();
    }
    void waitCallerCommands() {
        // ONLY_NOW copies are in our tagging list; consumed follows that list,
        // before SDK Present. Resource teardown still waits the full SDK tail.
        waitForCallerCommands(bridge, previousD3DWaitCpu);
        callerGpuTimestamps.completed(gpuTimestampResources);
        acceptCallerTimestamp(callerGpuTimestamps.latest());
        pollCallerTimestamps();
    }
    void acceptCallerTimestamp(const xess_vk::GpuTimestampSample& sample) noexcept {
        if (sample.sequence > latestCallerTimestamp.sequence) latestCallerTimestamp = sample;
    }
    void pollCallerTimestamps() noexcept {
        for (auto& slot : frameSlots) if (slot.receipt.consumedValid && !slot.receipt.poisoned &&
            bridge.consumedComplete(slot.receipt.consumedValue) == xess_vk::FrameCompletionState::Complete) {
            slot.timestamps.completed(slot.timestampResources);
            acceptCallerTimestamp(slot.timestamps.latest());
        }
    }
    xess_vk::GpuTimestampSample callerTimestampSample() noexcept {
        pollCallerTimestamps(); return latestCallerTimestamp;
    }
    xess_vk::FrameCompletionState frameComplete(const xess_vk::FrameCompletion& receipt) const noexcept {
        using State = xess_vk::FrameCompletionState;
        if (!receipt.key.valid()) return State::Invalid;
        return frameReceiptState(completed.p, frameSlots[receipt.key.index].receipt, receipt);
    }
    void waitFrame(const xess_vk::FrameCompletion& receipt, std::uint32_t timeoutMs) {
        const auto state = frameComplete(receipt);
        if (state == xess_vk::FrameCompletionState::Invalid || state == xess_vk::FrameCompletionState::DeviceLost)
            throw std::logic_error("Frame receipt is stale/poisoned or the D3D12 device was removed");
        if (state == xess_vk::FrameCompletionState::Pending) {
            xess_vk::CpuWallScope elapsed(previousD3DWaitCpu);
            xess_vk::detail::waitForD3D12Fence(completed.p, receipt.sdkTailValue, event, timeoutMs, "Presenter slot SDK tail");
        }
        pollCallerTimestamps();
    }
    void prepareFrameSlot(const xess_vk::FrameCompletion& receipt) {
        if (!xess_vk::detail::freshFrameCompletion(receipt))
            throw std::logic_error("Presenter requires a fresh caller-owned frame slot receipt");
        auto& slot = frameSlots[receipt.key.index];
        const auto previousState = slot.receipt.key.valid() ? frameComplete(slot.receipt) : xess_vk::FrameCompletionState::Invalid;
        if (!xess_vk::detail::frameSlotCanReuse(slot.receipt.key, receipt.key, previousState))
            throw std::logic_error("Presenter slot is stale or its exact SDK tail remains incomplete");
        pollCallerTimestamps();
        if (!slot.initialized) {
            auto* device = bridge.device();
            Com<ID3D12CommandAllocator> newAllocator;
            Com<ID3D12GraphicsCommandList> newList;
            hrCheck(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&newAllocator.p)), "Create slot allocator");
            hrCheck(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, newAllocator.p, nullptr, IID_PPV_ARGS(&newList.p)), "Create slot list");
            hrCheck(newList->Close(), "Close initial slot list");
            std::swap(slot.allocator.p, newAllocator.p);
            std::swap(slot.list.p, newList.p);
            slot.timestampResources.device = device; slot.timestampResources.queue = bridge.queue();
            slot.timestampResources.list = slot.list.p;
            slot.timestamps.initialize(gpuTimestampsRequested, slot.timestampResources);
            slot.initialized = true;
        }
        if (receipt.sharedKey.valid() && !slot.postList.p) {
            Com<ID3D12CommandAllocator> postAllocator;
            Com<ID3D12GraphicsCommandList> postList;
            hrCheck(bridge.device()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&postAllocator.p)), "Create post-Present allocator");
            hrCheck(bridge.device()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, postAllocator.p, nullptr,
                IID_PPV_ARGS(&postList.p)), "Create post-Present list");
            hrCheck(postList->Close(), "Close initial post-Present list");
            std::swap(slot.postAllocator.p, postAllocator.p); std::swap(slot.postList.p, postList.p);
        }
        slot.receipt = receipt;
    }
    void poisonFrame(xess_vk::FrameCompletion& receipt, bool prepared) noexcept {
        poisonPreparedFrameReceipt(receipt, receipt.key.valid() ? &frameSlots[receipt.key.index].receipt : nullptr, prepared);
        if (prepared && receipt.sharedKey.valid()) bridge.poisonSharedFrame(receipt.sharedKey);
    }
    void marker(uint32_t frame, xell_latency_marker_type_t markerType) {
        const auto result = sdk.xellAddMarkerData(latency, frame, markerType);
        if (markerType == XELL_PRESENT_START) { presentStartResult = result; ++presentMarkerCalls; }
        if (markerType == XELL_PRESENT_END) { presentEndResult = result; ++presentMarkerCalls; }
        if (!window.owned && continuousMode)
            std::cerr << "XeLL policy=borrowed-real-frame-ticket frame=" << frame
                      << " marker=" << int(markerType) << " result=" << int(result) << '\n';
        sdkCheck(result, "xellAddMarkerData");
    }
    void beginBorrowedPresent(std::uint32_t frame) {
        if (!frame || frame <= borrowedSleepFrame)
            throw std::logic_error("Borrowed present IDs must increase and Sleep may run once per ID");
        borrowedSleepReturned = false; borrowedSleepCalls = presentMarkerCalls = 0;
        presentStartResult = presentEndResult = 0;
        borrowedSleepResult = beginBorrowedPresentTiming(frame, [&] { waitFrame(); }, [&](std::uint32_t id) {
            borrowedSleepFrame = id; ++borrowedSleepCalls;
            return static_cast<int>(sdk.xellSleep(latency, id));
        });
        if (continuousMode)
            std::cerr << "XeLL partial policy=borrowed-late-sleep-present-only-partial frame=" << frame
                      << " sleepResult=" << borrowedSleepResult << " calls=" << borrowedSleepCalls << '\n';
        sdkCheck(borrowedSleepResult, "xellSleep(borrowed present)");
        borrowedSleepReturned = true;
    }
    void beginFrame(uint32_t frame) {
        waitFrame();
        sdkCheck(sdk.xellSleep(latency, frame), "xellSleep");
        marker(frame, XELL_SIMULATION_START);
    }
#ifndef XESS_PRESENTER_EMBEDDED
    void render(uint32_t frame, float elapsedMs) {
        beginFrame(frame);
        auto planes = scene(frame);
        marker(frame, XELL_SIMULATION_END);
        marker(frame, XELL_RENDERSUBMIT_START);
        bridge.publishFrame(planes);
        renderPublishedFrame(frame, elapsedMs);
    }
#endif
    // The caller begins the frame, emits simulation/render-submit markers and
    // publishes exactly once. Source production can be CPU upload or native
    // VkImage copy; SDK tagging, queue waits and presentation are identical.
    bool renderPublishedFrame(uint32_t frame, float elapsedMs,
                              const xefg_swapchain_frame_constant_data_t* nativeConstants = nullptr,
                               bool colorPublication = false, bool nativeTiming = false,
                               VkRect2D outputInterpolationRect = {}, xess_vk::FrameCompletion* receipt = nullptr) {
        lastStatusValid = false;
        lastStatusQueryAttempted = false;
        lastStatusQueryResult = 0;
        lastStatusPresentId = 0;
        const bool shared = receipt && receipt->sharedKey.valid();
        const auto publicationConfig = shared ? bridge.config(receipt->sharedKey) : bridge.config();
        const unsigned planeCount = colorPublication || publicationConfig.colorOnly ? 1u : 3u;
        if (fgEnabled && (planeCount != 3 || (continuousMode && !nativeConstants)))
            throw std::logic_error("Continuous XeFG requires three real planes and real frame constants");
        if (receipt && !receipt->key.valid()) throw std::logic_error("Invalid Presenter frame slot key");
        auto* slot = receipt ? &frameSlots[receipt->key.index] : nullptr;
        if (receipt && slot->receipt.key != receipt->key)
            throw std::logic_error("Published frame does not match the prepared Presenter slot");
        auto& allocator = slot ? slot->allocator : this->allocator;
        auto& list = slot ? slot->list : this->list;
        auto& gpuTimestampResources = slot ? slot->timestampResources : this->gpuTimestampResources;
        auto& callerGpuTimestamps = slot ? slot->timestamps : this->callerGpuTimestamps;
        if (receipt) { bridge.queueWaitForProduced(*receipt); slot->receipt = *receipt; }
        else bridge.queueWaitForProduced();
        hrCheck(allocator->Reset(), "Reset allocator");
        hrCheck(list->Reset(allocator.p, nullptr), "Reset list");
        if (callerTimestampSequence != UINT64_MAX && callerTimestampGeneration != UINT64_MAX)
            callerGpuTimestamps.begin(gpuTimestampResources, frame, ++callerTimestampSequence, callerTimestampGeneration,measurementContext);
        Com<ID3D12Resource> backbuffer;
        hrCheck(swapchain->GetBuffer(swapchain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&backbuffer.p)), "GetBuffer");
        xefg_swapchain_d3d12_resource_data_t region{};
        if (fgEnabled) {
            region = backbufferRegionTag({publicationConfig.width, publicationConfig.height}, outputInterpolationRect);
            outputInterpolationRect = {{static_cast<std::int32_t>(region.resourceBase.x),
                                        static_cast<std::int32_t>(region.resourceBase.y)},
                                       {region.resourceSize.x, region.resourceSize.y}};
        }
        if (shared) recordSharedPublishedResources(list.p, bridge, receipt->sharedKey, backbuffer.p, fgEnabled);
        else recordPublishedResources(list.p, bridge, backbuffer.p, fgEnabled, region, [&](const auto& tag) {
            sdkCheck(sdk.xefgSwapChainD3D12TagFrameResource(context, list.p, frame, &tag),
                     tag.type == XEFG_SWAPCHAIN_RES_BACKBUFFER ? "TagBackbufferRegion" : "TagFrameResource");
        });
        if (fgEnabled) {
            xefg_swapchain_frame_constant_data_t constants{};
            if (nativeConstants) constants = *nativeConstants;
            else {
                // The standalone synthetic fixture retains its original constants.
                for (int i = 0; i < 4; ++i) constants.viewMatrix[i*5] = constants.projectionMatrix[i*5] = 1.f;
                constants.motionVectorScaleX = constants.motionVectorScaleY = 1.f;
                constants.frameRenderTime = elapsedMs; constants.resetHistory = frame == 1;
            }
            if (!interpolationHistoryValid || interpolationRectChanged(previousInterpolationRect, outputInterpolationRect))
                constants.resetHistory = 1;
            if (constants.resetHistory) enabledRunFrames = 0;
            sdkCheck(sdk.xefgSwapChainTagFrameConstants(context, frame, &constants), "TagFrameConstants");
            if(constants.resetHistory&&xess_vk::diagnostic_flags::runtimeDiagnostics()){++tuningTotals.historyResets;xess_vk::measurement::increment(xess_vk::measurement::Counter::FgHistoryResets);}
        }
        callerGpuTimestamps.end(gpuTimestampResources);
        hrCheck(list->Close(), "Close frame list");
        ID3D12CommandList* lists[]{list.p};
        callerGpuTimestamps.submitted(gpuTimestampResources);
        bridge.queue()->ExecuteCommandLists(1, lists);
        xess_vk::measurement::submittedCommand(xess_vk::measurement::CopyRole::Color,xess_vk::measurement::Command::D3dCopy);
        if (receipt) { receipt->callerSubmitted = true; slot->receipt = *receipt; }
        if (shared && fgEnabled) tagSharedPublishedResources(bridge, receipt->sharedKey, region, [&](ID3D12GraphicsCommandList* commandList, const auto& tag) {
            sdkCheck(sdk.xefgSwapChainD3D12TagFrameResource(context, commandList, frame, &tag), "TagSharedFrameResource");
        });
        if (!shared) {
            if (receipt) { bridge.queueSignalConsumed(*receipt); slot->receipt = *receipt; }
            else bridge.queueSignalConsumed();
        }
        if (injectSubmissionFailure && frame == 2) throw std::runtime_error("Injected failure after GPU submission");
        sdkCheck(sdk.xefgSwapChainSetPresentId(context, frame), "SetPresentId");
        // Borrowed game rendering has already completed. Only the actual present
        // interval can be marked here; simulation/render markers belong upstream.
        if (window.owned && !bridge.config().colorOnly) marker(frame, XELL_RENDERSUBMIT_END);
        const bool presentMarkers = (window.owned && !bridge.config().colorOnly) || (continuousMode && nativeTiming);
        if (presentMarkers) marker(frame, XELL_PRESENT_START);
        const HRESULT presentResult = [&] {
            xess_vk::CpuWallScope elapsed(dxgiPresentCpu,xess_vk::measurement::Metric::DxgiPresent);
            const UINT flags = syncInterval == 0 && (swapChainFlags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) ? DXGI_PRESENT_ALLOW_TEARING : 0;
            return swapchain->Present(syncInterval, flags);
        }();
        if (presentMarkers) marker(frame, XELL_PRESENT_END);
        if (shared && SUCCEEDED(presentResult)) {
            if (fgEnabled) {
                hrCheck(slot->postAllocator->Reset(), "Reset post-Present allocator");
                hrCheck(slot->postList->Reset(slot->postAllocator.p, nullptr), "Reset post-Present list");
                restoreSharedPublishedResources(slot->postList.p, bridge, receipt->sharedKey);
                hrCheck(slot->postList->Close(), "Close post-Present list");
                ID3D12CommandList* postLists[]{slot->postList.p}; bridge.queue()->ExecuteCommandLists(1, postLists);
                receipt->postPresentSubmitted = true; slot->receipt = *receipt;
            }
            bridge.queueSignalSharedConsumed(receipt->sharedKey, *receipt); slot->receipt = *receipt;
        }
        // Even an occluded present has submitted real tagging/copy work. Its
        // completion and real marker pair must be closed before reporting a
        // temporary lack of visible output.
        const auto tail = nextSdkTailValue(completedValue);
        hrCheck(bridge.queue()->Signal(completed.p, tail), "Presenter queue Signal");
        completedValue = tail;
        if (receipt) { receipt->sdkTailValue = tail; receipt->sdkTailValid = true; slot->receipt = *receipt; }
        hrCheck(presentResult, "Present");
        if (presentResult == DXGI_STATUS_OCCLUDED) {
            enabledRunFrames = 0;
            interpolationHistoryValid = false;
            return false;
        }
        if (presentResult != S_OK) throw std::runtime_error("Present did not display normally, HRESULT=" + std::to_string(presentResult));
        if (fgEnabled) { previousInterpolationRect = outputInterpolationRect; interpolationHistoryValid = true; }
        if (fgEnabled) ++enabledRunFrames; else enabledRunFrames = 0;
        const bool beyondWarmup = continuousMode ? enabledRunFrames > 4 : frame > 4;
        xefg_swapchain_present_status_t status{};
        const auto result = sdk.xefgSwapChainGetLastPresentStatus(context, &status);
        lastStatusQueryAttempted = true;
        lastStatusPresentId = frame;
        lastStatus = status; lastStatusQueryResult = result; lastStatusValid = result == 0;
        if(xess_vk::diagnostic_flags::runtimeDiagnostics())tuningTotals.observe(fgEnabled,result,status.isFrameGenEnabled!=0,status.framesPresented,status.frameGenResult);
        if(fgEnabled){
            using C=xess_vk::measurement::Counter;using xess_vk::measurement::increment;
            increment(C::FgRequestedFrames);
            if(result!=0)increment(C::FgQueryUnavailable);
            else{
                if(status.frameGenResult>0)increment(C::FgResultWarnings);
                if(status.frameGenResult<0)increment(C::FgResultErrors);
                if(status.isFrameGenEnabled){increment(C::FgEnabledFrames);
                    increment(status.framesPresented>1?C::FgMultiOutput:status.framesPresented==1?C::FgSingleOutput:C::FgZeroOutput);}
            }
        }
        if(result<0)xess_vk::measurement::increment(xess_vk::measurement::Counter::SdkErrors);
        if(result>0)xess_vk::measurement::increment(xess_vk::measurement::Counter::SdkWarnings);
        sdkCheck(result, "GetLastPresentStatus");
        ++submitted;
        if (result == 0) {
            if(status.frameGenResult<0)xess_vk::measurement::increment(xess_vk::measurement::Counter::SdkErrors);
            if(status.frameGenResult>0)xess_vk::measurement::increment(xess_vk::measurement::Counter::SdkWarnings);
            sdkPresented += status.framesPresented;
            ++frameResults[status.frameGenResult];
            if (status.frameGenResult == 0 && status.isFrameGenEnabled && status.framesPresented > 1) ++successfulInterpolations;
            else ++noInterpolation;
            if (status.frameGenResult == 0 && !status.isFrameGenEnabled && status.framesPresented == 1) ++validOffPresents;
            if (fgEnabled && beyondWarmup && !(status.frameGenResult == 0 && status.isFrameGenEnabled &&
                                               status.framesPresented == appliedInterpolatedFrames + 1)) ++postWarmupFailures;
            std::cerr << "frame=" << frame << " SDK frames=" << status.framesPresented << " enabled=" << status.isFrameGenEnabled << " result=" << status.frameGenResult << '\n';
        } else { ++noInterpolation; ++statusQueryFailures; if (fgEnabled && beyondWarmup) ++postWarmupFailures; }
        return true;
    }
    bool shutdown() {
        if (shutdownFailed)
            return false;
        if (stopped)
            return true;
        try {
            if (!xess_vk::detail::runPresenterShutdownStages(shutdownFailed, [&] {
                    std::cerr << "Presentation shutdown: D3Dtail enter\n";
                    // Covers work after ExecuteCommandLists and SDK Present,
                    // even if the normal per-frame completion signal was missed.
                    if (completed.p && event) {
                        const uint64_t drainValue = nextSdkTailValue(completedValue);
                        hrCheck(bridge.queue()->Signal(completed.p, drainValue), "Shutdown drain Signal");
                        completedValue = drainValue;
                    }
                    waitFrame();
                    std::cerr << "Presentation shutdown: D3Dtail return\n";
                }, [&] {
                    std::cerr << "Presentation shutdown: proxy release enter\n";
                    gpuTimestampResources.release();
                    for (auto& slot : frameSlots) {
                        slot.timestampResources.release(); slot.list.reset(); slot.allocator.reset();
                        slot.postList.reset(); slot.postAllocator.reset();
                    }
                    list.reset(); allocator.reset(); swapchain.reset();
                    std::cerr << "Presentation shutdown: proxy release return\n";
                }, [&] {
                    return xess_vk::detail::destroyPresenterContexts(sdk, context, latency, shutdownFailed,
                        [](const char* stage) { std::cerr << "Presentation shutdown: " << stage << '\n'; });
                }))
                return false;
            std::cerr << "XeFG / XeLL shutdown succeeded.\n";
            if (event) {
                CloseHandle(event);
                event = nullptr;
            }
            stopped = true;
            return true;
        } catch (...) {
            shutdownFailed = true;
            throw;
        }
    }
    ~Presenter() {
        try {
            if (!shutdown())
                throw std::runtime_error("SDK shutdown failed");
        } catch (const std::exception &error) {
            // Do not run member destructors while outstanding GPU references may exist.
            std::cerr << "Cleanup: " << error.what() << '\n';
            std::cout << "{\"stage\":\"independent-xefg-presenter\",\"testPassed\":false,\"error\":"
                      << quote(std::string("GPU cleanup failed: ") + error.what()) << "}\n"
                      << std::flush;
            if (window.owned)
                std::_Exit(3);
            // Only the standalone fixture may terminate its process. Borrowed
            // presentation retains resources when completion is unproven.
            factory.p = nullptr;
            swapchain.p = nullptr;
            allocator.p = nullptr;
            list.p = nullptr;
            completed.p = nullptr;
            gpuTimestampResources.retain();
            for (auto& slot : frameSlots) {
                slot.timestampResources.retain(); slot.list.p = nullptr; slot.allocator.p = nullptr;
            }
            event = nullptr;
            sdk.fg.h = nullptr;
            sdk.ll.h = nullptr;
        }
    }
};
} // namespace

#ifndef XESS_PRESENTER_EMBEDDED
int main(int argc, char **argv) {
    try {
        std::filesystem::path sdkBin;
        uint32_t frameLimit = 180;
        bool enabled = true;
        bool injectSubmissionFailure = false;
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--sdk-bin" && i+1 < argc) sdkBin = std::filesystem::absolute(argv[++i]);
            else if (arg == "--frames" && i+1 < argc) frameLimit = static_cast<uint32_t>(std::stoul(argv[++i]));
            else if (arg == "--off") enabled = false;
            else if (arg == "--fail-after-submit") injectSubmissionFailure = true;
            else throw std::runtime_error("Usage: --sdk-bin PATH [--frames 180] [--off]");
        }
        if (sdkBin.empty() || frameLimit < 8 || frameLimit > 3600) throw std::runtime_error("SDK path required; frames must be 8..3600");
        xess_vk::InteropBridge bridge({Width, Height, 10000});
        Presenter presenter(bridge, sdkBin, enabled);
        presenter.injectSubmissionFailure = injectSubmissionFailure;
        presenter.initialize();
        auto started = std::chrono::steady_clock::now(), previous = started;
        bool closed = false;
        for (uint32_t frame = 1; frame <= frameLimit && !closed; ++frame) {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_QUIT) { closed = true; break; }
                TranslateMessage(&message); DispatchMessageW(&message);
            }
            if (closed) break;
            auto now = std::chrono::steady_clock::now();
            float elapsedMs = frame == 1 ? 33.333f : std::chrono::duration<float, std::milli>(now-previous).count();
            previous = now;
            presenter.render(frame, elapsedMs);
        }
        bool cleanup = presenter.shutdown();
        bool passed = !closed && cleanup && presenter.submitted == frameLimit && presenter.statusQueryFailures == 0
            && (enabled ? presenter.postWarmupFailures == 0 && presenter.successfulInterpolations >= frameLimit-4 : presenter.validOffPresents == frameLimit);
        double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count();
        std::cout << "{\"stage\":\"independent-xefg-presenter\",\"inputSource\":\"CPU-authored synthetic planes uploaded through Vulkan shared textures\",\"gameIntegrationTested\":false,\"visualInterpolationVerified\":false,\"frameGenerationRequested\":" << (enabled?"true":"false")
            << ",\"submittedFrames\":" << presenter.submitted << ",\"sdkReportedPresentedFrames\":" << presenter.sdkPresented
            << ",\"successfulInterpolationPresents\":" << presenter.successfulInterpolations << ",\"presentsWithoutVerifiedInterpolation\":" << presenter.noInterpolation
            << ",\"validOffPresents\":" << presenter.validOffPresents << ",\"statusQueryFailures\":" << presenter.statusQueryFailures
            << ",\"postWarmupInterpolationFailures\":" << presenter.postWarmupFailures
            << ",\"elapsedSeconds\":" << seconds << ",\"cleanupSucceeded\":" << (cleanup?"true":"false") << ",\"testPassed\":" << (passed?"true":"false") << "}\n";
        return passed ? 0 : 2;
    } catch (const std::exception &error) {
        std::cout << "{\"stage\":\"independent-xefg-presenter\",\"testPassed\":false,\"gameIntegrationTested\":false,\"error\":" << quote(error.what()) << "}\n";
        return 1;
    }
}
#endif
