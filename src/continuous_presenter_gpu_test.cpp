// Self-owned Vulkan producer and HWND, borrowed by the production presenter.
// This exercises the continuous native path; it does not synthesize XeLL
// simulation/render markers or use the standalone Presenter's beginFrame.
#include "color_presenter.hpp"
#include "native_source_fixture.hpp"
#include "continuous_provider.hpp"
#include "continuous_timing.hpp"
#include "fence_wait.hpp"
#include <d3d12.h>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <filesystem>
#include <exception>
#include <functional>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

namespace {
constexpr std::uint32_t offFrames = 4;
constexpr std::uint32_t onWarmup = 4;

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

void queuedColorLayout(xess_vk::testing::SourceOwner& source, VkImageLayout next) {
    source.wait();
    xess_vk::testing::check(source.dev<PFN_vkResetCommandBuffer>("vkResetCommandBuffer")(source.command, 0), "Reset queued color layout");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    xess_vk::testing::check(source.dev<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(source.command, &begin), "Begin queued color layout");
    auto& color = source.frame.planes[0];
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.oldLayout = color.currentLayout; barrier.newLayout = next;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = color.image; barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    source.dev<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(source.command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    xess_vk::testing::check(source.dev<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(source.command), "End queued color layout");
    xess_vk::testing::check(source.dev<PFN_vkResetFences>("vkResetFences")(source.device, 1, &source.completion), "Reset queued color layout fence");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &source.command;
    xess_vk::testing::check(source.dev<PFN_vkQueueSubmit>("vkQueueSubmit")(source.queue, 1, &submit, source.completion), "Submit queued color layout");
    color.currentLayout = next;
}

std::string jsonString(const std::string& value) {
    std::ostringstream out;
    out << '"';
    for (const auto ch : value) {
        if (ch == '"' || ch == '\\') out << '\\';
        if (ch == '\n') out << "\\n";
        else if (ch == '\r') out << "\\r";
        else if (ch == '\t') out << "\\t";
        else if (static_cast<unsigned char>(ch) < 0x20) out << '?';
        else out << ch;
    }
    out << '"';
    return out.str();
}

struct Watchdog {
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE thread = event ? CreateThread(nullptr, 0, [](void* value) -> DWORD {
        if (WaitForSingleObject(static_cast<HANDLE>(value), 120000) == WAIT_TIMEOUT)
            TerminateProcess(GetCurrentProcess(), 124);
        return 0;
    }, event, 0, nullptr) : nullptr;
    Watchdog() {
        if (!event || !thread) {
            if (event) CloseHandle(event);
            throw std::runtime_error("Cannot create fixture watchdog");
        }
    }
    ~Watchdog() {
        SetEvent(event);
        if (WaitForSingleObject(thread, 3000) != WAIT_OBJECT_0)
            TerminateProcess(GetCurrentProcess(), 125);
        CloseHandle(thread);
        CloseHandle(event);
    }
};

struct BorrowedWindow {
    HWND hwnd{};
    bool retained{};
    BorrowedWindow(std::uint32_t width, std::uint32_t height) {
        WNDCLASSW cls{};
        cls.lpfnWndProc = DefWindowProcW;
        cls.hInstance = GetModuleHandleW(nullptr);
        cls.lpszClassName = L"XeSSContinuousBorrowedFixture";
        require(RegisterClassW(&cls) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS,
                "Register fixture window class failed");
        RECT rect{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
        require(AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE), "Adjust fixture window failed");
        hwnd = CreateWindowExW(0, cls.lpszClassName, L"XeSS continuous borrowed OFF / ON / OFF fixture",
                               WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                               rect.right - rect.left, rect.bottom - rect.top,
                               nullptr, nullptr, cls.hInstance, nullptr);
        require(hwnd != nullptr, "Create fixture window failed");
        ShowWindow(hwnd, SW_SHOW);
        // A hidden parent process can override the first ShowWindow through
        // STARTUPINFO. The fixture owns this test window and displays it here.
        ShowWindow(hwnd, SW_SHOW);
    }
    ~BorrowedWindow() {
        if (!retained && hwnd && IsWindow(hwnd)) DestroyWindow(hwnd);
    }
    void pump() const {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            require(message.message != WM_QUIT, "Fixture closed before completing OFF / ON / OFF");
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        require(IsWindow(hwnd) && IsWindowVisible(hwnd), "Borrowed fixture HWND was closed");
    }
};

// Real callback-like publication on the CPU update thread, followed by GPU
// recording/submission on another thread. Uses the actual production Provider.
struct CrossThreadCapture {
    xess_vk::testing::SourceOwner& source;
    xess_vk::continuous_timing::Coordinator& coordinator;
    std::unique_ptr<xess_vk::continuous::Provider> provider;
    VkSemaphore ready{};
    VkFence waited{};
    VkFence copyComplete{};
    VkBuffer readback{};
    VkDeviceMemory readbackMemory{};
    VkCommandBuffer readCommand{};
    VkCommandBuffer copyCommand{};
    std::thread worker;
    std::mutex packetMutex;
    std::condition_variable packetChanged;
    bool packetOpened{}, allowCopy{}, copyFinished{}, presentFinished{}, cancelPacket{};
    std::exception_ptr workerFailure;
    std::uint64_t verifiedPixelPairs{};
    std::uint64_t recordingGeneration{};
    std::uint32_t publishingThread{};
    std::uint32_t recordingThread{};
    bool stopped{};

    CrossThreadCapture(xess_vk::testing::SourceOwner& owner,
                       xess_vk::continuous_timing::Coordinator& timing) : source(owner), coordinator(timing) {
        xess_vk::continuous::Config config;
        config.device = source.device;
        config.queue = source.queue;
        config.queueFamily = source.family;
        config.memory = source.memory;
        config.functions = source.gdpa;
        provider = std::make_unique<xess_vk::continuous::Provider>(config);
        VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        xess_vk::testing::check(source.dev<PFN_vkCreateSemaphore>("vkCreateSemaphore")(
            source.device, &semaphore, nullptr, &ready), "Create cross-thread ready semaphore");
        VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        xess_vk::testing::check(source.dev<PFN_vkCreateFence>("vkCreateFence")(
            source.device, &fence, nullptr, &waited), "Create cross-thread wait fence");
        xess_vk::testing::check(source.dev<PFN_vkCreateFence>("vkCreateFence")(
            source.device, &fence, nullptr, &copyComplete), "Create native-packet copy fence");
        VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        buffer.size = 8;
        buffer.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        xess_vk::testing::check(source.dev<PFN_vkCreateBuffer>("vkCreateBuffer")(
            source.device, &buffer, nullptr, &readback), "Create cross-thread readback");
        VkMemoryRequirements requirements{};
        source.dev<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(
            source.device, readback, &requirements);
        std::uint32_t memoryType = UINT32_MAX;
        for (std::uint32_t index = 0; index < source.memory.memoryTypeCount; ++index) {
            constexpr auto flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            if ((requirements.memoryTypeBits & (1u << index)) &&
                (source.memory.memoryTypes[index].propertyFlags & flags) == flags) {
                memoryType = index;
                break;
            }
        }
        require(memoryType != UINT32_MAX, "No coherent memory for cross-thread pixel oracle");
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = memoryType;
        xess_vk::testing::check(source.dev<PFN_vkAllocateMemory>("vkAllocateMemory")(
            source.device, &allocate, nullptr, &readbackMemory), "Allocate cross-thread readback");
        xess_vk::testing::check(source.dev<PFN_vkBindBufferMemory>("vkBindBufferMemory")(
            source.device, readback, readbackMemory, 0), "Bind cross-thread readback");
        VkCommandBufferAllocateInfo command{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        command.commandPool = source.pool;
        command.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        command.commandBufferCount = 1;
        xess_vk::testing::check(source.dev<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(
            source.device, &command, &readCommand), "Allocate cross-thread readback command");
        xess_vk::testing::check(source.dev<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(
            source.device, &command, &copyCommand), "Allocate native-packet copy command");
    }
    ~CrossThreadCapture() {
        finishWorker(true);
        if (!stopped) {
            provider.release();
            return;
        }
        source.dev<PFN_vkDestroyFence>("vkDestroyFence")(source.device, waited, nullptr);
        source.dev<PFN_vkDestroyFence>("vkDestroyFence")(source.device, copyComplete, nullptr);
        source.dev<PFN_vkDestroySemaphore>("vkDestroySemaphore")(source.device, ready, nullptr);
        source.dev<PFN_vkDestroyBuffer>("vkDestroyBuffer")(source.device, readback, nullptr);
        source.dev<PFN_vkFreeMemory>("vkFreeMemory")(source.device, readbackMemory, nullptr);
    }
    xess_vk::descriptor_image_metadata::DispatchEvidence evidence() const {
        xess_vk::descriptor_image_metadata::DispatchEvidence result;
        result.metadataAccepted = true;
        result.count = 2;
        result.command = {reinterpret_cast<std::uint64_t>(copyCommand), recordingGeneration};
        for (std::size_t index = 0; index < result.count; ++index) {
            const auto& image = source.frame.planes[index + 1];
            const auto& info = image.successfulImageCreateInfo;
            auto& binding = result.bindings[index];
            binding.setIndex = 0;
            binding.binding = index == 0 ? 5 : 7;
            binding.image = {reinterpret_cast<std::uint64_t>(image.image), index + 1};
            binding.viewType = VK_IMAGE_VIEW_TYPE_2D;
            binding.viewFormat = info.format;
            binding.range = {index == 0 ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            binding.declaredLayout = VK_IMAGE_LAYOUT_GENERAL;
            auto& description = binding.imageDescription;
            description.format = info.format;
            description.width = info.extent.width;
            description.height = info.extent.height;
            description.depth = info.extent.depth;
            description.mipLevels = info.mipLevels;
            description.arrayLayers = info.arrayLayers;
            description.samples = info.samples;
            description.usage = info.usage;
            description.tiling = info.tiling;
            description.sharingMode = info.sharingMode;
        }
        return result;
    }
    void stage(xess_vk::game_timing::Stage kind, std::uint32_t engineFrame) {
        xess_vk::game_timing::Event event;
        event.stage = kind;
        event.engineFrame = engineFrame;
        event.threadId = GetCurrentThreadId();
        event.frameKeyValid = true;
        require(coordinator.event(event), "Production Coordinator refused a real fixture boundary");
    }
    void publish(std::uint32_t engineFrame,
                 const xess_vk::frame_input::FrameConstantsSample& constants) {
        publishingThread = GetCurrentThreadId();
        xess_vk::continuous_timing::ScopeSnapshot scope;
        const auto association = coordinator.registrationAssociation(publishingThread, engineFrame, true, &scope);
        require(association.valid && bool(association.ticket) && association.engineFrame == engineFrame &&
                scope.cpuFrameActive && !scope.inSimulation,
                "Post-SIMEND registration did not retain the exact owned CPU frame");
        const auto metadata = evidence();
        xess_vk::continuous::SourcePair pair;
        pair.depth = metadata.bindings[0].image;
        pair.motion = metadata.bindings[1].image;
        pair.invocation = constants.producerToken;
        pair.thread = publishingThread;
        pair.constants = constants;
        pair.timing = association.ticket;
        pair.engineFrame = association.engineFrame;
        pair.frameValid = true;
        pair.valid = true;
        provider->invocation(pair);
    }
    void verifyPixels(const xess_vk::continuous::Bundle& bundle, float expectedDepth) {
        xess_vk::testing::check(source.dev<PFN_vkResetCommandBuffer>("vkResetCommandBuffer")(
            readCommand, 0), "Reset cross-thread readback command");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        xess_vk::testing::check(source.dev<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(
            readCommand, &begin), "Begin cross-thread pixel oracle");
        const std::array images{bundle.depth.image, bundle.motion.image};
        for (std::size_t index = 0; index < images.size(); ++index) {
            VkBufferImageCopy copy{};
            copy.bufferOffset = index * 4;
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {1, 1, 1};
            source.dev<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(
                readCommand, images[index], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &copy);
        }
        VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        host.buffer = readback;
        host.size = VK_WHOLE_SIZE;
        host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        source.dev<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(
            readCommand, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
            0, 0, nullptr, 1, &host, 0, nullptr);
        xess_vk::testing::check(source.dev<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(
            readCommand), "End cross-thread pixel oracle");
        xess_vk::testing::check(source.dev<PFN_vkResetFences>("vkResetFences")(
            source.device, 1, &waited), "Reset cross-thread readback fence");
        VkSubmitInfo read{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        read.commandBufferCount = 1;
        read.pCommandBuffers = &readCommand;
        xess_vk::testing::check(source.dev<PFN_vkQueueSubmit>("vkQueueSubmit")(
            source.queue, 1, &read, waited), "Submit cross-thread pixel oracle");
        xess_vk::testing::check(source.dev<PFN_vkWaitForFences>("vkWaitForFences")(
            source.device, 1, &waited, VK_TRUE, static_cast<std::uint64_t>(source.cfg.timeoutMs) * 1'000'000),
            "Complete cross-thread pixel readback");
        void* mapped{};
        xess_vk::testing::check(source.dev<PFN_vkMapMemory>("vkMapMemory")(
            source.device, readbackMemory, 0, VK_WHOLE_SIZE, 0, &mapped), "Map cross-thread pixel oracle");
        std::array<std::uint32_t, 2> observed{};
        std::memcpy(observed.data(), mapped, 8);
        source.dev<PFN_vkUnmapMemory>("vkUnmapMemory")(source.device, readbackMemory);
        std::uint32_t expectedBits{};
        std::memcpy(&expectedBits, &expectedDepth, 4);
        require(observed[0] == expectedBits && observed[1] == 0,
                "Cross-thread converted depth/motion pixels came from another frame");
        ++verifiedPixelPairs;
    }
    void finishWorker(bool cancel) noexcept {
        if (!worker.joinable()) return;
        {
            std::lock_guard lock(packetMutex);
            cancelPacket = cancel;
            presentFinished = true;
        }
        packetChanged.notify_all();
        worker.join();
    }
    xess_vk::continuous::Bundle capture(std::uint32_t engineFrame,
                                       const xess_vk::frame_input::FrameConstantsSample& constants,
                                       const std::array<VkClearColorValue, 3>& planes,
                                       bool alreadyPublished = false,
                                       const std::function<void()>& whilePacketOpen = {},
                                       bool captureInputs = true, bool queuedInputs = false) {
        require(!worker.joinable(), "Previous native present packet was not closed");
        ++recordingGeneration;
        const auto timing = coordinator.frame(engineFrame);
        require(bool(timing), "Fixture render has no exact CPU timing ticket");
        if (!alreadyPublished)
            publish(engineFrame, constants);
        {
            std::lock_guard lock(packetMutex);
            packetOpened = allowCopy = copyFinished = presentFinished = cancelPacket = false;
            workerFailure = nullptr;
        }
        worker = std::thread([this, engineFrame, timing, planes, captureInputs, queuedInputs] {
            bool opened{};
            std::exception_ptr failure;
            try {
                recordingThread = GetCurrentThreadId();
                stage(xess_vk::game_timing::Stage::RenderSubmitStart, engineFrame);
                source.renderClear(planes);
                if (!queuedInputs) source.wait();
                stage(xess_vk::game_timing::Stage::RenderSubmitEnd, engineFrame);
                stage(xess_vk::game_timing::Stage::NativePresentStart, engineFrame);
                opened = true;
                {
                    std::unique_lock lock(packetMutex);
                    packetOpened = true;
                    packetChanged.notify_all();
                    require(packetChanged.wait_for(lock, std::chrono::milliseconds(source.cfg.timeoutMs),
                                [&] { return allowCopy || cancelPacket; }) && !cancelPacket,
                            "CPU did not release the active native present packet");
                }
                xess_vk::continuous_timing::ScopeSnapshot scope;
                const auto frame = coordinator.recordingAssociation(recordingThread, &scope);
                require(frame.valid && frame.engineFrame == engineFrame && frame.ticket == timing &&
                        scope.inPresent && !scope.inRender && !scope.inSimulation,
                        "Post-render dispatch did not select the exact active present packet");
                xess_vk::testing::check(source.dev<PFN_vkResetCommandBuffer>("vkResetCommandBuffer")(
                    copyCommand, 0), "Reset native-packet copy command");
                VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
                begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                xess_vk::testing::check(source.dev<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(
                    copyCommand, &begin), "Begin native-packet copy command");
                xess_vk::CommandRecordingFacts facts;
                facts.command = reinterpret_cast<std::uint64_t>(copyCommand);
                facts.pool = reinterpret_cast<std::uint64_t>(source.pool);
                facts.contextIncarnation = 17;
                facts.rpRecordingGeneration = recordingGeneration;
                facts.queueFamily = source.family;
                facts.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                facts.beginUsageFlags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                facts.recording = facts.stateKnown = facts.scopeKnown = true;
                const bool captured = provider->record(recordingThread, facts, evidence(), frame, captureInputs);
                require(captured == captureInputs,
                        "Production Provider did not consume the exact source under the requested auxiliary policy");
                xess_vk::testing::check(source.dev<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(
                    copyCommand), "End native-packet copy command");
                if (captured) {
                    xess_vk::testing::check(source.dev<PFN_vkResetFences>("vkResetFences")(
                        source.device, 1, &copyComplete), "Reset native-packet copy fence");
                    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
                    submit.commandBufferCount = 1;
                    submit.pCommandBuffers = &copyCommand;
                    submit.signalSemaphoreCount = 1;
                    submit.pSignalSemaphores = &ready;
                    xess_vk::testing::check(source.dev<PFN_vkQueueSubmit>("vkQueueSubmit")(
                        source.queue, 1, &submit, copyComplete), "Submit native-packet input copies");
                    provider->submitted({reinterpret_cast<std::uint64_t>(copyCommand), recordingGeneration},
                                        std::span<const VkSemaphore>(&ready, 1), source.queue,
                                        recordingGeneration, VK_SUCCESS);
                }
                {
                    std::unique_lock lock(packetMutex);
                    copyFinished = true;
                    packetChanged.notify_all();
                    require(packetChanged.wait_for(lock, std::chrono::milliseconds(source.cfg.timeoutMs),
                                [&] { return presentFinished || cancelPacket; }) && !cancelPacket,
                            "Actual borrowed DXGI present did not finish the native packet");
                }
            } catch (...) {
                failure = std::current_exception();
            }
            if (opened) {
                try {
                    stage(xess_vk::game_timing::Stage::NativePresentEnd, engineFrame);
                    require(!coordinator.recordingAssociation(recordingThread).valid,
                            "NativePresentEnd left a usable recording association");
                } catch (...) {
                    if (!failure) failure = std::current_exception();
                }
            }
            {
                std::lock_guard lock(packetMutex);
                workerFailure = failure;
                copyFinished = true;
            }
            packetChanged.notify_all();
        });
        try {
            {
                std::unique_lock lock(packetMutex);
                require(packetChanged.wait_for(lock, std::chrono::milliseconds(source.cfg.timeoutMs),
                            [&] { return packetOpened || copyFinished; }),
                        "Worker did not reach a native present packet");
                if (workerFailure) std::rethrow_exception(workerFailure);
            }
            if (whilePacketOpen) whilePacketOpen();
            {
                std::unique_lock lock(packetMutex);
                allowCopy = true;
                packetChanged.notify_all();
                require(packetChanged.wait_for(lock, std::chrono::milliseconds(source.cfg.timeoutMs),
                            [&] { return copyFinished; }), "Worker did not submit native-packet copies");
                if (workerFailure) std::rethrow_exception(workerFailure);
            }
            require(recordingThread != publishingThread, "Publication and recording did not cross threads");
            if (!captureInputs) {
                xess_vk::continuous::Bundle skipped;
                skipped.engineFrame = engineFrame;
                skipped.timing = timing;
                return skipped;
            }
            xess_vk::testing::check(source.dev<PFN_vkResetFences>("vkResetFences")(
                source.device, 1, &waited), "Reset cross-thread consumer fence");
            VkPipelineStageFlags stages = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            VkSubmitInfo wait{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            wait.waitSemaphoreCount = 1;
            wait.pWaitSemaphores = &ready;
            wait.pWaitDstStageMask = &stages;
            const auto cutoff = provider->submittedCutoff(source.queue);
            xess_vk::testing::check(source.dev<PFN_vkQueueSubmit>("vkQueueSubmit")(
                source.queue, 1, &wait, waited), "Wait actual cross-thread producer semaphore");
            xess_vk::continuous::Bundle bundle;
            if (queuedInputs) {
                bundle = provider->acquireQueuedForPresent(source.queue, cutoff, coordinator.present(recordingThread));
            } else {
                xess_vk::testing::check(source.dev<PFN_vkWaitForFences>("vkWaitForFences")(
                    source.device, 1, &waited, VK_TRUE, static_cast<std::uint64_t>(source.cfg.timeoutMs) * 1'000'000),
                    "Complete actual cross-thread input fence");
                bundle = provider->acquireForPresent(source.queue, std::span<const VkSemaphore>(&ready, 1), waited, cutoff);
            }
            require(bundle.ready && bundle.engineFrame == engineFrame && bundle.timing == timing &&
                    bundle.invocation == constants.producerToken && bundle.constants.producerToken == constants.producerToken &&
                    bundle.constants.recordingGeneration == recordingGeneration,
                    "Cross-thread bundle lost frame/ticket/constant provenance");
            if (!queuedInputs) verifyPixels(bundle, planes[1].float32[0]);
            return bundle;
        } catch (...) {
            finishWorker(true);
            throw;
        }
    }
    xess_vk::NativeFrame nativeFrame(const xess_vk::continuous::Bundle& bundle) const {
        auto frame = source.frame;
        frame.planes[1] = bundle.depth;
        frame.planes[2] = bundle.motion;
        // Actual same-queue producer submission fence, possibly still pending
        // for the explicit queued bundle path. The slot remains leased.
        frame.producerCompleteFence = copyComplete;
        return frame;
    }
    void release(const xess_vk::continuous::Bundle& bundle) {
        const auto frame = coordinator.present(recordingThread);
        require(frame.valid && frame.engineFrame == bundle.engineFrame && frame.ticket == bundle.timing,
                "Actual DXGI present lost its complete exact-key timing association");
        coordinator.presented(frame, true);
        if (bundle.ready) provider->releaseAfterBridge(bundle.ticket);
        provider->retireFrame(frame);
        finishWorker(false);
        if (workerFailure) std::rethrow_exception(workerFailure);
    }
    void stop() {
        require(!worker.joinable(), "Native present packet remained active at shutdown");
        xess_vk::testing::check(source.dev<PFN_vkResetFences>("vkResetFences")(
            source.device, 1, &waited), "Reset cross-thread drain fence");
        VkSubmitInfo drain{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        xess_vk::testing::check(source.dev<PFN_vkQueueSubmit>("vkQueueSubmit")(
            source.queue, 1, &drain, waited), "Submit cross-thread queue drain");
        xess_vk::testing::check(source.dev<PFN_vkWaitForFences>("vkWaitForFences")(
            source.device, 1, &waited, VK_TRUE, static_cast<std::uint64_t>(source.cfg.timeoutMs) * 1'000'000),
            "Complete cross-thread queue drain");
        xess_vk::testing::check(provider->stop(waited), "Stop actual cross-thread Provider");
        stopped = true;
    }
};

std::uint32_t onFrameArgument(const char* text) {
    const std::string value = text;
    if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos)
        throw std::invalid_argument("--on-frames requires an unsigned integer");
    const auto count = std::stoull(value);
    if (count < 8 || count > 120)
        throw std::invalid_argument("--on-frames must be 8..120 (120-second watchdog)");
    return static_cast<std::uint32_t>(count);
}

xess_vk::frame_input::FrameConstantsSample stationaryConstants(const xess_vk::InteropConfig& config) {
    namespace fi = xess_vk::frame_input;
    fi::FrameConstantsSample sample;
    const auto input = config.extent(xess_vk::Format::DepthR32F);
    sample.validityMask = fi::View | fi::Projection | fi::Jitter | fi::MotionScale | fi::Extents;
    sample.renderSize = {static_cast<std::int32_t>(input.width), static_cast<std::int32_t>(input.height)};
    sample.upscaleSize = {static_cast<std::int32_t>(config.width), static_cast<std::int32_t>(config.height)};
    // Actual fixture camera at (1.5, 2, 3), looking down -Z. A stationary
    // fronto-parallel plane at view Z=-5 gives constant nonlinear inverted depth.
    // Matrices use SDK row-vector, row-major storage, without jitter.
    sample.view = {1,0,0,0, 0,1,0,0, 0,0,1,0, -1.5f,-2,-3,1};
    constexpr float nearPlane = 0.1f;
    constexpr float farPlane = 1000;
    const float yScale = 1.0f / std::tan(3.14159265358979323846f / 6.0f);
    const float xScale = yScale * static_cast<float>(config.height) / static_cast<float>(config.width);
    const float a = nearPlane / (farPlane - nearPlane);
    const float b = nearPlane * farPlane / (farPlane - nearPlane);
    sample.projection = {xScale,0,0,0, 0,-yScale,0,0, 0,0,a,-1, 0,0,b,0};
    sample.motionVectorScale = {1, 1}; // Source is UV displacement; zero for this stationary scene.
    sample.jitter = {0, 0};
    return sample;
}

std::array<VkClearColorValue, 3> stationaryPlanes() {
    std::array<VkClearColorValue, 3> planes{};
    planes[0].float32[0] = 0.08f;
    planes[0].float32[1] = 0.18f;
    planes[0].float32[2] = 0.35f;
    planes[0].float32[3] = 1;
    constexpr float nearPlane = 0.1f;
    constexpr float farPlane = 1000;
    planes[1].float32[0] = (nearPlane * farPlane / 5.0f - nearPlane) / (farPlane - nearPlane);
    // RG16F motion is zero in both axes. No CPU pixel upload participates.
    return planes;
}

void emitStatus(const char* phase, std::uint32_t index, const xess_vk::ColorPresentStatus& status) {
    std::cout << "{\"stage\":\"continuous-borrowed-present-status\",\"phase\":" << jsonString(phase)
              << ",\"phaseFrame\":" << index << ",\"presentId\":" << status.presentId
              << ",\"requestedEnabled\":" << (status.requestedEnabled ? "true" : "false")
              << ",\"sdkEnabled\":" << (status.isFrameGenEnabled ? "true" : "false")
              << ",\"framesPresented\":" << status.framesPresented
              << ",\"fgTuningAvailable\":" << (status.fgTuning.available?"true":"false")
              << ",\"fgThresholdIndex\":" << status.fgTuning.requested.threshold
              << ",\"fgThresholdAttempted\":" << (status.fgTuning.thresholdAttempted?"true":"false")
              << ",\"fgThresholdApplied\":" << (status.fgTuning.thresholdApplied?"true":"false")
              << ",\"fgThresholdResult\":" << status.fgTuning.thresholdResult
              << ",\"fgDebugMode\":" << status.fgTuning.requested.debug
              << ",\"fgDebugAttempted\":" << (status.fgTuning.debugAttempted?"true":"false")
              << ",\"fgDebugApplied\":" << (status.fgTuning.debugApplied?"true":"false")
              << ",\"fgDebugResult\":" << status.fgTuning.debugResult
              << ",\"fgTotalRequested\":" << status.fgOutcomes.requested
              << ",\"fgTotalSingle\":" << status.fgOutcomes.singleOutput
              << ",\"fgTotalMulti\":" << status.fgOutcomes.multiOutput
              << ",\"frameGenResult\":" << status.frameGenResult
              << ",\"queryResult\":" << status.queryResult
              << ",\"sleepFrameId\":" << status.sleepFrameId << ",\"sleepCalls\":" << status.sleepCalls
              << ",\"presentMarkerCalls\":" << status.presentMarkerCalls
              << ",\"sleepResult\":" << status.sleepResult
              << ",\"presentStartResult\":" << status.presentStartResult
              << ",\"presentEndResult\":" << status.presentEndResult
              << ",\"consecutiveEnabledPresents\":" << status.consecutiveEnabledPresents
              << ",\"partialXeLLTiming\":" << (status.partialXeLLTiming ? "true" : "false")
              << ",\"latencyCapability\":" << (status.latencyTransitionAdmitted ? "true" : "false")
              << ",\"occluded\":" << (status.occluded ? "true" : "false")
              << ",\"timingPolicy\":" << jsonString(status.timingPolicy ? status.timingPolicy : "")
              << ",\"valid\":" << (status.valid ? "true" : "false") << "}\n";
    std::cout.flush();
}

void reportLimits(bool fullFixtureTiming = true) {
    std::cout << ",\"partialXeLL\":false,\"fullFixtureTiming\":" << (fullFixtureTiming ? "true" : "false")
              << ",\"fullLatencyIntegration\":false"
              << ",\"gameTested\":false,\"gameMotionSemanticsVerified\":false"
              << ",\"visualInterpolationVerified\":false,\"dynamicMotionQualityVerified\":false"
              << ",\"syntheticSimulationRenderMarkers\":false,\"cpuUploadedPixels\":false";
}

xess_vk::continuous_timing::Callbacks timingCallbacks(xess_vk::ColorPresenter& presenter) {
    xess_vk::continuous_timing::Callbacks callbacks;
    callbacks.user = &presenter;
    callbacks.contextGeneration = [](void* owner) {
        return static_cast<xess_vk::ColorPresenter*>(owner)->timingContextGeneration();
    };
    callbacks.nextSdkId = [](void* owner) {
        return static_cast<xess_vk::ColorPresenter*>(owner)->nextSdkId();
    };
    callbacks.begin = [](void* owner, std::uint32_t id) {
        return static_cast<xess_vk::ColorPresenter*>(owner)->beginGameFrame(id);
    };
    callbacks.simulationEnd = [](void* owner, xess_vk::FrameTimingTicket ticket) {
        static_cast<xess_vk::ColorPresenter*>(owner)->simulationEnd(ticket);
    };
    callbacks.renderBegin = [](void* owner, xess_vk::FrameTimingTicket ticket) {
        static_cast<xess_vk::ColorPresenter*>(owner)->renderSubmitBegin(ticket);
    };
    callbacks.renderEnd = [](void* owner, xess_vk::FrameTimingTicket ticket) {
        static_cast<xess_vk::ColorPresenter*>(owner)->renderSubmitEnd(ticket);
    };
    callbacks.abandon = [](void* owner, xess_vk::FrameTimingTicket ticket) {
        static_cast<xess_vk::ColorPresenter*>(owner)->abandonFrameTiming(ticket);
    };
    return callbacks;
}

// The CPU wrapper/late-callback fixtures prove when the owner may retire.
// This sequence separately exercises real SDK refusal and recovery on that
// same owner; its future SDK callback has returned before stop is requested.
void untimedRecovery(xess_vk::ColorPresenter& presenter, xess_vk::InteropBridge& bridge,
                     xess_vk::testing::SourceOwner& source, const BorrowedWindow& window,
                     std::uint32_t& submitted, std::uint32_t& acceptedOff) {
    namespace ct=xess_vk::continuous_timing;
    using Stage=xess_vk::game_timing::Stage;
    ct::Coordinator coordinator(timingCallbacks(presenter));
    std::uint32_t lastId{};
    const auto output=[&](const char* phase) {
        window.pump();bridge.waitUntilConsumed();source.render(submitted+1);
        const auto id=presenter.presentUntimedOff(source.frame.planes[0],source.frame.producerQueue,
                                                 source.frame.producerCompleteFence);
        const auto status=presenter.lastPresentStatus();
        ++submitted;++acceptedOff;
        require(id>lastId && status.sdkPresentId==id && status.presentId==id && status.untimedOff &&
                status.valid && status.framesPresented==1 && !status.isFrameGenEnabled &&
                !status.requestedEnabled && !status.sleepCalls && !status.presentMarkerCalls &&
                presenter.frames()==submitted,"Recovered ordinary OFF did not present one frame with a fresh SDK ID");
        lastId=id;coordinator.untimedPresented(id);emitStatus(phase,submitted,status);
    };
    output("untimed-startup");output("untimed-startup");
    xess_vk::game_timing::Event retained;
    retained.stage=Stage::SimulationStart;retained.engineFrame=99;retained.threadId=GetCurrentThreadId();retained.frameKeyValid=true;
    require(coordinator.event(retained),"Retained future real SDK begin was refused");
    const auto retainedTicket=coordinator.frame(99);
    const auto retainedNextId=presenter.nextSdkId();
    const auto cancelledBefore=presenter.timingStats().cancelled;
    const auto abandonedBeforeRetained=coordinator.stats().abandoned;
    require(retainedTicket && retainedTicket.sdkId>lastId && presenter.timingStats().occupied==1 &&
            coordinator.stats().pending==0,"Retained future reservation was not owned after its SDK callback returned");
    bool retainedRefused{};
    try {
        presenter.presentUntimedOff(source.frame.planes[0],source.frame.producerQueue,
                                    source.frame.producerCompleteFence);
    } catch(const xess_vk::ColorPresenterUnavailable&) { retainedRefused=true; }
    require(retainedRefused && presenter.frames()==submitted && presenter.nextSdkId()==retainedNextId &&
            presenter.timingStats().occupied==1 && coordinator.frame(99)==retainedTicket &&
            presenter.timingStats().cancelled==cancelledBefore,
            "Untimed refusal consumed, advanced or cancelled the retained future ticket");
    retained.stage=Stage::SimulationEnd;
    require(coordinator.event(retained),"Retained real SDK simulation-end marker failed");
    retained.stage=Stage::RenderSubmitStart;
    require(coordinator.event(retained),"Retained real SDK render-start marker failed");
    window.pump();bridge.waitUntilConsumed();source.render(submitted+1);
    retained.stage=Stage::RenderSubmitEnd;
    require(coordinator.event(retained),"Retained real SDK render-end marker failed");
    retained.stage=Stage::NativePresentStart;
    require(coordinator.event(retained),"Retained native-present scope was refused");
    const auto retainedFrame=coordinator.present(retained.threadId);
    require(retainedFrame.valid && retainedFrame.engineFrame==99 && retainedFrame.ticket==retainedTicket,
            "Completed retained frame lost its original future ticket");
    require(coordinator.stats().abandoned==abandonedBeforeRetained,
            "Future ticket was abandoned before its timed OFF present");
    presenter.present(source.frame.planes[0],source.frame.producerQueue,
                      source.frame.producerCompleteFence,retainedTicket);
    ++submitted;++acceptedOff;
    const auto retainedStatus=presenter.lastPresentStatus();
    require(retainedStatus.valid && retainedStatus.queryResult==0 && retainedStatus.frameGenResult==0 &&
            retainedStatus.presentId==retainedTicket.sdkId && retainedStatus.sdkPresentId==retainedTicket.sdkId &&
            !retainedStatus.untimedOff && !retainedStatus.requestedEnabled && !retainedStatus.isFrameGenEnabled &&
            retainedStatus.framesPresented==1 && retainedStatus.completeFrameTiming &&
            retainedStatus.sleepFrameId==retainedTicket.sdkId && retainedStatus.sleepCalls==1 &&
            retainedStatus.presentMarkerCalls==2 && retainedStatus.totalMarkerCalls==6 &&
            retainedStatus.sleepResult==0 && retainedStatus.simulationStartResult==0 && retainedStatus.simulationEndResult==0 &&
            retainedStatus.renderStartResult==0 && retainedStatus.renderEndResult==0 &&
            retainedStatus.presentStartResult==0 && retainedStatus.presentEndResult==0 && presenter.frames()==submitted,
            "Retained original ticket did not complete one real timed OFF frame");
    coordinator.presented(retainedFrame,true);
    retained.stage=Stage::NativePresentEnd;
    require(coordinator.event(retained) && !presenter.timingStats().occupied && !coordinator.stats().occupied &&
            presenter.timingStats().cancelled==cancelledBefore && coordinator.stats().timedPresents==1,
            "Completed retained ticket was cancelled or left owned timing reservations");
    lastId=retainedTicket.sdkId;emitStatus("retained-future-timed-off",submitted,retainedStatus);
    std::cout << "{\"stage\":\"real-sdk-reserved-ticket-to-timed-off\",\"passed\":true"
              << ",\"selfOwnedSdkSequence\":true,\"untimedUnavailableWithoutIdAdvance\":true"
              << ",\"originalTicketRetained\":true,\"timedOffSingleFrame\":true"
              << ",\"gamePathVerified\":false,\"startupLayerGateTested\":false}\n";
    xess_vk::game_timing::Event future;
    future.stage=Stage::SimulationStart;future.engineFrame=100;future.threadId=GetCurrentThreadId();future.frameKeyValid=true;
    require(coordinator.event(future),"Future real SDK begin was refused");
    const auto ticket=coordinator.frame(100);
    require(ticket && ticket.sdkId>lastId && presenter.timingStats().occupied==1 &&
            coordinator.stats().pending==0,"Future reservation did not remain owned after its SDK callback returned");
    future.stage=Stage::SimulationEnd;
    require(coordinator.event(future),"Future real SDK simulation-end marker failed");
    const auto nextId=presenter.nextSdkId();
    bool refused{};
    try {
        presenter.presentUntimedOff(source.frame.planes[0],source.frame.producerQueue,
                                    source.frame.producerCompleteFence);
    } catch(const xess_vk::ColorPresenterUnavailable&) { refused=true; }
    require(refused && presenter.frames()==submitted && presenter.nextSdkId()==nextId &&
            presenter.timingStats().occupied==1,"Untimed OFF bypassed a real future reservation or advanced its SDK ID");
    const auto abandonedBeforeStop=coordinator.stats().abandoned;
    coordinator.stop();
    require(!presenter.timingStats().occupied && !coordinator.stats().occupied &&
            coordinator.stats().abandoned==abandonedBeforeStop+1,
            "Timing owner failed to return exactly its own future reservation");
    lastId=ticket.sdkId;
    for(unsigned index=0;index<3;++index)output("untimed-after-owner-retired");
    require(coordinator.stats().timedPresents==1 && coordinator.stats().untimedPresents==2,
            "Retired ordinary OFF was counted as continuous timing success");
    future.stage=Stage::SimulationStart;future.engineFrame=101;
    require(!coordinator.event(future),"Stopped timing accepted a new reservation after OFF recovered");
    std::cout << "{\"stage\":\"real-sdk-untimed-reservation-recovery\",\"passed\":true"
              << ",\"startupOffPresents\":2,\"realFutureReservationRefusedUntimed\":true"
              << ",\"ownerReservationsReturned\":1,\"recoveredOffPresents\":3,\"lastSdkId\":" << lastId
              << ",\"timingAdmissionStayedClosed\":true,\"pendingCallbackLifetimeCoveredByCpuFixture\":true}\n";
}
// Keep all borrowed device/queue identities; only this native color is resized.
void resizeSourceColor(xess_vk::testing::SourceOwner& source,const xess_vk::InteropConfig& config) {
    source.wait();
    require(source.drainQueue(config.timeoutMs),"Source queue did not finish before resize");
    auto imageInfo=source.frame.planes[0].successfulImageCreateInfo;
    imageInfo.extent={config.width,config.height,1};
    VkImage replacement{};
    VkDeviceMemory allocation{};
    try {
        xess_vk::testing::check(source.dev<PFN_vkCreateImage>("vkCreateImage")(
            source.device,&imageInfo,nullptr,&replacement),"Create resized source color");
        VkMemoryRequirements memory{};
        source.dev<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(source.device,replacement,&memory);
        std::uint32_t type=UINT32_MAX;
        for(std::uint32_t index=0;index<source.memory.memoryTypeCount;++index) {
            if((memory.memoryTypeBits&(1u<<index)) &&
               (source.memory.memoryTypes[index].propertyFlags&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                type=index;
                break;
            }
        }
        require(type!=UINT32_MAX,"No device-local memory for resized source color");
        VkMemoryAllocateInfo info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        info.allocationSize=memory.size;
        info.memoryTypeIndex=type;
        xess_vk::testing::check(source.dev<PFN_vkAllocateMemory>("vkAllocateMemory")(
            source.device,&info,nullptr,&allocation),"Allocate resized source color");
        xess_vk::testing::check(source.dev<PFN_vkBindImageMemory>("vkBindImageMemory")(
            source.device,replacement,allocation,0),"Bind resized source color");
    } catch(...) {
        if(replacement)source.dev<PFN_vkDestroyImage>("vkDestroyImage")(source.device,replacement,nullptr);
        if(allocation)source.dev<PFN_vkFreeMemory>("vkFreeMemory")(source.device,allocation,nullptr);
        throw;
    }
    source.dev<PFN_vkDestroyImage>("vkDestroyImage")(source.device,source.frame.planes[0].image,nullptr);
    source.dev<PFN_vkFreeMemory>("vkFreeMemory")(source.device,source.allocation[0],nullptr);
    source.frame.planes[0].image=replacement;
    source.frame.planes[0].successfulImageCreateInfo=imageInfo;
    source.allocation[0]=allocation;
    source.cfg=config;
    source.initializedImages=false;
}
void verifySharedColor(xess_vk::InteropBridge& bridge,std::uint8_t expectedRed) {
    bridge.waitUntilConsumed();
    ID3D12CommandAllocator* allocator{};
    ID3D12GraphicsCommandList* list{};
    ID3D12Resource* readback{};
    ID3D12Fence* fence{};
    const auto check=[](HRESULT result,const char* operation) {
        require(SUCCEEDED(result),operation);
    };
    auto* device=bridge.device();
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"Create pixel allocator");
    check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator,nullptr,IID_PPV_ARGS(&list)),"Create pixel list");
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width=256;desc.Height=desc.DepthOrArraySize=desc.MipLevels=desc.SampleDesc.Count=1;
    desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,
          nullptr,IID_PPV_ARGS(&readback)),"Create pixel readback");
    check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"Create pixel fence");
    const auto event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    require(event!=nullptr,"Create pixel event failed");
    auto* color=bridge.texture(xess_vk::Format::ColorRGBA8);
    D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition={color,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE};
    list->ResourceBarrier(1,&barrier);
    D3D12_TEXTURE_COPY_LOCATION from{};from.pResource=color;from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION to{};to.pResource=readback;to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    to.PlacedFootprint.Footprint={DXGI_FORMAT_R8G8B8A8_UNORM,1,1,1,256};
    D3D12_BOX box{0,0,0,1,1,1};
    list->CopyTextureRegion(&to,0,0,0,&from,&box);
    std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);
    list->ResourceBarrier(1,&barrier);
    check(list->Close(),"Close pixel list");
    ID3D12CommandList* submitted[]{list};
    bridge.queue()->ExecuteCommandLists(1,submitted);
    check(bridge.queue()->Signal(fence,1),"Signal pixel completion");
    xess_vk::detail::waitForD3D12Fence(fence,1,event,10000,"Shared color pixel completion");
    void* mapped{};D3D12_RANGE read{0,4};
    check(readback->Map(0,&read,&mapped),"Map shared color pixel");
    const auto* bytes=static_cast<const std::uint8_t*>(mapped);
    const bool matches=bytes[0]==expectedRed && bytes[3]==255;
    D3D12_RANGE written{0,0};readback->Unmap(0,&written);
    CloseHandle(event);
    fence->Release();readback->Release();list->Release();allocator->Release();
    require(matches,"Resized shared color contains stale pixels");
}
void windowLifecycle(xess_vk::ColorPresenter& presenter,xess_vk::InteropBridge& bridge,
                     xess_vk::testing::SourceOwner& source,BorrowedWindow& window,
                     std::uint32_t onFrames,std::uint32_t& submitted,
                     std::uint32_t& acceptedOn,std::uint32_t& acceptedOff) {
    const auto context=presenter.timingContextGeneration();
    const auto device=source.device;
    auto* d3d=bridge.device();
    auto* queue=bridge.queue();
    std::uint32_t lastId{},occludedPresents{};
    xess_vk::FrameTimingTicket reserved;
    const auto phase=[&](const char* name,bool enabled,std::uint32_t count,bool mayBeOccluded=false) {
        for(std::uint32_t index=1;index<=count;++index) {
            window.pump();
            const auto ticket=reserved?reserved:presenter.beginGameFrame();
            reserved={};
            require(bool(ticket),"Window lifecycle could not reserve a real frame");
            auto constants=stationaryConstants(source.cfg);
            constants.producerToken=submitted+1;
            presenter.simulationEnd(ticket);
            presenter.renderSubmitBegin(ticket);
            bridge.waitUntilConsumed();
            auto planes=stationaryPlanes();
            planes[0].float32[0]=source.cfg.width==960?1.0f:0.0f;
            source.renderClear(planes);
            presenter.renderSubmitEnd(ticket);
            presenter.presentBundle(source.frame,constants,enabled,enabled && index==1,ticket);
            ++submitted;
            const auto status=presenter.lastPresentStatus();
            emitStatus(name,index,status);
            require(status.sdkPresentId>lastId && presenter.frames()==submitted &&
                    presenter.timingContextGeneration()==context && source.device==device &&
                    bridge.device()==d3d && bridge.queue()==queue && status.latencyTransitionAdmitted &&
                    status.completeFrameTiming && status.totalMarkerCalls==6 && status.sleepCalls==1 &&
                    status.presentMarkerCalls==2 && status.presentStartResult==0 && status.presentEndResult==0,
                    "Resize/minimize changed an owner or lost a real frame's monotonic ID/markers");
            lastId=status.sdkPresentId;
            if(status.occluded) {
                require(mayBeOccluded && !status.valid && !status.framesPresented,
                        "Occluded present was counted as a visible frame");
                ++occludedPresents;
            } else {
                require(status.valid && status.queryResult==0 && status.isFrameGenEnabled==enabled,
                        "Window lifecycle did not restore valid SDK output");
                if(enabled && index>onWarmup) {
                    require(status.frameGenResult==0 && status.interpolatedFrames>=1 &&
                            status.framesPresented==status.interpolatedFrames+unsigned(!(status.fgTuning.debugApplied&&status.fgTuning.requested.debug==3)),
                            "Resize/restore ON did not recover its configured SDK frames after warmup");
                    ++acceptedOn;
                } else if(!enabled) {
                    require(status.frameGenResult==0 && status.framesPresented==1,
                            "Window lifecycle OFF did not complete one SDK frame");
                    ++acceptedOff;
                }
            }
        }
        if(!mayBeOccluded)verifySharedColor(bridge,source.cfg.width==960?255:0);
    };
    const auto resize=[&](std::uint32_t width,std::uint32_t height) {
        // An actual pre-resize Sleep/SIM-start ticket must survive ResizeBuffers.
        reserved=presenter.beginGameFrame();
        require(bool(reserved),"Could not reserve pre-resize future ticket");
        auto config=source.cfg;
        config.width=width;
        config.height=height;
        RECT client{0,0,static_cast<LONG>(width),static_cast<LONG>(height)};
        require(AdjustWindowRect(&client,WS_OVERLAPPEDWINDOW,FALSE),"Adjust resized fixture client failed");
        require(SetWindowPos(window.hwnd,nullptr,0,0,client.right-client.left,client.bottom-client.top,
                             SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE),"Resize fixture window failed");
        presenter.resizeOutput(width,height);
        resizeSourceColor(source,config);
        const auto desc=bridge.texture(xess_vk::Format::ColorRGBA8)->GetDesc();
        require(desc.Width==width && desc.Height==height &&
                presenter.timingContextGeneration()==context && bridge.frameValue()==submitted,
                "Output resize changed the context/fence sequence or kept a stale shared extent");
        std::cout << "{\"stage\":\"window-output-resized\",\"contextGeneration\":" << context
                  << ",\"width\":" << width << ",\"height\":" << height
                  << ",\"reservedSdkId\":" << reserved.sdkId << ",\"bridgeFenceValue\":" << bridge.frameValue() << "}\n";
    };
    phase("window-prime-off",false,2);
    phase("window-on-before-resize",true,onFrames);
    resize(800,450);
    phase("window-on-after-resize",true,onFrames);
    resize(960,540);
    phase("window-on-after-resize-back",true,onFrames);
    ShowWindow(window.hwnd,SW_MINIMIZE);
    window.pump();
    require(IsIconic(window.hwnd),"Fixture did not minimize");
    phase("window-minimized-off",false,2,true);
    ShowWindow(window.hwnd,SW_RESTORE);
    window.pump();
    require(!IsIconic(window.hwnd),"Fixture did not restore");
    phase("window-on-after-restore",true,onFrames);
    phase("window-off-after",false,2);
    require(acceptedOn==4*(onFrames-onWarmup) && presenter.timingStats().occupied==0,
            "Window lifecycle lost recovered ON outputs or left a timing reservation");
    std::cout << "{\"stage\":\"same-context-window-lifecycle\",\"passed\":true"
              << ",\"contextGeneration\":" << context << ",\"occludedPresents\":" << occludedPresents
              << ",\"verifiedOnConfiguredFramePresents\":" << acceptedOn
                  << ",\"verifiedOnTwoFramePresents\":" << acceptedOn
              << ",\"newSharedColorPixelsVerified\":true,\"preResizeReservationPreserved\":true"
              << ",\"borrowedDeviceUnchanged\":true,\"ll1OwnerUnchanged\":true,\"providerUsed\":false}\n";
}
} // namespace

int main(int argc, char** argv) {
    bool cleanShutdown{};
    bool borrowedWindowSurvived{};
    bool borrowedDeviceSurvived{};
    bool untimedRecoveryOnly{};
    bool windowLifecycleOnly{};
    // The game requests IMMEDIATE while its FG menu is on: interval 0 with tearing.
    bool vsync = true;unsigned verifiedTwoFramePresents{};
    // Each fixture initializes a fixed capacity, matching a fresh game launch.
    std::uint32_t interpolatedFrames = 1, appliedInterpolatedFrames{};
    std::uint32_t acceptedOn{}, acceptedOff{}, submitted{}, queuedOffPresents{}, queuedOnPresents{};
    try {
        std::filesystem::path sdkBin;
        std::uint32_t onFrames = 32;
        for (int index = 1; index < argc; ++index) {
            const std::string argument = argv[index];
            if (argument == "--sdk-bin" && index + 1 < argc)
                sdkBin = std::filesystem::absolute(argv[++index]);
            else if (argument == "--on-frames" && index + 1 < argc)
                onFrames = onFrameArgument(argv[++index]);
            else if (argument == "--untimed-recovery-only")
                untimedRecoveryOnly = true;
            else if (argument == "--window-lifecycle-only")
                windowLifecycleOnly = true;
            else if (argument == "--no-vsync")
                vsync = false;
            else if (argument == "--interpolated-frames" && index + 1 < argc) {
                const std::string value = argv[++index];
                if (value == "max") interpolatedFrames = xess_vk::ColorPresenter::useMaxSupportedInterpolatedFrames;
                else if (value == "1" || value == "2" || value == "3") interpolatedFrames = static_cast<std::uint32_t>(value[0] - '0');
                else throw std::invalid_argument("--interpolated-frames must be 1, 2, 3 or max");
            } else
                throw std::invalid_argument("Usage: --sdk-bin PATH [--on-frames 32] [--no-vsync] [--interpolated-frames 1|2|3|max] [--untimed-recovery-only | --window-lifecycle-only]");
        }
        require(!untimedRecoveryOnly || !windowLifecycleOnly,"Choose one fixture mode");
        require(!sdkBin.empty(), "--sdk-bin is required");
        require(!GetModuleHandleW(L"xess-vulkan-observer.dll"), "Fixture must not load the game observer");
        Watchdog watchdog;
        xess_vk::InteropConfig config{960, 540, 10000, 480, 270, false};
        xess_vk::testing::SourceOwner source;
        source.initialize(config, !windowLifecycleOnly);
        BorrowedWindow window(config.width, config.height);
        auto sample = stationaryConstants(config);
        auto planes = stationaryPlanes();
        std::uint64_t nativeFrames{};
        const auto started = std::chrono::steady_clock::now();
        source.retainForPossibleBorrowedChildren = true;
        window.retained = true;
        {
            auto bridgeConfig = config;
            bridgeConfig.colorOnly = true;
            xess_vk::InteropBridge bridge(bridgeConfig, source.context());
            auto* color = bridge.texture(xess_vk::Format::ColorRGBA8);
            auto* device = bridge.device();
            auto* queue = bridge.queue();
            require(color && !bridge.texture(xess_vk::Format::DepthR32F) &&
                    !bridge.texture(xess_vk::Format::MotionRG16F), "Color-only bridge allocated auxiliary textures early");
            {
                require(source.deviceInfo.queueCreateInfoCount == 1 && source.queueInfo.queueCount == 1 &&
                        bridge.frameValue() == 0 && source.drainQueue(config.timeoutMs),
                        "Fresh LL1 needs the sole Vulkan queue completed and no submitted bridge work");
                xess_vk::ColorPresenter presenter(bridge, sdkBin, window.hwnd, vsync, true, true, interpolatedFrames);
                std::cout << "{\"stage\":\"fresh-borrowed-ll1-initialized\",\"soleVulkanQueueDrained\":true,\"priorBridgeSubmissions\":0,\"freshContext\":true,\"laterSleepModeChanges\":false}\n";
                if(untimedRecoveryOnly) {
                    untimedRecovery(presenter,bridge,source,window,submitted,acceptedOff);
                    presenter.shutdown();cleanShutdown=true;
                } else if(windowLifecycleOnly) {
                    windowLifecycle(presenter,bridge,source,window,onFrames,submitted,acceptedOn,acceptedOff);
                    presenter.shutdown();cleanShutdown=true;
                } else {
                const auto nextIdBefore = presenter.nextSdkId();
                bool missingQueuedTicketRefused{};
                try { presenter.presentQueuedOff(source.frame.planes[0], source.queue, source.completion); }
                catch (const std::logic_error&) { missingQueuedTicketRefused = true; }
                require(missingQueuedTicketRefused && presenter.nextSdkId() == nextIdBefore && bridge.frameValue() == 0,
                        "Continuous queued OFF accepted no ticket or advanced the SDK sequence on refusal");
                for (const auto bad : {xess_vk::FrameTimingTicket{}, xess_vk::FrameTimingTicket{1,1,false}}) {
                    bool refused{};
                    try { presenter.presentBundleQueued(source.frame, sample, false, bad, {}); }
                    catch (const std::logic_error&) { refused=true; }
                    require(refused && presenter.nextSdkId()==nextIdBefore && bridge.frameValue()==0,
                            "Queued ON accepted missing/latency-disabled timing or advanced on refusal");
                }
                xess_vk::continuous_timing::Coordinator coordinator(timingCallbacks(presenter));
                CrossThreadCapture capture(source, coordinator);
                ID3D12Resource* depth{};
                ID3D12Resource* motion{};
                const auto bootstrapFrame = [&](std::uint32_t engineFrame) {
                    capture.stage(xess_vk::game_timing::Stage::NativePresentStart, engineFrame);
                    const auto association = coordinator.present(GetCurrentThreadId());
                    require(association.valid && !association.ticket && association.bootstrapSdkId == engineFrame,
                            "Coordinator bootstrap engine-to-SDK mapping disagrees with real SDK sequence");
                    presenter.presentBootstrap(source.frame.planes[0], source.frame.producerQueue,
                                               source.frame.producerCompleteFence, association.bootstrapSdkId);
                    coordinator.presented(association, true);
                    capture.stage(xess_vk::game_timing::Stage::NativePresentEnd, engineFrame);
                };
                // Bootstrap occurs before any timing ID is reserved. It uses the
                // same SDK sequence with FG disabled and emits no markers.
                source.renderClear(planes);
                bootstrapFrame(1);
                ++submitted;
                const auto bootstrap = presenter.lastPresentStatus();
                emitStatus("bootstrap", 1, bootstrap);
                require(bootstrap.valid && bootstrap.framesPresented == 1 && !bootstrap.isFrameGenEnabled &&
                        bootstrap.sleepCalls == 0 && bootstrap.presentMarkerCalls == 0,
                        "Untimed bootstrap OFF failed or fabricated XeLL markers");
                // Reserve a future frame before older bootstrap presents arrive.
                // All IDs use the same explicit engine-to-SDK ordering; tickets
                // cannot enable FG until a complete timed frame was presented.
                capture.stage(xess_vk::game_timing::Stage::SimulationStart, 4);
                const auto ahead = coordinator.frame(4);
                require(bool(ahead) && !ahead.latencyEnabled, "CPU-ahead bootstrap enabled XeLL prematurely");
                sample = stationaryConstants(config);
                planes = stationaryPlanes();
                sample.producerToken = 4;
                planes[1].float32[0] = 0.03125f;
                const auto firstAheadSample = sample;
                const auto firstAheadPlanes = planes;
                capture.stage(xess_vk::game_timing::Stage::SimulationEnd, 4);
                capture.publish(4, firstAheadSample);
                for (std::uint32_t oldId : {2u, 3u}) {
                    bootstrapFrame(oldId);
                    ++submitted;
                    const auto old = presenter.lastPresentStatus();
                    emitStatus("older-bootstrap", oldId, old);
                    require(old.presentId == oldId && old.valid && old.framesPresented == 1 &&
                            !old.isFrameGenEnabled && old.sleepCalls == 0 && old.presentMarkerCalls == 0,
                            "CPU-ahead timing blocked or mislabeled an older bootstrap frame");
                }
                xess_vk::FrameTimingTicket secondAhead;
                xess_vk::frame_input::FrameConstantsSample secondAheadSample;
                std::array<VkClearColorValue, 3> secondAheadPlanes{};
                bool overlappingCpuAheadVerified{};
                const auto primingBundle = capture.capture(4, firstAheadSample, firstAheadPlanes, true, [&] {
                    require(coordinator.scopeSnapshot(capture.recordingThread).inPresent,
                            "Worker K packet was not active before CPU K+1 update");
                    capture.stage(xess_vk::game_timing::Stage::SimulationStart, 5);
                    secondAhead = coordinator.frame(5);
                    require(bool(secondAhead) && !secondAhead.latencyEnabled,
                            "Second CPU-ahead packet enabled LL before a complete present");
                    // This actual CPU scene update is inside the K+1 SIM interval.
                    secondAheadSample = stationaryConstants(config);
                    secondAheadSample.producerToken = 5;
                    secondAheadPlanes = stationaryPlanes();
                    secondAheadPlanes[1].float32[0] = 0.0625f;
                    capture.stage(xess_vk::game_timing::Stage::SimulationEnd, 5);
                    capture.publish(5, secondAheadSample);
                    const auto workerFrame = coordinator.recordingAssociation(capture.recordingThread);
                    const auto cpuFrame = coordinator.registrationAssociation(GetCurrentThreadId(), 5, true);
                    require(workerFrame.valid && workerFrame.engineFrame == 4 && workerFrame.ticket == ahead &&
                            cpuFrame.valid && cpuFrame.engineFrame == 5 && cpuFrame.ticket == secondAhead,
                            "CPU K+1 replaced worker K's exact present-packet association");
                    overlappingCpuAheadVerified = true;
                });
                const auto primingFrame = capture.nativeFrame(primingBundle);
                presenter.presentBundle(primingFrame, primingBundle.constants, false, true, ahead);
                capture.release(primingBundle);
                ++submitted;
                const auto primed = presenter.lastPresentStatus();
                emitStatus("timing-prime", 1, primed);
                require(primed.presentId == 4 && primed.completeFrameTiming && primed.totalMarkerCalls == 6 &&
                        primed.valid && !primed.isFrameGenEnabled && primed.framesPresented == 1,
                        "First real timing packet failed to prime OFF presentation");
                const auto secondBundle = capture.capture(5, secondAheadSample, secondAheadPlanes, true);
                const auto secondFrame = capture.nativeFrame(secondBundle);
                presenter.presentBundle(secondFrame, secondBundle.constants, false, false, secondAhead);
                capture.release(secondBundle);
                ++submitted;
                const auto secondStatus = presenter.lastPresentStatus();
                emitStatus("second-ahead-packet", 2, secondStatus);
                require(secondStatus.presentId == 5 && secondStatus.completeFrameTiming &&
                        secondStatus.valid && !secondStatus.isFrameGenEnabled && secondStatus.framesPresented == 1,
                        "Second ahead packet lost its exact source constants/ticket");
                const auto phase = [&](const char* name, bool enabled, std::uint32_t count, VkRect2D region = {}) {
                    for (std::uint32_t index = 1; index <= count; ++index) {
                        window.pump();
                        const auto engineFrame = submitted + 1;
                        capture.stage(xess_vk::game_timing::Stage::SimulationStart, engineFrame);
                        const auto timing = coordinator.frame(engineFrame);
                        require(bool(timing), "Real fixture frame timing reservation refused");
                        // Real CPU scene/camera update, before GPU command recording.
                        sample = stationaryConstants(config);
                        if (region.extent.width) sample.upscaleSize = {640,360};
                        planes = stationaryPlanes();
                        sample.producerToken = engineFrame;
                        capture.stage(xess_vk::game_timing::Stage::SimulationEnd, engineFrame);
                        const auto beforeCapture = capture.provider->stats();
                        const auto bundle = capture.capture(engineFrame, sample, planes, false, {}, enabled, enabled);
                        if (enabled) {
                            const auto nativeFrame = capture.nativeFrame(bundle);
                            // The owner resets changed regions and OFF-to-ON
                            // history without modifying these raw constants.
                            presenter.presentBundleQueued(nativeFrame, bundle.constants, false, timing, region);
                            // The readback oracle drains actual consumption;
                            // production retains this lease until next reuse.
                            bridge.waitUntilConsumed();
                            capture.verifyPixels(bundle, planes[1].float32[0]);
                            ++queuedOnPresents;
                        } else {
                            const auto afterCapture = capture.provider->stats();
                            require(!bundle.ready && afterCapture.captured == beforeCapture.captured &&
                                    afterCapture.skippedOffCaptures == beforeCapture.skippedOffCaptures + 1 &&
                                    afterCapture.matchedDispatches == beforeCapture.matchedDispatches + 1,
                                    "Observed OFF did not consume its exact invocation without auxiliary copies");
                            queuedColorLayout(source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                            presenter.presentQueuedOff(source.frame.planes[0], source.queue,
                                                       source.frame.producerCompleteFence, timing);
                            // Same-queue source reuse follows the already queued
                            // Bridge read/restore; no current consumed CPU wait.
                            queuedColorLayout(source, VK_IMAGE_LAYOUT_GENERAL);
                            ++queuedOffPresents;
                        }
                        capture.release(bundle);
                        ++submitted;
                        const auto status = presenter.lastPresentStatus();
                        emitStatus(name, index, status);
                        require(status.valid && status.queryResult == 0 && status.presentId == submitted &&
                                presenter.frames() == submitted && status.requestedEnabled == enabled,
                                "Production status/ID does not match the submitted native frame");
                        require(status.sleepFrameId == status.presentId && status.sleepCalls == 1 &&
                                status.presentMarkerCalls == 2 && status.sleepResult == 0 &&
                                status.presentStartResult == 0 && status.presentEndResult == 0 &&
                                !status.partialXeLLTiming && status.completeFrameTiming && status.totalMarkerCalls == 6 &&
                                status.simulationStartResult == 0 && status.simulationEndResult == 0 &&
                                status.renderStartResult == 0 && status.renderEndResult == 0 && status.timingPolicy &&
                                std::strcmp(status.timingPolicy, "borrowed-real-frame-ticket") == 0,
                                "Production borrowed Sleep / Present timing calls, IDs or API results disagree");
                        require(status.consecutiveEnabledPresents == (enabled ? index : 0),
                                "Production ON warmup did not reset at the native OFF / ON transition");
                        if (enabled) {
                            require(status.isFrameGenEnabled, "SDK did not enable frame generation in ON phase");
                            require(status.frameGenResult >= 0, "SDK returned a frame generation error during ON warmup");
                            if (index > onWarmup) {
                                // An explicit request must be applied exactly; "max" accepts the
                                // SDK's applied cap, which the user's Intel override may lower.
                                require(status.interpolatedFrames >= 1 && status.interpolatedFrames <= 3 &&
                                        (interpolatedFrames == xess_vk::ColorPresenter::useMaxSupportedInterpolatedFrames ||
                                         status.interpolatedFrames == interpolatedFrames),
                                        "SDK applied an unexpected number of interpolated frames");
                                // SHOW_ONLY_INTERPOLATION intentionally omits the real backbuffer.
                                const unsigned realOutput=!(status.fgTuning.debugApplied&&status.fgTuning.requested.debug==3);
                                require(status.frameGenResult == 0 && status.framesPresented == status.interpolatedFrames + realOutput,
                                        "ON frame after warmup did not report its configured SDK frames");
                                appliedInterpolatedFrames = status.interpolatedFrames;
                                if(status.framesPresented==2)++verifiedTwoFramePresents;
                                ++acceptedOn;
                            }
                            if (!depth) {
                                depth = bridge.texture(xess_vk::Format::DepthR32F);
                                motion = bridge.texture(xess_vk::Format::MotionRG16F);
                                require(depth && motion && !bridge.config().colorOnly,
                                        "First ON bundle did not materialize lazy auxiliary inputs");
                                const auto depthDesc = depth->GetDesc();
                                const auto motionDesc = motion->GetDesc();
                                require(depthDesc.Width == config.inputWidth && depthDesc.Height == config.inputHeight &&
                                        motionDesc.Width == config.inputWidth && motionDesc.Height == config.inputHeight,
                                        "Lazy auxiliary input extents disagree with actual producer");
                            }
                        } else {
                            require(!status.isFrameGenEnabled && status.frameGenResult == 0 && status.framesPresented == 1,
                                    "OFF frame did not report exactly one non-generated SDK frame");
                            ++acceptedOff;
                        }
                        require(bridge.texture(xess_vk::Format::ColorRGBA8) == color &&
                                bridge.device() == device && bridge.queue() == queue,
                                "Lazy auxiliary initialization replaced color, device or queue");
                        require(bridge.texture(xess_vk::Format::DepthR32F) == depth &&
                                bridge.texture(xess_vk::Format::MotionRG16F) == motion,
                                "Auxiliary textures were allocated early or replaced between phases");
                    }
                };
                phase("off-before", false, offFrames);
                phase("on", true, onFrames);
                phase("on-letterbox", true, onFrames, {{8,17},{936,504}});
                phase("off-between-regions", false, offFrames);
                phase("on-full-restored", true, onFrames);
                phase("off-after", false, offFrames);
                require(overlappingCpuAheadVerified, "CPU K+1 / worker K overlap was not exercised");
                coordinator.stop();
                presenter.shutdown();
                capture.stop();
                const auto captured = capture.provider->stats();
                require(captured.captured + captured.skippedOffCaptures == submitted - 3 &&
                        captured.skippedOffCaptures == 3 * offFrames && captured.consumed == captured.captured &&
                        captured.matchedDispatches == captured.captured + captured.skippedOffCaptures &&
                        capture.verifiedPixelPairs == captured.captured && captured.ambiguousDispatches == 0 &&
                        captured.missingInvocationFrame == 0 && captured.missingRecordingFrame == 0,
                        "Cross-thread production capture/consume counters disagree");
                std::cout << "{\"stage\":\"continuous-cross-thread-capture\",\"publishingThread\":"
                          << capture.publishingThread << ",\"recordingThread\":" << capture.recordingThread
                          << ",\"captured\":" << captured.captured << ",\"consumed\":" << captured.consumed
                          << ",\"skippedOffCaptures\":" << captured.skippedOffCaptures
                          << ",\"offAuxiliarySubmissionSkipped\":true"
                          << ",\"verifiedPixelPairs\":" << capture.verifiedPixelPairs
                          << ",\"twoAheadPacketsVerified\":true,\"cpuAheadActivePresentVerified\":true"
                          << ",\"postSimulationRegistrationVerified\":true,\"postRenderPresentRecordingVerified\":true"
                          << ",\"exactFrameAndTicketVerified\":true,\"sourceDepthD32S8\":true}\n";
                cleanShutdown = true;
                }
            }
            borrowedWindowSurvived = IsWindow(window.hwnd) && IsWindowVisible(window.hwnd);
            require(borrowedWindowSurvived, "Production presenter destroyed the borrowed HWND");
            bridge.waitUntilConsumed();
            nativeFrames = bridge.stats().nativeImageFrames;
            require(nativeFrames == submitted && bridge.stats().cpuUploadFrames == 0,
                    "Presentation source was not exclusively native GPU images");
        }
        source.retainForPossibleBorrowedChildren = false;
        source.renderClear(planes);
        source.wait();
        require(source.drainQueue(config.timeoutMs), "Fixture Vulkan tail did not drain");
        borrowedDeviceSurvived = true;
        window.retained = false;
        require(windowLifecycleOnly ? acceptedOn==4*(onFrames-onWarmup) && acceptedOff>=4 :
                untimedRecoveryOnly ? acceptedOn==0 && acceptedOff==6 :
                acceptedOn == 3u * (onFrames - onWarmup) && acceptedOff == 3 * offFrames &&
                queuedOffPresents == 3 * offFrames && queuedOnPresents == 3u * onFrames,
                "OFF / ON / OFF verification counts disagree");
        std::cout << "{\"stage\":" << jsonString(windowLifecycleOnly ? "window-lifecycle-gpu" : untimedRecoveryOnly ? "untimed-off-recovery-gpu" : "continuous-borrowed-presenter-gpu")
                  << ",\"passed\":true"
                  << ",\"freshLl1InitializationVerified\":true,\"laterSleepModeChanges\":false"
                  << ",\"onWarmupFrames\":" << (untimedRecoveryOnly ? 0 : onWarmup) << ",\"verifiedOnConfiguredFramePresents\":" << acceptedOn
                  << ",\"verifiedOnTwoFramePresents\":" << verifiedTwoFramePresents
                  << ",\"appliedInterpolatedFrames\":" << appliedInterpolatedFrames
                  << ",\"outputMultiplier\":" << (appliedInterpolatedFrames ? appliedInterpolatedFrames + 1 : 0)
                  << ",\"verifiedOffSingleFramePresents\":" << acceptedOff << ",\"submittedFrames\":" << submitted
                  << ",\"queuedColorOffPresents\":" << queuedOffPresents
                  << ",\"queuedBundleOnPresents\":" << queuedOnPresents
                  << ",\"nativeImageFrames\":" << nativeFrames << ",\"lazyAuxiliaryInputsVerified\":" << (untimedRecoveryOnly ? "false" : "true")
                  << ",\"outputInterpolationRegionsExercised\":" << (!untimedRecoveryOnly && !windowLifecycleOnly ? "true" : "false")
                  << ",\"crossThreadProviderVerified\":" << (untimedRecoveryOnly || windowLifecycleOnly ? "false" : "true")
                  << ",\"convertedProviderInputsUsed\":" << (untimedRecoveryOnly || windowLifecycleOnly ? "false" : "true")
                  << ",\"borrowedWindowSurvived\":true,\"borrowedDeviceSurvived\":true,\"cleanupSucceeded\":true"
                  << ",\"elapsedSeconds\":" << std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        reportLimits(!untimedRecoveryOnly);
        std::cout << "}\n";
        return 0;
    } catch (const std::exception& error) {
        std::cout << "{\"stage\":" << jsonString(windowLifecycleOnly ? "window-lifecycle-gpu" : untimedRecoveryOnly ? "untimed-off-recovery-gpu" : "continuous-borrowed-presenter-gpu")
                  << ",\"passed\":false"
                  << ",\"verifiedOnConfiguredFramePresents\":" << acceptedOn
                  << ",\"verifiedOnTwoFramePresents\":" << verifiedTwoFramePresents << ",\"verifiedOffSingleFramePresents\":" << acceptedOff
                  << ",\"submittedFrames\":" << submitted
                  << ",\"sdkShutdownCompleted\":" << (cleanShutdown ? "true" : "false")
                  << ",\"cleanupSucceeded\":" << (cleanShutdown && borrowedDeviceSurvived ? "true" : "false")
                  << ",\"borrowedWindowSurvived\":" << (borrowedWindowSurvived ? "true" : "false")
                  << ",\"borrowedDeviceSurvived\":" << (borrowedDeviceSurvived ? "true" : "false")
                  << ",\"error\":" << jsonString(error.what());
        reportLimits(!untimedRecoveryOnly);
        std::cout << "}\n";
        return 1;
    }
}
