#include "game_frame_cap.hpp"
#include "build_flavor.hpp"
#include "launcher_preferences.hpp"
#include "measurement_log.hpp"
#include "research_build.hpp"
#include "frame_spike_window.hpp"
// Default Vulkan diagnostics do not change parameters, read pixels, or submit
// GPU work. XESS_BRIDGE_DEVICE=1 separately opts into planned device requirements;
// the version-locked Streamline observer has its own opt-in forwarding hooks.
#include <windows.h>
#ifndef XESS_DEVELOPMENT_CPU_DIAGNOSTICS
#define XESS_DEVELOPMENT_CPU_DIAGNOSTICS 1
#endif
#if XESS_DEVELOPMENT_CPU_DIAGNOSTICS
#include "thread_cpu_sampler.hpp"
#endif
#include <vulkan/vulkan.h>
#include "streamline_observer.hpp"
#include "device_bridge_requirements.hpp"
#include "compute_observer.hpp"
#include "image_inventory.hpp"
#include "wsi_image_inventory.hpp"
#include "wsi_acquire_observer.hpp"
#include "candidate_sync_observer.hpp"
#include "render_pass_observer.hpp"
#include "render_tail_diagnostics.hpp"
#include "descriptor_image_observer.hpp"
#include "resource_memory_observer.hpp"
#include "rawcolor_capture_probe.hpp"
#include "presentation_runtime.hpp"
#include "fake_swapchain_layout.hpp"
#include "continuous_provider.hpp"
#include "continuous_registration.hpp"
#include "frame_camera_input.hpp"
#include "fsr_menu_control.hpp"
#include "diagnostic_flags.hpp"
#include "sr_native_resolution.hpp"
#include "sr_runtime_status.hpp"
#include "launcher_preferences.hpp"
#include "async_log.hpp"
#include "release_logging.hpp"
#include "xess_sr_device_caps.hpp"
#include "sr_game_creation.hpp"
#include "sr_execute_overlay.hpp"
#include "sr_native_replacement.hpp"
#include "xess_sr.hpp"
#include "continuous_timing.hpp"
#include "game_frame_timing.hpp"
#include "present_packet_carrier.hpp"
#include <shlobj.h>
#include <sddl.h>
#include "fsr_input_probe.hpp"
#include "upscale_input_probe.hpp"
#include "sr_observation.hpp"
#include "sr_tail_commands.hpp"
#include "sr_gpu_timestamps.hpp"
#ifdef XESS_FSR_DETOUR_AVAILABLE
#include "fsr_callback_install.hpp"
#include <atomic>
#include <new>
#endif
#include <cstdint>
#include <algorithm>
#include <cstdio>
#include <charconv>
#include <cstring>
#include <vector>
#include <optional>
#include <mutex>

#define OBS_EXPORT extern "C" __declspec(dllexport)
namespace {std::uint32_t diagnosticRuntimeState() noexcept;}
OBS_EXPORT VKAPI_ATTR uint32_t VKAPI_CALL xessObserverDiagnosticStateV1() {return diagnosticRuntimeState();}
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkDestroyDevice(VkDevice,const VkAllocationCallbacks*);
// Package capability only; the installer inspects this export without loading the DLL.
OBS_EXPORT VKAPI_ATTR uint32_t VKAPI_CALL xessObserverSingleChainLocalFiOffV1() { return 1; }
OBS_EXPORT VKAPI_ATTR uint32_t VKAPI_CALL xessObserverResearchDiagnosticsV1() { return researchDiagnostics; }
OBS_EXPORT VKAPI_ATTR uint32_t VKAPI_CALL xessObserverMinimalReleaseV1() { return releaseMinimal; }
OBS_EXPORT VKAPI_ATTR uint32_t VKAPI_CALL xessObserverCpuDiagnosticsV1() { return XESS_DEVELOPMENT_CPU_DIAGNOSTICS; }
#if XESS_RELEASE_MINIMAL
OBS_EXPORT VKAPI_ATTR uint32_t VKAPI_CALL xessObserverReleaseRetentionV1() { return 1; }
#endif
// vk_layer.h declares this symbol; establish DLL visibility before that declaration.
struct VkNegotiateLayerInterface;
OBS_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface*);
#include <vulkan/vk_layer.h>

OBS_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance, const char*);
OBS_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice, const char*);
OBS_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_layerGetPhysicalDeviceProcAddr(VkInstance, const char*);

namespace {
constexpr LONG64 kPresentInterval = 300;
constexpr LONG64 kHealthSummaryInterval = 3000;
constexpr ULONGLONG kMaxLogBytes = 32ull * 1024 * 1024;
SRWLOCK stateLock = SRWLOCK_INIT;
SRWLOCK logLock = SRWLOCK_INIT;
thread_local unsigned srSdkDepth{},srTailInjectedDepth{};
thread_local VkPhysicalDevice* srPhysicalWitness{};
struct SrSdkScope {
    SrSdkScope(){++srSdkDepth;++srTailInjectedDepth;}
    ~SrSdkScope(){--srTailInjectedDepth;--srSdkDepth;}
};
INIT_ONCE logOnce = INIT_ONCE_STATIC_INIT;
HANDLE logFile = INVALID_HANDLE_VALUE;
ULONGLONG logBytes = 0;
wchar_t actualLogPath[32768]{};
volatile LONG rawProcessLease=0;
alignas(8) volatile LONG64 packetDeviceIncarnations=0;

struct SrInstanceCreation {
    std::unique_ptr<xess_vk::sr::Sdk> sdk;
    xess_vk::sr_game::RequiredQueries queries;
    xess_vk::sr_game::Creation creation;
};
#ifdef XESS_SR_CREATION_TEST
SrInstanceCreation* (*srCreationForTest)(){};
#endif
struct InstanceState {
    SrInstanceCreation* srCreation;
    bool srSdkPrivate;
    bool logClient;
    InstanceState* next;
    void* key;
    VkInstance instance;
    uint32_t requestedApiVersion;
    PFN_vkGetInstanceProcAddr gipa;
    PFN_GetPhysicalDeviceProcAddr gpdpa;
    PFN_vkDestroyInstance destroy;
    PFN_vkEnumeratePhysicalDevices enumerate;
    PFN_vkEnumeratePhysicalDeviceGroups groups;
    PFN_vkEnumeratePhysicalDeviceGroupsKHR groupsKhr;
    PFN_vkGetPhysicalDeviceProperties properties;
    PFN_vkGetPhysicalDeviceProperties2 properties2;
    PFN_vkGetPhysicalDeviceProperties2KHR properties2Khr;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR surfaceCapabilities;
    PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR surfaceCapabilities2;
    PFN_vkCreateWin32SurfaceKHR createWin32Surface;
    PFN_vkDestroySurfaceKHR destroySurface;
    xess_vk::presentation::Surface* presentationSurface;
    alignas(8) volatile LONG64 propertyEvents;
    bool defaultAllocator;
    bool applicationCallbacksAbsentKnown;
    volatile LONG rawQuarantined;
    bool presentationQuarantined;
    volatile LONG continuousSurfaceRefused;
};
enum class TrackingCpuGroup : unsigned { DescriptorUpdate, DescriptorLifetime, Bind, Dispatch, Recording, ResourceLifetime, Draw, Count };
constexpr auto trackingCpuGroupCount = static_cast<unsigned>(TrackingCpuGroup::Count);
enum class MeasurementFrameState : unsigned { Steady, Off, Invalid, Transition, Unknown, Count };
struct MeasurementInterval {
    std::array<std::uint64_t,static_cast<unsigned>(MeasurementFrameState::Count)> frames;
    std::uint64_t resets, historyResets, warnings, steadyWarnings, fallbackWarnings, unknownWarnings;
    std::uint64_t errors, steadyErrors, outputFailures;
    bool baselineKnown;
    std::uint64_t controlEpoch, viewportGeneration;
};
struct GameSrRuntime;
struct DeviceState {
    bool srSdkPrivate;
    VkPhysicalDevice srSdkPhysical;
    xess_vk::sr_game::RuntimeRequirementsAdmission* srAdmission;
    bool srRuntimeRequested;
    GameSrRuntime* srRuntime;
    std::uint32_t srQueueFamily;
    bool srSingleQueue,srSeparateLayouts;
    SRWLOCK trackingCpuLock;
    std::array<xess_vk::CpuWallTime, trackingCpuGroupCount> trackingCpu;
    struct TrackingPeak {std::uint64_t ns{},qpc{};DWORD thread{};};
    std::array<TrackingPeak,trackingCpuGroupCount> trackingPeaks;
    std::uint64_t trackingReportSequence{},trackingReportQpc{};
    xess_vk::CpuWallTime trackingEmptyScope;
    // frame_spike baseline: cumulative totals at the previous present. The
    // single admitted present queue is externally synchronized by the app.
    std::int64_t frameSpikePresentNs;
    std::array<std::uint64_t, 10> frameSpikePresentation;
    std::uint64_t frameSpikeCapWaits;
    std::uint64_t frameSpikeTracking;
    bool frameSpikeValid;
    xess_vk::FrameSpikeWindow frameSpikeWindow;
    xess_vk::measurement::Window* measurements;
    xess_vk::measurement::Snapshot* measurementSnapshot;
    std::uint64_t measurementCopyFailures, measurementProviderNoFree, measurementSharedNoFree;
    std::uint64_t measurementPresentEntryNs;
    std::uint64_t measurementPreviousCopyWaitNs;
    MeasurementInterval measurementInterval;
    volatile LONG64 frameSpikesLogged;
    volatile LONG64 slowQueueCalls, slowAcquireCalls, slowCopyCalls;
    volatile LONG64 acquireOutOfDateErrors, acquireOtherErrors;
    std::uint64_t lastSlowCopyReturnNs;
    std::uint64_t lastLongFrameResetTraceSequence;
    volatile LONG64 longFrameResetTracesLogged;
    DeviceState* next;
    void* key;
    VkDevice device;
    PFN_vkGetDeviceProcAddr gdpa;
    PFN_vkDestroyDevice destroy;
    PFN_vkCreateSwapchainKHR createSwapchain;
    PFN_vkDestroySwapchainKHR destroySwapchain;
    PFN_vkAcquireNextImageKHR acquireNextImage;
    PFN_vkAcquireNextImage2KHR acquireNextImage2;
    PFN_vkGetSwapchainStatusKHR getSwapchainStatus;
    xess_vk::wsi_acquire::Observer* wsiAcquire;
    volatile LONG wsiPresentThread;
    volatile LONG wsiAcquireFinalLogged;
    PFN_vkQueuePresentKHR present;
    xess_vk::presentation::Runtime* presentation;
    xess_vk::presentation::DeviceRecord* presentationRecord;
    xess_vk::presentation::Surface* presentationSurface;
    bool presentationRequested;
    bool fakeEnabled;
    volatile LONG fakeRecordingError;
    xess_vk::presentation::QueueGate* queueGate;
    xess_vk::continuous::Provider* continuous;
    xess_vk::continuous_timing::Coordinator* continuousTiming;
    xess_vk::presentation_packet::IdentityRegistry* packetIdentities;
    xess_vk::presentation_packet::Ledger* packets;
    std::uint64_t deviceIncarnation;
    volatile LONG continuousClosing;
    volatile LONG continuousTimingRetired;
    VkQueue continuousQueue;
    VkFence continuousDrainFence;
    VkPhysicalDevice physical;
    PFN_vkGetPhysicalDeviceImageFormatProperties continuousImageProperties;
    alignas(8) volatile LONG64 continuousSubmissions;
    alignas(8) volatile LONG64 timingEvents;
    alignas(8) volatile LONG64 continuousDiagnosticSequence;
    alignas(8) volatile LONG64 continuousInvocationSamples;
    alignas(8) volatile LONG64 continuousCameraMissing;
    alignas(8) volatile LONG64 continuousRegistrationFrameMissing;
    alignas(8) volatile LONG64 continuousResolvedTicketMissing;
    alignas(8) volatile LONG64 continuousWindowPresents;
    alignas(8) volatile LONG64 continuousRegistrationCalls;
    alignas(8) volatile LONG64 continuousEnqueueCalls, continuousEnqueueQueueValid, continuousEnqueueQueueMatched;
    alignas(8) volatile LONG64 continuousEnqueueShapes, continuousEnqueueKeys, continuousEnqueueAssociations;
    alignas(8) volatile LONG64 continuousEnqueuePublishAttempts, continuousEnqueueFailures;
    std::uintptr_t continuousUnityBase;
    volatile LONG presentOriginInstallAttempted;
    alignas(8) volatile LONG64 continuousDirectSeen, continuousDirectAccepted, continuousDirectRefused;
    alignas(8) volatile LONG64 continuousOriginCalls, continuousOriginConsumers, continuousOriginWaitTransforms;
    alignas(8) volatile LONG64 continuousOriginSubmitProofs;
    alignas(8) volatile LONG64 continuousUnknownPresentCallers;
    alignas(8) volatile LONG64 continuousWindowBegins;
    alignas(8) volatile LONG64 continuousWindowSubmits;
    alignas(8) volatile LONG64 continuousWindowDispatches;
    volatile LONG continuousWindowBudget;
    volatile LONG continuousWindowState;
    volatile LONG continuousWindowStacks;
    PFN_vkCreateImage createImage;
    PFN_vkDestroyImage destroyImage;
    PFN_vkCreateBuffer createBuffer;
    PFN_vkDestroyBuffer destroyBuffer;
    PFN_vkAllocateMemory allocateMemory;
    PFN_vkFreeMemory freeMemory;
    PFN_vkGetImageMemoryRequirements imageMemoryRequirements;
    PFN_vkGetBufferMemoryRequirements bufferMemoryRequirements;
    PFN_vkBindImageMemory bindImageMemory;
    PFN_vkBindImageMemory2 bindImageMemory2;
    PFN_vkBindImageMemory2KHR bindImageMemory2Khr;
    PFN_vkBindBufferMemory bindBufferMemory;
    PFN_vkBindBufferMemory2 bindBufferMemory2;
    PFN_vkBindBufferMemory2KHR bindBufferMemory2Khr;
    PFN_vkQueueBindSparse bindSparse;
    PFN_vkCreateFence createFence;PFN_vkDestroyFence destroyFence;PFN_vkGetFenceStatus fenceStatus;PFN_vkWaitForFences waitFences;
    PFN_vkCreateSemaphore createSemaphore;
    PFN_vkDestroySemaphore destroySemaphore;
    PFN_vkMapMemory mapMemory;PFN_vkUnmapMemory unmapMemory;PFN_vkInvalidateMappedMemoryRanges invalidateMemory;
    PFN_vkCmdCopyImage cmdCopyImage;PFN_vkCmdCopyImageToBuffer cmdCopyImageToBuffer;
    PFN_vkCmdCopyImage2 cmdCopyImage2;PFN_vkCmdCopyImage2KHR cmdCopyImage2Khr;
    PFN_vkCmdBlitImage cmdBlitImage;PFN_vkCmdBlitImage2 cmdBlitImage2;PFN_vkCmdBlitImage2KHR cmdBlitImage2Khr;
    PFN_vkCmdClearColorImage cmdClearColorImage;
    PFN_vkCmdClearDepthStencilImage cmdClearDepthStencilImage;
    PFN_vkCmdResolveImage cmdResolveImage;
    PFN_vkCmdResolveImage2 cmdResolveImage2;PFN_vkCmdResolveImage2KHR cmdResolveImage2Khr;
    PFN_vkCmdCopyBufferToImage2 cmdCopyBufferToImage2;PFN_vkCmdCopyBufferToImage2KHR cmdCopyBufferToImage2Khr;
    PFN_vkCmdCopyImageToBuffer2 cmdCopyImageToBuffer2;PFN_vkCmdCopyImageToBuffer2KHR cmdCopyImageToBuffer2Khr;
    PFN_vkCmdDraw cmdDraw;PFN_vkCmdDrawIndexed cmdDrawIndexed;
    PFN_vkCmdDrawIndirect cmdDrawIndirect;PFN_vkCmdDrawIndexedIndirect cmdDrawIndexedIndirect;
    PFN_vkQueueWaitIdle queueWaitIdle;PFN_vkDeviceWaitIdle deviceWaitIdle;
    PFN_vkCreateImageView createImageView;
    PFN_vkDestroyImageView destroyImageView;
    PFN_vkCreateDescriptorSetLayout createDescriptorSetLayout;
    PFN_vkDestroyDescriptorSetLayout destroyDescriptorSetLayout;
    PFN_vkCreatePipelineLayout createPipelineLayout;
    PFN_vkDestroyPipelineLayout destroyPipelineLayout;
    PFN_vkCreateDescriptorPool createDescriptorPool;
    PFN_vkResetDescriptorPool resetDescriptorPool;
    PFN_vkDestroyDescriptorPool destroyDescriptorPool;
    PFN_vkAllocateDescriptorSets allocateDescriptorSets;
    PFN_vkFreeDescriptorSets freeDescriptorSets;
    PFN_vkUpdateDescriptorSets updateDescriptorSets;
    PFN_vkCmdBindDescriptorSets cmdBindDescriptorSets;
    PFN_vkCreateDescriptorUpdateTemplate createDescriptorUpdateTemplate;
    PFN_vkDestroyDescriptorUpdateTemplate destroyDescriptorUpdateTemplate;
    PFN_vkCreateDescriptorUpdateTemplateKHR createDescriptorUpdateTemplateKhr;
    PFN_vkDestroyDescriptorUpdateTemplateKHR destroyDescriptorUpdateTemplateKhr;
    PFN_vkUpdateDescriptorSetWithTemplate updateDescriptorSetWithTemplate;
    PFN_vkUpdateDescriptorSetWithTemplateKHR updateDescriptorSetWithTemplateKhr;
    PFN_vkCmdPushDescriptorSet cmdPushDescriptorSet;
    PFN_vkCmdPushDescriptorSetWithTemplate cmdPushDescriptorSetWithTemplate;
    PFN_vkCmdBindDescriptorSets2 cmdBindDescriptorSets2;
    PFN_vkCmdPushDescriptorSet2 cmdPushDescriptorSet2;
    PFN_vkCmdPushDescriptorSetWithTemplate2 cmdPushDescriptorSetWithTemplate2;
    PFN_vkCmdPushDescriptorSetKHR cmdPushDescriptorSetKHR;
    PFN_vkCmdPushDescriptorSetWithTemplateKHR cmdPushDescriptorSetWithTemplateKHR;
    PFN_vkCmdBindDescriptorSets2KHR cmdBindDescriptorSets2KHR;
    PFN_vkCmdPushDescriptorSet2KHR cmdPushDescriptorSet2KHR;
    PFN_vkCmdPushDescriptorSetWithTemplate2KHR cmdPushDescriptorSetWithTemplate2KHR;
    PFN_vkCreateRenderPass createRenderPass;
    PFN_vkCreateRenderPass2 createRenderPass2;
    PFN_vkCreateRenderPass2KHR createRenderPass2Khr;
    PFN_vkDestroyRenderPass destroyRenderPass;
    PFN_vkCreateFramebuffer createFramebuffer;
    PFN_vkDestroyFramebuffer destroyFramebuffer;
    PFN_vkGetSwapchainImagesKHR swapchainImages;
    PFN_vkGetDeviceQueue getQueue;
    PFN_vkGetDeviceQueue2 getQueue2;
    PFN_vkQueueSubmit submit;
    PFN_vkQueueSubmit2 submit2;
    PFN_vkQueueSubmit2KHR submit2Khr;
    PFN_vkCreateShaderModule createShaderModule;
    PFN_vkDestroyShaderModule destroyShaderModule;
    PFN_vkCreateComputePipelines createComputePipelines;
    PFN_vkDestroyPipeline destroyPipeline;
    PFN_vkAllocateCommandBuffers allocateCommandBuffers;
    PFN_vkCreateCommandPool createCommandPool;
    PFN_vkFreeCommandBuffers freeCommandBuffers;
    PFN_vkBeginCommandBuffer beginCommandBuffer;
    PFN_vkEndCommandBuffer endCommandBuffer;
    PFN_vkResetCommandBuffer resetCommandBuffer;
    PFN_vkResetCommandPool resetCommandPool;
    PFN_vkDestroyCommandPool destroyCommandPool;
    PFN_vkCmdPipelineBarrier cmdPipelineBarrier;
    PFN_vkCmdPipelineBarrier2 cmdPipelineBarrier2;
    PFN_vkCmdPipelineBarrier2KHR cmdPipelineBarrier2Khr;
    PFN_vkCmdWaitEvents cmdWaitEvents;
    PFN_vkCmdWaitEvents2 cmdWaitEvents2;
    PFN_vkCmdWaitEvents2KHR cmdWaitEvents2Khr;
    PFN_vkCmdSetEvent2 cmdSetEvent2;
    PFN_vkCmdSetEvent2KHR cmdSetEvent2Khr;
    PFN_vkCmdBeginRenderPass cmdBeginRenderPass;
    PFN_vkCmdBeginRenderPass2 cmdBeginRenderPass2;
    PFN_vkCmdBeginRenderPass2KHR cmdBeginRenderPass2Khr;
    PFN_vkCmdNextSubpass cmdNextSubpass;
    PFN_vkCmdNextSubpass2 cmdNextSubpass2;
    PFN_vkCmdNextSubpass2KHR cmdNextSubpass2Khr;
    PFN_vkCmdEndRenderPass cmdEndRenderPass;
    PFN_vkCmdEndRenderPass2 cmdEndRenderPass2;
    PFN_vkCmdEndRenderPass2KHR cmdEndRenderPass2Khr;
    PFN_vkCmdBeginRendering cmdBeginRendering;
    PFN_vkCmdBeginRenderingKHR cmdBeginRenderingKhr;
    PFN_vkCmdEndRendering cmdEndRendering;
    PFN_vkCmdEndRenderingKHR cmdEndRenderingKhr;
    PFN_vkCmdExecuteCommands cmdExecuteCommands;
    PFN_vkCmdBindPipeline cmdBindPipeline;
    PFN_vkCmdDispatch cmdDispatch;
    PFN_vkCmdDispatchIndirect cmdDispatchIndirect;
    PFN_vkCmdDispatchBase cmdDispatchBase;
    PFN_vkCmdDispatchBaseKHR cmdDispatchBaseKhr;
    PFN_vkCmdPushConstants cmdPushConstants;
    PFN_vkCmdPushConstants2 cmdPushConstants2;
    PFN_vkCmdPushConstants2KHR cmdPushConstants2Khr;
    PFN_vkCmdSetViewport cmdSetViewport;
    PFN_vkCmdSetScissor cmdSetScissor;
    PFN_vkCmdSetViewportWithCount cmdSetViewportWithCount;
    PFN_vkCmdSetScissorWithCount cmdSetScissorWithCount;
    PFN_vkCmdSetViewportWithCountEXT cmdSetViewportWithCountExt;
    PFN_vkCmdSetScissorWithCountEXT cmdSetScissorWithCountExt;
    PFN_vkCmdBindVertexBuffers cmdBindVertexBuffers;
    PFN_vkCmdBindVertexBuffers2 cmdBindVertexBuffers2;
    PFN_vkCmdBindVertexBuffers2EXT cmdBindVertexBuffers2Ext;
    PFN_vkCmdBindIndexBuffer cmdBindIndexBuffer;
    PFN_vkCmdBindIndexBuffer2 cmdBindIndexBuffer2;
    PFN_vkCmdBindIndexBuffer2KHR cmdBindIndexBuffer2Khr;
    PFN_vkCmdDrawIndirectCount cmdDrawIndirectCount;
    PFN_vkCmdDrawIndirectCountKHR cmdDrawIndirectCountKhr;
    PFN_vkCmdDrawIndexedIndirectCount cmdDrawIndexedIndirectCount;
    PFN_vkCmdDrawIndexedIndirectCountKHR cmdDrawIndexedIndirectCountKhr;
    PFN_vkCmdSetLineWidth cmdSetLineWidth;
    PFN_vkCmdSetDepthBias cmdSetDepthBias;
    PFN_vkCmdSetDepthBounds cmdSetDepthBounds;
    PFN_vkCmdSetStencilCompareMask cmdSetStencilCompareMask;
    PFN_vkCmdSetStencilWriteMask cmdSetStencilWriteMask;
    PFN_vkCmdSetStencilReference cmdSetStencilReference;
    PFN_vkCmdSetEvent cmdSetEvent;
    PFN_vkCmdResetEvent cmdResetEvent;
    PFN_vkCmdResetEvent2 cmdResetEvent2;
    PFN_vkCmdResetEvent2KHR cmdResetEvent2Khr;
    PFN_vkCmdFillBuffer cmdFillBuffer;
    PFN_vkCmdSetBlendConstants cmdSetBlendConstants;
    PFN_vkCmdCopyBuffer cmdCopyBuffer;
    PFN_vkCmdCopyBufferToImage cmdCopyBufferToImage;
    xess_vk::compute_observer::Context* compute;
    xess_vk::compute_observer::ShaderArchive* srShaders;
    SRWLOCK srTailLock;
    xess_vk::sr_tail_commands::Observer* srTailCommands;
    std::uint64_t srTailEmitted;
    bool srTailDiagnostics;
    SRWLOCK srGpuLock;
    xess_vk::sr_gpu_timestamps::Observer* srGpu;
    xess_vk::sr_gpu_timestamps::Functions srGpuFunctions;
    VkQueueFamilyProperties srGpuFamilies[64];
    std::uint32_t srGpuFamilyCount,srGpuFamilyReported;
    double srGpuPeriod;
    std::uint64_t srGpuWindow,srGpuRejectedWindow;
    bool srGpuRequested,srGpuClosed,srGpuFinalSummary,srGpuIdleComplete;
    xess_vk::image_inventory::Inventory<>* imageInventory;
    xess_vk::wsi_image_inventory::Inventory* wsiInventory;
    xess_vk::candidate_sync::Context* candidateSync;
    xess_vk::render_pass_observer::Context* renderPass;
    xess_vk::render_tail::Store* renderTail;
    wchar_t tailArmPath[4096];ULONGLONG tailArmCheck;
    bool tailArmLatched,tailSummaryEmitted;
    xess_vk::descriptor_image_observer::Context* descriptors;
    xess_vk::resource_memory_observer::Context* resourceMemory;
    VkPhysicalDeviceMemoryProperties physicalMemory;
    VkDeviceSize actualBufferImageGranularity;
    xess_vk::resource_memory_facts::DeviceScope actualDeviceScope;
    InstanceState* parent;
    xess_vk::rawcolor_capture_probe::Context* rawProbe;
    VkQueue rawQueue;VkQueueFlags rawQueueFlags;VkDeviceSize nonCoherentAtomSize;
    VkImageFormatProperties rawFormatProperties;
    bool rawEnabled,rawUniqueQueue,rawDefaultAllocators,rawCallbackAbsent,rawFormatSupported,rawArmed;
    wchar_t rawArmPath[4096],rawBmpPath[4096];ULONGLONG rawArmLastCheck;std::uint64_t processCreationTime;
    SRWLOCK rawLock; // Heap-zero initialization is SRWLOCK_INIT.
    volatile LONG rawOperation,rawQuarantined,rawObservationMissed;
    alignas(8) volatile LONG64 rawBusySkips;
    std::uint64_t rawArmChecks;std::uint32_t rawHealthEmitted;
    alignas(8) volatile LONG64 presents;
    alignas(8) volatile LONG64 presentationAttempts;
    alignas(8) volatile LONG64 presentErrors;
    alignas(8) volatile LONG64 imageCreates;
    alignas(8) volatile LONG64 imageDestroys;
    alignas(8) volatile LONG64 lateRenderImages;
    alignas(8) volatile LONG64 submits;
    alignas(8) volatile LONG64 upscaleMetadataCalls;
    alignas(8) volatile LONG64 upscaleMetadataLookups;
    alignas(8) volatile LONG64 upscaleMetadataMatches;
    alignas(8) volatile LONG64 wsiMetadataEvents;
    // False on the product XeFG route: no per-object resource lifetime log lines.
    bool researchDiagnostics;
    bool srObservationRequested;
};
void srTailPoll(DeviceState*) noexcept;
void srTailClose(DeviceState*,const char*) noexcept;
bool srTailHooksRequested() noexcept;
struct SrGpuSubmit {std::uint64_t window{};xess_vk::sr_gpu_timestamps::SubmitTicket ticket;};
SrGpuSubmit srGpuBeforeSubmit(DeviceState*,const xess_vk::render_pass_observer::SubmitTicket&) noexcept;
void srGpuAfterSubmit(DeviceState*,const SrGpuSubmit*,VkResult) noexcept;
void srGpuRetire(DeviceState*,std::uint64_t,std::uint64_t) noexcept;
void srGpuRetirePool(DeviceState*,VkCommandPool) noexcept;
void srGpuDeviceIdle(DeviceState*,VkResult) noexcept;
void srGpuBeforeDeviceDestroy(DeviceState*) noexcept;
void srGpuDeviceTerminated(DeviceState*) noexcept;
void srGpuClose(DeviceState*,const char*) noexcept;
InstanceState* instances = nullptr;
DeviceState* devices = nullptr;
bool trackingCpuTimingRequested() noexcept {
    if constexpr(!XESS_DEVELOPMENT_CPU_DIAGNOSTICS)return false;
    const DWORD error = GetLastError();
    wchar_t value[2]{};
    const bool requested = GetEnvironmentVariableW(L"XESS_TRACKING_CPU_TIMING", value, 2) == 1 && value[0] == L'1';
    SetLastError(error);
    return requested;
}
#ifdef XESS_METADATA_CONSUMER_TEST
int trackingCpuTimingOverride = -1;
struct TrackingCpuAudit { std::uint64_t clocks{}, counterLocks{}, snapshotLocks{}; };
TrackingCpuAudit* trackingCpuAudit = nullptr;
#endif
bool trackingCpuTimingEnabled() noexcept {
    static const bool enabled = trackingCpuTimingRequested();
#ifdef XESS_METADATA_CONSUMER_TEST
    if (trackingCpuTimingOverride >= 0) return trackingCpuTimingOverride != 0;
#endif
    return enabled;
}
xess_vk::CpuWallClock::time_point trackingCpuNow() noexcept {
#ifdef XESS_METADATA_CONSUMER_TEST
    if (trackingCpuAudit) ++trackingCpuAudit->clocks;
#endif
    return xess_vk::CpuWallClock::now();
}
// Keep the original pre-findDevice start when enabled; OFF never reads the clock.
xess_vk::CpuWallClock::time_point trackingCpuStart() noexcept {
    return trackingCpuTimingEnabled() ? trackingCpuNow() : xess_vk::CpuWallClock::time_point{};
}
std::uint64_t trackingCpuElapsedNs(xess_vk::CpuWallClock::time_point start) noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(trackingCpuNow() - start).count());
}
struct TrackingCpuScope {
    DeviceState* state;
    TrackingCpuGroup group;
    xess_vk::CpuWallClock::time_point started;
    bool enabled;
    std::uint64_t ownNs{};
    TrackingCpuScope(DeviceState* s, TrackingCpuGroup g, xess_vk::CpuWallClock::time_point start)
        : state(s), group(g), started(start), enabled(trackingCpuTimingEnabled()) {}
    template<class F> decltype(auto) driver(F&& call) {
        if (!enabled) return call();
        const DWORD error = GetLastError();
        ownNs += trackingCpuElapsedNs(started);
        SetLastError(error);
        struct Resume {
            TrackingCpuScope& owner;
            ~Resume() {
                const DWORD error = GetLastError();
                owner.started = trackingCpuNow();
                SetLastError(error);
            }
        } resume{*this};
        return call();
    }
    ~TrackingCpuScope() {
        if (!enabled) return;
        const DWORD error = GetLastError();
        const auto ns = ownNs + trackingCpuElapsedNs(started);
        LARGE_INTEGER ended{};QueryPerformanceCounter(&ended);
#ifdef XESS_METADATA_CONSUMER_TEST
        if (trackingCpuAudit) ++trackingCpuAudit->counterLocks;
#endif
        AcquireSRWLockExclusive(&state->trackingCpuLock);
        state->trackingCpu[static_cast<unsigned>(group)].add(ns);
        auto& peak=state->trackingPeaks[static_cast<unsigned>(group)];
        if(ns>peak.ns)peak={ns,static_cast<std::uint64_t>(ended.QuadPart),GetCurrentThreadId()};
        ReleaseSRWLockExclusive(&state->trackingCpuLock);
        SetLastError(error);
    }
    TrackingCpuScope(const TrackingCpuScope&) = delete;
    TrackingCpuScope& operator=(const TrackingCpuScope&) = delete;
};
std::array<xess_vk::CpuWallTime, trackingCpuGroupCount> trackingCpuSnapshot(DeviceState* state) noexcept {
    std::array<xess_vk::CpuWallTime, trackingCpuGroupCount> totals{};
    if (!trackingCpuTimingEnabled()) return totals;
#ifdef XESS_METADATA_CONSUMER_TEST
    if (trackingCpuAudit) ++trackingCpuAudit->snapshotLocks;
#endif
    AcquireSRWLockShared(&state->trackingCpuLock);
    totals = state->trackingCpu;
    ReleaseSRWLockShared(&state->trackingCpuLock);
    return totals;
}
void calibrateTrackingCpu(DeviceState* state) noexcept {
    if (!trackingCpuTimingEnabled()) return;
    // Unpublished device only. Baseline includes aggregation/clock cost, not contention.
    for (unsigned i = 0; i < 64; ++i) {
        const auto start = trackingCpuNow();
        {
            TrackingCpuScope scope(state, TrackingCpuGroup::Bind, start);
            scope.driver([] {});
        }
        state->trackingEmptyScope.add(trackingCpuElapsedNs(start));
    }
    state->trackingCpu = {};
    state->trackingPeaks = {};
}
struct TrackingCpuReport {
    std::array<xess_vk::CpuWallTime,trackingCpuGroupCount> totals{};
    std::array<DeviceState::TrackingPeak,trackingCpuGroupCount> peaks{};
    std::uint64_t sequence{},beginQpc{},endQpc{};
};
TrackingCpuReport takeTrackingCpuReport(DeviceState* state) noexcept {
    TrackingCpuReport report;if(!trackingCpuTimingEnabled())return report;
    AcquireSRWLockExclusive(&state->trackingCpuLock);
    LARGE_INTEGER now{};QueryPerformanceCounter(&now);report.endQpc=now.QuadPart;
    report.totals=state->trackingCpu;report.peaks=state->trackingPeaks;state->trackingPeaks={};
    report.beginQpc=state->trackingReportQpc;state->trackingReportQpc=report.endQpc;report.sequence=++state->trackingReportSequence;
    ReleaseSRWLockExclusive(&state->trackingCpuLock);return report;
}
// The FSR installer supports one device generation only. Retire metadata
// matching under the same lock as the list before any generation can be reused.
std::uint64_t inventoryDeviceCreations = 0;
bool inventorySessionRetired = false;

template<class T> void* dispatchKey(T handle) noexcept {
    return handle ? *reinterpret_cast<void* const*>(handle) : nullptr;
}
template<class T> InstanceState* findInstance(T handle) noexcept {
    const auto key = dispatchKey(handle);
    AcquireSRWLockShared(&stateLock);
    auto* state = instances;
    while (state && state->key != key) state = state->next;
    ReleaseSRWLockShared(&stateLock);
    // Valid Vulkan use externally synchronizes object destruction with its use.
    return state;
}
template<class T> DeviceState* findDevice(T handle) noexcept {
    const auto key = dispatchKey(handle);
    AcquireSRWLockShared(&stateLock);
    auto* state = devices;
    while (state && state->key != key) state = state->next;
    ReleaseSRWLockShared(&stateLock);
    return state;
}

template<class T> uint64_t bits(T handle) noexcept;
void queueCallSlow(void*, const char*, std::uint64_t, const xess_vk::wsi_acquire::QueueCall&) noexcept;
template<class Call> VkResult queueDriver(DeviceState* state, const char* api, VkQueue queue, Call&& call) {
    if (!state->queueGate) return call();
    const auto entryError = GetLastError();
    const bool measured=state->fakeEnabled&&xess_vk::diagnostic_flags::runtimeDiagnostics();
    xess_vk::wsi_acquire::QueueCall timing;
    if (measured) timing.entryNs = xess_vk::cpuWallNowNs();
    VkResult result; DWORD originalError;
    {
        std::lock_guard lock(*state->queueGate);
        if (measured) timing.driverEntryNs = xess_vk::cpuWallNowNs();
        SetLastError(entryError);
        result = call(); originalError = GetLastError();
        if (measured) timing.returnNs = xess_vk::cpuWallNowNs();
    }
    timing.result = result;
    if (measured && timing.slow()) queueCallSlow(state, api, bits(queue), timing);
    SetLastError(originalError); return result;
}
VkResult fakeRecordingError(DeviceState* state) noexcept {
    return state && state->fakeEnabled ? static_cast<VkResult>(InterlockedCompareExchange(&state->fakeRecordingError, 0, 0)) : VK_SUCCESS;
}
void fakeRecordingFailed(DeviceState* state) noexcept {
    InterlockedCompareExchange(&state->fakeRecordingError, VK_ERROR_OUT_OF_HOST_MEMORY, VK_SUCCESS);
}
auto fakeImagePredicate(DeviceState* state) {
    return [state](VkImage image) {
        return state->fakeEnabled && state->presentation && state->presentation->isFakeImage(image);
    };
}
bool fakeSwapchainRequested() noexcept {
    wchar_t value[2]{};
    return GetEnvironmentVariableW(L"XESS_FAKE_SWAPCHAIN", value, 2) == 1 && value[0] == L'1';
}

bool diagnosticLoggingEnabled() noexcept {return xess_vk::diagnostic_flags::runtimeDiagnostics();}
void initializeMeasurements(DeviceState* state) noexcept {
    if(!diagnosticLoggingEnabled())return;
    try {
        state->measurements=new xess_vk::measurement::Window(xess_vk::cpuWallNowNs());
        state->measurementSnapshot=new xess_vk::measurement::Snapshot;
    }catch(...){delete state->measurements;state->measurements=nullptr;delete state->measurementSnapshot;state->measurementSnapshot=nullptr;}
}
BOOL CALLBACK initializeLog(PINIT_ONCE, PVOID, PVOID*) noexcept {
    if(!diagnosticLoggingEnabled())return TRUE;
    wchar_t path[32768];
    const DWORD length = GetEnvironmentVariableW(L"XESS_OBSERVER_LOG", path, 32768);
    if (length < 3 || length >= 32768) return TRUE;
    // Only an explicitly supplied absolute drive path; never CWD or network paths.
    const bool drive = (path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z');
    if (!drive || path[1] != L':' || (path[2] != L'\\' && path[2] != L'/')) return TRUE;
    logFile = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                         nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (logFile != INVALID_HANDLE_VALUE) {
        wchar_t finalPath[32768]{};
        const auto finalLength=GetFinalPathNameByHandleW(logFile,finalPath,32768,FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
        if(finalLength>4&&finalLength<32768&&std::wcsncmp(finalPath,L"\\\\?\\",4)==0&&finalPath[5]==L':')
            std::wmemcpy(actualLogPath,finalPath+4,finalLength-3);
        LARGE_INTEGER size{};
        if (GetFileSizeEx(logFile, &size)) logBytes = static_cast<ULONGLONG>(size.QuadPart);
    }
    return TRUE;
}

bool writeLogLineDirect(const char* data, DWORD size) noexcept {
    AcquireSRWLockExclusive(&logLock);
    bool writtenAll=false;
    if (logFile != INVALID_HANDLE_VALUE && logBytes + size <= kMaxLogBytes) {
        DWORD written = 0;
        if (!WriteFile(logFile, data, size, &written, nullptr) || written != size) {
            CloseHandle(logFile); logFile = INVALID_HANDLE_VALUE;
        } else { logBytes += written;writtenAll=true; }
    }
    ReleaseSRWLockExclusive(&logLock);
    return writtenAll;
}
// Two bounded files retain recent statistics independently of verbose-log exhaustion.
xess_vk::measurement::Log measurementLog;
xess_vk::measurement::SummaryLog measurementSummaryLog;
xess_vk::measurement::Log srObservationLog(8ull*1024*1024,xess_vk::measurement::LogStream::Observation);
using DiagnosticStream=xess_vk::diagnostics::Stream;
// Process owner: no blocking thread destructor runs under the DLL loader lock.
std::atomic<xess_vk::diagnostics::AsyncLogWriter*> asyncLogWriter{};
std::atomic<bool> asyncLogRequested{};
SRWLOCK logLifecycleLock=SRWLOCK_INIT;
unsigned logClients{};
std::atomic<std::uint64_t> logStartFailures{};
std::atomic<bool> runtimeClientSeen{};
std::uint32_t diagnosticRuntimeState() noexcept {
    const auto* writer=asyncLogWriter.load();
    return (diagnosticLoggingEnabled()?1u:0u)|(writer&&writer->stats().active?2u:0u)|(runtimeClientSeen.load()?4u:0u);
}
void startThreadCpuSampling() noexcept;
void stopThreadCpuSampling() noexcept;
bool diagnosticSink(void*,DiagnosticStream stream,const char* data,DWORD size) noexcept {
    if(stream==DiagnosticStream::Verbose)return writeLogLineDirect(data,size);
    AcquireSRWLockExclusive(&logLock);
    const auto result=stream==DiagnosticStream::Detail?measurementLog.write(actualLogPath,data,size):
        stream==DiagnosticStream::Summary?measurementSummaryLog.write(actualLogPath,data,size):srObservationLog.write(actualLogPath,data,size);
    ReleaseSRWLockExclusive(&logLock);return result;
}
void diagnosticDrained(void*,const xess_vk::diagnostics::WriterStats& stats) noexcept {
    char line[768]{};
    const auto size=std::snprintf(line,sizeof(line),"{\"event\":\"diagnostic_writer_drained\",\"pid\":%lu,\"accepted\":%llu,\"written\":%llu,\"sink_failures\":%llu,\"pending\":%llu,\"dropped_verbose\":%llu,\"dropped_detail\":%llu,\"dropped_summary\":%llu,\"dropped_observation\":%llu}\n",
        GetCurrentProcessId(),static_cast<unsigned long long>(stats.accepted),static_cast<unsigned long long>(stats.written),
        static_cast<unsigned long long>(stats.writeFailures),static_cast<unsigned long long>(stats.pending),
        static_cast<unsigned long long>(stats.dropped[0]),static_cast<unsigned long long>(stats.dropped[1]),
        static_cast<unsigned long long>(stats.dropped[2]),static_cast<unsigned long long>(stats.dropped[3]));
    if(size>0&&size<static_cast<int>(sizeof(line)))diagnosticSink(nullptr,DiagnosticStream::Summary,line,static_cast<DWORD>(size));
}
void acquireLogClient() noexcept {
    runtimeClientSeen=true;
    if(!diagnosticLoggingEnabled())return;
    InitOnceExecuteOnce(&logOnce,initializeLog,nullptr,nullptr);
    AcquireSRWLockExclusive(&logLifecycleLock);
    if(logClients++==0){
        asyncLogRequested.store(true);
        if(actualLogPath[0]){
            auto* writer=asyncLogWriter.load();
            if(!writer){writer=new(std::nothrow) xess_vk::diagnostics::AsyncLogWriter;asyncLogWriter.store(writer);}
            if(!writer||!writer->start(diagnosticSink,nullptr,diagnosticDrained))++logStartFailures;
        }
        startThreadCpuSampling();
    }
    ReleaseSRWLockExclusive(&logLifecycleLock);
}
void releaseLogClient(InstanceState* state) noexcept {
    if(!state->logClient)return;
    state->logClient=false;
    AcquireSRWLockExclusive(&logLifecycleLock);
    if(logClients&&--logClients==0){stopThreadCpuSampling();if(auto* writer=asyncLogWriter.load())writer->stop();}
    ReleaseSRWLockExclusive(&logLifecycleLock);
}
void deliverDiagnostic(DiagnosticStream stream,const char* data,DWORD size) noexcept {
    InitOnceExecuteOnce(&logOnce,initializeLog,nullptr,nullptr);
    if(asyncLogRequested.load()){
        if(auto* writer=asyncLogWriter.load())writer->enqueue(stream,data,size);
        return;
    }
    // Loader negotiation precedes vkCreateInstance; only this bootstrap may write inline.
    diagnosticSink(nullptr,stream,data,size);
}
xess_vk::release_log::Policy releaseLogPolicy;
void writeDiagnostic(DiagnosticStream stream,const char* data,DWORD size) noexcept {
    if(!diagnosticLoggingEnabled())return;
    if constexpr(!releaseMinimal){deliverDiagnostic(stream,data,size);return;}
    if(!data||!size)return;
    auto selected=static_cast<unsigned>(stream);
    if(!releaseLogPolicy.accept(selected,{data,size}))return;
    stream=static_cast<DiagnosticStream>(selected);
    if(stream==DiagnosticStream::Detail){
        std::array<char,32768> compact{};
        const auto bytes=xess_vk::release_log::projectStatus({data,size},compact.data(),compact.size());
        if(!bytes){
            static constexpr char failure[]="{\"event\":\"release_status_projection_failed\"}\n";
            deliverDiagnostic(DiagnosticStream::Verbose,failure,sizeof(failure)-1);return;
        }
        deliverDiagnostic(stream,compact.data(),static_cast<DWORD>(bytes));
        if(releaseLogPolicy.stateChanged({compact.data(),bytes})){
            std::array<char,4096> change{};const auto n=xess_vk::release_log::projectStatus({compact.data(),bytes},change.data(),change.size(),true);
            if(n)deliverDiagnostic(DiagnosticStream::Verbose,change.data(),static_cast<DWORD>(n));
        }
    }else deliverDiagnostic(stream,data,size);
}
void writeLogLine(const char* data,DWORD size) noexcept {writeDiagnostic(DiagnosticStream::Verbose,data,size);}
std::uint64_t droppedDiagnostic(DiagnosticStream stream) noexcept {
    if(const auto* writer=asyncLogWriter.load())return writer->stats().dropped[static_cast<unsigned>(stream)];
    return 0;
}
#ifdef XESS_METADATA_CONSUMER_TEST
void (*measurementSummaryTestSink)(const char*,DWORD){};
void (*measurementDetailTestSink)(const char*,DWORD){};
#endif
void writeMeasurementLine(const char* data,DWORD size) noexcept {
#ifdef XESS_METADATA_CONSUMER_TEST
    if(measurementDetailTestSink){measurementDetailTestSink(data,size);return;}
#endif
    writeDiagnostic(DiagnosticStream::Detail,data,size);
}
void writeMeasurementSummaryLine(const char* data,DWORD size) noexcept {
#ifdef XESS_METADATA_CONSUMER_TEST
    if(measurementSummaryTestSink){measurementSummaryTestSink(data,size);return;}
#endif
    writeDiagnostic(DiagnosticStream::Summary,data,size);
}
void streamlineLog(const char* line) noexcept {
    if (!diagnosticLoggingEnabled() || !line) return;
    const size_t size = strnlen(line, 32768);
    if (!size || size >= 32767) return;
    char record[32768]; std::memcpy(record, line, size);
    const bool newline = line[size - 1] == '\n';
    if (!newline) record[size] = '\n';
    const auto count=static_cast<DWORD>(size+(newline?0:1));
    if(std::strstr(line,"\"event\":\"sr_")){
        writeDiagnostic(DiagnosticStream::Observation,record,count);
    }else writeLogLine(record,count);
}

struct JsonLine {
    char buffer[32768];
    size_t used = 0;
    bool good = true;
    explicit JsonLine(const char* event) noexcept {
        if(!diagnosticLoggingEnabled()){good=false;return;}
        raw("{\"event\":"); quoted(event);
        number("pid", GetCurrentProcessId());
        number("uptime_ms", GetTickCount64());
    }
    void raw(const char* value) noexcept {
        if(!good)return;
        const size_t length = std::strlen(value);
        if (!good || used + length >= sizeof(buffer)) { good = false; return; }
        std::memcpy(buffer + used, value, length); used += length;
    }
    void quoted(const char* value) noexcept {
        if(!good)return;
        raw("\"");
        if (value) for (const unsigned char* p = reinterpret_cast<const unsigned char*>(value); *p; ++p) {
            char escaped[8];
            if (*p == '"' || *p == '\\') { escaped[0] = '\\'; escaped[1] = static_cast<char>(*p); escaped[2] = 0; }
            else if (*p < 32) std::snprintf(escaped, sizeof(escaped), "\\u%04x", *p);
            else { escaped[0] = static_cast<char>(*p); escaped[1] = 0; }
            raw(escaped);
        }
        raw("\"");
    }
    void key(const char* name) noexcept { raw(","); quoted(name); raw(":"); }
    void number(const char* name, int64_t value) noexcept {
        if(!good)return;
        char text[32]; std::snprintf(text, sizeof(text), "%lld", static_cast<long long>(value));
        key(name); raw(text);
    }
    void text(const char* name, const char* value) noexcept { key(name); quoted(value); }
    void ticks(const char* name, std::uint64_t value) noexcept {
        if(!good)return;
        char text[32];std::snprintf(text,sizeof(text),"%llu",static_cast<unsigned long long>(value));this->text(name,text);
    }
    void real(const char* name, double value) noexcept {
        if(!good)return;
        key(name); if (!std::isfinite(value)) { raw("null"); return; }
        char text[48]; const auto result=std::to_chars(text,text+sizeof(text)-1,value);
        if(result.ec!=std::errc{}){raw("null");return;}*result.ptr=0;raw(text);
    }
    void handle(const char* name, uint64_t value) noexcept {
        if(!good)return;
        char text[24]; std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(value));
        this->text(name, text);
    }
    void boolean(const char* name, bool value) noexcept { key(name); raw(value ? "true" : "false"); }
    void cpuWall(const char* name, const xess_vk::CpuWallTime& value) noexcept {
        if(!good)return;
        char text[160];
        std::snprintf(text, sizeof(text), "{\"count\":%llu,\"total_ns\":%llu,\"max_ns\":%llu}",
            static_cast<unsigned long long>(value.count),
            static_cast<unsigned long long>(value.totalNs),
            static_cast<unsigned long long>(value.maxNs));
        key(name);
        raw(text);
    }
    void names(const char* name, uint32_t count, const char* const* values) noexcept {
        key(name); raw("[");
        for (uint32_t i = 0; i < count && good; ++i) { if (i) raw(","); quoted(values[i]); }
        raw("]");
    }
    void write(bool measurement=false) noexcept {
        raw("}\n");
        static constexpr char overflow[] = "{\"event\":\"observer_event_too_large\"}\n";
        (measurement?writeMeasurementLine:writeLogLine)(good ? buffer : overflow, good ? static_cast<DWORD>(used) : static_cast<DWORD>(sizeof(overflow) - 1));
    }
    void writeSummary() noexcept {
        raw("}\n");
        static constexpr char overflow[] = "{\"event\":\"measurement_summary_too_large\"}\n";
        writeMeasurementSummaryLine(good?buffer:overflow,good?static_cast<DWORD>(used):sizeof(overflow)-1);
    }
    void writeObservation() noexcept {
        if constexpr(releaseMinimal)return;
        raw("}\n");
        static constexpr char overflow[]="{\"event\":\"sr_observation_too_large\"}\n";
        writeDiagnostic(DiagnosticStream::Observation,good?buffer:overflow,good?static_cast<DWORD>(used):sizeof(overflow)-1);
    }
};

bool slowCallBudget(DeviceState* state, volatile LONG64& count, const char* kind) noexcept {
    const auto ordinal = InterlockedIncrement64(&count);
    constexpr auto limit = static_cast<LONG64>(xess_vk::wsi_acquire::slowCallLimit);
    if (ordinal <= limit) return true;
    if (ordinal == limit + 1) {
        JsonLine log("fake_slow_call_log_limit"); log.handle("device", bits(state->device));
        log.text("kind", kind); log.number("limit", xess_vk::wsi_acquire::slowCallLimit); log.write();
    }
    return false;
}
void queueCallSlow(void* context, const char* api, std::uint64_t queue, const xess_vk::wsi_acquire::QueueCall& timing) noexcept {
    auto* state = static_cast<DeviceState*>(context);
    if (!state || !state->fakeEnabled || !timing.slow() || !slowCallBudget(state, state->slowQueueCalls, "queue")) return;
    JsonLine log("fake_queue_call_slow"); log.handle("device", bits(state->device)); log.handle("queue", queue);
    log.text("api", api); log.number("thread_id", GetCurrentThreadId()); log.number("result", timing.result);
    log.number("entry_qpc_us", timing.entryNs / 1000); log.number("driver_entry_qpc_us", timing.driverEntryNs / 1000);
    log.number("driver_return_qpc_us", timing.returnNs / 1000);
    log.number("gate_wait_us", (timing.driverEntryNs - timing.entryNs) / 1000);
    log.number("driver_call_us", (timing.returnNs - timing.driverEntryNs) / 1000); log.write();
}

void gpuTimestampFields(JsonLine& log, const char* name, const xess_vk::GpuTimestampSample& sample) noexcept {
    log.key(name); log.raw("{\"requested\":"); log.raw(sample.requested?"true":"false");
    log.boolean("available",sample.available); log.boolean("duration_known",sample.durationKnown);
    log.boolean("query_state_known",sample.queryStateKnown);
    if(sample.queryStateKnown){
        log.boolean("diagnostic_active",sample.diagnosticActive); log.boolean("query_read_attempted",sample.queryReadAttempted);
        log.boolean("query_result_known",sample.queryResultKnown); if(sample.queryResultKnown)log.number("query_result",sample.queryResult);
        log.boolean("query_availability_known",sample.queryAvailabilityKnown);
        if(sample.queryAvailabilityKnown){log.number("begin_availability",sample.beginAvailability);log.number("end_availability",sample.endAvailability);}
    }
    log.number("sequence",sample.sequence); log.number("context_generation",sample.contextGeneration);
    log.number("source_engine_frame",sample.sourceEngineFrame); log.boolean("source_engine_frame_known",sample.sourceEngineFrameKnown);
    log.number("sdk_present_id",sample.sdkPresentId); log.boolean("sdk_present_id_known",sample.sdkPresentIdKnown);
    log.number("source_copy_token",sample.sourceCopyToken); log.boolean("source_copy_token_known",sample.sourceCopyTokenKnown);
    log.number("resource_generation",sample.generation); log.ticks("begin_ticks",sample.beginTicks); log.ticks("end_ticks",sample.endTicks);
    log.number("valid_bits",sample.validBits); log.real("timestamp_period_ns",sample.timestampPeriodNs); log.number("frequency_hz",sample.frequencyHz);
    if(sample.durationKnown)log.real("duration_ns",sample.durationNs);
    log.number("submit_qpc_ticks",sample.submitQpcTicks); log.number("fence_observed_qpc_ticks",sample.fenceObservedQpcTicks); log.number("qpc_frequency",sample.qpcFrequency);
    const auto& c=sample.calibration;
    log.boolean("calibration_available",c.available); log.ticks("calibration_gpu_ticks",c.gpuTicks); log.number("calibration_qpc_ticks",c.qpcTicks);
    log.number("calibration_qpc_frequency",c.qpcFrequency); log.number("calibration_capture_qpc_ticks",c.captureQpcTicks);
    log.boolean("max_deviation_known",c.maxDeviationKnown); if(c.maxDeviationKnown)log.number("max_deviation_ns",c.maxDeviationNs);
    if(c.available&&c.qpcFrequency&&sample.fenceObservedQpcTicks>=c.captureQpcTicks)
        log.real("calibration_age_ns",static_cast<double>(sample.fenceObservedQpcTicks-c.captureQpcTicks)*1e9/c.qpcFrequency);
    double begin{},end{};const bool aligned=xess_vk::timestampQpcUs(sample,sample.beginTicks,begin)&&xess_vk::timestampQpcUs(sample,sample.endTicks,end);
    log.boolean("qpc_alignment_known",aligned); if(aligned){log.real("begin_qpc_us",begin);log.real("end_qpc_us",end);}
    log.raw("}");
}
void sharedCopyFields(JsonLine& log,const xess_vk::presentation::FrameStatus& frame) noexcept {
    log.boolean("shared_copy_path",frame.sharedCopyPath);
    log.boolean("shared_color_direct",frame.sharedColorDirect);
    log.boolean("shared_auxiliary_direct",frame.sharedAuxiliaryDirect);
    log.boolean("depth_snapshot_compute",frame.depthCompute);
    log.key("shared_plane_key");
    if(frame.sharedPlaneKey.valid()) {
        log.raw("{\"index\":");char index[16];std::snprintf(index,sizeof(index),"%u",frame.sharedPlaneKey.index);log.raw(index);
        log.ticks("ring_generation",frame.sharedPlaneKey.ringGeneration);
        log.ticks("lease_generation",frame.sharedPlaneKey.leaseGeneration);log.raw("}");
    } else log.raw("null");
    log.key("shared_fallback_reason");if(frame.sharedFallbackReason)log.quoted(frame.sharedFallbackReason);else log.raw("null");
    const auto& planes=frame.sharedPlanes;
    log.raw(",\"shared_planes\":{\"available\":");log.raw(planes.available?"true":"false");
    if(planes.available) {
        log.boolean("ready",planes.ready);log.boolean("depth_storage",planes.depthStorage);log.ticks("ring_generation",planes.ringGeneration);
        log.number("resource_bytes",planes.resourceBytes);log.number("leases",planes.leases);
        log.number("reservations",planes.reservations);log.number("no_free_fallbacks",planes.noFreeFallbacks);
        log.number("extent_fallbacks",planes.extentFallbacks);log.number("cancelled",planes.cancelled);
        log.number("abandoned",planes.abandoned);log.number("released",planes.released);log.number("poisoned",planes.poisoned);
        log.number("direct_frames",planes.directFrames);log.number("legacy_source_frames",planes.legacySourceFrames);
    }
    log.raw("}");
}
#if XESS_DEVELOPMENT_CPU_DIAGNOSTICS
xess_vk::diagnostics::ThreadCpuSampler* threadCpuSampler{};
void threadCpuSampleSink(void*,const xess_vk::diagnostics::ThreadCpuWindow& window) noexcept {
    // A game can have >200 threads. Batch unchanged JSONL records so one
    // sampling burst does not occupy one queue slot per thread.
    struct Batch {
        char bytes[32768];DWORD used{};
        void flush() noexcept {if(used){writeMeasurementSummaryLine(bytes,used);used=0;}}
        void append(JsonLine& line) noexcept {
            line.raw("}\n");
            if(!line.good){flush();static constexpr char failed[]="{\"event\":\"thread_cpu_record_too_large\"}\n";writeMeasurementSummaryLine(failed,sizeof(failed)-1);return;}
            if(used+line.used>sizeof(bytes))flush();
            std::memcpy(bytes+used,line.buffer,line.used);used+=static_cast<DWORD>(line.used);
        }
        ~Batch(){flush();}
    } batch;
    std::uint64_t user{},kernel{},cycles{};unsigned measured{},cycleMeasured{};
    for(const auto& row:window.threads){
        if(row.cpuKnown){user+=row.userNs;kernel+=row.kernelNs;++measured;}if(row.cyclesKnown){cycles+=row.cycles;++cycleMeasured;}
        JsonLine line("thread_cpu_thread");line.number("schema",2);line.number("sampler_generation",window.generation);line.number("sequence",window.sequence);
        line.number("thread_id",row.identity.id);line.ticks("creation_filetime",row.identity.creation);
        line.number("begin_qpc",window.beginQpc);line.number("end_qpc",window.endQpc);line.number("qpc_frequency",window.frequency);
        line.boolean("cpu_known",row.cpuKnown);line.number("user_ns",row.userNs);line.number("kernel_ns",row.kernelNs);
        line.boolean("cycles_known",row.cyclesKnown);line.ticks("cycles",row.cycles);
        line.boolean("cycle_runtime_known",row.cyclesKnown&&window.calibration.valid);
        if(row.cyclesKnown&&window.calibration.valid)line.real("cycle_runtime_estimate_ns",xess_vk::diagnostics::cycleEstimateNs(row.cycles,window.calibration));
        line.boolean("start_known",row.identity.startKnown);line.handle("start_address",row.identity.startAddress);
        char name[2048]{},module[4096]{};
        const auto nameBytes=WideCharToMultiByte(CP_UTF8,0,row.identity.name.c_str(),-1,name,sizeof(name),nullptr,nullptr);
        const auto moduleBytes=WideCharToMultiByte(CP_UTF8,0,row.identity.module.c_str(),-1,module,sizeof(module),nullptr,nullptr);
        line.text("thread_name",nameBytes?name:"");line.text("start_module",moduleBytes?module:"");
        line.boolean("metadata_complete",nameBytes&&moduleBytes);line.text("attribution","thread_origin_not_sampled_stacks");batch.append(line);
    }
    for(const auto& group:window.groups){
        JsonLine line("thread_cpu_group");line.number("schema",2);line.number("sampler_generation",window.generation);line.number("sequence",window.sequence);
        line.number("begin_qpc",window.beginQpc);line.number("end_qpc",window.endQpc);line.number("qpc_frequency",window.frequency);
        char name[2048]{},module[4096]{};
        const auto n=WideCharToMultiByte(CP_UTF8,0,group.name.c_str(),-1,name,sizeof(name),nullptr,nullptr);
        const auto m=WideCharToMultiByte(CP_UTF8,0,group.module.c_str(),-1,module,sizeof(module),nullptr,nullptr);
        line.text("thread_name",n?name:"");line.text("start_module",m?module:"");line.boolean("metadata_complete",n&&m);
        line.number("threads",group.threads);line.number("measured",group.measured);line.number("user_ns",group.userNs);line.number("kernel_ns",group.kernelNs);line.ticks("cycles",group.cycles);
        const auto seconds=window.beginQpc&&window.frequency?double(window.endQpc-window.beginQpc)/window.frequency:0;
        if(seconds>0){line.real("user_ms_per_second",group.userNs/1e6/seconds);line.real("kernel_ms_per_second",group.kernelNs/1e6/seconds);}
        line.text("attribution","thread_origin_not_sampled_stacks");batch.append(line);
    }
    JsonLine line("thread_cpu_window");line.number("schema",2);line.number("sampler_generation",window.generation);line.number("sequence",window.sequence);
    line.number("sampler_thread",GetCurrentThreadId());line.boolean("baseline",window.baseline);
    line.number("begin_qpc",window.beginQpc);line.number("end_qpc",window.endQpc);line.number("qpc_frequency",window.frequency);
    line.number("enumerated",window.enumerated);line.number("unreadable",window.unreadable);line.number("measured",measured);
    line.number("new_threads",window.newThreads);line.number("ended_threads",window.endedThreads);
    line.number("observed_user_ns",user);line.number("observed_kernel_ns",kernel);line.ticks("observed_cycles",cycles);
    line.number("cycles_measured",cycleMeasured);line.text("cpu_time_semantics","gettimes_accounting_reference_only");
    line.boolean("cycle_calibration_valid",window.calibration.valid);line.boolean("invariant_tsc",window.calibration.invariant);
    line.real("tsc_hz",window.calibration.tscHz);line.real("alternate_tsc_hz",window.calibration.alternateHz);line.real("cycle_busy_validation_ratio",window.calibration.busyRatio);
    line.text("cycle_calibration_reason",window.calibration.reason);line.text("cycle_runtime_semantics","locally_calibrated_estimate_not_portable_elapsed_time");
    line.boolean("process_cycles_known",window.processCyclesKnown);line.ticks("process_cycles",window.processCycles);
    line.boolean("collection_cycles_known",window.sampleCyclesKnown);line.ticks("collection_cycles",window.sampleCycles);
    if(window.calibration.valid){
        line.real("observed_cycle_runtime_estimate_ns",xess_vk::diagnostics::cycleEstimateNs(cycles,window.calibration));
        if(window.processCyclesKnown)line.real("process_cycle_runtime_estimate_ns",xess_vk::diagnostics::cycleEstimateNs(window.processCycles,window.calibration));
        if(window.sampleCyclesKnown)line.real("collection_cycle_runtime_estimate_ns",xess_vk::diagnostics::cycleEstimateNs(window.sampleCycles,window.calibration));
    }
    line.boolean("process_known",window.processKnown);line.number("process_user_ns",window.processUserNs);line.number("process_kernel_ns",window.processKernelNs);
    line.number("collection_cpu_ns",window.sampleCpuNs);line.number("collection_wall_ns",window.sampleWallNs);
    line.text("coverage","interval_end_live_threads; new_and_exited_threads_can_be_missing");line.number("jsonl_batch_schema",1);batch.append(line);
}
#endif
void startThreadCpuSampling() noexcept {
#if XESS_DEVELOPMENT_CPU_DIAGNOSTICS
    if(!xess_vk::diagnostics::threadCpuSamplingRequested())return;
    if(!threadCpuSampler)threadCpuSampler=new(std::nothrow)xess_vk::diagnostics::ThreadCpuSampler;
    const bool started=threadCpuSampler&&threadCpuSampler->start(threadCpuSampleSink,nullptr);
    JsonLine line("thread_cpu_sampling");line.boolean("requested",true);line.boolean("started",started);line.number("period_ms",5000);line.writeSummary();
#endif
}
void stopThreadCpuSampling() noexcept {
#if XESS_DEVELOPMENT_CPU_DIAGNOSTICS
    if(threadCpuSampler){const bool stopped=threadCpuSampler->stop();JsonLine line("thread_cpu_sampling_stopped");line.boolean("drained",stopped);line.writeSummary();}
#endif
}
template<class Sink> void fgTuningFields(Sink& log,const xess_vk::presentation::FrameStatus& frame) noexcept {
    log.boolean("fg_gameplay_gate_required",frame.gameplayGateRequired);
    log.boolean("fg_gameplay_ready",frame.gameplayReady);log.boolean("fg_character_menu",frame.characterMenu);
    log.boolean("fg_gameplay_state_known",frame.gameplayState>=0);
    log.number("fg_gameplay_state",frame.gameplayState>=0?frame.gameplayState:0);
    const auto& a=frame.fgTuning;const auto& t=frame.fgOutcomes;
    const auto& cap=xess_vk::launcher_preferences::startupFrameCap();
    log.number("game_frame_cap_schema",3);log.boolean("game_frame_cap_requested",cap.requested);
    log.number("game_frame_cap_target_fps",cap.target);log.boolean("game_frame_cap_invalid_config",cap.invalid);
    log.boolean("game_frame_cap_installed",xess_vk::game_frame_cap::installed.load());
    log.text("game_frame_cap_state",xess_vk::game_frame_cap::state.load());
    log.number("game_frame_cap_calls",xess_vk::game_frame_cap::calls.load());
    log.number("game_frame_cap_remapped",xess_vk::game_frame_cap::remapped.load());
    log.number("game_frame_cap_last_requested",xess_vk::game_frame_cap::lastRequested.load());
    log.number("game_frame_cap_last_forwarded",xess_vk::game_frame_cap::lastForwarded.load());
    log.number("game_frame_cap_immediate_readback",xess_vk::game_frame_cap::readback.load());
    log.text("game_frame_cap_readback_semantics","setter_callback_immediate_getter_not_effective_limit");
    if constexpr(!releaseMinimal)log.number("game_frame_cap_readback",xess_vk::game_frame_cap::readback.load());
    log.number("fg_settings_schema",1);log.boolean("fg_settings_available",a.available);log.text("fg_settings_policy","next_process");
    log.number("fg_threshold_requested_index",a.requested.threshold);log.number("fg_settings_invalid",a.requested.invalid);
    log.boolean("fg_threshold_attempted",a.thresholdAttempted);log.boolean("fg_threshold_applied",a.thresholdApplied);
    log.number("fg_threshold_result",a.thresholdResult);
    if(a.requested.threshold<xess_vk::fg_tuning::thresholds.size())log.real("fg_threshold_requested",xess_vk::fg_tuning::thresholds[a.requested.threshold]);
    log.number("fg_debug_requested",a.requested.debug);log.boolean("fg_debug_attempted",a.debugAttempted);
    log.boolean("fg_debug_applied",a.debugApplied);log.number("fg_debug_result",a.debugResult);
    log.text("fg_totals_scope","presenter_lifetime; window_counts_in_measurement_summary");
    log.number("fg_total_requested",t.requested);log.number("fg_total_enabled",t.enabled);
    log.number("fg_total_multi_output",t.multiOutput);log.number("fg_total_single_output",t.singleOutput);
    log.number("fg_total_zero_output",t.zeroOutput);log.number("fg_total_query_unavailable",t.queryUnavailable);
    log.number("fg_total_result_warnings",t.resultWarnings);log.number("fg_total_result_errors",t.resultErrors);
    log.number("fg_total_history_resets",t.historyResets);
}
void cpuWallFields(JsonLine& log, const xess_vk::presentation::FrameStatus& frame) noexcept {
    log.number("output_width", frame.outputWidth);
    log.number("output_height", frame.outputHeight);
    log.number("present_mode", frame.presentMode);
    log.number("cpu_wall_control_epoch", frame.controlEpoch);
    // Difference cumulative totals only within stable extent/epoch intervals.
    // Detail overlaps output, LL is cross-thread, and lifetime maxima do not subtract.
    log.boolean("cpu_wall_cumulative", true);
    log.boolean("cpu_wall_nested_not_additive", true);
    log.boolean("cpu_wall_ll_cross_thread", true);
    log.text("cpu_wall_max_scope", "owner_lifetime");
    log.cpuWall("cpu_runtime_copy", frame.runtimeCopyCpu);
    log.cpuWall("cpu_native_present", frame.nativePresentCpu);
    log.cpuWall("cpu_output", frame.outputCpu);
    log.cpuWall("cpu_bridge_publish", frame.bridgePublishCpu);
    log.cpuWall("cpu_previous_d3d_wait", frame.previousD3DWaitCpu);
    log.cpuWall("cpu_dxgi_present", frame.dxgiPresentCpu);
    log.cpuWall("cpu_xell_sleep", frame.xellSleepCpu);
    log.boolean("async_copy_path", frame.asyncCopyPath);
    sharedCopyFields(log,frame);
    log.number("inflight_frames", frame.inflightFrames);
    log.number("inflight_limit", frame.inflightLimit);
    log.number("inflight_high_water", frame.inflightHighWater);
    log.number("inflight_cap_waits_total", frame.inflightCapWaitCpu.count);
    log.cpuWall("cpu_inflight_cap_wait", frame.inflightCapWaitCpu);
    if(frame.vkPrivateCopyGpu.requested)gpuTimestampFields(log,"vk_private_copy_gpu",frame.vkPrivateCopyGpu);
    if(frame.vkInteropCopyGpu.requested)gpuTimestampFields(log,"vk_interop_copy_gpu",frame.vkInteropCopyGpu);
    if(frame.d3dCallerCopyTagGpu.requested)gpuTimestampFields(log,"d3d_caller_copy_tag_gpu",frame.d3dCallerCopyTagGpu);
}
void srIdentityCacheFields(JsonLine&,GameSrRuntime*) noexcept;
void trackingCpuFields(JsonLine& log, DeviceState* state) noexcept {
    if constexpr(releaseMinimal)return;
    const auto window=takeTrackingCpuReport(state);const auto& totals=window.totals;const auto& peaks=window.peaks;
    static constexpr const char* fields[]{"cpu_tracking_descriptor_update", "cpu_tracking_descriptor_lifetime",
        "cpu_tracking_bind", "cpu_tracking_dispatch", "cpu_tracking_recording", "cpu_tracking_resource_lifetime",
        "cpu_tracking_draw"}; // direct draws only; indirect-count draws are not timed
    log.handle("device", reinterpret_cast<std::uint64_t>(state->device));
    log.number("cpu_wall_owner_generation", state->presentation->timingContextGeneration());
    // A false flag means unavailable counters, not a measurement of zero work.
    log.boolean("cpu_tracking_timing_enabled", trackingCpuTimingEnabled());
    log.boolean("cpu_tracking_driver_excluded", true);
    log.boolean("cpu_tracking_aggregate_across_threads", true);
    log.boolean("cpu_tracking_overlaps_runtime", true);
    log.boolean("cpu_tracking_counter_lock_excluded", true);
    log.number("cpu_tracking_schema",2);log.text("cpu_tracking_max_scope","report_window");
    for (unsigned i = 0; i < trackingCpuGroupCount; ++i){auto report=totals[i];report.maxNs=peaks[i].ns;log.cpuWall(fields[i], report);}
    if(trackingCpuTimingEnabled()){
        JsonLine report("tracking_cpu_window");report.number("schema",2);report.handle("device",bits(state->device));
        report.number("sequence",window.sequence);report.number("begin_qpc",window.beginQpc);report.number("end_qpc",window.endQpc);
        LARGE_INTEGER frequency{};QueryPerformanceFrequency(&frequency);report.number("qpc_frequency",frequency.QuadPart);
        report.text("max_scope","report_window");report.boolean("max_time_is_scope_end",true);
        for(unsigned i=0;i<trackingCpuGroupCount;++i){auto value=totals[i];value.maxNs=peaks[i].ns;report.cpuWall(fields[i],value);
            char name[100];std::snprintf(name,sizeof(name),"%s_peak_qpc",fields[i]);report.number(name,peaks[i].qpc);
            std::snprintf(name,sizeof(name),"%s_peak_thread",fields[i]);report.number(name,peaks[i].thread);
            std::snprintf(name,sizeof(name),"%s_lifetime_max_ns",fields[i]);report.number(name,totals[i].maxNs);}
        const auto descriptor=xess_vk::descriptor_image_observer::stats(state->descriptors);const auto& metadata=descriptor.metadata;
        LARGE_INTEGER countersAt{};QueryPerformanceCounter(&countersAt);report.number("reference_counters_qpc",countersAt.QuadPart);
        report.boolean("reference_consumption_measured",metadata.referenceConsumptionMeasured);
        report.text("reference_mask_policy","on_query");
        report.number("reference_mask_builds",metadata.referenceMaskBuilds);report.number("reference_mask_measured_builds",metadata.referenceMaskMeasuredBuilds);
        report.number("reference_mask_retired_builds",metadata.referenceMaskRetiredBuilds);report.number("reference_mask_consumed_builds",metadata.referenceMaskConsumedBuilds);
        report.number("reference_mask_unconsumed_builds",metadata.referenceMaskUnconsumedBuilds);report.number("reference_mask_pending_builds",metadata.referenceMaskPendingBuilds);
        report.number("reference_edges",metadata.referenceEdges);report.number("reference_edge_overflows",metadata.referenceEdgeOverflows);
        for(unsigned i=0;i<descriptor.referenceQueries.size();++i){const auto& query=descriptor.referenceQueries[i];char name[80];
            const auto* prefix=i?"reference_query_draw":"reference_query_bind";
            std::snprintf(name,sizeof(name),"%s_count",prefix);report.number(name,query.calls);
            std::snprintf(name,sizeof(name),"%s_sets",prefix);report.number(name,query.sets);
            std::snprintf(name,sizeof(name),"%s_contended",prefix);report.number(name,query.contended);
            std::snprintf(name,sizeof(name),"%s_total_ns",prefix);report.number(name,query.totalNs);
            std::snprintf(name,sizeof(name),"%s_wait_ns",prefix);report.number(name,query.waitNs);}
        srIdentityCacheFields(report,state->srRuntime);report.writeSummary();
    }
    log.cpuWall("cpu_tracking_empty_scope", trackingCpuTimingEnabled() ? state->trackingEmptyScope : xess_vk::CpuWallTime{});
#ifdef XESS_FSR_DETOUR_AVAILABLE
    log.cpuWall("cpu_upscale_observe", xess_vk::upscale_callback_observer::observeCpuTime());
#endif
}
constexpr std::int64_t kFrameSpikeThresholdNs = 25'000'000;
constexpr LONG64 kFrameSpikeLogLimit = 4000;
constexpr LONG64 kLongFrameResetTraceLogLimit = 4000;
bool longFrameResetTrace(DeviceState* state, const xess_vk::presentation::FrameStatus& frame,
                         VkResult result, LONG64 attempt) noexcept {
    const auto& trace=frame.longFrameReset;
    if(!state->fakeEnabled||!trace.requested||!trace.traceSequence||
        trace.traceSequence<=state->lastLongFrameResetTraceSequence)return false;
    // Native may retain a snapshot when this attempt fails before output().
    state->lastLongFrameResetTraceSequence=trace.traceSequence;
    const bool outputFresh=trace.outputEntryKnown;
    if(!trace.emit||InterlockedIncrement64(&state->longFrameResetTracesLogged)>kLongFrameResetTraceLogLimit)return outputFresh;
    JsonLine log("long_frame_reset_trace");log.handle("device",bits(state->device));
    log.number("output_attempt",attempt);log.number("trace_sequence",trace.traceSequence);
    log.number("followup_policy_n",trace.policyEnabled?1:0);
    log.number("engine_frame",trace.engineFrame);log.number("sdk_present_id",trace.sdkPresentId);
    log.number("source_copy_token",trace.copyToken);log.number("copy_generation",trace.copyGeneration);
    log.number("timing_context",trace.timingContext);log.boolean("output_entry_known",trace.outputEntryKnown);
    if(trace.outputEntryKnown){log.number("output_entry_qpc_ticks",trace.outputEntryQpcTicks);log.number("qpc_frequency",trace.qpcFrequency);}
    log.boolean("interval_known",trace.intervalKnown);if(trace.intervalKnown)log.number("output_interval_ns",trace.outputIntervalNs);
    log.number("threshold_ns",trace.thresholdNs);log.boolean("long_frame",trace.longFrame);log.boolean("eligible",trace.eligible);
    log.boolean("history_reset_requested",trace.resetRequested);log.number("history_reset_reasons",trace.resetReasons);
    log.boolean("extra_reset_requested",trace.extraRequested);log.boolean("extra_reset_output_succeeded",trace.extraSucceeded);
    log.boolean("on_output_succeeded",trace.outputSucceeded);log.boolean("armed",trace.armed);
    log.boolean("pending_before",trace.pendingBefore);log.boolean("pending_after",trace.pendingAfter);
    log.number("trigger_before_engine_frame",trace.triggerBefore.engineFrame);log.number("trigger_before_sdk_present_id",trace.triggerBefore.sdkPresentId);
    log.number("trigger_before_copy_token",trace.triggerBefore.copyToken);
    log.number("trigger_after_engine_frame",trace.triggerAfter.engineFrame);log.number("trigger_after_sdk_present_id",trace.triggerAfter.sdkPresentId);
    log.number("trigger_after_copy_token",trace.triggerAfter.copyToken);if(trace.clearReason)log.text("clear_reason",trace.clearReason);
    log.number("runtime_result",result);
    const bool outputReturned=state->presentation&&state->presentation->lastSdkOutputSucceeded();
    const bool sdkResultCurrent=outputReturned&&outputFresh;
    log.boolean("runtime_sdk_output_succeeded",outputReturned);log.boolean("sdk_result_current",sdkResultCurrent);
    if(sdkResultCurrent){
        log.boolean("sdk_status_valid",frame.sdkStatusValid);log.number("sdk_query_result",frame.queryResult);log.number("sdk_framegen_result",frame.frameGenResult);
        log.boolean("sdk_requested_enabled",frame.requestedEnabled);log.boolean("sdk_enabled",frame.sdkEnabled);
        log.number("sdk_frames_presented",frame.framesPresented);log.number("sdk_interpolated_frames",frame.interpolatedFrames);
        log.boolean("output_occluded",frame.outputOccluded);
    }
    log.boolean("cpu_wall_totals_current",sdkResultCurrent);
    if(sdkResultCurrent){
        log.boolean("cpu_wall_cumulative",true);log.boolean("cpu_wall_nested_not_additive",true);log.boolean("cpu_wall_ll_cross_thread",true);
        log.cpuWall("cpu_runtime_copy",frame.runtimeCopyCpu);log.cpuWall("cpu_native_present",frame.nativePresentCpu);log.cpuWall("cpu_output",frame.outputCpu);
        log.cpuWall("cpu_copy_fence_wait",frame.copyFenceWaitCpu);log.cpuWall("cpu_copy_consumed_wait",frame.copyConsumedWaitCpu);
        log.cpuWall("cpu_previous_d3d_wait",frame.previousD3DWaitCpu);log.cpuWall("cpu_bridge_publish",frame.bridgePublishCpu);
        log.cpuWall("cpu_dxgi_present",frame.dxgiPresentCpu);log.cpuWall("cpu_xell_sleep",frame.xellSleepCpu);
    }
    log.write();
    return outputFresh;
}
std::int64_t steadyNowNs() noexcept {
    return static_cast<std::int64_t>(xess_vk::cpuWallNowNs());
}
void copyFenceSlow(DeviceState* state, const xess_vk::presentation::CopyFenceTiming& copy) noexcept {
    if (!state->fakeEnabled || !copy.sequence || !copy.waitReturnNs ||
        copy.waitReturnNs - copy.waitEntryNs < xess_vk::wsi_acquire::slowCallThresholdNs ||
        copy.waitReturnNs <= state->lastSlowCopyReturnNs) return;
    state->lastSlowCopyReturnNs = copy.waitReturnNs;
    if (!slowCallBudget(state, state->slowCopyCalls, "copy_fence")) return;
    JsonLine log("fake_copy_fence_slow"); log.handle("device", bits(state->device));
    log.number("copy_sequence", copy.sequence); log.number("source_engine_frame", copy.engineFrame);
    log.number("source_copy_token", copy.sequence); log.handle("queue", copy.queue);
    log.number("swapchain_generation", copy.generation); log.handle("source_image", copy.image); log.handle("fence", copy.fence);
    log.number("submit_entry_qpc_us", copy.submit.entryNs / 1000);
    log.number("submit_driver_entry_qpc_us", copy.submit.driverEntryNs / 1000);
    log.number("submit_return_qpc_us", copy.submit.returnNs / 1000);
    log.number("wait_entry_qpc_us", copy.waitEntryNs / 1000); log.number("wait_return_qpc_us", copy.waitReturnNs / 1000);
    log.number("fence_wait_us", (copy.waitReturnNs - copy.waitEntryNs) / 1000);
    log.number("submit_to_wait_return_us", (copy.waitReturnNs - copy.submit.returnNs) / 1000);
    log.number("wait_result", copy.waitResult); log.text("completion_scope", "cpu_fence_wait_return_not_gpu_timestamp");
    log.boolean("query_match",copy.privateCopyQueryMatch);
    if(copy.privateCopyGpu.requested)gpuTimestampFields(log,"vk_private_copy_gpu",copy.privateCopyGpu);
    log.write();
}
void flushFrameSpikes(DeviceState* state,std::int64_t nowNs,const char* reason) noexcept {
    if constexpr(releaseMinimal)return;
    if constexpr(researchDiagnostics)return;
    auto& w=state->frameSpikeWindow;if(!w.startedNs||!w.intervals)return;
    JsonLine summary("frame_spike_summary");summary.text("reason",reason);
    summary.number("window_start_qpc_us",w.startedNs/1000);summary.number("window_end_qpc_us",nowNs/1000);
    summary.number("present_intervals",w.intervals);summary.number("spikes_over_25ms",w.spikes);
    summary.number("maximum_interval_us",w.maximumNs/1000);summary.number("dropped_details",w.droppedDetails);
    std::uint64_t retained{};for(const auto& d:w.worst)retained+=d.bytes!=0;
    summary.number("retained_details",retained);summary.text("detail_policy","worst_three_per_60s");summary.write();
    for(const auto& d:w.worst)if(d.bytes)writeLogLine(d.text.data(),static_cast<DWORD>(d.bytes));
    w.reset(nowNs);
}
// Research: one record per present interval over 25 ms. Product: periodic
// summary and three worst records, preserving each frame's own share of
// every timed component (cumulative totals differenced). The steady clock is
// QPC based, so qpc_us lines up with PresentMon --qpc_time_ms.
void recordFrameSpike(DeviceState* state, const xess_vk::presentation::FrameStatus& frame, bool outputFresh,
                      std::int64_t entryNs, std::uint32_t engineFrame, LONG64 attempt) noexcept {
    if constexpr(releaseMinimal)return;
    const auto nowNs = steadyNowNs();
    copyFenceSlow(state, frame.previousCopy);
    copyFenceSlow(state, frame.immediateCopy);
    std::array<std::uint64_t, 10> presentation{frame.runtimeCopyCpu.totalNs, frame.nativePresentCpu.totalNs,
        frame.outputCpu.totalNs, frame.bridgePublishCpu.totalNs, frame.previousD3DWaitCpu.totalNs,
        frame.dxgiPresentCpu.totalNs, frame.xellSleepCpu.totalNs, frame.copyFenceWaitCpu.totalNs,
        frame.copyConsumedWaitCpu.totalNs, frame.inflightCapWaitCpu.totalNs};
    // Backend totals read as zero on a frame without SDK output; keep the
    // previous baseline so the next frame does not report the whole total.
    for (std::size_t i = 0; i < presentation.size(); ++i)
        if (!presentation[i]) presentation[i] = state->frameSpikePresentation[i];
    std::uint64_t tracking{};
    for (const auto& group : trackingCpuSnapshot(state)) tracking += group.totalNs;
    // A new presenter owner restarts its totals; then its current total is this frame's share.
    const auto delta = [](std::uint64_t now, std::uint64_t before) {
        return static_cast<std::int64_t>((now >= before ? now - before : now) / 1000);
    };
    int retainedSlot=-1;
    if constexpr(!researchDiagnostics){
        if(state->frameSpikeWindow.due(nowNs))flushFrameSpikes(state,nowNs,"periodic");
        state->frameSpikeWindow.observe(nowNs,state->frameSpikeValid?nowNs-state->frameSpikePresentNs:0);
        if(state->frameSpikeValid&&nowNs-state->frameSpikePresentNs>kFrameSpikeThresholdNs)
            retainedSlot=state->frameSpikeWindow.consider(nowNs-state->frameSpikePresentNs);
    }
    const bool longFrame=state->frameSpikeValid&&nowNs-state->frameSpikePresentNs>kFrameSpikeThresholdNs;
    const bool keep=researchDiagnostics?longFrame&&InterlockedIncrement64(&state->frameSpikesLogged)<=kFrameSpikeLogLimit:retainedSlot>=0;
    if (longFrame && keep) {
        static constexpr const char* names[]{"runtime_copy_us", "native_present_us", "output_us", "bridge_publish_us",
            "previous_d3d_wait_us", "dxgi_present_us", "xell_sleep_us", "copy_fence_wait_us", "copy_consumed_wait_us", "inflight_cap_wait_us"};
        JsonLine log("frame_spike");
        log.number("qpc_us", nowNs / 1000);
        log.number("interval_us", (nowNs - state->frameSpikePresentNs) / 1000);
        log.number("present_call_us", (nowNs - entryNs) / 1000);
        log.number("engine_frame", engineFrame);
        log.number("output_attempt", attempt);
        log.boolean("async_copy_path", frame.asyncCopyPath);
        sharedCopyFields(log,frame);
        log.number("inflight_frames", frame.inflightFrames);
        log.number("inflight_limit", frame.inflightLimit);
        log.number("inflight_high_water", frame.inflightHighWater);
        const auto capWaits=frame.inflightCapWaitCpu.count;
        log.number("inflight_cap_waits",capWaits>=state->frameSpikeCapWaits?capWaits-state->frameSpikeCapWaits:capWaits);
        log.number("inflight_cap_waits_total",capWaits);
        log.boolean("long_frame_trace_current",outputFresh);
        if(outputFresh){
            const auto& trace=frame.longFrameReset;
            log.number("long_frame_trace_sequence",trace.traceSequence);log.boolean("output_interval_known",trace.intervalKnown);
            if(trace.intervalKnown)log.number("output_interval_ns",trace.outputIntervalNs);
            log.number("output_threshold_ns",trace.thresholdNs);log.boolean("long_frame",trace.longFrame);
            log.number("history_reset_reasons",trace.resetReasons);log.boolean("history_reset_requested",trace.resetRequested);
        }
        for (std::size_t i = 0; i < presentation.size(); ++i)
            log.number(names[i], delta(presentation[i], state->frameSpikePresentation[i]));
        log.number("tracking_us", delta(tracking, state->frameSpikeTracking));
        log.boolean("cpu_tracking_timing_enabled", trackingCpuTimingEnabled());
        if(frame.vkPrivateCopyGpu.requested)gpuTimestampFields(log,"vk_private_copy_gpu",frame.vkPrivateCopyGpu);
        if(frame.vkInteropCopyGpu.requested)gpuTimestampFields(log,"vk_interop_copy_gpu",frame.vkInteropCopyGpu);
        if(frame.d3dCallerCopyTagGpu.requested)gpuTimestampFields(log,"d3d_caller_copy_tag_gpu",frame.d3dCallerCopyTagGpu);
        if constexpr(researchDiagnostics)log.write();
        else {log.raw("}\n");state->frameSpikeWindow.retain(retainedSlot,nowNs-state->frameSpikePresentNs,log.good?log.buffer:nullptr,log.used);}
    }
    state->frameSpikePresentNs = nowNs;
    state->frameSpikePresentation = presentation;
    state->frameSpikeCapWaits=frame.inflightCapWaitCpu.count;
    state->frameSpikeTracking = tracking;
    state->frameSpikeValid = true;
}

constexpr const char* measurementMetricNames[]{"present_entry_interval","present_call","captured_frame_submit_interval","cap_wait","copy_fence_wait","consumed_wait","dxgi_present",
    "app_transfer_commands","depth_dispatches","provider_depth_compute_gpu","provider_depth_transfer_gpu","provider_aux_gpu","native_gpu","bridge_gpu","caller_gpu","statistics_emit_cpu","sr_gpu"};
static_assert(std::size(measurementMetricNames)==xess_vk::measurement::metricCount);
void recordMeasurementFailures(DeviceState* state,const xess_vk::continuous::Stats& inputs,
                               const xess_vk::presentation::FrameStatus& frame) noexcept {
    using namespace xess_vk::measurement;
    const auto delta=[](std::uint64_t value,std::uint64_t& previous){const auto result=value>=previous?value-previous:value;previous=value;return result;};
    increment(Counter::CopyFailures,delta(inputs.copyFailures,state->measurementCopyFailures));
    increment(Counter::NoFreeSlots,delta(inputs.saturated,state->measurementProviderNoFree));
    if(frame.sharedPlanes.available)increment(Counter::NoFreeSlots,delta(frame.sharedPlanes.noFreeFallbacks,state->measurementSharedNoFree));
}
template<class Sink> void srMeasurementFields(Sink&,GameSrRuntime*) noexcept;
void emitMeasurementSummary(DeviceState* state,std::uint64_t now,bool force=false) noexcept {
    using namespace xess_vk::measurement;
    if(!state->measurements||!diagnosticLoggingEnabled())return;
    SummarySnapshot snapshot;
    if(!state->measurements->takeSummary(now,snapshot,force))return;
    JsonLine line("measurement_summary");line.number("schema",1);line.handle("device",bits(state->device));
    line.number("window",snapshot.sequence);line.number("begin_ns",snapshot.beginNs);line.number("end_ns",snapshot.endNs);
    line.boolean("partial",force);line.text("percentile_method","full_window_nearest_rank_histogram_bounds");
    line.number("histogram_precision_bits",histogramPrecisionBits);
    line.text("submission_interval_scope","successfully_submitted_captured_game_frames");
    line.text("copy_command_scope","app_injected_commands_in_successful_submissions_not_sdk_internal_work");
    line.boolean("sdk_internal_copy_count_available",false);line.key("sdk_internal_copy_commands");line.raw("null");
    line.number("writer_failures_total",measurementSummaryLog.failures()+droppedDiagnostic(DiagnosticStream::Summary));
    line.raw(",\"metrics\":{");
    for(unsigned i=0;i<metricCount;++i){if(i)line.raw(",");line.quoted(measurementMetricNames[i]);line.raw(":{");
        const auto& m=snapshot.metrics[i];line.raw("\"count\":");char count[32];std::snprintf(count,sizeof(count),"%llu",static_cast<unsigned long long>(m.count));line.raw(count);
        line.number("sum",m.totalNs);line.boolean("sum_saturated",m.totalSaturated);line.number("maximum",m.maximumNs);
        line.number("over_15ms",m.over15Ms);line.number("over_25ms",m.over25Ms);line.number("over_40ms",m.over40Ms);line.number("over_100ms",m.over100Ms);
        const auto percentile=[&](const char* key,const Percentile& value){line.key(key);if(!m.count){line.raw("null");return;}
            char bounds[96];std::snprintf(bounds,sizeof(bounds),"[%llu,%llu]",static_cast<unsigned long long>(value.lowerNs),static_cast<unsigned long long>(value.upperNs));line.raw(bounds);};
        percentile("p10",m.p10);percentile("p50",m.p50);percentile("p90",m.p90);percentile("p95",m.p95);percentile("p99",m.p99);line.raw("}");}
    line.raw("}");
    constexpr const char* counterNames[]{"menu_on_frames","menu_off_frames","menu_unknown_frames","sdk_single_frames","sdk_double_frames","copy_failures","no_free_slots","sdk_errors","sdk_warnings","device_lost_observations",
        "sr_executes","sr_skips","sr_skip_not_ready","sr_skip_inputs","sr_skip_recording","sr_skip_configuration","sr_skip_sdk","sr_skip_other","sdk_triple_frames","sdk_quad_frames",
        "fg_requested_frames","fg_enabled_frames","fg_multi_output","fg_single_output","fg_zero_output","fg_query_unavailable","fg_history_resets","fg_result_warnings","fg_result_errors"};
    static_assert(std::size(counterNames)==counterCount);
    line.raw(",\"counters\":{");for(unsigned i=0;i<counterCount;++i){if(i)line.raw(",");line.quoted(counterNames[i]);line.raw(":");char count[32];std::snprintf(count,sizeof(count),"%llu",static_cast<unsigned long long>(snapshot.counters[i]));line.raw(count);}line.raw("}");
    const auto single=snapshot.counters[static_cast<unsigned>(Counter::SdkSingleFrames)],doubles=snapshot.counters[static_cast<unsigned>(Counter::SdkDoubleFrames)];
    const auto allFrames=single+doubles+snapshot.counters[static_cast<unsigned>(Counter::SdkTripleFrames)]+snapshot.counters[static_cast<unsigned>(Counter::SdkQuadFrames)];
    line.key("sdk_double_frame_ratio");if(allFrames)line.raw("{");else line.raw("null");
    if(allFrames){line.raw("\"numerator\":");char ratio[96];std::snprintf(ratio,sizeof(ratio),"%llu,\"denominator\":%llu}",static_cast<unsigned long long>(doubles),static_cast<unsigned long long>(allFrames));line.raw(ratio);}
    constexpr const char* roles[]{"color","depth","motion","sdk","unclassified"};constexpr const char* commands[]{"image_copy","image_to_buffer","buffer_to_image","blit","depth_dispatch","d3d_copy"};
    static_assert(std::size(roles)==copyRoleCount);static_assert(std::size(commands)==static_cast<unsigned>(Command::Count));
    line.raw(",\"copy_commands\":{");for(unsigned i=0;i<copyRoleCount;++i){if(i)line.raw(",");line.quoted(roles[i]);if(i==static_cast<unsigned>(CopyRole::Sdk)){line.raw(":null");continue;}line.raw(":{");
        for(unsigned j=0;j<std::size(commands);++j){if(j)line.raw(",");line.quoted(commands[j]);line.raw(":");char value[32];std::snprintf(value,sizeof(value),"%llu",static_cast<unsigned long long>(snapshot.copyCommands[i][j]));line.raw(value);}line.raw("}");}line.raw("}");
    line.boolean("sr_runtime_connected",state->srRuntime!=nullptr);
    line.text("sr_counter_scope","application_command_recorded_not_gpu_complete");
    srMeasurementFields(line,state->srRuntime);line.writeSummary();
}
MeasurementFrameState classifyMeasurementFrame(MeasurementInterval& interval,
        const xess_vk::presentation::FrameStatus& frame,bool outputAttempted,bool outputSucceeded) noexcept {
    using namespace xess_vk::presentation;
    using namespace xess_vk::frame_input;
    constexpr auto required=Extents|Jitter|MotionScale|View|Projection|CameraPlanes|Reset;
    // Stall remedies do not turn a real hitch into an excluded scene transition.
    constexpr auto semanticResetMask=~(HistoryResetLongFrame|HistoryResetFollowup);
    auto result=MeasurementFrameState::Unknown;
    const bool resetKnown=outputAttempted&&frame.historyResetKnown;
    const bool reset=resetKnown&&(frame.historyResetReasons&semanticResetMask);
    if(outputAttempted&&frame.menuPaired){
        if(!frame.menuRequestedEnabled)result=MeasurementFrameState::Off;
        else if(!frame.menuGatesConsistent||!frame.menuGatesReady||!frame.menuInputObserved||
                !frame.inputMatched||(frame.inputValidity&required)!=required||
                (frame.onBlockedReason&&frame.onBlockedReason[0]))result=MeasurementFrameState::Invalid;
        else if(resetKnown){
            if(reset||(interval.baselineKnown&&(interval.controlEpoch!=frame.controlEpoch||
                    interval.viewportGeneration!=frame.viewportGeneration)))result=MeasurementFrameState::Transition;
            else if(interval.baselineKnown)result=MeasurementFrameState::Steady;
        }
        interval.baselineKnown=true;
        interval.controlEpoch=frame.controlEpoch;interval.viewportGeneration=frame.viewportGeneration;
    }else interval.baselineKnown=false;
    ++interval.frames[static_cast<unsigned>(result)];
    interval.resets+=reset;interval.historyResets+=resetKnown&&frame.historyResetReasons!=0;
    interval.outputFailures+=outputAttempted&&!outputSucceeded;
    // SDK results are outcomes, never an admission condition for steady frames.
    if(outputAttempted&&(frame.sdkResultKnown||frame.sdkStatusValid)){
        const bool warning=frame.queryResult>0||frame.frameGenResult>0;
        const bool error=frame.queryResult<0||frame.frameGenResult<0;
        interval.warnings+=warning;interval.errors+=error;
        if(error&&result==MeasurementFrameState::Steady)++interval.steadyErrors;
        if(warning){
            if(resetKnown&&frame.historyResetReasons)++interval.fallbackWarnings;
            else if(result==MeasurementFrameState::Steady)++interval.steadyWarnings;
            else if(result==MeasurementFrameState::Off||result==MeasurementFrameState::Invalid||result==MeasurementFrameState::Transition)
                ++interval.fallbackWarnings;
            else ++interval.unknownWarnings;
        }
    }
    return result;
}
template<class Sink> void srMeasurementFields(Sink&,GameSrRuntime*) noexcept;
template<class Sink> void measurementIntervalFields(Sink& status,const MeasurementInterval& interval,std::uint64_t begin,std::uint64_t end) noexcept {
    std::uint64_t attempts{};unsigned kinds{};const char* selected="unknown";
    constexpr const char* names[]{"steady_fg_input_valid","fg_off","input_invalid","reset_transition","unknown"};
    for(unsigned i=0;i<interval.frames.size();++i){attempts+=interval.frames[i];if(interval.frames[i]){++kinds;selected=names[i];}}
    status.number("status_begin_ns",begin);status.number("status_end_ns",end);
    status.text("status_interval_state",kinds>1?"mixed":selected);
    status.number("status_interval_attempts",attempts);
    status.number("status_interval_steady_frames",interval.frames[0]);status.number("status_interval_fg_off_frames",interval.frames[1]);
    status.number("status_interval_invalid_frames",interval.frames[2]);status.number("status_interval_transition_frames",interval.frames[3]);
    status.number("status_interval_unknown_frames",interval.frames[4]);status.number("status_interval_resets",interval.resets);
    status.number("status_interval_history_resets",interval.historyResets);
    status.text("status_reset_exclusion","semantic_only_stall_remedies_retained");
    status.number("status_interval_sdk_warnings",interval.warnings);status.number("status_interval_steady_sdk_warnings",interval.steadyWarnings);
    status.number("status_interval_fallback_sdk_warnings",interval.fallbackWarnings);status.number("status_interval_unknown_sdk_warnings",interval.unknownWarnings);
    status.number("status_interval_sdk_errors",interval.errors);status.number("status_interval_steady_sdk_errors",interval.steadyErrors);
    status.number("status_interval_output_failures",interval.outputFailures);
}
void recordMeasurements(DeviceState* state,const xess_vk::presentation::FrameStatus& frame,bool fresh,
                        std::uint64_t entryNs,std::uint64_t attempt,const xess_vk::measurement::Commands& commands) noexcept {
    using namespace xess_vk::measurement;
    if(!state->measurements||!state->measurementSnapshot)return;
    const auto now=xess_vk::cpuWallNowNs();
    classifyMeasurementFrame(state->measurementInterval,frame,
        state->presentation?state->presentation->lastSdkOutputAttempted():fresh,fresh);
    const auto entryInterval=state->measurementPresentEntryNs&&entryNs>=state->measurementPresentEntryNs?entryNs-state->measurementPresentEntryNs:0;
    observe(Metric::PresentCall,now-entryNs,attempt,frame.controlEpoch);
    if(state->measurementPresentEntryNs&&entryNs>=state->measurementPresentEntryNs)
        observe(Metric::PresentEntryInterval,entryNs-state->measurementPresentEntryNs,attempt,frame.controlEpoch);
    state->measurementPresentEntryNs=entryNs;
    increment(!frame.menuPaired?Counter::MenuUnknownFrames:frame.menuRequestedEnabled?Counter::MenuEnabledFrames:Counter::MenuDisabledFrames);
    if(fresh&&frame.sdkStatusValid){
        if(frame.framesPresented==1)increment(Counter::SdkSingleFrames);
        else if(frame.framesPresented==2)increment(Counter::SdkDoubleFrames);
        else if(frame.framesPresented==3)increment(Counter::SdkTripleFrames);
        else if(frame.framesPresented==4)increment(Counter::SdkQuadFrames);
    }
    if(state->continuous)recordMeasurementFailures(state,state->continuous->stats(),frame);
    emitMeasurementSummary(state,now);
    if(fresh) {
        std::uint64_t transfers{},dispatches{};
        for(unsigned i=0;i<commands.size();++i){const auto value=commands[i]+frame.inputCommands[i];
            if(i==static_cast<unsigned>(Command::DepthDispatch))dispatches+=value;else transfers+=value;}
        observe(Metric::TransferCommands,transfers,frame.sdkPresentId,frame.controlEpoch);
        observe(Metric::DepthDispatches,dispatches,frame.sdkPresentId,frame.controlEpoch);
    }
    const auto copyWait=frame.copyFenceWaitCpu.totalNs;
    const auto copyDelta=copyWait>=state->frameSpikePresentation[7]?copyWait-state->frameSpikePresentation[7]:copyWait;
    if(!releaseMinimal&&(entryInterval>25'000'000||now-entryNs>25'000'000||copyDelta>10'000'000||state->measurementPreviousCopyWaitNs>10'000'000)){
        JsonLine line("measurement_long_frame");line.number("qpc_ns",now);line.number("entry_ns",entryNs);
        line.number("attempt",attempt);line.number("engine_frame",frame.engineFrame);line.number("sdk_id",frame.sdkPresentId);
        line.number("call_ns",now-entryNs);line.number("entry_interval_ns",entryInterval);line.number("copy_wait_ns",copyDelta);
        line.number("previous_copy_wait_ns",state->measurementPreviousCopyWaitNs);
        line.number("dxgi_ns",frame.dxgiPresentCpu.totalNs>=state->frameSpikePresentation[5]?frame.dxgiPresentCpu.totalNs-state->frameSpikePresentation[5]:frame.dxgiPresentCpu.totalNs);
        line.boolean("output_fresh",fresh);
        gpuTimestampFields(line,"provider_depth_gpu",frame.providerDepthGpu);gpuTimestampFields(line,"provider_aux_gpu",frame.providerAuxGpu);
        gpuTimestampFields(line,"vk_private_copy_gpu",frame.vkPrivateCopyGpu);gpuTimestampFields(line,"vk_interop_copy_gpu",frame.vkInteropCopyGpu);
        gpuTimestampFields(line,"d3d_caller_copy_tag_gpu",frame.d3dCallerCopyTagGpu);line.write(true);
    }
    state->measurementPreviousCopyWaitNs=copyDelta;
    auto& snapshot=*state->measurementSnapshot;
    const auto dumpStart=xess_vk::cpuWallNowNs();
    if(!state->measurements->take(now,snapshot))return;
    if constexpr(!releaseMinimal)for(unsigned i=0;i<metricCount;++i){const auto& metric=snapshot.metrics[i];
        JsonLine line("measurement_window");line.number("schema",1);line.handle("device",bits(state->device));
        line.number("window",snapshot.sequence);line.number("begin_ns",snapshot.beginNs);line.number("end_ns",snapshot.endNs);
        line.text("metric",measurementMetricNames[i]);line.text("unit",i==static_cast<unsigned>(Metric::TransferCommands)||i==static_cast<unsigned>(Metric::DepthDispatches)?"commands":"ns");
        line.number("count",metric.count);line.number("sum",metric.totalNs);line.number("maximum",metric.maximumNs);
        line.number("over_15ms",metric.over15Ms);line.number("over_40ms",metric.over40Ms);line.number("dropped",metric.dropped);
        line.raw(",\"values_key_owner\":[");
        for(unsigned j=0;j<metric.stored;++j){if(j)line.raw(",");char value[96];const auto& v=metric.values[j];
            std::snprintf(value,sizeof(value),"[%llu,%llu,%llu]",static_cast<unsigned long long>(v.value),static_cast<unsigned long long>(v.key),static_cast<unsigned long long>(v.owner));line.raw(value);}
        line.raw("]");line.write(true);
    }
    const auto baseline=state->measurementInterval;
    state->measurementInterval={};state->measurementInterval.baselineKnown=baseline.baselineKnown;
    state->measurementInterval.controlEpoch=baseline.controlEpoch;state->measurementInterval.viewportGeneration=baseline.viewportGeneration;
    JsonLine status("measurement_status");status.number("window",snapshot.sequence);status.number("qpc_ns",now);
    status.text("build_flavor",releaseMinimal?"release-minimal":"development");
    if constexpr(releaseMinimal)status.number("release_status_schema",1);
    measurementIntervalFields(status,baseline,snapshot.beginNs,snapshot.endNs);
    status.number("writer_failures_total",measurementLog.failures()+droppedDiagnostic(DiagnosticStream::Detail));
    status.boolean("async_log_requested",asyncLogRequested.load());
    status.number("async_log_start_failures",logStartFailures.load());
    if(const auto* writer=asyncLogWriter.load()){
        const auto diagnostics=writer->stats();
        status.boolean("async_log_active",diagnostics.active);status.number("async_log_accepted",diagnostics.accepted);
        status.number("async_log_written",diagnostics.written);status.number("async_log_pending",diagnostics.pending);
        status.number("async_log_sink_failures",diagnostics.writeFailures);status.number("async_log_max_write_ns",diagnostics.maximumWriteNs);
        status.number("async_log_dropped_verbose",diagnostics.dropped[0]);status.number("async_log_dropped_detail",diagnostics.dropped[1]);
        status.number("async_log_dropped_summary",diagnostics.dropped[2]);status.number("async_log_dropped_observation",diagnostics.dropped[3]);
    }
    status.number("attempt",attempt);status.number("engine_frame",frame.engineFrame);status.number("sdk_id",frame.sdkPresentId);
    status.number("sdk_query_result",frame.queryResult);status.number("sdk_framegen_result",frame.frameGenResult);
    status.number("fg_multiplier_requested",frame.multiplierRequested);status.number("fg_multiplier_actual",frame.interpolatedFrames+1);
    status.number("fg_multiplier_maximum",frame.multiplierMaximum);
    status.text("fg_multiplier_change_policy","next_process");
    fgTuningFields(status,frame);
    status.number("sdk_frames",frame.framesPresented);status.boolean("output_fresh",fresh);status.boolean("sdk_result_known",frame.sdkResultKnown);
    status.number("control_epoch",frame.controlEpoch);status.number("inflight_limit",frame.inflightLimit);
    status.boolean("async",frame.asyncCopyPath);sharedCopyFields(status,frame);
    status.text("copy_route",frame.asyncCopyPath?(frame.sharedCopyPath?"async_shared":"async_copy"):
        (frame.sharedCopyPath?"synchronous_shared":"synchronous"));
    status.boolean("menu_paired",frame.menuPaired);status.boolean("menu_requested_enabled",frame.menuRequestedEnabled);
    status.boolean("menu_gates_consistent",frame.menuGatesConsistent);status.boolean("input_matched",frame.inputMatched);
    status.number("input_validity",frame.inputValidity);status.boolean("history_reset_known",frame.historyResetKnown);
    status.number("history_reset_reasons",frame.historyResetReasons);
    srMeasurementFields(status,state->srRuntime);
    status.boolean("sr_runtime_connected",state->srRuntime!=nullptr);
    if(!state->srRuntime)status.text("sr_state",state->srRuntimeRequested?"sr_unavailable":"off_not_connected");
    if(state->continuous){const auto inputs=state->continuous->stats();status.number("input_captured",inputs.captured);status.number("input_consumed",inputs.consumed);
        status.number("copy_failures",inputs.copyFailures);status.number("query_failures",inputs.timestampFailures);status.number("query_unavailable",inputs.timestampUnavailable);status.number("query_completed",inputs.timestampCompleted);
        status.number("submission_interval_resets",inputs.submissionIntervalResets);}
    gpuTimestampFields(status,"provider_depth_gpu",frame.providerDepthGpu);gpuTimestampFields(status,"provider_aux_gpu",frame.providerAuxGpu);
    gpuTimestampFields(status,"vk_private_copy_gpu",frame.vkPrivateCopyGpu);gpuTimestampFields(status,"vk_interop_copy_gpu",frame.vkInteropCopyGpu);
    gpuTimestampFields(status,"d3d_caller_copy_tag_gpu",frame.d3dCallerCopyTagGpu);
    status.write(true);
    observe(Metric::StatisticsEmitCpu,xess_vk::cpuWallNowNs()-dumpStart,attempt,frame.controlEpoch);
}

template<class T> uint64_t bits(T handle) noexcept { return reinterpret_cast<uint64_t>(handle); }
bool wsiAcquireRequested() noexcept {
    if constexpr(releaseMinimal)return false;
    wchar_t value[2]{};
    return GetEnvironmentVariableW(L"XESS_WSI_ACQUIRE_OBSERVER",value,2)==1 && value[0]==L'1';
}
const char* wsiAcquireApi(xess_vk::wsi_acquire::Api api) noexcept {
    using xess_vk::wsi_acquire::Api;
    return api==Api::Acquire?"vkAcquireNextImageKHR":api==Api::Acquire2?"vkAcquireNextImage2KHR":"vkGetSwapchainStatusKHR";
}
void wsiAcquireIdentity(JsonLine& log,DeviceState* state,const xess_vk::wsi_acquire::Call& call) noexcept {
    log.handle("device",bits(state->device));log.handle("swapchain",call.swapchain);
    log.number("swapchain_generation",call.generation);
    log.boolean("swapchain_generation_known",call.generation!=0);
    log.text("api",wsiAcquireApi(call.api));log.number("thread_id",call.thread);
    log.number("last_present_thread_id_at_entry",call.presentThread);
    log.text("present_thread_relation",!call.presentThread?"not_observed":call.thread==call.presentThread?"same":"different");
}
void wsiAcquireSummary(DeviceState* state,const char* reason) noexcept {
    if(!state->wsiAcquire)return;
    try {
        const auto totals=state->wsiAcquire->snapshot();
        for(unsigned api=0;api<xess_vk::wsi_acquire::apiCount;++api) {
            const auto& stats=totals.api[api];auto latest=stats.latest;latest.api=static_cast<xess_vk::wsi_acquire::Api>(api);
            JsonLine log("wsi_acquire_summary");wsiAcquireIdentity(log,state,latest);
            log.text("reason",reason);log.text("scope","device_lifetime_api");
            log.number("calls",stats.calls);log.number("all_api_calls",totals.calls);
            log.number("cpu_total_ns",stats.totalNs);log.number("cpu_max_ns",stats.maxNs);
            log.number("thread_not_observed",stats.threadUnknown);log.number("thread_same",stats.threadSame);log.number("thread_different",stats.threadDifferent);
            log.number("sync_none",stats.synchronization[0]);log.number("sync_semaphore_only",stats.synchronization[1]);
            log.number("sync_fence_only",stats.synchronization[2]);log.number("sync_both",stats.synchronization[3]);
            log.number("unknown_arguments",stats.unknownArguments);
            log.number("pnext_calls",stats.pNextCalls);log.number("device_mask_not_one_calls",stats.deviceMaskNotOneCalls);
            log.number("latest_result",stats.latest.result);log.number("latest_cpu_elapsed_ns",stats.latest.elapsedNs);
            log.key("timeout_values_ns");log.raw("[");bool first=true;
            for(const auto& item:stats.timeouts)if(item.count){
                char text[128];std::snprintf(text,sizeof(text),"%s{\"value\":\"%llu\",\"count\":%llu}",first?"":",",static_cast<unsigned long long>(item.value),static_cast<unsigned long long>(item.count));
                log.raw(text);first=false;
            }
            log.raw("]");log.number("timeout_value_overflow",stats.timeoutValueOverflow);
            log.key("results");log.raw("[");first=true;
            for(const auto& item:stats.results)if(item.count){
                char text[96];std::snprintf(text,sizeof(text),"%s{\"result\":%d,\"count\":%llu}",first?"":",",item.value,static_cast<unsigned long long>(item.count));
                log.raw(text);first=false;
            }
            log.raw("]");log.number("result_value_overflow",stats.resultValueOverflow);
            log.number("periodic_summaries",totals.periodicSummaries);log.number("omitted_periodic_summaries",totals.omittedPeriodicSummaries);
            log.boolean("synchronization_verified",false);log.write();
        }
    }catch(...){} // Observation must never change a Vulkan result.
}
xess_vk::wsi_acquire::Call wsiAcquireBegin(DeviceState* state,xess_vk::wsi_acquire::Api api,VkSwapchainKHR chain) noexcept {
    xess_vk::wsi_acquire::Call call;call.api=api;call.swapchain=bits(chain);call.thread=GetCurrentThreadId();
    call.presentThread=static_cast<std::uint32_t>(InterlockedCompareExchange(&state->wsiPresentThread,0,0));
    if(state->wsiInventory && !state->fakeEnabled){
        AcquireSRWLockShared(&stateLock);
        call.generation=state->wsiInventory->beforeGetImages(bits(chain),false,0).chain.generation;
        ReleaseSRWLockShared(&stateLock);
    }
    return call;
}
void wsiAcquireEnd(DeviceState* state,const xess_vk::wsi_acquire::Call& call) noexcept {
    try {
        const auto emit=state->wsiAcquire->record(call);
        if(emit.sample){
            JsonLine log("wsi_acquire_sample");wsiAcquireIdentity(log,state,call);
            log.boolean("arguments_known",call.argumentsKnown);
            if(call.api!=xess_vk::wsi_acquire::Api::Status){
                // Decimal text preserves UINT64_MAX without JsonLine's signed conversion.
                char timeout[32];std::snprintf(timeout,sizeof(timeout),"%llu",static_cast<unsigned long long>(call.timeout));
                log.text("timeout_ns",timeout);log.boolean("semaphore_nonnull",call.semaphore);log.boolean("fence_nonnull",call.fence);
                log.boolean("pnext_present",call.pNext);log.number("device_mask",call.deviceMask);
            }
            log.number("result",call.result);log.number("cpu_elapsed_ns",call.elapsedNs);
            log.number("entry_qpc_us",call.entryNs/1000);log.number("return_qpc_us",(call.entryNs+call.elapsedNs)/1000);
            log.boolean("image_index_known",call.imageIndexKnown);if(call.imageIndexKnown)log.number("image_index",call.imageIndex);
            log.boolean("synchronization_verified",false);log.write();
        }
        if(emit.summary)wsiAcquireSummary(state,"periodic");
    }catch(...){}
}
void fakeAcquireSlow(DeviceState* state, const xess_vk::wsi_acquire::Call& call) noexcept {
    if (!state->fakeEnabled || call.elapsedNs < xess_vk::wsi_acquire::slowCallThresholdNs ||
        !slowCallBudget(state, state->slowAcquireCalls, "acquire")) return;
    JsonLine log("fake_acquire_slow"); wsiAcquireIdentity(log, state, call);
    log.number("entry_qpc_us", call.entryNs / 1000); log.number("return_qpc_us", (call.entryNs + call.elapsedNs) / 1000);
    log.number("total_us", call.elapsedNs / 1000); log.number("result", call.result);
    char timeout[32]; std::snprintf(timeout,sizeof(timeout),"%llu",static_cast<unsigned long long>(call.timeout));
    log.text("timeout_ns",timeout);log.boolean("semaphore_nonnull",call.semaphore);log.boolean("fence_nonnull",call.fence);
    log.cpuWall("no_slot_poll_cpu", call.noSlotPollCpu); log.cpuWall("no_slot_wait_cpu", call.noSlotWaitCpu);
    log.number("signal_entry_qpc_us", call.signal.entryNs / 1000);
    log.number("signal_driver_entry_qpc_us", call.signal.driverEntryNs / 1000);
    log.number("signal_return_qpc_us", call.signal.returnNs / 1000);
    log.number("signal_gate_wait_us", (call.signal.driverEntryNs - call.signal.entryNs) / 1000);
    log.number("signal_driver_call_us", (call.signal.returnNs - call.signal.driverEntryNs) / 1000);
    log.boolean("image_index_known", call.imageIndexKnown); if (call.imageIndexKnown) log.number("image_index", call.imageIndex);
    log.write();
}
void fakeAcquireError(DeviceState* state, const xess_vk::wsi_acquire::Call& call) noexcept {
    if (!state->fakeEnabled || call.result >= 0) return;
    const bool outOfDate=call.result==VK_ERROR_OUT_OF_DATE_KHR;
    const LONG64 limit=outOfDate ? 4 : 8;
    const auto ordinal=InterlockedIncrement64(outOfDate ? &state->acquireOutOfDateErrors : &state->acquireOtherErrors);
    if (ordinal>limit) return;
    JsonLine log("fake_acquire_error");wsiAcquireIdentity(log,state,call);
    log.number("result",call.result);log.number("error_ordinal",ordinal);log.number("error_limit",limit);
    log.text("error_budget",outOfDate ? "out_of_date" : "other_negative");
    log.boolean("arguments_known",call.argumentsKnown);
    char timeout[32];std::snprintf(timeout,sizeof(timeout),"%llu",static_cast<unsigned long long>(call.timeout));
    log.text("timeout_ns",timeout);log.boolean("semaphore_nonnull",call.semaphore);log.boolean("fence_nonnull",call.fence);
    log.boolean("pnext_present",call.pNext);log.number("device_mask",call.deviceMask);
    log.number("entry_qpc_us",call.entryNs/1000);log.number("return_qpc_us",(call.entryNs+call.elapsedNs)/1000);
    log.number("total_us",call.elapsedNs/1000);log.boolean("signal_observed",call.signal.entryNs!=0);
    if(call.signal.entryNs) {
        log.number("signal_result",call.signal.result);
        log.number("signal_entry_qpc_us",call.signal.entryNs/1000);
        log.number("signal_driver_entry_qpc_us",call.signal.driverEntryNs/1000);
        log.number("signal_return_qpc_us",call.signal.returnNs/1000);
    }
    log.write();
}
struct ContinuousDiagnosticErrorGuard {
    DWORD saved{GetLastError()};
    ~ContinuousDiagnosticErrorGuard() { SetLastError(saved); }
};
bool continuousWindowActive(DeviceState* state) noexcept {
    return state && InterlockedCompareExchange(&state->continuousWindowState, 0, 0) == 1;
}
bool renderTailRequested() noexcept {
    if constexpr(!researchDiagnostics)return false;
    static const bool enabled=[] {wchar_t value[8]{};return GetEnvironmentVariableW(L"XESS_RENDER_TAIL_DIAGNOSTIC",value,8)==1&&value[0]==L'1';}();
    return enabled;
}
bool renderTailArmPath(DeviceState* s) noexcept {
    if constexpr(!researchDiagnostics)return false;
    InitOnceExecuteOnce(&logOnce,initializeLog,nullptr,nullptr);if(!actualLogPath[0])return false;
    const auto* slash=std::wcsrchr(actualLogPath,L'\\');if(!slash)slash=std::wcsrchr(actualLogPath,L'/');if(!slash)return false;
    const auto directoryLength=std::size_t(slash-actualLogPath);std::array<wchar_t,4096> directory{};
    if(directoryLength>=directory.size())return false;std::wmemcpy(directory.data(),actualLogPath,directoryLength);
    const auto* session=std::wcsrchr(directory.data(),L'\\');if(!session)session=std::wcsrchr(directory.data(),L'/');if(!session||!*++session)return false;
    for(const auto* p=session;*p;++p)if(!((*p>=L'a'&&*p<=L'z')||(*p>=L'A'&&*p<=L'Z')||(*p>=L'0'&&*p<=L'9')||*p==L'-'||*p==L'_'))return false;
    FILETIME created{},exited{},kernel{},user{};if(!GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user))return false;
    const auto creation=(std::uint64_t(created.dwHighDateTime)<<32)|created.dwLowDateTime;
    const auto n=std::swprintf(s->tailArmPath,std::size(s->tailArmPath),L"%ls\\render-tail-%lu-%016llx.arm",directory.data(),GetCurrentProcessId(),static_cast<unsigned long long>(creation));
    char path[16384]{},sessionText[512]{};
    if(n<=0||std::size_t(n)>=std::size(s->tailArmPath)||!WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,s->tailArmPath,-1,path,sizeof(path),nullptr,nullptr)
       ||!WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,session,-1,sessionText,sizeof(sessionText),nullptr,nullptr))return false;
    JsonLine log("render_tail_waiting_for_arm");log.text("marker_path",path);log.text("expected_literal","END_FIELD_RENDER_TAIL_ONCE_V1");
    log.text("session",sessionText);log.number("pid",GetCurrentProcessId());log.handle("process_creation_filetime",creation);
    log.number("command_limit",xess_vk::render_tail::commandLimit);log.number("tail_event_limit",xess_vk::render_tail::eventLimit);
    log.number("sample_limit",2);log.number("byte_limit",xess_vk::render_tail::byteLimit);log.number("fixed_storage_bytes",xess_vk::render_tail::Store::storageBytes());
    log.boolean("automatic_arm",false);log.boolean("pixels_captured",false);log.write();return true;
}
void renderTailPresent(DeviceState* s) noexcept {
    if constexpr(!researchDiagnostics)return;
    if(!s||!s->renderTail)return;const ContinuousDiagnosticErrorGuard guard;
    if(!s->tailArmLatched) {
        const auto now=GetTickCount64();if(s->tailArmCheck&&now-s->tailArmCheck<500)return;s->tailArmCheck=now;
        const auto file=CreateFileW(s->tailArmPath,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(file==INVALID_HANDLE_VALUE)return;LARGE_INTEGER size{};char contents[128]{};DWORD read{};
        static constexpr char expected[]="END_FIELD_RENDER_TAIL_ONCE_V1";const auto attributes=GetFileAttributesW(s->tailArmPath);
        const bool accepted=attributes!=INVALID_FILE_ATTRIBUTES&&!(attributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))
            &&GetFileSizeEx(file,&size)&&size.QuadPart==sizeof(expected)-1&&ReadFile(file,contents,sizeof(contents),&read,nullptr)
            &&read==sizeof(expected)-1&&std::memcmp(contents,expected,sizeof(expected)-1)==0;
        CloseHandle(file);if(!accepted)return;s->tailArmLatched=true;s->renderTail->arm();
        JsonLine log("render_tail_armed");log.handle("device",bits(s->device));log.number("pid",GetCurrentProcessId());log.write();
    }
    s->renderTail->present();const auto stats=s->renderTail->stats();
    if(stats.stopped&&!s->tailSummaryEmitted) {
        s->tailSummaryEmitted=true;JsonLine log("render_tail_summary");log.handle("device",bits(s->device));
        log.number("successful_wsi_write_samples",stats.emittedSamples);log.number("failed_target_submissions",stats.failedTargets);
        log.number("observed_events",stats.observed);log.number("overwritten_events",stats.overwritten);log.number("refused_events",stats.refused);
        log.number("evicted_commands",stats.evictedCommands);log.number("emitted_bytes",stats.emittedBytes);log.number("omitted_lines",stats.omittedLines);
        log.number("accepted_prepare_contexts",stats.sourceObservations);log.number("present_attempts",stats.presentAttempts);
        log.text("reason",stats.emittedSamples>=2?"two_successful_wsi_write_samples":stats.omittedLines?"byte_limit":"no_more_than_120_present_attempts");
        log.boolean("complete_resource_chain",false);log.boolean("gpu_complete",false);log.boolean("copy_permitted",false);log.write();
    }
}
xess_vk::FrameTimingAssociation renderTailFrame(DeviceState* s) noexcept {
    return s->continuousTiming?s->continuousTiming->recordingAssociation(GetCurrentThreadId()):xess_vk::FrameTimingAssociation{};
}
xess_vk::render_tail::Image renderTailImage(DeviceState* s,std::uint64_t image,unsigned role) noexcept {
    if constexpr(!researchDiagnostics)return {};
    xess_vk::render_tail::Image out;out.role=role;
    if(s->imageInventory){try{const auto snapshot=s->imageInventory->lookup(image);if(snapshot.status==xess_vk::image_inventory::Status::LiveHandleObserved)out.record=snapshot.record;}catch(...){}}
    if(!out.record.image){out.record.image=image;out.issues=1;}
    if(s->resourceMemory&&out.record.generation)out.memory=xess_vk::resource_memory_observer::query(s->resourceMemory,{out.record.image,out.record.generation});
    return out;
}
xess_vk::render_tail::Image renderTailBinding(DeviceState* s,const xess_vk::descriptor_image_metadata::ImageBinding& binding) noexcept {
    if constexpr(!researchDiagnostics)return {};
    xess_vk::render_tail::Image out;out.role=binding.descriptorType==VK_DESCRIPTOR_TYPE_STORAGE_IMAGE?xess_vk::render_tail::Storage:xess_vk::render_tail::Sampled;
    out.record={binding.image.handle,binding.image.generation,binding.imageDescription};out.view=binding.view;out.set=binding.set;
    // A retired descriptor can name an older generation of a reused handle.
    // Its memory observation must use that exact generation too.
    out.memory={};if(s->resourceMemory&&binding.image.generation)out.memory=xess_vk::resource_memory_observer::query(s->resourceMemory,{binding.image.handle,binding.image.generation});
    out.descriptorVersion=binding.descriptorVersion;out.setIndex=binding.setIndex;out.binding=binding.binding;out.arrayElement=binding.arrayElement;
    out.descriptorType=binding.descriptorType;out.range=binding.range;out.layout=binding.declaredLayout;return out;
}
void renderTailPass(DeviceState* s,VkCommandBuffer command,const VkRenderPassBeginInfo* info) noexcept {
    if constexpr(!researchDiagnostics)return;
    if(!s->renderTail||!s->renderTail->active())return;
    const ContinuousDiagnosticErrorGuard guard;
    const auto snapshot=xess_vk::render_pass_observer::diagnosticPassSnapshot(s->renderPass,command,info);
    xess_vk::render_tail::Event event;event.kind=xess_vk::render_tail::Kind::Pass;event.recording=snapshot.recording;event.frame=renderTailFrame(s);
    event.renderPass={snapshot.pass.key.handle,snapshot.pass.key.generation};event.framebuffer={snapshot.framebuffer.key.handle,snapshot.framebuffer.key.generation};
    if(info)event.renderArea=info->renderArea;event.issues=snapshot.command.issues|snapshot.pass.issues|snapshot.framebuffer.issues;
    for(std::size_t n=0;n<snapshot.count&&event.imageCount<event.images.size();++n) {
        const auto& view=snapshot.views[n];auto image=renderTailImage(s,view.image.handle,xess_vk::render_tail::Attachment);
        image.view={view.key.handle,view.key.generation};image.range=view.rawRange;image.issues|=view.issues;
        if(n<snapshot.pass.copiedAttachmentCount)image.layout=snapshot.pass.attachments[n].reference;
        event.images[event.imageCount++]=image;
    }
    s->renderTail->record(event);
}
void renderTailDraw(DeviceState* s,VkCommandBuffer command) noexcept {
    if constexpr(!researchDiagnostics)return;
    if(!s->renderTail||!s->renderTail->active())return;
    const ContinuousDiagnosticErrorGuard guard;
    const auto facts=xess_vk::render_pass_observer::recordingFacts(s->renderPass,command);const auto graphics=s->renderTail->graphics(facts);
    std::array<xess_vk::render_tail::Image,128> images{};std::size_t count=0;
    for(std::size_t setIndex=0;setIndex<graphics.sets.size();++setIndex)if(graphics.sets[setIndex]) {
        const auto snapshot=xess_vk::descriptor_image_observer::diagnosticSetImages(s->descriptors,reinterpret_cast<VkDescriptorSet>(graphics.sets[setIndex]));
        for(std::size_t n=0;n<snapshot.count&&count<images.size();++n) {
            auto image=renderTailBinding(s,snapshot.bindings[n]);image.setIndex=static_cast<std::uint32_t>(setIndex);image.issues|=snapshot.issues;
            images[count++]=image;
        }
    }
    s->renderTail->draw(facts,{images.data(),count});
}
void renderTailDispatch(DeviceState* s,const xess_vk::CommandRecordingFacts& facts,const xess_vk::descriptor_image_metadata::DispatchEvidence& evidence) noexcept {
    if constexpr(!researchDiagnostics)return;
    if(!s->renderTail||!s->renderTail->active())return;
    const ContinuousDiagnosticErrorGuard guard;
    xess_vk::render_tail::Event event;event.kind=xess_vk::render_tail::Kind::Dispatch;event.recording=facts;event.frame=renderTailFrame(s);
    event.pipeline=evidence.pipeline.handle;event.issues=evidence.issues;
    for(std::size_t n=0;n<evidence.count;++n) {
        if(event.imageCount==event.images.size()){++event.imageDrops;continue;}
        event.images[event.imageCount++]=renderTailBinding(s,evidence.bindings[n]);
    }
    s->renderTail->record(event);
}
void renderTailDynamic(DeviceState* s,VkCommandBuffer command,const VkRenderingInfo* info) noexcept {
    if constexpr(!researchDiagnostics)return;
    if(!s->renderTail||!s->renderTail->active()||!info)return;const ContinuousDiagnosticErrorGuard guard;
    xess_vk::render_tail::Event event;event.kind=xess_vk::render_tail::Kind::Pass;
    event.recording=xess_vk::render_pass_observer::recordingFacts(s->renderPass,command);event.frame=renderTailFrame(s);
    event.renderArea=info->renderArea;event.issues=info->pNext?1:0;
    if(info->pColorAttachments)for(std::size_t n=0;n<info->colorAttachmentCount;++n) {
        if(event.imageCount==event.images.size()){++event.imageDrops;continue;}
        const auto& attachment=info->pColorAttachments[n];const auto view=xess_vk::render_pass_observer::diagnosticViewSnapshot(s->renderPass,attachment.imageView);
        auto image=renderTailImage(s,view.image.handle,xess_vk::render_tail::Attachment);image.view={view.key.handle,view.key.generation};
        image.range=view.rawRange;image.layout=attachment.imageLayout;image.issues|=view.issues;event.images[event.imageCount++]=image;
    }
    s->renderTail->record(event);
}
xess_vk::render_tail::Ticket renderTailBeforeSubmit(DeviceState* s,const xess_vk::render_pass_observer::SubmitTicket& ticket) noexcept {
    if constexpr(!researchDiagnostics)return {};
    if(!s->renderTail||!s->renderTail->active())return {};const ContinuousDiagnosticErrorGuard guard;
    std::array<xess_vk::render_tail::Key,xess_vk::render_pass_observer::maxSubmittedCommands> keys{};
    const auto count=std::min<std::size_t>(ticket.commandCount,keys.size());
    for(std::size_t n=0;n<count;++n)keys[n]={ticket.commands[n].handle,ticket.commands[n].generation};
    return s->renderTail->beforeSubmit(ticket.queue,ticket.token,{keys.data(),count});
}
void renderTailAfterSubmit(DeviceState* s,const xess_vk::render_pass_observer::SubmitTicket& ticket,xess_vk::render_tail::Ticket captured,VkResult result) noexcept {
    if constexpr(!researchDiagnostics)return;
    if(!s->renderTail||(!captured.serial&&!s->renderTail->active()))return;const ContinuousDiagnosticErrorGuard guard;
    std::array<xess_vk::render_tail::Key,xess_vk::render_pass_observer::maxSubmittedCommands> keys{};
    const auto count=std::min<std::size_t>(ticket.commandCount,keys.size());
    for(std::size_t n=0;n<count;++n)keys[n]={ticket.commands[n].handle,ticket.commands[n].generation};
    s->renderTail->submitted(ticket.queue,ticket.token,{keys.data(),count},result);s->renderTail->afterSubmit(captured,result);
}
bool continuousWindowLog(DeviceState* state) noexcept {
    if (!continuousWindowActive(state))
        return false;
    if (InterlockedDecrement(&state->continuousWindowBudget) >= 0)
        return true;
    if (InterlockedCompareExchange(&state->continuousWindowState, 2, 1) == 1) {
        JsonLine log("continuous_window_end");
        log.number("sequence", InterlockedIncrement64(&state->continuousDiagnosticSequence));
        log.text("reason", "512_record_budget");
        log.write();
    }
    return false;
}
void continuousDiagnosticFields(JsonLine& log, DeviceState* state, std::uint32_t thread,
                                const xess_vk::continuous_timing::ScopeSnapshot& scope) noexcept {
    ContinuousDiagnosticErrorGuard preserve;
    log.number("sequence", InterlockedIncrement64(&state->continuousDiagnosticSequence));
    log.number("thread_id", thread);
    log.boolean("thread_observed", scope.threadObserved);
    log.boolean("in_simulation", scope.inSimulation);
    log.boolean("in_render", scope.inRender);
    log.boolean("in_native_present", scope.inPresent);
    log.boolean("cpu_frame_active", scope.cpuFrameActive);
    log.number("cpu_frame_key", scope.cpuFrame);
    log.number("simulation_key_diagnostic", scope.simulationFrame);
    log.number("render_key_diagnostic", scope.renderFrame);
    log.number("present_key_diagnostic", scope.presentFrame);
    log.number("context_generation", scope.contextGeneration);
    log.boolean("sdk_anchored", scope.anchored);
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(L"UnityPlayer.dll"));
    std::uint64_t owner{}, simulation{}, queued{};
    SIZE_T copied{};
    const auto read = [&](std::uint64_t address, void* output, std::size_t bytes) {
        return address && address <= UINTPTR_MAX - bytes &&
               ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), output,
                                 bytes, &copied) && copied == bytes;
    };
    const bool valid = base && read(base + 0x1D17E58, &owner, sizeof(owner)) &&
                       owner <= UINTPTR_MAX - 0xD8 && read(owner + 0xC8, &simulation, sizeof(simulation)) &&
                       read(owner + 0xD0, &queued, sizeof(queued));
    log.boolean("global_counter_read_diagnostic", valid);
    log.number("global_c8_diagnostic", static_cast<std::uint32_t>(simulation));
    log.number("global_d0_diagnostic", static_cast<std::uint32_t>(queued));
    log.boolean("global_counters_used_for_association", false);
}
void continuousMissingStack(JsonLine& log) noexcept {
    ContinuousDiagnosticErrorGuard preserve;
    void* frames[32]{};
    const auto count = CaptureStackBackTrace(0, 32, frames, nullptr);
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(L"UnityPlayer.dll"));
    log.key("unity_stack_rvas");
    log.raw("[");
    std::uint32_t emitted{};
    for (USHORT index = 0; base && index < count && emitted < 12; ++index) {
        const auto address = reinterpret_cast<std::uintptr_t>(frames[index]);
        if (address < base + 0x1000 || address >= base + 0x185B000)
            continue;
        if (emitted++)
            log.raw(",");
        char rva[24];
        std::snprintf(rva, sizeof(rva), "\"0x%llx\"", static_cast<unsigned long long>(address - base));
        log.raw(rva);
    }
    log.raw("]");
    log.number("unity_stack_count", emitted);
}
namespace raw=xess_vk::rawcolor_capture_probe;
namespace borrowed=xess_vk::borrowed_capture_completion;
struct RawOperation {
    DeviceState* state{};bool acquired{};
    explicit RawOperation(DeviceState* s,bool reportMiss=false,bool tryOnly=false) noexcept:state(s){
        if constexpr(!researchDiagnostics)return;
        if(!s||InterlockedCompareExchange(&s->rawQuarantined,0,0))return;
        // Admitted private recipes use unhooked PFNs and exclude application
        // callbacks. Detect unexpected same-thread reentry anyway: lifecycle
        // evidence cannot be silently dropped, and SRW locks are not recursive.
        if(InterlockedCompareExchange(&s->rawOperation,0,0)==static_cast<LONG>(GetCurrentThreadId())){
            if(reportMiss){InterlockedOr(&s->rawObservationMissed,1);if(s->rawProbe)raw::observationUnsafe(s->rawProbe);}return;
        }
        if(tryOnly)acquired=TryAcquireSRWLockExclusive(&s->rawLock)!=0;
        else{AcquireSRWLockExclusive(&s->rawLock);acquired=true;}
        if(acquired){InterlockedExchange(&s->rawOperation,static_cast<LONG>(GetCurrentThreadId()));
            if(s->rawProbe&&InterlockedCompareExchange(&s->rawObservationMissed,0,0))raw::observationUnsafe(s->rawProbe);}
    }
    ~RawOperation(){if constexpr(researchDiagnostics)if(acquired){
        if(state->rawProbe&&InterlockedCompareExchange(&state->rawObservationMissed,0,0))raw::observationUnsafe(state->rawProbe);
        InterlockedExchange(&state->rawOperation,0);ReleaseSRWLockExclusive(&state->rawLock);
    }}
    RawOperation(const RawOperation&)=delete;RawOperation& operator=(const RawOperation&)=delete;
};
void rawHealth(DeviceState* s,std::uint32_t flag,const char* phase) noexcept {
    if constexpr(!researchDiagnostics)return;
    if(s->rawHealthEmitted&flag)return;s->rawHealthEmitted|=flag;
    JsonLine log("rawcolor_probe_health");log.handle("device",bits(s->device));log.text("phase",phase);
    log.number("arm_checks",s->rawArmChecks);log.number("busy_skips",InterlockedCompareExchange64(&s->rawBusySkips,0,0));
    const auto unsafe=InterlockedCompareExchange(&s->rawObservationMissed,0,0);log.number("unsafe_observation",unsafe);
    if(unsafe&1)log.text("unsafe_reason","same_thread_lifecycle_reentry");else if(unsafe&2)log.text("unsafe_reason","queue_identity_changed");
    if(s->rawProbe){const auto status=raw::status(s->rawProbe);log.number("source_query_attempts",status.sourceQueryAttempts);log.number("reasons",status.reasons);log.number("completion_reasons",status.completion.reasons);}
    log.write();
}
bool claimRawLease(void*) noexcept {return InterlockedCompareExchange(&rawProcessLease,1,0)==0;}
bool rawDeviceQueueShape(const VkDeviceCreateInfo* info) noexcept {
    if(!info||info->sType!=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO||info->flags||!info->pQueueCreateInfos||info->queueCreateInfoCount!=1)return false;
    const auto& q=info->pQueueCreateInfos[0];return q.sType==VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO&&!q.pNext&&!q.flags&&q.queueFamilyIndex==0&&q.queueCount==1;
}
bool rawMemoryCallbackAbsent(const VkDeviceCreateInfo* info) noexcept {
    auto* p=info?static_cast<const VkBaseInStructure*>(info->pNext):nullptr;
    for(unsigned n=0;p&&n<64;++n,p=p->pNext)if(p->sType==VK_STRUCTURE_TYPE_DEVICE_DEVICE_MEMORY_REPORT_CREATE_INFO_EXT)return false;
    return !p;
}
bool rawInstanceCallbacksAbsent(const VkInstanceCreateInfo* info) noexcept {
    if(!info||info->sType!=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO||info->flags||(info->enabledExtensionCount&&!info->ppEnabledExtensionNames)||info->enabledExtensionCount>256)return false;
    for(std::uint32_t n=0;n<info->enabledExtensionCount;++n){const auto* name=info->ppEnabledExtensionNames[n];if(!name||std::strcmp(name,VK_EXT_DEBUG_UTILS_EXTENSION_NAME)==0||std::strcmp(name,VK_EXT_DEBUG_REPORT_EXTENSION_NAME)==0)return false;}
    auto* p=static_cast<const VkBaseInStructure*>(info->pNext);for(unsigned n=0;p&&n<64;++n,p=p->pNext)if(p->sType!=VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO)return false;return !p;
}
bool rawArmPaths(DeviceState* s) noexcept {
    if constexpr(!researchDiagnostics)return false;
    InitOnceExecuteOnce(&logOnce,initializeLog,nullptr,nullptr);
    if(!actualLogPath[0])return false;
    const wchar_t* slash=std::wcsrchr(actualLogPath,L'\\');if(!slash)slash=std::wcsrchr(actualLogPath,L'/');
    if(!slash||slash==actualLogPath)return false;const auto dirLength=std::size_t(slash-actualLogPath);
    std::array<wchar_t,4096> directory{};if(dirLength>=directory.size())return false;std::wmemcpy(directory.data(),actualLogPath,dirLength);
    const auto* session=std::wcsrchr(directory.data(),L'\\');if(!session)session=std::wcsrchr(directory.data(),L'/');if(!session||!*++session)return false;
    const auto sessionLength=std::wcslen(session);if(sessionLength>96)return false;
    for(const auto* p=session;*p;++p)if(!((*p>=L'a'&&*p<=L'z')||(*p>=L'A'&&*p<=L'Z')||(*p>=L'0'&&*p<=L'9')||*p==L'-'||*p==L'_'))return false;
    FILETIME created{},exited{},kernel{},user{};if(!GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user))return false;
    s->processCreationTime=(std::uint64_t(created.dwHighDateTime)<<32)|created.dwLowDateTime;
    // Resolve the current process token's KnownFolder, rather than trusting a
    // potentially different elevated launch account's environment variable.
    const auto advapi=LoadLibraryW(L"advapi32.dll"),shell=LoadLibraryW(L"shell32.dll"),ole=LoadLibraryW(L"ole32.dll");
    const auto open=advapi?reinterpret_cast<decltype(&OpenProcessToken)>(GetProcAddress(advapi,"OpenProcessToken")):nullptr;
    const auto tokenInfo=advapi?reinterpret_cast<decltype(&GetTokenInformation)>(GetProcAddress(advapi,"GetTokenInformation")):nullptr;
    const auto sidString=advapi?reinterpret_cast<decltype(&ConvertSidToStringSidW)>(GetProcAddress(advapi,"ConvertSidToStringSidW")):nullptr;
    const auto folder=shell?reinterpret_cast<decltype(&SHGetKnownFolderPath)>(GetProcAddress(shell,"SHGetKnownFolderPath")):nullptr;
    const auto freeTask=ole?reinterpret_cast<decltype(&CoTaskMemFree)>(GetProcAddress(ole,"CoTaskMemFree")):nullptr;
    HANDLE token{};std::array<unsigned char,1024> tokenBytes{};DWORD needed{};wchar_t* sid{};wchar_t* local{};bool ready=false;
    if(open&&tokenInfo&&sidString&&folder&&freeTask&&open(GetCurrentProcess(),TOKEN_QUERY,&token)
        &&tokenInfo(token,TokenUser,tokenBytes.data(),DWORD(tokenBytes.size()),&needed)
        &&sidString(reinterpret_cast<const TOKEN_USER*>(tokenBytes.data())->User.Sid,&sid)
        &&SUCCEEDED(folder(GUID{0xf1b32785,0x6fba,0x4fcf,{0x9d,0x55,0x7b,0x8e,0x7f,0x15,0x70,0x91}},0,token,&local))&&local){
        const auto n=std::swprintf(s->rawArmPath,std::size(s->rawArmPath),L"%ls\\EndfieldXeSSObserver\\CaptureRequests\\%ls\\rawcolor-%lu-%016llx.arm",local,session,GetCurrentProcessId(),static_cast<unsigned long long>(s->processCreationTime));
        const auto bmp=std::swprintf(s->rawBmpPath,std::size(s->rawBmpPath),L"%ls\\rawcolor-%lu-%016llx.bmp",directory.data(),GetCurrentProcessId(),static_cast<unsigned long long>(s->processCreationTime));
        if(n>0&&std::size_t(n)<std::size(s->rawArmPath)&&bmp>0&&std::size_t(bmp)<std::size(s->rawBmpPath)){
            char marker[16384]{},sidText[512]{},sessionText[128]{},output[16384]{};
            ready=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,s->rawArmPath,-1,marker,sizeof(marker),nullptr,nullptr)>0
                &&WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,sid,-1,sidText,sizeof(sidText),nullptr,nullptr)>0
                &&WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,session,-1,sessionText,sizeof(sessionText),nullptr,nullptr)>0
                &&WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,s->rawBmpPath,-1,output,sizeof(output),nullptr,nullptr)>0;
            if(ready){JsonLine log("rawcolor_probe_waiting_for_arm");log.text("marker_path",marker);log.text("expected_literal","END_FIELD_RAWCOLOR_ONCE_V1");log.text("user_sid",sidText);log.text("session",sessionText);log.text("output_bmp",output);log.handle("process_creation_filetime",s->processCreationTime);log.write();}
        }
    }
    if(local&&freeTask)freeTask(local);if(sid)LocalFree(sid);if(token)CloseHandle(token);
    if(advapi)FreeLibrary(advapi);if(shell)FreeLibrary(shell);if(ole)FreeLibrary(ole);return ready;
}
bool rawCheckArm(DeviceState* s) noexcept {
    if constexpr(!researchDiagnostics)return false;
    if(s->rawArmed)return true;if(!s->rawArmPath[0])return false;
    const auto now=GetTickCount64();if(s->rawArmLastCheck&&now-s->rawArmLastCheck<500)return false;s->rawArmLastCheck=now;++s->rawArmChecks;
    const auto file=CreateFileW(s->rawArmPath,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)return false;LARGE_INTEGER size{};char token[128]{};DWORD read{};
    static constexpr char expected[]="END_FIELD_RAWCOLOR_ONCE_V1";
    const auto attributes=GetFileAttributesW(s->rawArmPath);
    const bool accepted=attributes!=INVALID_FILE_ATTRIBUTES&&!(attributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))&&GetFileSizeEx(file,&size)
        &&size.QuadPart==sizeof(expected)-1&&ReadFile(file,token,sizeof(token),&read,nullptr)&&read==sizeof(expected)-1&&std::memcmp(token,expected,sizeof(expected)-1)==0;
    CloseHandle(file);if(accepted){s->rawArmed=true;JsonLine log("rawcolor_probe_armed");log.handle("device",bits(s->device));log.handle("process_creation_filetime",s->processCreationTime);log.write();}return accepted;
}
void rawQueueObserved(DeviceState* s,VkQueue queue,std::uint32_t family,std::uint32_t index,VkDeviceQueueCreateFlags flags,bool ordinary) noexcept {
    if constexpr(!researchDiagnostics)return;
    const auto error=GetLastError();RawOperation op(s,true);if(!op.acquired){SetLastError(error);return;}
    if(!s->fakeEnabled&&s->rawEnabled&&s->rawUniqueQueue&&ordinary&&!flags&&!family&&!index&&queue){
        if(s->rawQueue&&s->rawQueue!=queue){s->rawUniqueQueue=false;InterlockedOr(&s->rawObservationMissed,2);rawHealth(s,64,"unsafe_terminal");}
        else if(!s->rawProbe){s->rawQueue=queue;raw::Config c;c.device=s->device;c.queue=queue;c.queueFamily=0;c.queueFlags=s->rawQueueFlags;c.memory=s->physicalMemory;c.nonCoherentAtomSize=s->nonCoherentAtomSize;
            c.singlePhysicalDeviceKnown=s->actualDeviceScope==xess_vk::resource_memory_facts::DeviceScope::SinglePhysicalDevice;
            c.uniqueOrdinaryExternallySynchronizedQueueKnown=s->rawUniqueQueue;c.defaultDeviceAndInstanceAllocatorsKnown=s->rawDefaultAllocators;c.applicationDeviceMemoryCallbackAbsentKnown=s->rawCallbackAbsent;
            c.applicationInstanceCallbacksAbsentKnown=s->parent&&s->parent->applicationCallbacksAbsentKnown;
            c.targetFormatTransferSupported=s->rawFormatSupported;c.targetFormatProperties=s->rawFormatProperties;c.claimProcessLease=claimRawLease;c.enabled=true;c.sink=streamlineLog;
            std::wmemcpy(c.outputBmpPath.data(),s->rawBmpPath,std::wcslen(s->rawBmpPath)+1);
            c.functions={s->createImage,s->destroyImage,s->imageMemoryRequirements,s->createBuffer,s->destroyBuffer,s->bufferMemoryRequirements,s->allocateMemory,s->freeMemory,s->bindImageMemory,s->bindBufferMemory,
                s->createCommandPool,s->destroyCommandPool,s->allocateCommandBuffers,s->beginCommandBuffer,s->endCommandBuffer,s->createFence,s->destroyFence,s->fenceStatus,s->waitFences,
                s->cmdPipelineBarrier,s->cmdCopyImage,s->cmdCopyImageToBuffer,s->submit,s->mapMemory,s->unmapMemory,s->invalidateMemory};
            s->rawProbe=raw::create(c);
            rawHealth(s,1,s->rawProbe?"context_ready":"context_creation_failed");
        }
    }
    SetLastError(error);
}
borrowed::RecordingKey rawKey(DeviceState* s,VkCommandBuffer command) noexcept {
    const auto f=xess_vk::render_pass_observer::recordingFacts(s->renderPass,command);return {s->device,f.contextIncarnation,command,f.rpRecordingGeneration};
}
borrowed::QueueWindow rawWindow(DeviceState* s,VkQueue queue) noexcept {return {queue,queue==s->rawQueue?0u:VK_QUEUE_FAMILY_IGNORED,s->rawUniqueQueue&&queue==s->rawQueue,s->rawUniqueQueue&&queue==s->rawQueue};}
void rawRetireRecording(DeviceState* s,const borrowed::RecordingKey& key, bool cancelContinuous = true) noexcept {
    if (s->continuous && cancelContinuous) s->continuous->retireUnsubmitted({bits(key.command), key.rpRecordingGeneration});
    if constexpr(!researchDiagnostics)return;
    RawOperation op(s,true);if(op.acquired&&s->rawProbe)raw::recordingRetired(s->rawProbe,key);
}
void rawRetirePool(DeviceState* s,VkCommandPool pool, bool cancelContinuous = true) noexcept {
    if (s->continuous && cancelContinuous) s->continuous->retirePool(bits(pool));
    if constexpr(!researchDiagnostics)return;
    RawOperation op(s,true);if(!op.acquired||!s->rawProbe)return;const auto current=raw::status(s->rawProbe).completion.borrowed;
    if(current.key.command){const auto facts=xess_vk::render_pass_observer::recordingFacts(s->renderPass,current.key.command);if(facts.pool==bits(pool))raw::recordingRetired(s->rawProbe,current.key);}
}
void rawQuarantine(DeviceState* s,const char* boundary,VkResult driver) noexcept {
    if constexpr(!researchDiagnostics)return;
    InterlockedExchange(&s->rawQuarantined,1);if(s->parent)InterlockedExchange(&s->parent->rawQuarantined,1);
    HMODULE pinned{};GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN|GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,reinterpret_cast<LPCWSTR>(&rawQuarantine),&pinned);
    if(s->destroy)GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN|GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,reinterpret_cast<LPCWSTR>(s->destroy),&pinned);
    if(s->gdpa)GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN|GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,reinterpret_cast<LPCWSTR>(s->gdpa),&pinned);
    if(s->parent&&s->parent->destroy)GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN|GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,reinterpret_cast<LPCWSTR>(s->parent->destroy),&pinned);
    bool originalsPinned=true;
#define RAW_PIN_FUNCTION(field) if(s->field&&!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN|GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,reinterpret_cast<LPCWSTR>(s->field),&pinned))originalsPinned=false
    RAW_PIN_FUNCTION(createImage);RAW_PIN_FUNCTION(destroyImage);RAW_PIN_FUNCTION(imageMemoryRequirements);RAW_PIN_FUNCTION(createBuffer);RAW_PIN_FUNCTION(destroyBuffer);RAW_PIN_FUNCTION(bufferMemoryRequirements);
    RAW_PIN_FUNCTION(allocateMemory);RAW_PIN_FUNCTION(freeMemory);RAW_PIN_FUNCTION(bindImageMemory);RAW_PIN_FUNCTION(bindBufferMemory);RAW_PIN_FUNCTION(createCommandPool);RAW_PIN_FUNCTION(destroyCommandPool);
    RAW_PIN_FUNCTION(allocateCommandBuffers);RAW_PIN_FUNCTION(beginCommandBuffer);RAW_PIN_FUNCTION(endCommandBuffer);RAW_PIN_FUNCTION(createFence);RAW_PIN_FUNCTION(destroyFence);RAW_PIN_FUNCTION(fenceStatus);RAW_PIN_FUNCTION(waitFences);
    RAW_PIN_FUNCTION(cmdPipelineBarrier);RAW_PIN_FUNCTION(cmdCopyImage);RAW_PIN_FUNCTION(cmdCopyImageToBuffer);RAW_PIN_FUNCTION(submit);RAW_PIN_FUNCTION(mapMemory);RAW_PIN_FUNCTION(unmapMemory);RAW_PIN_FUNCTION(invalidateMemory);
#undef RAW_PIN_FUNCTION
    if(GetModuleHandleW(L"vulkan-1.dll")&&!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN,L"vulkan-1.dll",&pinned))originalsPinned=false;
    JsonLine log("rawcolor_probe_destroy_deferred");log.text("boundary",boundary);log.handle("device",bits(s->device));log.number("driver",driver);log.boolean("original_forwarded",false);log.boolean("cleanup",false);log.boolean("private_original_modules_pinned",originalsPinned);log.boolean("quarantined_until_process_exit",true);log.write();
}
struct RawMembership {
    std::array<borrowed::RecordingKey,borrowed::maxMembership> keys{};std::size_t count{};bool known{true};
    void add(DeviceState* s,VkCommandBuffer command) noexcept {if(count>=keys.size()||!command){known=false;return;}const auto key=rawKey(s,command);keys[count++]=key;if(!key.contextIncarnation||!key.rpRecordingGeneration)known=false;}
};
RawMembership rawMembership(DeviceState* s,std::uint32_t count,const VkSubmitInfo* infos) noexcept {
    RawMembership out;if((count&&!infos)||count>borrowed::maxMembership){out.known=false;return out;}
    for(std::uint32_t n=0;n<count;++n){const auto& i=infos[n];
        if(i.sType!=VK_STRUCTURE_TYPE_SUBMIT_INFO||(i.commandBufferCount&&!i.pCommandBuffers)||(i.waitSemaphoreCount&&(!i.pWaitSemaphores||!i.pWaitDstStageMask))||(i.signalSemaphoreCount&&!i.pSignalSemaphores)){out.known=false;continue;}
        if(i.pNext){const auto* type=static_cast<const VkBaseInStructure*>(i.pNext);
            if(type->sType!=VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO||type->pNext)out.known=false;
            else{const auto& timeline=*static_cast<const VkTimelineSemaphoreSubmitInfo*>(i.pNext);
                if((timeline.waitSemaphoreValueCount&&timeline.waitSemaphoreValueCount!=i.waitSemaphoreCount)||(timeline.signalSemaphoreValueCount&&timeline.signalSemaphoreValueCount!=i.signalSemaphoreCount)
                    ||(timeline.waitSemaphoreValueCount&&!timeline.pWaitSemaphoreValues)||(timeline.signalSemaphoreValueCount&&!timeline.pSignalSemaphoreValues))out.known=false;}
        }
        if(i.commandBufferCount>out.keys.size()-out.count){out.known=false;continue;}
        for(std::uint32_t j=0;j<i.commandBufferCount;++j)out.add(s,i.pCommandBuffers[j]);
    }
    return out;
}
RawMembership rawMembership(DeviceState* s,std::uint32_t count,const VkSubmitInfo2* infos) noexcept {
    RawMembership out;if((count&&!infos)||count>borrowed::maxMembership){out.known=false;return out;}
    for(std::uint32_t n=0;n<count;++n){const auto& i=infos[n];
        if(i.sType!=VK_STRUCTURE_TYPE_SUBMIT_INFO_2||i.pNext||i.flags||(i.commandBufferInfoCount&&!i.pCommandBufferInfos)||(i.waitSemaphoreInfoCount&&!i.pWaitSemaphoreInfos)||(i.signalSemaphoreInfoCount&&!i.pSignalSemaphoreInfos)){out.known=false;continue;}
        const auto semaphoresKnown=[&](std::uint32_t size,const VkSemaphoreSubmitInfo* semaphores){if(size>borrowed::maxMembership)return false;for(std::uint32_t j=0;j<size;++j)if(semaphores[j].sType!=VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO||semaphores[j].pNext||semaphores[j].deviceIndex)return false;return true;};
        if(!semaphoresKnown(i.waitSemaphoreInfoCount,i.pWaitSemaphoreInfos)||!semaphoresKnown(i.signalSemaphoreInfoCount,i.pSignalSemaphoreInfos))out.known=false;
        if(i.commandBufferInfoCount>out.keys.size()-out.count){out.known=false;continue;}
        for(std::uint32_t j=0;j<i.commandBufferInfoCount;++j){const auto& command=i.pCommandBufferInfos[j];if(command.sType!=VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO||command.pNext||command.deviceMask>1)out.known=false;out.add(s,command.commandBuffer);}
    }return out;
}
template<class Info> borrowed::SubmitTicket rawBeforeSubmit(DeviceState* s,VkQueue queue,std::uint32_t count,const Info* infos,VkFence fence) noexcept {
    if constexpr(!researchDiagnostics)return {};
    if(!s->rawProbe)return {};const auto members=rawMembership(s,count,infos);borrowed::SubmitObservation observation;
    observation.window=rawWindow(s,queue);observation.applicationFence=fence;observation.completeMembership={members.keys.data(),members.count};
    observation.structureKnown=members.known&&!InterlockedCompareExchange(&s->rawObservationMissed,0,0);return raw::preSubmit(s->rawProbe,observation);
}
xess_vk::descriptor_image_metadata::DispatchEvidence rawPreDispatch(DeviceState* s,VkCommandBuffer command,std::uint32_t x,std::uint32_t y,std::uint32_t z,xess_vk::descriptor_image_observer::DispatchKind kind,const xess_vk::CommandRecordingFacts& f) noexcept {
    const auto error=GetLastError();
    bool live=false;
#if XESS_RESEARCH_DIAGNOSTICS
    RawOperation operation(s,false,true);
    if(!operation.acquired){const auto skips=InterlockedIncrement64(&s->rawBusySkips);if(skips==1){JsonLine log("rawcolor_probe_health");log.handle("device",bits(s->device));log.text("phase",InterlockedCompareExchange(&s->rawQuarantined,0,0)?"quarantined":"candidate_busy_skip");log.number("busy_skips",skips);log.write();}}
    else if(!s->rawProbe)rawHealth(s,2,s->rawEnabled?"waiting_for_queue_context":"disabled");
    else{
        // Check an exact arm request even when a real terminal state prevents
        // capture. An unrelated lock contender can never suppress this latch.
        const bool armed=rawCheckArm(s);if(s->rawArmChecks)rawHealth(s,4,armed?"arm_checked_and_latched":"arm_checked_waiting");
        const auto status=raw::status(s->rawProbe);const bool attempt=raw::canAttempt(s->rawProbe);
        if(!attempt)rawHealth(s,status.sourceQueryAttempts>=64?16:32,status.sourceQueryAttempts>=64?"query_budget_exhausted":status.sourceInserted?"source_already_inserted":"probe_terminal");
        if(InterlockedCompareExchange(&s->rawObservationMissed,0,0))rawHealth(s,64,"unsafe_terminal");
        live=armed&&attempt;
    }
#endif
    xess_vk::descriptor_image_observer::DispatchSourceFacts owned;
    xess_vk::descriptor_image_observer::SourceQueryDemand demand;demand.everyMatchedBinding=false;demand.rawPatternProbeLive=live;
    if constexpr(researchDiagnostics){demand.requestProbeQuery=raw::requestSourceQuery;demand.queryUser=live?s->rawProbe:nullptr;}
    demand.diagnosticAllBindings=s->srObservationRequested&&(!s->srRuntimeRequested||xess_vk::diagnostic_flags::srNativeReplacementAvailable())&&xess_vk::sr_observation::active(bits(s->device));
    const auto evidence = xess_vk::descriptor_image_observer::preDispatch(s->descriptors,command,x,y,z,kind,&f,xess_vk::render_pass_observer::sourceLayoutQuery(s->renderPass),xess_vk::resource_memory_observer::sourceQuery(s->resourceMemory),live?&owned:nullptr,demand);
    if constexpr(researchDiagnostics)if(live&&owned.observed){bool rawQueried=false;for(std::size_t n=0;n<owned.evidence.count;++n)if(owned.evidence.bindings[n].setIndex==0&&owned.evidence.bindings[n].binding==6&&owned.layouts[n].queried)rawQueried=true;
        if(rawQueried)raw::preDispatch(s->rawProbe,owned);}
    SetLastError(error);
    return evidence;
}
xess_vk::descriptor_image_metadata::Config descriptorDispatchConfig(bool productRoute,bool renderTail,bool compactRequested=false) noexcept {
    xess_vk::descriptor_image_metadata::Config config;config.continuousDispatchSelection=productRoute&&!renderTail;config.srDispatchSelection=productRoute&&xess_vk::diagnostic_flags::srReplaceRequested();
    config.compactOrdinaryDispatch=productRoute&&!renderTail&&compactRequested;return config;
}
xess_vk::sr_observation::DispatchTicket srObservationDispatch(DeviceState* state,
        const xess_vk::CommandRecordingFacts& recording,const xess_vk::descriptor_image_metadata::DispatchEvidence& evidence) noexcept {
    if(evidence.membershipOnly)return {}; // Ordinary consumers have no SR/FG input proof to match.
    return xess_vk::sr_observation::beforeDispatch(bits(state->device),recording,evidence,renderTailFrame(state),GetCurrentThreadId(),
        xess_vk::render_pass_observer::sourceLayoutQuery(state->renderPass),xess_vk::resource_memory_observer::sourceQuery(state->resourceMemory),
        xess_vk::render_pass_observer::imageIntervalQuery(state->renderPass));
}
#include "sr_game_runtime.inc"
template<class Sink> void srMeasurementFields(Sink& line,GameSrRuntime* sr) noexcept {if(sr)sr->measurementFields(line);}
void srIdentityCacheFields(JsonLine& line,GameSrRuntime* sr) noexcept {
    if(!sr)return;std::lock_guard lock(sr->mutex);
    line.number("identity_token_cache_schema",1);line.number("identity_token_cache_hits",sr->replacementIdentityCacheHits);
    line.number("identity_token_cache_misses",sr->replacementIdentityCacheMisses);
}
#ifdef XESS_METADATA_CONSUMER_TEST
bool (*srReplacementInterceptForTest)(const xess_vk::CommandRecordingFacts&,bool){};
void (*srContinuousRecordForTest)(const xess_vk::CommandRecordingFacts&){};
#endif
bool srInterceptDispatch(DeviceState* state,const xess_vk::CommandRecordingFacts& f,
    const xess_vk::descriptor_image_metadata::DispatchEvidence& e,const xess_vk::sr_observation::DispatchTicket& t,bool direct) noexcept {
#ifdef XESS_METADATA_CONSUMER_TEST
    if(srReplacementInterceptForTest)return srReplacementInterceptForTest(f,direct);
#endif
    return state->srRuntime&&state->srRuntime->intercept(f,e,t,direct);
}


// This immutable provider reads the existing common inventory only. RP calls it
// outside its mutex; descriptor preDispatch has already released its own mutex.
xess_vk::image_inventory::Snapshot recordedLayoutInventory(void* user,std::uint64_t image) noexcept {
    auto* state=static_cast<DeviceState*>(user);if(!state||!state->imageInventory)return {};
    try{return state->imageInventory->lookup(image);}catch(...){return {};}
}

// Caller holds stateLock exclusively; the WSI inventory has already updated the
// common image inventory. Metadata registration follows that exact serial order.
void applyWsiChanges(DeviceState* state, const xess_vk::wsi_image_inventory::Changes& changes) noexcept {
    const auto inventoryStatus = changes.inventoryStatus;
    if (inventoryStatus != xess_vk::image_inventory::Status::NotObserved &&
        inventoryStatus != xess_vk::image_inventory::Status::LiveHandleObserved) {
        if constexpr(researchDiagnostics) xess_vk::candidate_sync::retire(state->candidateSync);
        xess_vk::render_pass_observer::retire(state->renderPass);
        xess_vk::descriptor_image_observer::retire(state->descriptors);
        return;
    }
    for (std::size_t i = 0; i < changes.removedCount; ++i) {
        const auto& r = changes.removed[i].record;
        if constexpr(researchDiagnostics) if(state->renderTail)state->renderTail->wsiRemoved({r.image,r.generation});
        xess_vk::render_pass_observer::swapchainImageRetiring(state->renderPass, {r.image, r.generation});
        xess_vk::descriptor_image_observer::imageRetiring(state->descriptors, {r.image, r.generation});
        try {
            const auto current = state->imageInventory->lookup(r.image);
            // Never retire a newer regular/WSI image that reused this handle.
            if (current.status == xess_vk::image_inventory::Status::NotObserved) {
                if constexpr(researchDiagnostics) xess_vk::candidate_sync::destroyingImage(state->candidateSync, r.image);
            }
            else if (current.status != xess_vk::image_inventory::Status::LiveHandleObserved) {
                if constexpr(researchDiagnostics) xess_vk::candidate_sync::retire(state->candidateSync);
                xess_vk::render_pass_observer::retire(state->renderPass);
                xess_vk::descriptor_image_observer::retire(state->descriptors);
            }
        } catch (...) {
            if constexpr(researchDiagnostics) xess_vk::candidate_sync::retire(state->candidateSync);
            xess_vk::render_pass_observer::retire(state->renderPass);
            xess_vk::descriptor_image_observer::retire(state->descriptors);
        }
    }
    for (std::size_t i = 0; i < changes.addedCount; ++i) {
        const auto& r = changes.added[i].record;
        if constexpr(researchDiagnostics) if(state->renderTail)state->renderTail->wsiAdded(changes.added[i]);
        const auto& d = r.description;
        xess_vk::render_pass_metadata::ImageDescription description;
        description.key = {r.image, r.generation};
        description.type = VK_IMAGE_TYPE_2D; description.format = static_cast<VkFormat>(d.format);
        description.extent = {d.width, d.height, d.depth};
        description.mipLevels = d.mipLevels; description.arrayLayers = d.arrayLayers;
        description.samples = static_cast<VkSampleCountFlagBits>(d.samples);
        description.usage = d.usage; description.flags = d.flags;
        description.tiling = static_cast<VkImageTiling>(d.tiling);
        description.sharing = static_cast<VkSharingMode>(d.sharingMode);
        description.unsupportedPNext = changes.description.pNextPresent || !changes.description.copied;
        xess_vk::render_pass_observer::swapchainImageObserved(state->renderPass, description);
        xess_vk::descriptor_image_observer::imageObserved(state->descriptors, r);
    }
}

void reportWsiChanges(DeviceState* state, const char* boundary,
    const xess_vk::wsi_image_inventory::Changes& changes) noexcept {
    const auto count = InterlockedIncrement64(&state->wsiMetadataEvents);
    if (count > 128 && count % 300 != 0) return;
    JsonLine log("wsi_image_metadata"); log.handle("device", bits(state->device));
    log.text("boundary", boundary); log.number("observation", count);
    log.text("status", xess_vk::wsi_image_inventory::name(changes.status));
    log.text("inventory_status", xess_vk::image_inventory::name(changes.inventoryStatus));
    log.handle("swapchain", changes.chain.chain); log.number("swapchain_generation", changes.chain.generation);
    log.number("added", changes.addedCount); log.number("removed", changes.removedCount);
    log.number("ownership_mismatches", changes.ownershipMismatches);
    log.boolean("old_chain_retired", changes.oldSwapchainRetired);
    log.key("images"); log.raw("[");
    for (std::size_t i = 0; i < changes.addedCount; ++i) {
        if (i) log.raw(","); log.raw("{");
        // JsonLine fields start with a comma, so seed this small nested object.
        log.raw("\"origin\":\"swapchain_enumeration\"");
        const auto& image = changes.added[i];
        log.handle("image", image.record.image); log.number("generation", image.record.generation);
        log.number("index", image.index); log.number("format", image.record.description.format);
        log.number("width", image.record.description.width); log.number("height", image.record.description.height);
        log.raw("}");
    }
    log.raw("]"); log.boolean("acquired_verified", false); log.boolean("gpu_complete", false);
    log.boolean("resource_lifetime_pinned", false); log.boolean("copy_permitted", false); log.write();
}

namespace packet = xess_vk::presentation_packet;
bool packetRead(const void* source, void* output, std::size_t bytes) noexcept {
    SIZE_T copied{};
    return source && ReadProcessMemory(GetCurrentProcess(), source, output, bytes, &copied) && copied == bytes;
}
struct PacketSnapshot {
    VkPresentInfoKHR info{};
    VkSwapchainKHR chain{};
    std::uint32_t image{};
    std::array<std::uint64_t, packet::maxWaits> waits{};
    bool valid{};
};
PacketSnapshot snapshotPacket(const VkPresentInfoKHR* source, bool allowZeroWaits = false) noexcept {
    PacketSnapshot out;
    if (!packetRead(source, &out.info, sizeof(out.info)) ||
        out.info.sType != VK_STRUCTURE_TYPE_PRESENT_INFO_KHR || out.info.pNext ||
        out.info.swapchainCount != 1 || (!allowZeroWaits && !out.info.waitSemaphoreCount) || out.info.waitSemaphoreCount > out.waits.size() ||
        !packetRead(out.info.pSwapchains, &out.chain, sizeof(out.chain)) ||
        !packetRead(out.info.pImageIndices, &out.image, sizeof(out.image))) return out;
    std::array<VkSemaphore, packet::maxWaits> waits{};
    if (out.info.waitSemaphoreCount &&
        !packetRead(out.info.pWaitSemaphores, waits.data(), out.info.waitSemaphoreCount * sizeof(VkSemaphore))) return out;
    for (std::uint32_t i = 0; i < out.info.waitSemaphoreCount; ++i)
        out.waits[i] = bits(waits[i]);
    out.valid = true;
    return out;
}
packet::Key packetKey(DeviceState* state, VkQueue queue, const PacketSnapshot& snapshot) {
    if (!state->packetIdentities || !snapshot.valid) return {};
    return state->packetIdentities->makeKey(bits(queue), bits(snapshot.chain), snapshot.image,
        {snapshot.waits.data(), snapshot.info.waitSemaphoreCount});
}
void packetCallerFields(JsonLine& log, std::uintptr_t caller, const char* moduleField, const char* rvaField) noexcept {
    HMODULE module{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(caller), &module))
        return;
    wchar_t path[1024]{};
    char name[1024]{};
    const auto length = GetModuleFileNameW(module, path, 1024);
    if (length && length < 1024) {
        const auto* leaf = std::wcsrchr(path, L'\\');
        leaf = leaf ? leaf + 1 : path;
        if (WideCharToMultiByte(CP_UTF8, 0, leaf, -1, name, sizeof(name), nullptr, nullptr))
            log.text(moduleField, name);
    }
    log.handle(rvaField, caller - reinterpret_cast<std::uintptr_t>(module));
}
void invalidatePacketCandidates(DeviceState* state, bool provenance = false) noexcept {
    if (provenance && state->packetIdentities) state->packetIdentities->invalidate();
    if (state->packets) state->packets->invalidateUnclaimed();
}
enum class PresentOriginRoute { Direct, Queued, Unknown };
struct PresentOriginBinding {
    DeviceState* state{};
    PFN_vkQueuePresentKHR next{};
    std::uintptr_t moduleBase{};
    std::atomic<bool> active{};
    std::atomic<std::uint32_t> inFlight{};
};
std::atomic<PresentOriginBinding*> presentOriginBinding{};
struct PresentOriginScope;
thread_local PresentOriginScope* presentOriginScope{};
struct PresentOriginScope {
    PresentOriginBinding& binding;
    DeviceState* state{};
    VkQueue queue{};
    const VkPresentInfoKHR* info{};
    PresentOriginRoute route;
    PacketSnapshot source;
    packet::Key key;
    packet::Claim claim;
    std::uint64_t contextGeneration{};
    PresentOriginScope* previous{};
    const char* refusal{};
    bool active{}, nested{}, used{}, submitPending{}, submitSeen{}, waitsConsumed{};
    PresentOriginScope(PresentOriginBinding& owner, VkQueue q, const VkPresentInfoKHR* p, PresentOriginRoute r)
        : binding(owner), queue(q), info(p), route(r) {
        binding.inFlight.fetch_add(1);
        try { open(); }
        catch (...) { refusal = "origin_snapshot_exception"; }
    }
    void open() {
        if (!binding.active.load()) return;
        state = binding.state;
        if (!state) return;
        if (InterlockedCompareExchange(&state->continuousClosing, 0, 0) ||
            InterlockedCompareExchange(&state->continuousTimingRetired, 0, 0)) {
            binding.active.store(false);
            return;
        }
#ifdef XESS_FSR_DETOUR_AVAILABLE
        if (!xess_vk::fsr_callback_install::currentContinuousReady(bits(state->device))) {
            binding.active.store(false);
            return;
        }
#endif
        previous = presentOriginScope;
        presentOriginScope = this;
        active = true;
        InterlockedIncrement64(&state->continuousOriginCalls);
        if (previous) {
            previous->nested = nested = true;
            refusal = "nested_origin";
            return;
        }
        if (route == PresentOriginRoute::Unknown) {
            refusal = "unknown_origin_caller";
            return;
        }
        if (queue != state->continuousQueue || !state->presentation || !state->packets || !state->continuousTiming) {
            refusal = "origin_context_or_queue";
            return;
        }
        source = snapshotPacket(info);
        key = packetKey(state, queue, source);
        if (!key.valid) {
            refusal = "origin_packet_key_invalid";
            return;
        }
        const auto context = state->presentation->timingContextGeneration();
        contextGeneration = context;
        if (route == PresentOriginRoute::Direct) {
            const auto association = state->continuousTiming->packetAssociation(GetCurrentThreadId());
            if (!association.valid) refusal = "source_scope_or_ticket_missing";
            else if (!state->packets->publish(key, association, context)) refusal = "source_publish_refused";
        }
        if (!refusal) claim = state->packets->claim(key, context);
        if (!claim.valid && !refusal) refusal = "ticket_incomplete_or_not_current";
    }
    ~PresentOriginScope() {
        const DWORD error = GetLastError();
        if (claim.lease) state->packets->finish(claim.lease);
        if (active) presentOriginScope = previous;
        binding.inFlight.fetch_sub(1);
        SetLastError(error);
    }
    PresentOriginScope(const PresentOriginScope&) = delete;
    PresentOriginScope& operator=(const PresentOriginScope&) = delete;
};
struct PresentOriginBorrow {
    bool present{}, matched{}, direct{}, transformed{};
    packet::Key key;
    packet::Claim claim;
    const char* refusal{};
};
PresentOriginBorrow borrowPresentOrigin(DeviceState* state, VkQueue queue, const VkPresentInfoKHR* info,
                                       const PacketSnapshot& snapshot, const packet::Key& key) {
    auto* origin = presentOriginScope;
    if (!origin || !origin->active) return {};
    PresentOriginBorrow out;
    out.present = true;
    out.direct = origin->route == PresentOriginRoute::Direct;
    out.refusal = "origin_packet_changed";
    const bool alreadyUsed = origin->used;
    origin->used = true;
    if (origin->state != state || !origin->binding.active.load() ||
        InterlockedCompareExchange(&state->continuousClosing, 0, 0) || origin->nested || alreadyUsed ||
        InterlockedCompareExchange(&state->continuousTimingRetired, 0, 0) ||
        queue != origin->queue || info != origin->info || !snapshot.valid || !origin->key.valid)
        return out;
    if (!state->packetIdentities->currentSwapchain(origin->key) ||
        (origin->claim.valid && (state->presentation->timingContextGeneration() != origin->contextGeneration ||
         !state->packets->claimCurrent(origin->claim.lease, origin->contextGeneration))))
        return out;
    const auto& a = origin->source;
    const auto& b = snapshot;
    if (a.info.sType != b.info.sType || a.info.pNext != b.info.pNext ||
        a.info.swapchainCount != b.info.swapchainCount || a.info.pSwapchains != b.info.pSwapchains ||
        a.info.pImageIndices != b.info.pImageIndices || a.info.pResults != b.info.pResults ||
        a.chain != b.chain || a.image != b.image)
        return out;
    const bool unchanged = key.valid && key == origin->key;
    const bool consumed = origin->waitsConsumed && !b.info.waitSemaphoreCount && !b.info.pWaitSemaphores;
    if (!unchanged && !consumed) return out;
    out.matched = true;
    out.key = origin->key;
    out.claim = origin->claim;
    out.refusal = origin->refusal;
    out.transformed = consumed;
    InterlockedIncrement64(&state->continuousOriginConsumers);
    if (consumed) InterlockedIncrement64(&state->continuousOriginWaitTransforms);
    return out;
}
VKAPI_ATTR VkResult VKAPI_CALL unityOriginPresent(VkQueue, const VkPresentInfoKHR*);
void installPresentOrigin(DeviceState* state) noexcept {
#ifdef XESS_FSR_DETOUR_AVAILABLE
    if (!state->continuousUnityBase || InterlockedCompareExchange(&state->presentOriginInstallAttempted, 1, 0)) return;
    const auto slot = xess_vk::fsr_callback_install::currentContinuousPresentSlot(bits(state->device), state->continuousUnityBase);
    PFN_vkQueuePresentKHR next{};
    MEMORY_BASIC_INFORMATION memory{}, target{};
    HMODULE originalModule{}, ourModule{};
    bool installed{};
    if (slot && !(slot % alignof(void*)) &&
        VirtualQuery(reinterpret_cast<void*>(slot), &memory, sizeof(memory)) == sizeof(memory) &&
        memory.State == MEM_COMMIT && memory.Type == MEM_IMAGE &&
        memory.AllocationBase == reinterpret_cast<void*>(state->continuousUnityBase) &&
        (memory.Protect == PAGE_READWRITE || memory.Protect == PAGE_WRITECOPY) &&
        packetRead(reinterpret_cast<void*>(slot), &next, sizeof(next)) &&
        next && next != &unityOriginPresent &&
        VirtualQuery(reinterpret_cast<void*>(next), &target, sizeof(target)) == sizeof(target) &&
        target.State == MEM_COMMIT && !(target.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
        (target.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) &&
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                          reinterpret_cast<LPCWSTR>(next), &originalModule) &&
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                          reinterpret_cast<LPCWSTR>(&unityOriginPresent), &ourModule)) {
        auto* binding = new(std::nothrow) PresentOriginBinding;
        if (binding) {
            binding->state = state;
            binding->next = next;
            binding->moduleBase = state->continuousUnityBase;
            PresentOriginBinding* empty{};
            if (presentOriginBinding.compare_exchange_strong(empty, binding)) {
                binding->active.store(true);
                installed = InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(slot),
                    reinterpret_cast<void*>(&unityOriginPresent), reinterpret_cast<void*>(next)) == reinterpret_cast<void*>(next);
                if (!installed) binding->active.store(false);
            } else delete binding;
        }
    }
    JsonLine log("presentation_origin_slot");
    log.boolean("installed", installed);
    log.handle("slot_rva", xess_vk::game_timing::presentFunctionSlotRva);
    log.boolean("original_module_pinned", originalModule != nullptr);
    log.boolean("forwarding_module_pinned", ourModule != nullptr);
    log.write();
#else
    (void)state;
#endif
}
__declspec(noinline) VKAPI_ATTR VkResult VKAPI_CALL unityOriginPresent(VkQueue queue, const VkPresentInfoKHR* info) {
    const auto caller = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    const DWORD error = GetLastError();
    auto* binding = presentOriginBinding.load();
    if (!binding || !binding->next) return VK_ERROR_INITIALIZATION_FAILED;
    const auto next = binding->next;
    if (!binding->active.load()) return next(queue, info);
    if (presentOriginScope) presentOriginScope->nested = true;
    const bool direct = xess_vk::game_timing::directPresentCaller(binding->moduleBase, caller);
    const bool queued = caller >= binding->moduleBase &&
        caller - binding->moduleBase == xess_vk::game_timing::queuedPresentReturnRva;
    const auto route = direct ? PresentOriginRoute::Direct :
        queued ? PresentOriginRoute::Queued : PresentOriginRoute::Unknown;
    PresentOriginScope origin(*binding, queue, info, route);
    SetLastError(error);
    return next(queue, info);
}
void consumePresentWaits(DeviceState* state, const VkPresentInfoKHR* source) noexcept {
    if (!state->packetIdentities) return;
    VkPresentInfoKHR info{};
    std::array<VkSemaphore, 64> semaphores{};
    std::array<std::uint64_t, 64> waits{};
    if (!packetRead(source, &info, sizeof(info)) || info.waitSemaphoreCount > waits.size() ||
        (info.waitSemaphoreCount && !packetRead(info.pWaitSemaphores, semaphores.data(), info.waitSemaphoreCount * sizeof(VkSemaphore)))) {
        invalidatePacketCandidates(state, true);
        return;
    }
    for (std::uint32_t i = 0; i < info.waitSemaphoreCount; ++i)
        waits[i] = bits(semaphores[i]);
    state->packetIdentities->consumedWaits({waits.data(), info.waitSemaphoreCount});
}
void initializePacketLedger(DeviceState* state) {
    if (!state->continuousTiming || !state->packetIdentities || state->packets) return;
    packet::Callbacks callbacks;
    callbacks.user = state->continuousTiming;
    callbacks.pin = [](void* user, xess_vk::FrameTimingAssociation frame, std::uint64_t context) {
        return static_cast<xess_vk::continuous_timing::Coordinator*>(user)->pinPacket(frame, context);
    };
    callbacks.resolve = [](void* user, xess_vk::continuous_timing::PacketPin pin) {
        return static_cast<xess_vk::continuous_timing::Coordinator*>(user)->resolvePacket(pin);
    };
    callbacks.unpin = [](void* user, xess_vk::continuous_timing::PacketPin pin) {
        static_cast<xess_vk::continuous_timing::Coordinator*>(user)->unpinPacket(pin);
    };
    state->packets = new packet::Ledger(callbacks);
}
bool continuousEnqueue(void* user, const xess_vk::game_timing::EnqueueEvent& event) {
    auto* state = static_cast<DeviceState*>(user);
    const auto call = InterlockedIncrement64(&state->continuousEnqueueCalls);
    bool ready{}, queueMatched{}, shapeValid{}, keyValid{}, associationValid{};
    std::uint64_t context{};
    const auto refused = [&](const char* reason) {
        const auto failures = InterlockedIncrement64(&state->continuousEnqueueFailures);
        if (failures <= 8) {
            JsonLine log("presentation_packet_source_refused");
            log.number("source_call", call); log.number("source_failure", failures);
            log.text("reason", reason); log.number("thread_id", event.threadId);
            log.handle("backend", event.state); log.handle("present_info", event.presentInfo);
            log.handle("queue_argument", event.queue); log.handle("expected_queue", bits(state->continuousQueue));
            log.boolean("context_ready", ready); log.boolean("queue_valid", event.queueValid);
            log.boolean("queue_matched", queueMatched); log.boolean("packet_shape_valid", shapeValid);
            log.boolean("packet_key_valid", keyValid); log.boolean("association_valid", associationValid);
            log.number("context_generation", context);
            log.boolean("timing_retired_at_snapshot", InterlockedCompareExchange(&state->continuousTimingRetired, 0, 0) != 0);
            if (ready) {
                const auto scope = state->continuousTiming->scopeSnapshot(event.threadId);
                log.boolean("scope_in_render", scope.inRender); log.boolean("scope_in_native_present", scope.inPresent);
                log.number("render_key_diagnostic", scope.renderFrame); log.number("present_key_diagnostic", scope.presentFrame);
            }
            log.write();
        }
        return false;
    };
    if (InterlockedCompareExchange(&state->continuousClosing, 0, 0)) return refused("device_closing");
    ready = state->packets && state->continuousTiming && state->presentation;
    if (event.queueValid) InterlockedIncrement64(&state->continuousEnqueueQueueValid);
    queueMatched = event.queueValid && event.queue == bits(state->continuousQueue);
    if (queueMatched) InterlockedIncrement64(&state->continuousEnqueueQueueMatched);
    if (!ready || !queueMatched) {
        invalidatePacketCandidates(state);
        return refused(!ready ? "context_not_ready" : !event.queueValid ? "queue_invalid" : "queue_mismatch");
    }
    const auto snapshot = snapshotPacket(reinterpret_cast<const VkPresentInfoKHR*>(event.presentInfo));
    shapeValid = snapshot.valid;
    if (shapeValid) InterlockedIncrement64(&state->continuousEnqueueShapes);
    const auto key = packetKey(state, reinterpret_cast<VkQueue>(event.queue), snapshot);
    keyValid = key.valid;
    if (keyValid) InterlockedIncrement64(&state->continuousEnqueueKeys);
    if (!key.valid) {
        invalidatePacketCandidates(state);
        return refused(shapeValid ? "packet_key_invalid" : "packet_shape_invalid");
    }
    context = state->presentation->timingContextGeneration();
    const auto association = state->continuousTiming->packetAssociation(event.threadId);
    associationValid = association.valid;
    if (associationValid) InterlockedIncrement64(&state->continuousEnqueueAssociations);
    InterlockedIncrement64(&state->continuousEnqueuePublishAttempts);
    if (!state->packets->publish(key, association, context))
        return refused(!context ? "sdk_context_unavailable" : !associationValid ? "association_invalid" : "publish_refused");
    return true;
}
struct PacketSubmitSnapshot {
    std::array<std::uint64_t, 64> waits{}, signals{};
    std::uint32_t waitCount{}, signalCount{};
    bool valid{true}, simple{};
};
PacketSubmitSnapshot snapshotSubmit(std::uint32_t count, const VkSubmitInfo* infos) noexcept {
    PacketSubmitSnapshot out;
    out.simple = count == 1;
    if (count > 64 || (count && !infos)) {
        out.valid = false;
        return out;
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        VkSubmitInfo info{};
        if (!packetRead(infos + i, &info, sizeof(info)) || info.waitSemaphoreCount > 64 - out.waitCount ||
            info.signalSemaphoreCount > 64 - out.signalCount) {
            out.valid = false;
            return out;
        }
        if (info.sType != VK_STRUCTURE_TYPE_SUBMIT_INFO || info.pNext) out.simple = false;
        std::array<VkSemaphore, 64> semaphores{};
        if (info.waitSemaphoreCount && !packetRead(info.pWaitSemaphores, semaphores.data(), info.waitSemaphoreCount * sizeof(VkSemaphore))) {
            out.valid = false;
            return out;
        }
        for (std::uint32_t j = 0; j < info.waitSemaphoreCount; ++j)
            out.waits[out.waitCount++] = bits(semaphores[j]);
        if (info.signalSemaphoreCount && !packetRead(info.pSignalSemaphores, semaphores.data(), info.signalSemaphoreCount * sizeof(VkSemaphore))) {
            out.valid = false;
            return out;
        }
        for (std::uint32_t j = 0; j < info.signalSemaphoreCount; ++j)
            out.signals[out.signalCount++] = bits(semaphores[j]);
    }
    return out;
}
PacketSubmitSnapshot snapshotSubmit(std::uint32_t count, const VkSubmitInfo2* infos) noexcept {
    PacketSubmitSnapshot out;
    if (count > 64 || (count && !infos)) {
        out.valid = false;
        return out;
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        VkSubmitInfo2 info{};
        if (!packetRead(infos + i, &info, sizeof(info)) || info.waitSemaphoreInfoCount > 64 - out.waitCount ||
            info.signalSemaphoreInfoCount > 64 - out.signalCount) {
            out.valid = false;
            return out;
        }
        for (std::uint32_t j = 0; j < info.waitSemaphoreInfoCount; ++j) {
            VkSemaphoreSubmitInfo wait{};
            if (!packetRead(info.pWaitSemaphoreInfos + j, &wait, sizeof(wait))) {
                out.valid = false;
                return out;
            }
            out.waits[out.waitCount++] = bits(wait.semaphore);
        }
        for (std::uint32_t j = 0; j < info.signalSemaphoreInfoCount; ++j) {
            VkSemaphoreSubmitInfo signal{};
            if (!packetRead(info.pSignalSemaphoreInfos + j, &signal, sizeof(signal))) {
                out.valid = false;
                return out;
            }
            out.signals[out.signalCount++] = bits(signal.semaphore);
        }
    }
    return out;
}
PresentOriginScope* beginOriginSubmit(DeviceState* state, VkQueue queue, const PacketSubmitSnapshot& snapshot) {
    auto* origin = presentOriginScope;
    if (!origin || !origin->active || origin->state != state || origin->queue != queue ||
        origin->nested || origin->used || !origin->key.valid || !snapshot.waitCount)
        return nullptr;
    origin->waitsConsumed = false;
    const bool repeated = origin->submitSeen;
    origin->submitSeen = true;
    origin->submitPending = false;
    if (repeated || !snapshot.valid || !snapshot.simple) return nullptr;
    for (std::uint32_t i = 0; i < snapshot.signalCount; ++i) {
        if (std::find(snapshot.waits.begin(), snapshot.waits.begin() + snapshot.waitCount, snapshot.signals[i]) !=
            snapshot.waits.begin() + snapshot.waitCount)
            return nullptr;
    }
    packet::Key key;
    try {
        key = state->packetIdentities->makeKey(bits(queue), bits(origin->source.chain), origin->source.image,
            {snapshot.waits.data(), snapshot.waitCount});
    } catch (...) { return nullptr; }
    if (!key.valid || key != origin->key) return nullptr;
    origin->submitPending = true;
    return origin;
}
void finishOriginSubmit(PresentOriginScope* origin, VkResult result) noexcept {
    if (!origin) return;
    origin->waitsConsumed = origin->submitPending && !origin->nested && origin->binding.active.load() && result == VK_SUCCESS;
    origin->submitPending = false;
    if (origin->waitsConsumed) InterlockedIncrement64(&origin->state->continuousOriginSubmitProofs);
}
packet::IdentityRegistry::Submission beginPacketSubmit(DeviceState* state, PacketSubmitSnapshot snapshot) noexcept {
    if (!state->packetIdentities) return {};
    if (!snapshot.valid) {
        invalidatePacketCandidates(state, true);
        return {};
    }
    state->packetIdentities->consumedWaits({snapshot.waits.data(), snapshot.waitCount});
    // A handle signalled and waited anywhere in the same multi-batch call has
    // ambiguous final provenance here. Preserve driver semantics and decline
    // association instead of inventing a batch-order interpretation.
    std::uint32_t count{};
    for (std::uint32_t i = 0; i < snapshot.signalCount; ++i)
        if (std::find(snapshot.waits.begin(), snapshot.waits.begin() + snapshot.waitCount, snapshot.signals[i]) ==
            snapshot.waits.begin() + snapshot.waitCount)
            snapshot.signals[count++] = snapshot.signals[i];
    return state->packetIdentities->beginSignals({snapshot.signals.data(), count});
}
xess_vk::FrameTimingAssociation continuousRegistrationFrame(void* user, std::uint32_t thread) {
    auto* state = static_cast<DeviceState*>(user);
    struct StateReadLock {
        StateReadLock() { AcquireSRWLockShared(&stateLock); }
        ~StateReadLock() { ReleaseSRWLockShared(&stateLock); }
    } guard;
    auto* live = devices;
    while (live && live != state) live = live->next;
    if (!live || InterlockedCompareExchange(&live->continuousClosing, 0, 0)) return {};
    xess_vk::continuous_timing::ScopeSnapshot scope;
    const auto key = xess_vk::game_timing::readCpuFrameKey();
    const auto frame = state->continuousTiming
                           ? state->continuousTiming->registrationAssociation(thread, key.engineFrame, key.valid, &scope)
                           : xess_vk::FrameTimingAssociation{};
    const auto count = InterlockedIncrement64(&state->continuousRegistrationCalls);
    const bool windowSample = continuousWindowLog(state);
    if (count <= 8 || windowSample) {
        JsonLine log("continuous_registration_frame");
        continuousDiagnosticFields(log, state, thread, scope);
        log.number("registration_call", count);
        log.text("association_source", "thread_owned_cpu_frame_verified_c8");
        log.boolean("actual_cpu_key_valid", key.valid);
        log.number("actual_cpu_key", key.engineFrame);
        log.boolean("frame_valid", frame.valid);
        log.number("engine_frame", frame.engineFrame);
        log.boolean("ticket_valid", bool(frame.ticket));
        log.number("sdk_id", frame.ticket.sdkId);
        log.number("ticket_context", frame.ticket.contextGeneration);
        log.write();
    }
    return frame;
}
#if defined(XESS_FSR_DETOUR_AVAILABLE) || defined(XESS_METADATA_CONSUMER_TEST)
void imageMetadataLog(std::uint64_t device, std::uint64_t sample, const xess_vk::fsr_probe::Result& result, bool emitDetail) noexcept {
    if constexpr(!researchDiagnostics)return;
    if (result.status != xess_vk::fsr_probe::Status::SnapshotParsed || !result.allPassPropertiesPresent ||
        result.pass == xess_vk::fsr_probe::PassKind::Unknown || !result.contextBoundsUnchanged) return;
    for (const auto& binding : result.bindings) {
        const bool nativeAvailable=binding.native.status == xess_vk::fsr_probe::Status::NativeCandidateReadUnverified;
        xess_vk::image_inventory::Snapshot snapshot;
        xess_vk::candidate_sync::SelectionNotice selection;
        const char* status = "device_or_inventory_unavailable";
        // Keep the device record alive through lookup. Release before logging;
        // the returned metadata does not pin the device/image after this point.
        AcquireSRWLockShared(&stateLock);
        if (inventorySessionRetired) status = "device_session_retired";
        else for (auto* item = devices; item; item = item->next) if (bits(item->device) == device) {
            if (!nativeAvailable) status = "native_candidate_unavailable";
            else if (item->imageInventory) {
                try { snapshot = item->imageInventory->lookup(binding.native.imageCandidate); status = xess_vk::image_inventory::name(snapshot.status); }
                catch (...) { status = "inventory_lookup_failed"; }
            }
            using DSource=xess_vk::descriptor_image_observer::Source;
            DSource source=DSource::Count;
            using FPass=xess_vk::fsr_probe::PassKind;
            using FProperty=xess_vk::fsr_probe::Property;
            if (result.pass==FPass::ReconstructPreviousDepth && binding.property==FProperty::ReadDilatedMotionVectors) source=DSource::FiMotion;
            if (result.pass==FPass::ReconstructPreviousDepth && binding.property==FProperty::ReadDilatedDepth) source=DSource::FiDepth;
            if (result.pass==FPass::FrameInpainting && binding.property==FProperty::PresentBackbuffer) source=DSource::FiRealColor;
            if (source!=DSource::Count) {
                const bool live=snapshot.status==xess_vk::image_inventory::Status::LiveHandleObserved;
                xess_vk::descriptor_image_observer::observeSource(item->descriptors, source,
                    live ? xess_vk::descriptor_image_observer::Key{snapshot.record.image,snapshot.record.generation} : xess_vk::descriptor_image_observer::Key{}, sample);
            }
            // Candidate sync is absent on the product route; its calls are null-safe
            // and the render-pass role selection must not depend on it.
            if (snapshot.status == xess_vk::image_inventory::Status::LiveHandleObserved) {
                using Pass = xess_vk::fsr_probe::PassKind;
                using Property = xess_vk::fsr_probe::Property;
                using Role = xess_vk::candidate_sync::Role;
                if (result.pass == Pass::ReconstructPreviousDepth && binding.property == Property::ReadDilatedMotionVectors) {
                    selection = xess_vk::candidate_sync::select(item->candidateSync, Role::Motion, snapshot.record);
                    xess_vk::render_pass_observer::select(item->renderPass, xess_vk::render_pass_observer::Role::Motion, snapshot.record);
                } else if (result.pass == Pass::ReconstructPreviousDepth && binding.property == Property::ReadDilatedDepth) {
                    selection = xess_vk::candidate_sync::select(item->candidateSync, Role::Depth, snapshot.record);
                    xess_vk::render_pass_observer::select(item->renderPass, xess_vk::render_pass_observer::Role::Depth, snapshot.record);
                } else if (result.pass == Pass::FrameInpainting && binding.property == Property::PresentBackbuffer) {
                    selection = xess_vk::candidate_sync::select(item->candidateSync, Role::RealColor, snapshot.record);
                    xess_vk::render_pass_observer::select(item->renderPass, xess_vk::render_pass_observer::Role::RealColor, snapshot.record);
                }
            } else if (snapshot.status != xess_vk::image_inventory::Status::NotObserved) {
                // Faulted inventory no longer supplies creation generations.
                if constexpr(researchDiagnostics) xess_vk::candidate_sync::retire(item->candidateSync);
                xess_vk::render_pass_observer::retire(item->renderPass);
                xess_vk::descriptor_image_observer::retire(item->descriptors);
            }
            break;
        }
        ReleaseSRWLockShared(&stateLock);
        // Selection returns values only: no context pointer escapes the lifetime
        // lock, and logging happens after releasing it.
        if constexpr(researchDiagnostics) xess_vk::candidate_sync::reportSelection(selection, streamlineLog);
        if (!emitDetail) continue;
        JsonLine log("fsr_image_metadata_match");
        log.handle("device", device); log.number("matched_call", sample);
        log.text("pass", xess_vk::fsr_probe::passName(result.pass));
        log.text("property", xess_vk::fsr_probe::propertyName(binding.property));
        log.handle("image_candidate", binding.native.imageCandidate); log.text("match_status", status);
        log.number("inventory_active_count", snapshot.active);
        const bool matched = snapshot.status == xess_vk::image_inventory::Status::LiveHandleObserved;
        log.boolean("numeric_live_handle_match", matched);
        if (matched) {
            const auto& d = snapshot.record.description;
            log.number("image_generation", snapshot.record.generation);
            log.number("format", d.format); log.number("width", d.width); log.number("height", d.height); log.number("depth", d.depth);
            log.number("mip_levels", d.mipLevels); log.number("array_layers", d.arrayLayers); log.number("samples", d.samples);
            log.number("usage", d.usage); log.number("flags", d.flags); log.number("tiling", d.tiling);
            log.number("sharing_mode", d.sharingMode); log.number("initial_layout", d.initialLayout);
            log.boolean("candidate_extent_matches", d.width == binding.native.widthCandidate && d.height == binding.native.heightCandidate);
        }
        log.boolean("resource_lifetime_pinned", false); log.boolean("pixel_semantics_verified", false);
        log.boolean("synchronization_verified", false); log.boolean("game_frame_inputs_verified", false); log.write();
    }
}

// Independent eight-input observations. Never overwrite the FI selection: the
// two callbacks have different counters and no proven same-frame association.
std::size_t continuousRead(void*, std::uint64_t address, void* output,
                           std::size_t bytes, std::size_t budget) {
    if (!address || bytes > budget || address > UINTPTR_MAX - bytes) return 0;
    SIZE_T copied{};
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address),
                             output, bytes, &copied) && copied == bytes ? bytes : 0;
}
bool continuousTimingEvent(void* user, const xess_vk::game_timing::Event& event) {
    auto* state = static_cast<DeviceState*>(user);
    if (InterlockedCompareExchange(&state->continuousClosing, 0, 0) || !state->continuousTiming) return false;
    if (event.stage == xess_vk::game_timing::Stage::NativePresentStart && event.frameKeyValid)
        installPresentOrigin(state);
    const bool accepted = state->continuousTiming->event(event);
    if (continuousWindowLog(state)) {
        JsonLine window("continuous_window_boundary");
        continuousDiagnosticFields(window, state, event.threadId,
                                   state->continuousTiming->scopeSnapshot(event.threadId));
        window.number("stage", static_cast<unsigned>(event.stage));
        window.number("engine_frame", event.engineFrame);
        window.boolean("frame_key_valid", event.frameKeyValid);
        window.number("source_rva", event.sourceRva);
        window.boolean("accepted", accepted);
        window.write();
    }
    const auto call = InterlockedIncrement64(&state->timingEvents);
    if (!accepted || call <= 24 || call % 1800 == 0) {
        JsonLine log("game_frame_timing_boundary");
        log.number("stage", static_cast<unsigned>(event.stage));
        log.number("engine_frame", event.engineFrame);
        log.number("source_rva", event.sourceRva);
        log.number("key_source", static_cast<unsigned>(event.keySource));
        log.number("boundary", static_cast<unsigned>(event.boundary));
        log.number("thread_id", event.threadId);
        log.boolean("frame_key_valid", event.frameKeyValid);
        log.boolean("accepted", accepted);
        log.write();
    }
    return accepted;
}
void srObservationImages(std::uint64_t device,const xess_vk::upscale_tail_probe::Result& parsed,
                         xess_vk::sr_observation::Images& output) noexcept {
    AcquireSRWLockShared(&stateLock);
    try {
        if(!inventorySessionRetired)for(auto* state=devices;state;state=state->next)
            if(bits(state->device)==device&&state->srObservationRequested&&state->imageInventory&&
               !InterlockedCompareExchange(&state->continuousClosing,0,0)) {
                for(std::size_t n=0;n<parsed.bindingCount&&n<output.size();++n)
                    output[n]=state->imageInventory->lookup(parsed.bindings[n].native.imageCandidate);
                break;
            }
    }catch(...) {output={};}
    ReleaseSRWLockShared(&stateLock);
}
void continuousInvocation(std::uint64_t device, std::uint64_t invocation,
                          std::uintptr_t payload, std::uintptr_t graphContext,
                          const xess_vk::upscale_probe::Result& parsed, bool) noexcept {
    try {
        xess_vk::continuous::SourcePair pair;
        pair.invocation = invocation;
        pair.thread = GetCurrentThreadId();
        xess_vk::frame_input::FrameConstantsSample provenance;
        provenance.producerToken = invocation;
        const auto constants = xess_vk::frame_input::readPrepareConstants(
            payload, {continuousRead, nullptr}, provenance);
        const auto camera = xess_vk::continuous_registration::consume(payload);
        pair.constants = constants.sample;
        if (camera.matched) {
            pair.constants = xess_vk::frame_input::convertFrameCamera(
                camera.camera, constants.sample,
                xess_vk::frame_input::pinnedUnityProjectionConvention()).sample;
            pair.constants.outputRegion = camera.outputRegion;
        }
        struct StateReadLock {
            StateReadLock() { AcquireSRWLockShared(&stateLock); }
            ~StateReadLock() { ReleaseSRWLockShared(&stateLock); }
        } guard;
        for (auto* item = devices; item; item = item->next) {
            if (bits(item->device) != device || !item->continuous || InterlockedCompareExchange(&item->continuousClosing,0,0)) continue;
            if (camera.matched && camera.frame.valid && item->continuousTiming) {
                pair.engineFrame = camera.frame.engineFrame;
                const auto current = item->continuousTiming->frame(camera.frame.engineFrame);
                if (current && current == camera.frame.ticket) {
                    pair.timing = camera.frame.ticket;
                    pair.frameValid = true;
                }
            }
            if (!inventorySessionRetired && parsed.status == xess_vk::upscale_probe::Status::SnapshotParsed &&
                parsed.snapshotCaptured && parsed.allPropertiesPresent && parsed.graphIndexedEntries &&
                parsed.contextTableUnchanged && parsed.graphEntriesObserved==xess_vk::upscale_probe::bindingCount &&
                parsed.graphTextureIdsRead==xess_vk::upscale_probe::bindingCount && parsed.nativeCandidatesRead==xess_vk::upscale_probe::bindingCount &&
                !parsed.allocationBoundsVerified && parsed.descriptorsUnchanged &&
                parsed.graphEntriesUnchanged && parsed.bindingMetadataUnchanged) {
                for (const auto& binding : parsed.bindings) {
                    if (binding.property != xess_vk::upscale_probe::Property::InputDepth &&
                        binding.property != xess_vk::upscale_probe::Property::InputMotionVectors) continue;
                    if (binding.native.status != xess_vk::fsr_probe::Status::NativeCandidateReadUnverified) continue;
                    const auto image = item->imageInventory->lookup(binding.native.imageCandidate);
                    if (image.status != xess_vk::image_inventory::Status::LiveHandleObserved) continue;
                    const xess_vk::descriptor_image_metadata::Key key{image.record.image, image.record.generation};
                    if (binding.property == xess_vk::upscale_probe::Property::InputDepth) pair.depth = key;
                    if (binding.property == xess_vk::upscale_probe::Property::InputMotionVectors) pair.motion = key;
                }
                pair.valid = pair.depth.handle && pair.motion.handle;
            }
            if constexpr(researchDiagnostics) if(item->renderTail&&item->renderTail->active()) {
                xess_vk::render_tail::Source source;source.frame=camera.frame;source.producer=invocation;source.registration=camera.registration;
                source.graphContext=graphContext;source.depth=pair.depth;source.motion=pair.motion;source.accepted=pair.valid&&pair.frameValid&&camera.matched;
                source.renderWidth=pair.constants.renderSize[0];source.renderHeight=pair.constants.renderSize[1];
                source.upscaleWidth=pair.constants.upscaleSize[0];source.upscaleHeight=pair.constants.upscaleSize[1];
                for(std::size_t n=0;n<parsed.bindings.size();++n){source.graphHandles[n]=parsed.bindings[n].graphHandle;source.graphKeys[n]=parsed.bindings[n].graphKey;source.textureIds[n]=parsed.bindings[n].textureId;}
                item->renderTail->source(source);
            }
            item->continuous->invocation(pair);
            const auto call = InterlockedIncrement64(&item->continuousInvocationSamples);
            if (pair.valid) {
                if (!camera.matched)
                    InterlockedIncrement64(&item->continuousCameraMissing);
                else if (!camera.frame.valid)
                    InterlockedIncrement64(&item->continuousRegistrationFrameMissing);
                else if (!pair.timing)
                    InterlockedIncrement64(&item->continuousResolvedTicketMissing);
            }
            const bool windowSample = continuousWindowLog(item);
            if (call <= 8 || windowSample) {
                JsonLine log("continuous_invocation_frame");
                continuousDiagnosticFields(log, item, pair.thread,
                    item->continuousTiming ? item->continuousTiming->scopeSnapshot(pair.thread)
                                           : xess_vk::continuous_timing::ScopeSnapshot{});
                log.number("producer_token", invocation);
                log.number("registration_token", camera.registration);
                log.boolean("camera_matched", camera.matched);
                log.boolean("registration_frame_valid", camera.frame.valid);
                log.number("registration_engine_frame", camera.frame.engineFrame);
                log.boolean("resolved_ticket_valid", bool(pair.timing));
                log.number("resolved_sdk_id", pair.timing.sdkId);
                log.number("resolved_ticket_context", pair.timing.contextGeneration);
                log.number("input_validity", pair.constants.validityMask);
                log.boolean("resource_pair_valid", pair.valid);
                log.handle("depth_image", pair.depth.handle);
                log.number("depth_generation", pair.depth.generation);
                log.handle("motion_image", pair.motion.handle);
                log.number("motion_generation", pair.motion.generation);
                log.write();
            }
            break;
        }
    } catch (...) {
        // Callback readers retain no game pointers. Missing data remains OFF.
    }
}
void upscaleImageMetadataLog(std::uint64_t device, std::uint64_t callbackCall,
    const xess_vk::upscale_probe::Result& result, bool emitDetail) noexcept {
    if (result.status != xess_vk::upscale_probe::Status::SnapshotParsed || !result.allPropertiesPresent ||
        !result.contextTableUnchanged || !result.graphIndexedEntries || result.allocationBoundsVerified ||
        !result.descriptorsUnchanged || !result.graphEntriesUnchanged || !result.bindingMetadataUnchanged) return;
    constexpr auto count = xess_vk::upscale_probe::bindingCount;
    xess_vk::image_inventory::Snapshot snapshots[count]{};
    const char* statuses[count]{};
    for (auto& status : statuses) status = "device_or_inventory_unavailable";
    // Device lifetime is protected across all lookups. Each image lookup is a
    // point-in-time numeric match; this does not freeze the eight-image set.
    AcquireSRWLockShared(&stateLock);
    if (inventorySessionRetired) {
        for (auto& status : statuses) status = "device_session_retired";
    } else for (auto* item = devices; item; item = item->next) if (bits(item->device) == device) {
        InterlockedIncrement64(&item->upscaleMetadataCalls);
        for (std::size_t i = 0; i < count; ++i) {
            const auto& binding = result.bindings[i];
            using DSource=xess_vk::descriptor_image_observer::Source;
            using UProperty=xess_vk::upscale_probe::Property;
            DSource source=DSource::Count;
            switch(binding.property) {
            case UProperty::InputMotionVectors: source=DSource::RawMotion; break;
            case UProperty::InputDepth: source=DSource::RawDepth; break;
            case UProperty::InputColorJittered: source=DSource::RawColor; break;
            case UProperty::WriteDilatedMotionVectors: source=DSource::DilatedMotion; break;
            case UProperty::WriteDilatedDepth: source=DSource::DilatedDepth; break;
            case UProperty::WriteReconstructedPreviousNearestDepth: source=DSource::ReconstructedDepth; break;
            case UProperty::WriteFarthestDepth: source=DSource::FarthestDepth; break;
            case UProperty::WriteCurrentLuma: source=DSource::Luma; break;
            default: break;
            }
            if (source!=DSource::Count)
                xess_vk::descriptor_image_observer::observeSource(item->descriptors, source, {}, callbackCall);
            if (binding.native.status != xess_vk::fsr_probe::Status::NativeCandidateReadUnverified) {
                statuses[i] = "native_candidate_unavailable";
                continue;
            }
            if (!item->imageInventory) continue;
            InterlockedIncrement64(&item->upscaleMetadataLookups);
            try {
                snapshots[i] = item->imageInventory->lookup(binding.native.imageCandidate);
                statuses[i] = xess_vk::image_inventory::name(snapshots[i].status);
                if (snapshots[i].status == xess_vk::image_inventory::Status::LiveHandleObserved)
                    InterlockedIncrement64(&item->upscaleMetadataMatches);
            } catch (...) { statuses[i] = "inventory_lookup_failed"; }
            if (source!=DSource::Count && snapshots[i].status==xess_vk::image_inventory::Status::LiveHandleObserved)
                xess_vk::descriptor_image_observer::observeSource(item->descriptors, source,
                    {snapshots[i].record.image,snapshots[i].record.generation},callbackCall);
        }
        break;
    }
    ReleaseSRWLockShared(&stateLock);
    // No device, inventory, or observer context pointer escapes the lock.
    // Keep a sparse current-binding sample after the initial detail quota.
    // These are callback ordinals, never frame IDs. The probe already runs on
    // each 120th callback; one of every ten refreshes is retained for mode/resize
    // comparisons so old startup identities are not reported as current ones.
    const bool periodicRefresh=!emitDetail && callbackCall && callbackCall%1200==0;
    if (!emitDetail && !periodicRefresh) return;
    for (std::size_t i = 0; i < count; ++i) {
        const auto& binding = result.bindings[i];
        const auto& snapshot = snapshots[i];
        const bool matched = snapshot.status == xess_vk::image_inventory::Status::LiveHandleObserved;
        JsonLine log("upscale_image_metadata_match");
        log.handle("device", device); log.number("callback_call", callbackCall);
        log.text("sample_reason", emitDetail ? "initial_detail" : "periodic_refresh");
        log.text("source", "upscaler_prepare_eb49d0");
        log.text("property", xess_vk::upscale_probe::propertyName(binding.property));
        log.handle("image_candidate", binding.native.imageCandidate); log.text("match_status", statuses[i]);
        log.number("inventory_active_count", snapshot.active);
        log.boolean("numeric_live_handle_match", matched);
        if (matched) {
            const auto& d = snapshot.record.description;
            log.number("image_generation", snapshot.record.generation);
            log.number("format", d.format); log.number("width", d.width); log.number("height", d.height); log.number("depth", d.depth);
            log.number("mip_levels", d.mipLevels); log.number("array_layers", d.arrayLayers); log.number("samples", d.samples);
            log.number("usage", d.usage); log.number("flags", d.flags); log.number("tiling", d.tiling);
            log.number("sharing_mode", d.sharingMode); log.number("initial_layout", d.initialLayout);
            log.boolean("candidate_extent_matches", d.width == binding.native.widthCandidate && d.height == binding.native.heightCandidate);
        }
        log.boolean("resource_lifetime_pinned", false); log.boolean("snapshot_atomic", false);
        log.boolean("undilated_verified", false); log.boolean("motion_semantics_verified", false);
        log.boolean("same_frame_verified", false); log.boolean("fi_roles_updated", false);
        log.boolean("copy_permitted", false); log.boolean("xeFG_integrated", false); log.write();
    }
}
#endif

void continuousRecord(DeviceState* state, const xess_vk::CommandRecordingFacts& recording,
                      const xess_vk::descriptor_image_metadata::DispatchEvidence& evidence) {
#ifdef XESS_METADATA_CONSUMER_TEST
    if(srContinuousRecordForTest){srContinuousRecordForTest(recording);return;}
#endif
    if (!state->continuous || evidence.filteredOut)
        return;
    const auto thread = GetCurrentThreadId();
    const xess_vk::descriptor_image_metadata::ImageBinding* depth = nullptr;
    const xess_vk::descriptor_image_metadata::ImageBinding* motion = nullptr;
    for (std::size_t index = 0; index < evidence.count && index < evidence.bindings.size(); ++index) {
        const auto& binding = evidence.bindings[index];
        if (binding.setIndex == 0 && binding.binding == 5 &&
            binding.viewFormat == VK_FORMAT_D32_SFLOAT_S8_UINT)
            depth = &binding;
        if (binding.setIndex == 0 && binding.binding == 7 &&
            binding.viewFormat == VK_FORMAT_R16G16_SFLOAT)
            motion = &binding;
    }
    const bool eligible = evidence.metadataAccepted && !evidence.issues && depth && motion &&
                          depth->image.handle && motion->image.handle && recording.recording &&
                          depth->imageDescription.width == motion->imageDescription.width &&
                          depth->imageDescription.height == motion->imageDescription.height;
    if (eligible && InterlockedCompareExchange(&state->continuousWindowState, -1, 0) == 0) {
        state->continuousWindowBudget = 512;
        InterlockedExchange(&state->continuousWindowState, 1);
        JsonLine log("continuous_window_begin");
        log.number("sequence", InterlockedIncrement64(&state->continuousDiagnosticSequence));
        log.text("trigger", "actual_b5_d32s8_b7_rg16_dispatch");
        log.number("present_budget", 16);
        log.number("record_budget", 512);
        log.number("stack_budget", 4);
        log.write();
    }
    xess_vk::continuous_timing::ScopeSnapshot scope;
    const auto frame = state->continuousTiming
                           ? state->continuousTiming->recordingAssociation(thread, &scope)
                           : xess_vk::FrameTimingAssociation{};
    const auto intent = xess_vk::fsr_menu_control::snapshot();
    const bool captureInputs = xess_vk::fsr_menu_control::auxiliaryCaptureRequired(intent) &&
                               !xess_vk::diagnostic_flags::skipInputSnapshot();
    const bool captured = state->continuous->record(thread, recording, evidence, frame, captureInputs);
    if (eligible && continuousWindowActive(state) &&
        InterlockedIncrement64(&state->continuousWindowDispatches) <= 128 && continuousWindowLog(state)) {
        JsonLine log("continuous_window_dispatch");
        continuousDiagnosticFields(log, state, thread, scope);
        log.handle("command", recording.command);
        log.number("rp_recording_generation", recording.rpRecordingGeneration);
        log.number("descriptor_recording_generation", evidence.command.generation);
        log.number("common_record_order", recording.commonRecordOrder);
        log.number("begin_usage_flags", recording.beginUsageFlags);
        log.boolean("recording_scope_known", recording.scopeKnown);
        log.boolean("in_render_pass", recording.inRenderPass);
        log.boolean("in_dynamic_rendering", recording.inDynamicRendering);
        log.boolean("producer_frame_valid", frame.valid);
        log.text("association_source", scope.inRender ? "active_render_submit_scope" :
                 scope.inPresent ? "active_native_present_packet" : "no_active_recording_scope");
        log.number("engine_frame", frame.engineFrame);
        log.boolean("producer_ticket_valid", bool(frame.ticket));
        log.number("sdk_id", frame.ticket.sdkId);
        log.number("ticket_context", frame.ticket.contextGeneration);
        log.boolean("captured", captured);
        log.handle("depth_image", depth->image.handle);
        log.number("depth_generation", depth->image.generation);
        log.handle("motion_image", motion->image.handle);
        log.number("motion_generation", motion->image.generation);
        if ((!frame.valid || !frame.ticket) && InterlockedIncrement(&state->continuousWindowStacks) <= 4)
            continuousMissingStack(log);
        log.write();
    }
}
void continuousCommandWindow(DeviceState* state, const char* event,
                             const xess_vk::CommandRecordingFacts& recording, VkResult result,
                             bool begin) noexcept {
    if (!continuousWindowActive(state))
        return;
    const auto count = begin ? InterlockedIncrement64(&state->continuousWindowBegins)
                             : InterlockedIncrement64(&state->continuousWindowSubmits);
    if (count > (begin ? 32 : 64) || !continuousWindowLog(state))
        return;
    const auto thread = GetCurrentThreadId();
    const auto scope = state->continuousTiming
                           ? state->continuousTiming->scopeSnapshot(thread)
                           : xess_vk::continuous_timing::ScopeSnapshot{};
    JsonLine log(event);
    continuousDiagnosticFields(log, state, thread, scope);
    log.handle("command", recording.command);
    log.number("rp_recording_generation", recording.rpRecordingGeneration);
    log.number("result", result);
    log.number("begin_usage_flags", recording.beginUsageFlags);
    log.write();
}

template<class Function> auto continuousSdkCall(Function function) {
    try { return function(); }
    catch (const std::exception& error) {
        JsonLine log("game_frame_timing_sdk_error");
        log.text("error", error.what());
        log.number("thread_id", GetCurrentThreadId());
        log.write();
        throw;
    }
}
VkLayerInstanceCreateInfo* instanceLink(const VkInstanceCreateInfo* info) noexcept {
    auto* node = reinterpret_cast<const VkBaseInStructure*>(info->pNext);
    while (node) {
        if (node->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO) {
            auto* candidate = const_cast<VkLayerInstanceCreateInfo*>(reinterpret_cast<const VkLayerInstanceCreateInfo*>(node));
            if (candidate->function == VK_LAYER_LINK_INFO) return candidate;
        }
        node = node->pNext;
    }
    return nullptr;
}
VkLayerDeviceCreateInfo* deviceLink(const VkDeviceCreateInfo* info) noexcept {
    auto* node = reinterpret_cast<const VkBaseInStructure*>(info->pNext);
    for (uint32_t i = 0; node && i < 256; ++i) {
        if (node->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO) {
            auto* candidate = const_cast<VkLayerDeviceCreateInfo*>(reinterpret_cast<const VkLayerDeviceCreateInfo*>(node));
            if (candidate->function == VK_LAYER_LINK_INFO) return candidate;
        }
        node = node->pNext;
    }
    return nullptr;
}

struct FeatureFlags {
    bool sawTimeline = false, timeline = false, sawSynchronization2 = false, synchronization2 = false;
};
FeatureFlags featureFlags(const VkDeviceCreateInfo* info) noexcept {
    FeatureFlags flags;
    auto* node = reinterpret_cast<const VkBaseInStructure*>(info->pNext);
    for (uint32_t i = 0; node && i < 256; ++i, node = node->pNext) {
        if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) {
            flags.sawTimeline = true; flags.timeline = reinterpret_cast<const VkPhysicalDeviceVulkan12Features*>(node)->timelineSemaphore != 0;
        } else if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES) {
            flags.sawTimeline = true; flags.timeline = reinterpret_cast<const VkPhysicalDeviceTimelineSemaphoreFeatures*>(node)->timelineSemaphore != 0;
        } else if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES) {
            flags.sawSynchronization2 = true; flags.synchronization2 = reinterpret_cast<const VkPhysicalDeviceVulkan13Features*>(node)->synchronization2 != 0;
        } else if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES) {
            flags.sawSynchronization2 = true; flags.synchronization2 = reinterpret_cast<const VkPhysicalDeviceSynchronization2Features*>(node)->synchronization2 != 0;
        }
    }
    return flags;
}
bool bridgeDeviceRequested() noexcept {
    wchar_t value[4]{};
    return GetEnvironmentVariableW(L"XESS_BRIDGE_DEVICE", value, 4) == 1 && value[0] == L'1';
}
bool computeObserverRequested() noexcept {
    if constexpr(!researchDiagnostics)return false;
    wchar_t value[4]{};
    return GetEnvironmentVariableW(L"XESS_COMPUTE_OBSERVER", value, 4) == 1 && value[0] == L'1';
}
bool rawCaptureRequested(bool fiMetadataRequested) noexcept {
    if constexpr(!researchDiagnostics)return false;
    wchar_t value[8]{};
    return fiMetadataRequested&&GetEnvironmentVariableW(L"XESS_RAWCOLOR_CAPTURE",value,8)==1&&value[0]==L'1';
}
bool upscaleOnlyRequested() noexcept {
    wchar_t upscale[8]{}, fi[8]{};
    return GetEnvironmentVariableW(L"XESS_FSR_UPSCALE_OBSERVER",upscale,8)==1 && upscale[0]==L'1'
        && !(GetEnvironmentVariableW(L"XESS_FSR_OBSERVER",fi,8)==1 && fi[0]==L'1');
}
bool upscaleObservationConflict() noexcept {
    wchar_t fi[8]{};
    if (!upscaleOnlyRequested() && !(GetEnvironmentVariableW(L"XESS_FSR_OBSERVER",fi,8)==1 && fi[0]==L'1')) return false;
    for (const auto* name : {L"XESS_BRIDGE_DEVICE",L"XESS_PRESENTATION_OFF",L"XESS_CONTINUOUS_INPUTS",
                             L"XESS_RAWCOLOR_CAPTURE",L"XESS_FSR_NATIVE_OFF"}) {
        wchar_t value[8]{};
        if (GetEnvironmentVariableW(name,value,8)==1 && value[0]==L'1') return true;
    }
    return false;
}
bool currentContinuousPresentationReady(InstanceState* parent) noexcept {
#ifdef XESS_FSR_DETOUR_AVAILABLE
    if (!parent || InterlockedCompareExchange(&parent->continuousSurfaceRefused,0,0) || !xess_vk::fsr_callback_install::currentContinuousRequested()) return false;
    AcquireSRWLockShared(&stateLock);
    bool ready=false;
    for (auto* state=devices;state;state=state->next) {
        if (state->parent==parent && state->presentationRequested && state->presentationRecord && state->continuous &&
            state->continuousTiming && state->packets && !InterlockedCompareExchange(&state->continuousClosing,0,0) &&
            xess_vk::fsr_callback_install::currentContinuousReady(bits(state->device))) {ready=true;break;}
    }
    ReleaseSRWLockShared(&stateLock);
    return ready;
#else
    (void)parent;return false;
#endif
}
const char* planStatusName(xess_vk::DeviceBridgePlanStatus status) noexcept {
    using Status = xess_vk::DeviceBridgePlanStatus;
    switch (status) {
    case Status::NotRequested: return "not_requested";
    case Status::Ready: return "ready";
    case Status::UnsupportedApi: return "unsupported_api";
    case Status::CapabilityQueryMissing: return "capability_query_missing";
    case Status::MissingRequiredExtension: return "missing_required_extension";
    case Status::TimelineUnsupported: return "timeline_unsupported";
    case Status::InvalidCreateInfo: return "invalid_create_info";
    case Status::UnsupportedPNext: return "unsupported_pnext";
    case Status::ConflictingFeatureChain: return "conflicting_feature_chain";
    case Status::BorrowedLoaderChainNotAllowed: return "borrowed_loader_chain_not_allowed";
    case Status::OutOfMemory: return "out_of_memory";
    }
    return "unknown";
}
struct BridgeSupportQuery {
    xess_vk::DeviceBridgeSupport support;
    std::vector<VkExtensionProperties> extensions;
    const char* status = "not_requested";
    VkResult extensionResult = VK_NOT_READY;
};
void queryBridgeSupport(InstanceState* parent, VkPhysicalDevice physical, BridgeSupportQuery& query) noexcept {
    query.support.instanceApiVersion = parent->requestedApiVersion;
    query.status = "query_function_missing";
    if (!parent->properties) return;
    VkPhysicalDeviceProperties properties{};
    parent->properties(physical, &properties);
    query.support.physicalDeviceApiVersion = properties.apiVersion;
    // The borrowed bridge itself requires 1.2. Do not query newer feature structs
    // on the intentionally retained 1.1 rejection test path.
    if (VK_API_VERSION_VARIANT(parent->requestedApiVersion) != 0
        || parent->requestedApiVersion < VK_API_VERSION_1_2 || properties.apiVersion < VK_API_VERSION_1_2) {
        query.status = "api_below_bridge_requirement"; return;
    }
    const auto enumerate = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
        parent->gipa(parent->instance, "vkEnumerateDeviceExtensionProperties"));
    const auto features2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
        parent->gipa(parent->instance, "vkGetPhysicalDeviceFeatures2"));
    if (!enumerate || !features2) return;
    try {
        // An incomplete enumeration is retried only as a capability query. There
        // is exactly one later vkCreateDevice call, regardless of its result.
        for (unsigned attempt = 0; attempt < 3; ++attempt) {
            uint32_t count = 0;
            query.extensionResult = enumerate(physical, nullptr, &count, nullptr);
            if (query.extensionResult != VK_SUCCESS) { query.status = "extension_count_query_failed"; return; }
            if (!count || count > 4096) { query.status = "extension_count_out_of_bounds"; return; }
            query.extensions.resize(count);
            const uint32_t capacity = count;
            query.extensionResult = enumerate(physical, nullptr, &count, query.extensions.data());
            if (query.extensionResult == VK_INCOMPLETE) continue;
            if (query.extensionResult != VK_SUCCESS || count > capacity) { query.status = "extension_list_query_failed"; return; }
            query.extensions.resize(count);
            query.support.extensions = query.extensions;
            query.support.extensionQueryComplete = true;
            break;
        }
        if (!query.support.extensionQueryComplete) { query.status = "extension_list_remained_incomplete"; return; }
        VkPhysicalDeviceTimelineSemaphoreFeatures timeline{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        features.pNext = &timeline;
        features2(physical, &features);
        query.support.timelineSemaphore = timeline.timelineSemaphore;
        query.support.timelineFeatureQueryComplete = true;
        query.status = "complete";
    } catch (...) {
        query.status = "capability_query_storage_failed";
        query.support.extensionQueryComplete = false;
        query.support.timelineFeatureQueryComplete = false;
    }
}
void logProperties(InstanceState* state, VkPhysicalDevice physical, const VkPhysicalDeviceProperties* props) noexcept {
    if (InterlockedIncrement64(&state->propertyEvents) > 16) return;
    JsonLine log("physical_device_properties");
    log.handle("instance", bits(state->instance)); log.handle("physical_device", bits(physical));
    log.text("device_name", props->deviceName); log.number("vendor_id", props->vendorID);
    log.number("device_id", props->deviceID); log.number("api_version", props->apiVersion);
    log.number("driver_version", props->driverVersion); log.number("device_type", props->deviceType); log.write();
}
void logSrDeviceCapabilities(InstanceState* parent,VkPhysicalDevice physical,const VkDeviceCreateInfo& original,
    const VkDeviceCreateInfo& effective,const BridgeSupportQuery& query,
    const xess_vk::sr_caps::Requested& originalRequested,const xess_vk::sr_caps::Requested& effectiveRequested,
    bool srPlanApplied=false,bool srQueriesAttempted=false) noexcept {
    const auto hasExtension=[](const VkDeviceCreateInfo& info,const char* name) {
        for(std::uint32_t n=0;n<info.enabledExtensionCount;++n)
            if(std::strcmp(info.ppEnabledExtensionNames[n],name)==0)return true;
        return false;
    };
    bool mutableSupported=false;
    for(const auto& extension:query.extensions)
        if(std::strcmp(extension.extensionName,VK_EXT_MUTABLE_DESCRIPTOR_TYPE_EXTENSION_NAME)==0)mutableSupported=true;
    const auto getFeatures=reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(parent->gipa(parent->instance,"vkGetPhysicalDeviceFeatures2"));
    const bool queryComplete=getFeatures&&query.support.extensionQueryComplete&&
        query.support.physicalDeviceApiVersion>=VK_API_VERSION_1_3;
    VkPhysicalDeviceFeatures2 core{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features v13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT mutableType{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT};
    if(queryComplete) {
        core.pNext=&v12;v12.pNext=&v13;
        if(mutableSupported)v13.pNext=&mutableType;
        getFeatures(physical,&core);
    }
    JsonLine log("sr_device_capabilities");
    const auto flag=[&](const char* name,bool known,bool value) {
        log.key(name);log.raw(known?(value?"true":"false"):"null");
    };
    log.handle("physical_device",bits(physical));log.number("instance_api_requested",parent->requestedApiVersion);
    log.boolean("original_feature_subset_parsed",originalRequested.complete);
    log.boolean("effective_feature_subset_parsed",effectiveRequested.complete);
    log.boolean("support_query_complete",queryComplete);
    log.boolean("mutable_extension_enumeration_complete",query.support.extensionQueryComplete);
    flag("mutable_extension_supported",query.support.extensionQueryComplete,mutableSupported);
    log.boolean("mutable_extension_original",hasExtension(original,VK_EXT_MUTABLE_DESCRIPTOR_TYPE_EXTENSION_NAME));
    log.boolean("mutable_extension_effective",hasExtension(effective,VK_EXT_MUTABLE_DESCRIPTOR_TYPE_EXTENSION_NAME));
    const auto fields=[&](const char* prefix,const xess_vk::sr_caps::Requested& requested) {
        char name[100]{};
        for(const auto& field:std::array<std::pair<const char*,bool>,5>{{
            {"storage_write_without_format",requested.storageWriteWithoutFormat},
            {"mutable_descriptor_type",requested.mutableDescriptorType},
            {"shader_int8",requested.shaderInt8},
            {"shader_integer_dot_product",requested.shaderIntegerDotProduct},
            {"scalar_block_layout",requested.scalarBlockLayout}}}) {
            std::snprintf(name,sizeof(name),"%s_%s",prefix,field.first);flag(name,requested.complete,field.second);
        }
    };
    fields("original",originalRequested);fields("effective",effectiveRequested);
    fields("supported",{queryComplete,core.features.shaderStorageImageWriteWithoutFormat==VK_TRUE,
        mutableType.mutableDescriptorType==VK_TRUE,v12.shaderInt8==VK_TRUE,v13.shaderIntegerDotProduct==VK_TRUE,v12.scalarBlockLayout==VK_TRUE});
    log.boolean("device_parameters_modified_by_sr",srPlanApplied);log.boolean("sdk_loaded_by_diagnostic",false);
    log.boolean("runtime_sdk_requirements_queried",srQueriesAttempted);
    log.boolean("execute_permitted",false);log.write();
}
} // namespace

OBS_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo* info,
    const VkAllocationCallbacks* allocator, VkInstance* instance) {
    if constexpr(researchDiagnostics) if (!srSdkDepth&&!upscaleOnlyRequested()) xess_vk::streamline_observer::tryInstall(streamlineLog);
    auto* chain = instanceLink(info);
    if (!chain || !chain->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;
    const auto gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    const auto gpdpa = chain->u.pLayerInfo->pfnNextGetPhysicalDeviceProcAddr;
    const auto create = reinterpret_cast<PFN_vkCreateInstance>(gipa(VK_NULL_HANDLE, "vkCreateInstance"));
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;
    // Reserve required dispatch bookkeeping before calling down; diagnostics never
    // convert a successful downstream creation into an error afterwards.
    auto* state = static_cast<InstanceState*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(InstanceState)));
    if (!state) return VK_ERROR_OUT_OF_HOST_MEMORY;
    const VkInstanceCreateInfo* effective=info;
    wchar_t srMode[2]{};
    state->srSdkPrivate=srSdkDepth!=0;
    if(!state->srSdkPrivate){
        acquireLogClient();state->logClient=true;
        if(fakeSwapchainRequested())(void)xess_vk::launcher_preferences::startupMultiplier();
    }
    if(!state->srSdkPrivate&&GetEnvironmentVariableW(L"XESS_SR_REPLACE",srMode,2)==1&&srMode[0]==L'1')try {
        // Required-extension queries create SDK probe instances through the loader.
        // Keep that nested graph out of SR admission and game presentation setup.
        SrSdkScope sdk;
        std::unique_ptr<SrInstanceCreation> sr;
#ifdef XESS_SR_CREATION_TEST
        if(srCreationForTest)sr.reset(srCreationForTest());
#endif
        if(!sr) {
        sr=std::make_unique<SrInstanceCreation>();
        HMODULE module{};wchar_t path[32768]{};
        if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&vkCreateInstance),&module))throw std::runtime_error("SR observer module path unavailable");
        const auto length=GetModuleFileNameW(module,path,32768);
        if(!length||length>=32768)throw std::runtime_error("SR observer module path truncated");
        sr->sdk=std::make_unique<xess_vk::sr::Sdk>(std::filesystem::path(path).parent_path(),
            "251659dd84a3e84de67c886a4186e01f3eca49b00641906fe38bb6b807e5d5b7");
        sr->queries={sr->sdk->xessVKGetRequiredInstanceExtensions,
            sr->sdk->xessVKGetRequiredDeviceExtensions,sr->sdk->xessVKGetRequiredDeviceFeatures};
        }
        effective=sr->creation.prepareInstance(true,sr->queries,*info);
        if(effective!=info&&!sr->creation.instanceSupported(
            reinterpret_cast<PFN_vkEnumerateInstanceVersion>(gipa(VK_NULL_HANDLE,"vkEnumerateInstanceVersion")),
            reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(gipa(VK_NULL_HANDLE,"vkEnumerateInstanceExtensionProperties")))) {
            effective=info;
            JsonLine srLog("sr_instance_fallback");srLog.text("reason","Required instance API/extensions unsupported or query unavailable");srLog.write();
        }
        JsonLine srLog("sr_instance_plan");srLog.boolean("applied",effective!=info);srLog.text("reason",sr->creation.reason());srLog.boolean("execute_permitted",false);srLog.write();
        if(effective!=info)state->srCreation=sr.release();
    }catch(const std::exception& error){effective=info;JsonLine srLog("sr_instance_fallback");srLog.text("reason",error.what());srLog.boolean("execute_permitted",false);srLog.write();}
    chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;
    const VkResult result = create(effective, allocator, instance);
    const auto originalError=GetLastError();
    if (result == VK_SUCCESS) {
        state->defaultAllocator=allocator==nullptr;
        state->applicationCallbacksAbsentKnown=rawInstanceCallbacksAbsent(info);
        state->instance = *instance; state->key = dispatchKey(*instance); state->gipa = gipa; state->gpdpa = gpdpa;
        state->requestedApiVersion = effective->pApplicationInfo && effective->pApplicationInfo->apiVersion
            ? effective->pApplicationInfo->apiVersion : VK_API_VERSION_1_0;
        if(state->srCreation)state->srCreation->creation.instanceCreated(*instance,result);
#define INSTANCE_PROC(field, name) state->field = reinterpret_cast<PFN_##name>(gipa(*instance, #name))
        INSTANCE_PROC(destroy, vkDestroyInstance);
        INSTANCE_PROC(enumerate, vkEnumeratePhysicalDevices);
        INSTANCE_PROC(groups, vkEnumeratePhysicalDeviceGroups);
        INSTANCE_PROC(groupsKhr, vkEnumeratePhysicalDeviceGroupsKHR);
        INSTANCE_PROC(createWin32Surface, vkCreateWin32SurfaceKHR);
        INSTANCE_PROC(destroySurface, vkDestroySurfaceKHR);
        INSTANCE_PROC(properties, vkGetPhysicalDeviceProperties);
        INSTANCE_PROC(properties2, vkGetPhysicalDeviceProperties2);
        INSTANCE_PROC(properties2Khr, vkGetPhysicalDeviceProperties2KHR);
        INSTANCE_PROC(surfaceCapabilities, vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
        INSTANCE_PROC(surfaceCapabilities2, vkGetPhysicalDeviceSurfaceCapabilities2KHR);
#undef INSTANCE_PROC
        AcquireSRWLockExclusive(&stateLock); state->next = instances; instances = state; ReleaseSRWLockExclusive(&stateLock);
    } else { delete state->srCreation;state->srCreation=nullptr; }
    JsonLine log("create_instance"); log.number("result", result);
    if (result == VK_SUCCESS) log.handle("instance", bits(*instance));
    log.boolean("sr_sdk_private",srSdkDepth!=0);
    log.boolean("research_diagnostics_compiled",researchDiagnostics);
    log.text("frame_spike_policy",researchDiagnostics?"per_spike_capped":"worst_three_per_60s");
    log.number("api_version_requested", info->pApplicationInfo ? info->pApplicationInfo->apiVersion : VK_API_VERSION_1_0);
    log.names("enabled_extensions", info->enabledExtensionCount, info->ppEnabledExtensionNames);
    log.names("enabled_layers", info->enabledLayerCount, info->ppEnabledLayerNames); log.write();
    if(result!=VK_SUCCESS){releaseLogClient(state);HeapFree(GetProcessHeap(),0,state);}
    SetLastError(originalError);return result;
}

OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks* allocator) {
    auto* state = findInstance(instance); if (!state) return;
    if (state->presentationQuarantined) {
        JsonLine log("presentation_off_destroy_instance_deferred"); log.boolean("original_forwarded", false); log.write();releaseLogClient(state);return;
    }
    if(InterlockedCompareExchange(&state->rawQuarantined,0,0)){const auto error=GetLastError();JsonLine log("rawcolor_probe_destroy_instance_deferred");log.handle("instance",bits(instance));log.boolean("original_forwarded",false);log.boolean("cleanup",false);log.write();releaseLogClient(state);SetLastError(error);return;}
    const auto destroy = state->destroy;
    destroy(instance, allocator);
    AcquireSRWLockExclusive(&stateLock);
    for (auto** link = &instances; *link; link = &(*link)->next) if (*link == state) { *link = state->next; break; }
    ReleaseSRWLockExclusive(&stateLock);
    JsonLine log("destroy_instance"); log.handle("instance", bits(instance)); log.write();
    delete state->srCreation;
    releaseLogClient(state);
    HeapFree(GetProcessHeap(), 0, state);
}

OBS_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumeratePhysicalDevices(VkInstance instance, uint32_t* count, VkPhysicalDevice* physical) {
    auto* state = findInstance(instance); if (!state || !state->enumerate) return VK_ERROR_INITIALIZATION_FAILED;
    const VkResult result = state->enumerate(instance, count, physical);
    JsonLine log("enumerate_physical_devices"); log.handle("instance", bits(instance)); log.number("result", result);
    if (result == VK_SUCCESS || result == VK_INCOMPLETE) log.number("count", *count);
    log.boolean("handles_returned", physical != nullptr); log.write();
    return result;
}

namespace {
VkResult enumerateGroups(VkInstance instance, uint32_t* count, VkPhysicalDeviceGroupProperties* groups, bool khr) {
    auto* state = findInstance(instance); if (!state) return VK_ERROR_INITIALIZATION_FAILED;
    const auto next = khr ? state->groupsKhr : state->groups;
    if (!next) return VK_ERROR_EXTENSION_NOT_PRESENT;
    const VkResult result = next(instance, count, groups);
    JsonLine log("enumerate_physical_device_groups"); log.handle("instance", bits(instance)); log.number("result", result);
    if (result == VK_SUCCESS || result == VK_INCOMPLETE) log.number("group_count", *count);
    log.boolean("khr_alias", khr); log.write(); return result;
}
}
OBS_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumeratePhysicalDeviceGroups(VkInstance instance, uint32_t* count, VkPhysicalDeviceGroupProperties* groups) {
    return enumerateGroups(instance, count, groups, false);
}
OBS_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumeratePhysicalDeviceGroupsKHR(VkInstance instance, uint32_t* count, VkPhysicalDeviceGroupProperties* groups) {
    return enumerateGroups(instance, count, groups, true);
}
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties(VkPhysicalDevice physical, VkPhysicalDeviceProperties* props) {
    auto* state = findInstance(physical); if (!state || !state->properties) return;
    state->properties(physical, props);if(srPhysicalWitness)*srPhysicalWitness=physical;logProperties(state, physical, props);
}
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties2(VkPhysicalDevice physical, VkPhysicalDeviceProperties2* props) {
    auto* state = findInstance(physical); if (!state || !state->properties2) return;
    state->properties2(physical, props); logProperties(state, physical, &props->properties);
}
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties2KHR(VkPhysicalDevice physical, VkPhysicalDeviceProperties2* props) {
    auto* state = findInstance(physical); if (!state || !state->properties2Khr) return;
    state->properties2Khr(physical, props); logProperties(state, physical, &props->properties);
}
namespace {
void synchronizePresentationExtent(InstanceState* state, VkSurfaceKHR surface) {
    auto* mapped = state->presentationSurface;
    if (!mapped || mapped->handle != surface || IsIconic(mapped->original())) return;
    RECT client{};
    if (GetClientRect(mapped->original(), &client) && client.right > 0 && client.bottom > 0)
        mapped->resize({static_cast<std::uint32_t>(client.right), static_cast<std::uint32_t>(client.bottom)});
}
}
OBS_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
    VkPhysicalDevice physical, VkSurfaceKHR surface, VkSurfaceCapabilitiesKHR* caps) {
    auto* state = findInstance(physical);
    if (!state || !state->surfaceCapabilities) return VK_ERROR_EXTENSION_NOT_PRESENT;
    // findInstance released stateLock before the bounded hidden UI message.
    synchronizePresentationExtent(state, surface);
    return state->surfaceCapabilities(physical, surface, caps);
}
OBS_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceCapabilities2KHR(
    VkPhysicalDevice physical, const VkPhysicalDeviceSurfaceInfo2KHR* info, VkSurfaceCapabilities2KHR* caps) {
    auto* state = findInstance(physical);
    if (!state || !state->surfaceCapabilities2) return VK_ERROR_EXTENSION_NOT_PRESENT;
    if (info) synchronizePresentationExtent(state, info->surface);
    return state->surfaceCapabilities2(physical, info, caps);
}

namespace {
#ifdef XESS_SR_CREATION_TEST
VkPhysicalDevice (*srPublicPhysicalForTest)(InstanceState*,VkPhysicalDevice){};
#endif
VkPhysicalDevice srPublicPhysical(InstanceState* parent,VkPhysicalDevice native) noexcept {
#ifdef XESS_SR_CREATION_TEST
    if(srPublicPhysicalForTest)return srPublicPhysicalForTest(parent,native);
#endif
    const auto loader=GetModuleHandleW(L"vulkan-1.dll");if(!loader)return VK_NULL_HANDLE;
    const auto enumerate=reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(GetProcAddress(loader,"vkEnumeratePhysicalDevices"));
    const auto properties=reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(GetProcAddress(loader,"vkGetPhysicalDeviceProperties"));
    if(!enumerate||!properties)return VK_NULL_HANDLE;
    std::uint32_t count{};if(enumerate(parent->instance,&count,nullptr)!=VK_SUCCESS||!count||count>64)return VK_NULL_HANDLE;
    std::array<VkPhysicalDevice,64> visible{};if(enumerate(parent->instance,&count,visible.data())!=VK_SUCCESS||count>visible.size())return VK_NULL_HANDLE;
    struct Witness {VkPhysicalDevice* saved=srPhysicalWitness;~Witness(){srPhysicalWitness=saved;}} witness;
    VkPhysicalDevice match{};
    for(unsigned n=0;n<count;++n){VkPhysicalDevice observed{};VkPhysicalDeviceProperties p{};srPhysicalWitness=&observed;
        properties(visible[n],&p);
        // The loader unwraps this exact public handle before our property hook.
        // Match the witnessed native handle, never a device name or ordinal.
        if(observed==native){if(match)return VK_NULL_HANDLE;match=visible[n];}
    }
    return match;
}
}
OBS_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo* info,
    const VkAllocationCallbacks* allocator, VkDevice* device) {
    auto* parent = findInstance(physical);
    auto* chain = deviceLink(info);
    if (!parent || !chain || !chain->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;
    const auto gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    const auto gdpa = chain->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    const auto create = reinterpret_cast<PFN_vkCreateDevice>(gipa(parent->instance, "vkCreateDevice"));
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;
    auto* state = static_cast<DeviceState*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DeviceState)));
    if (!state) return VK_ERROR_OUT_OF_HOST_MEMORY;
    if(parent->srSdkPrivate||srSdkDepth){
        chain->u.pLayerInfo=chain->u.pLayerInfo->pNext;
        const auto result=create(physical,info,allocator,device);const auto error=GetLastError();
        if(result==VK_SUCCESS){
            state->srSdkPrivate=true;state->device=*device;state->physical=physical;state->parent=parent;
            state->key=dispatchKey(*device);state->gdpa=gdpa;
            state->destroy=reinterpret_cast<PFN_vkDestroyDevice>(gdpa(*device,"vkDestroyDevice"));
            AcquireSRWLockExclusive(&stateLock);state->next=devices;devices=state;ReleaseSRWLockExclusive(&stateLock);
        }else HeapFree(GetProcessHeap(),0,state);
        SetLastError(error);return result;
    }
    const auto originalFeatures = featureFlags(info);
    const bool continuousRequested=xess_vk::continuous::requested();
    bool currentProfileReady=false;
#ifdef XESS_FSR_DETOUR_AVAILABLE
    if (continuousRequested && !InterlockedCompareExchange(&parent->continuousSurfaceRefused,0,0))
        currentProfileReady=xess_vk::fsr_callback_install::currentContinuousProfileReady();
#endif
    const bool upscaleConflict = (upscaleObservationConflict() || continuousRequested) && !currentProfileReady;
    const bool bridgeRequested = bridgeDeviceRequested();
    const bool timestampCalibrationRequested = bridgeRequested && !upscaleConflict &&
        xess_vk::presentation::enabled() && xess_vk::presentation::gpuTimestampsRequested();
    if(bridgeRequested&&!upscaleConflict&&xess_vk::presentation::enabled())initializeMeasurements(state);
    const bool computeRequested = computeObserverRequested();
    bool computeActive = false;
    BridgeSupportQuery query;
    xess_vk::DeviceBridgePlan plan;
    if (bridgeRequested && !upscaleConflict) {
        queryBridgeSupport(parent, physical, query);
        plan = xess_vk::planDeviceBridgeRequirements(*info, query.support,
            {true, xess_vk::AllowBorrowedLoaderChain::SynchronousCreateDeviceCallOnly, timestampCalibrationRequested});
    }
    // A refusal makes one unmodified-parameter downstream call, preserving the
    // application's behavior. A ready plan makes one call with owned parameters;
    // no second attempt is made if the downstream creation rejects them.
    const auto* effective = plan.ready() ? plan.createInfo() : info;
    xess_vk::sr_game::RuntimeRequirements srRequirements;
    xess_vk::sr_game::DeviceRequirementsPlan srPlan;
    if(parent->srCreation&&!upscaleConflict) {
        SrSdkScope sdk;
        state->srSdkPhysical=srPublicPhysical(parent,physical);
        if(state->srSdkPhysical)srRequirements=parent->srCreation->creation.deviceRequirements(state->srSdkPhysical);
        const auto loader=GetModuleHandleW(L"vulkan-1.dll");
        xess_vk::sr_game::DeviceSupportQueries support{
            loader?reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(GetProcAddress(loader,"vkEnumerateDeviceExtensionProperties")):nullptr,
            loader?reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(GetProcAddress(loader,"vkGetPhysicalDeviceFeatures2")):nullptr};
#ifdef XESS_SR_CREATION_TEST
        if(srPublicPhysicalForTest)support={
            reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(gipa(parent->instance,"vkEnumerateDeviceExtensionProperties")),
            reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(gipa(parent->instance,"vkGetPhysicalDeviceFeatures2"))};
#endif
        srPlan=xess_vk::sr_game::planDeviceRequirements(srRequirements,*effective,true,
            xess_vk::AllowBorrowedLoaderChain::SynchronousCreateDeviceCallOnly,support);
        JsonLine srLog("sr_device_plan");srLog.boolean("applied",srPlan.ready());srLog.text("reason",srPlan.reason());srLog.boolean("execute_permitted",false);srLog.write();
        if(srPlan.ready())effective=srPlan.createInfo();
    }
    const auto effectiveFeatures = featureFlags(effective);
    const bool srCapsRequested=!upscaleConflict&&xess_vk::diagnostic_flags::srObservation();
    const auto originalSrRequested=srCapsRequested?xess_vk::sr_caps::requested(*info):xess_vk::sr_caps::Requested{};
    const auto effectiveSrRequested=srCapsRequested?xess_vk::sr_caps::requested(*effective):xess_vk::sr_caps::Requested{};
    auto* effectiveChain = deviceLink(effective);
    const bool validChain = effectiveChain && effectiveChain->u.pLayerInfo;
    VkResult result = VK_ERROR_INITIALIZATION_FAILED;
    if (validChain) {
        // With a ready plan, only its copied loader LINK_INFO is advanced. All
        // borrowed union targets remain valid for this synchronous create call.
        effectiveChain->u.pLayerInfo = effectiveChain->u.pLayerInfo->pNext;
        result = create(physical, effective, allocator, device);
    }
    const DWORD originalCreateError=GetLastError();
    if (result == VK_SUCCESS) {
        state->device = *device; state->key = dispatchKey(*device); state->gdpa = gdpa;
        state->physical = physical;
        state->continuousImageProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceImageFormatProperties>(
            gipa(parent->instance, "vkGetPhysicalDeviceImageFormatProperties"));
        state->presentationRequested = !upscaleConflict && xess_vk::presentation::enabled();
        state->fakeEnabled = fakeSwapchainRequested() && state->presentationRequested &&
            rawDeviceQueueShape(effective) && !allocator && parent->defaultAllocator;
        if (state->presentationRequested) {
            const auto incarnation=InterlockedIncrement64(&packetDeviceIncarnations);
            if(incarnation>0) {
                state->deviceIncarnation=static_cast<std::uint64_t>(incarnation);
                try { state->packetIdentities=new packet::IdentityRegistry(state->deviceIncarnation); }
                catch(...) {} // Metadata refusal leaves native creation untouched.
            }
        }
        if(srPlan.ready()&&state->deviceIncarnation)try {
            state->srAdmission=new xess_vk::sr_game::RuntimeRequirementsAdmission(parent->srCreation->creation.admit(srRequirements,
                {parent->instance,state->srSdkPhysical,*device,state->deviceIncarnation},*effective,result));
            state->srRuntimeRequested=state->srAdmission->admitted();
            JsonLine srLog("sr_device_admission");srLog.boolean("admitted",state->srAdmission->admitted());
            srLog.text("reason",state->srAdmission->reason());srLog.boolean("execute_permitted",false);srLog.write();
        }catch(...){} // SR metadata allocation cannot turn successful device creation into failure.
        state->presentationSurface = parent->presentationSurface;
        if (state->presentationRequested) {
            try {
                if (allocator || !parent->defaultAllocator) throw std::invalid_argument("Presentation takeover requires default allocators");
                if (state->fakeEnabled) state->queueGate = new xess_vk::presentation::QueueGate;
                state->presentationRecord = new xess_vk::presentation::DeviceRecord(parent->instance, physical, *device, gipa, gdpa, parent->requestedApiVersion, *effective, state->fakeEnabled, state->queueGate,
                    state->fakeEnabled ? queueCallSlow : nullptr, state);
            } catch (const std::exception& error) {
                JsonLine log("presentation_off_device_rejected"); log.text("error", error.what()); log.write();
            }
        }
        state->parent=parent;state->rawDefaultAllocators=allocator==nullptr&&parent->defaultAllocator;
        state->rawUniqueQueue=rawDeviceQueueShape(effective);state->rawCallbackAbsent=rawMemoryCallbackAbsent(effective);
#define DEVICE_PROC(field, name) state->field = reinterpret_cast<PFN_##name>(gdpa(*device, #name))
        DEVICE_PROC(destroy, vkDestroyDevice);
        DEVICE_PROC(createSwapchain, vkCreateSwapchainKHR);
        DEVICE_PROC(destroySwapchain, vkDestroySwapchainKHR);
        DEVICE_PROC(acquireNextImage, vkAcquireNextImageKHR);
        DEVICE_PROC(acquireNextImage2, vkAcquireNextImage2KHR);
        DEVICE_PROC(getSwapchainStatus, vkGetSwapchainStatusKHR);
        DEVICE_PROC(present, vkQueuePresentKHR);
        DEVICE_PROC(createSemaphore, vkCreateSemaphore); DEVICE_PROC(destroySemaphore, vkDestroySemaphore);
        DEVICE_PROC(createImage, vkCreateImage);
        DEVICE_PROC(destroyImage, vkDestroyImage);
        DEVICE_PROC(createBuffer, vkCreateBuffer); DEVICE_PROC(destroyBuffer, vkDestroyBuffer);
        DEVICE_PROC(allocateMemory, vkAllocateMemory); DEVICE_PROC(freeMemory, vkFreeMemory);
        DEVICE_PROC(imageMemoryRequirements, vkGetImageMemoryRequirements); DEVICE_PROC(bufferMemoryRequirements, vkGetBufferMemoryRequirements);
        DEVICE_PROC(bindImageMemory, vkBindImageMemory); DEVICE_PROC(bindImageMemory2, vkBindImageMemory2); DEVICE_PROC(bindImageMemory2Khr, vkBindImageMemory2KHR);
        DEVICE_PROC(bindBufferMemory, vkBindBufferMemory); DEVICE_PROC(bindBufferMemory2, vkBindBufferMemory2); DEVICE_PROC(bindBufferMemory2Khr, vkBindBufferMemory2KHR);
        DEVICE_PROC(bindSparse, vkQueueBindSparse);
        DEVICE_PROC(createFence,vkCreateFence);DEVICE_PROC(destroyFence,vkDestroyFence);DEVICE_PROC(fenceStatus,vkGetFenceStatus);DEVICE_PROC(waitFences,vkWaitForFences);
        DEVICE_PROC(mapMemory,vkMapMemory);DEVICE_PROC(unmapMemory,vkUnmapMemory);DEVICE_PROC(invalidateMemory,vkInvalidateMappedMemoryRanges);
        DEVICE_PROC(cmdCopyImage,vkCmdCopyImage);DEVICE_PROC(cmdCopyImageToBuffer,vkCmdCopyImageToBuffer);
        if(renderTailRequested()||srTailHooksRequested()) {
            DEVICE_PROC(cmdCopyImage2,vkCmdCopyImage2);DEVICE_PROC(cmdCopyImage2Khr,vkCmdCopyImage2KHR);
            DEVICE_PROC(cmdBlitImage,vkCmdBlitImage);DEVICE_PROC(cmdBlitImage2,vkCmdBlitImage2);DEVICE_PROC(cmdBlitImage2Khr,vkCmdBlitImage2KHR);
            DEVICE_PROC(cmdClearColorImage,vkCmdClearColorImage);DEVICE_PROC(cmdClearDepthStencilImage,vkCmdClearDepthStencilImage);
            DEVICE_PROC(cmdResolveImage,vkCmdResolveImage);DEVICE_PROC(cmdResolveImage2,vkCmdResolveImage2);DEVICE_PROC(cmdResolveImage2Khr,vkCmdResolveImage2KHR);
            DEVICE_PROC(cmdCopyBufferToImage2,vkCmdCopyBufferToImage2);DEVICE_PROC(cmdCopyBufferToImage2Khr,vkCmdCopyBufferToImage2KHR);
            DEVICE_PROC(cmdCopyImageToBuffer2,vkCmdCopyImageToBuffer2);DEVICE_PROC(cmdCopyImageToBuffer2Khr,vkCmdCopyImageToBuffer2KHR);
            DEVICE_PROC(cmdDraw,vkCmdDraw);DEVICE_PROC(cmdDrawIndexed,vkCmdDrawIndexed);
            DEVICE_PROC(cmdDrawIndirect,vkCmdDrawIndirect);DEVICE_PROC(cmdDrawIndexedIndirect,vkCmdDrawIndexedIndirect);
        }
        DEVICE_PROC(queueWaitIdle,vkQueueWaitIdle);DEVICE_PROC(deviceWaitIdle,vkDeviceWaitIdle);
        DEVICE_PROC(createImageView, vkCreateImageView);
        DEVICE_PROC(destroyImageView, vkDestroyImageView);
        DEVICE_PROC(createDescriptorSetLayout, vkCreateDescriptorSetLayout);
        DEVICE_PROC(destroyDescriptorSetLayout, vkDestroyDescriptorSetLayout);
        DEVICE_PROC(createPipelineLayout, vkCreatePipelineLayout);
        DEVICE_PROC(destroyPipelineLayout, vkDestroyPipelineLayout);
        DEVICE_PROC(createDescriptorPool, vkCreateDescriptorPool);
        DEVICE_PROC(resetDescriptorPool, vkResetDescriptorPool);
        DEVICE_PROC(destroyDescriptorPool, vkDestroyDescriptorPool);
        DEVICE_PROC(allocateDescriptorSets, vkAllocateDescriptorSets);
        DEVICE_PROC(freeDescriptorSets, vkFreeDescriptorSets);
        DEVICE_PROC(updateDescriptorSets, vkUpdateDescriptorSets);
        DEVICE_PROC(cmdBindDescriptorSets, vkCmdBindDescriptorSets);
        DEVICE_PROC(createDescriptorUpdateTemplate, vkCreateDescriptorUpdateTemplate);
        DEVICE_PROC(destroyDescriptorUpdateTemplate, vkDestroyDescriptorUpdateTemplate);
        DEVICE_PROC(createDescriptorUpdateTemplateKhr, vkCreateDescriptorUpdateTemplateKHR);
        DEVICE_PROC(destroyDescriptorUpdateTemplateKhr, vkDestroyDescriptorUpdateTemplateKHR);
        DEVICE_PROC(updateDescriptorSetWithTemplate, vkUpdateDescriptorSetWithTemplate);
        DEVICE_PROC(updateDescriptorSetWithTemplateKhr, vkUpdateDescriptorSetWithTemplateKHR);
        DEVICE_PROC(cmdPushDescriptorSet, vkCmdPushDescriptorSet);
        DEVICE_PROC(cmdPushDescriptorSetWithTemplate, vkCmdPushDescriptorSetWithTemplate);
        DEVICE_PROC(cmdBindDescriptorSets2, vkCmdBindDescriptorSets2);
        DEVICE_PROC(cmdPushDescriptorSet2, vkCmdPushDescriptorSet2);
        DEVICE_PROC(cmdPushDescriptorSetWithTemplate2, vkCmdPushDescriptorSetWithTemplate2);
        DEVICE_PROC(cmdPushDescriptorSetKHR, vkCmdPushDescriptorSetKHR);
        DEVICE_PROC(cmdPushDescriptorSetWithTemplateKHR, vkCmdPushDescriptorSetWithTemplateKHR);
        DEVICE_PROC(cmdBindDescriptorSets2KHR, vkCmdBindDescriptorSets2KHR);
        DEVICE_PROC(cmdPushDescriptorSet2KHR, vkCmdPushDescriptorSet2KHR);
        DEVICE_PROC(cmdPushDescriptorSetWithTemplate2KHR, vkCmdPushDescriptorSetWithTemplate2KHR);
        DEVICE_PROC(createRenderPass, vkCreateRenderPass);
        DEVICE_PROC(createRenderPass2, vkCreateRenderPass2);
        DEVICE_PROC(createRenderPass2Khr, vkCreateRenderPass2KHR);
        DEVICE_PROC(destroyRenderPass, vkDestroyRenderPass);
        DEVICE_PROC(createFramebuffer, vkCreateFramebuffer);
        DEVICE_PROC(destroyFramebuffer, vkDestroyFramebuffer);
        DEVICE_PROC(swapchainImages, vkGetSwapchainImagesKHR);
        DEVICE_PROC(getQueue, vkGetDeviceQueue);
        DEVICE_PROC(getQueue2, vkGetDeviceQueue2);
        DEVICE_PROC(submit, vkQueueSubmit);
        DEVICE_PROC(submit2, vkQueueSubmit2);
        DEVICE_PROC(submit2Khr, vkQueueSubmit2KHR);
        DEVICE_PROC(createShaderModule, vkCreateShaderModule);
        DEVICE_PROC(destroyShaderModule, vkDestroyShaderModule);
        DEVICE_PROC(createComputePipelines, vkCreateComputePipelines);
        DEVICE_PROC(destroyPipeline, vkDestroyPipeline);
        DEVICE_PROC(allocateCommandBuffers, vkAllocateCommandBuffers);
        DEVICE_PROC(createCommandPool, vkCreateCommandPool);
        DEVICE_PROC(freeCommandBuffers, vkFreeCommandBuffers);
        DEVICE_PROC(beginCommandBuffer, vkBeginCommandBuffer);
        DEVICE_PROC(endCommandBuffer, vkEndCommandBuffer);
        DEVICE_PROC(resetCommandBuffer, vkResetCommandBuffer);
        DEVICE_PROC(resetCommandPool, vkResetCommandPool);
        DEVICE_PROC(destroyCommandPool, vkDestroyCommandPool);
        DEVICE_PROC(cmdPipelineBarrier, vkCmdPipelineBarrier);
        DEVICE_PROC(cmdPipelineBarrier2, vkCmdPipelineBarrier2);
        DEVICE_PROC(cmdPipelineBarrier2Khr, vkCmdPipelineBarrier2KHR);
        DEVICE_PROC(cmdWaitEvents, vkCmdWaitEvents);
        DEVICE_PROC(cmdWaitEvents2, vkCmdWaitEvents2);
        DEVICE_PROC(cmdWaitEvents2Khr, vkCmdWaitEvents2KHR);
        DEVICE_PROC(cmdSetEvent2, vkCmdSetEvent2);
        DEVICE_PROC(cmdSetEvent2Khr, vkCmdSetEvent2KHR);
        DEVICE_PROC(cmdBeginRenderPass, vkCmdBeginRenderPass);
        DEVICE_PROC(cmdBeginRenderPass2, vkCmdBeginRenderPass2);
        DEVICE_PROC(cmdBeginRenderPass2Khr, vkCmdBeginRenderPass2KHR);
        DEVICE_PROC(cmdNextSubpass, vkCmdNextSubpass);
        DEVICE_PROC(cmdNextSubpass2, vkCmdNextSubpass2);
        DEVICE_PROC(cmdNextSubpass2Khr, vkCmdNextSubpass2KHR);
        DEVICE_PROC(cmdEndRenderPass, vkCmdEndRenderPass);
        DEVICE_PROC(cmdEndRenderPass2, vkCmdEndRenderPass2);
        DEVICE_PROC(cmdEndRenderPass2Khr, vkCmdEndRenderPass2KHR);
        DEVICE_PROC(cmdBeginRendering, vkCmdBeginRendering);
        DEVICE_PROC(cmdBeginRenderingKhr, vkCmdBeginRenderingKHR);
        DEVICE_PROC(cmdEndRendering, vkCmdEndRendering);
        DEVICE_PROC(cmdEndRenderingKhr, vkCmdEndRenderingKHR);
        DEVICE_PROC(cmdExecuteCommands, vkCmdExecuteCommands);
        DEVICE_PROC(cmdBindPipeline, vkCmdBindPipeline);
        DEVICE_PROC(cmdDispatch, vkCmdDispatch);
        DEVICE_PROC(cmdDispatchIndirect, vkCmdDispatchIndirect);
        DEVICE_PROC(cmdDispatchBase, vkCmdDispatchBase);
        DEVICE_PROC(cmdDispatchBaseKhr, vkCmdDispatchBaseKHR);
        if(srTailHooksRequested()) {
        DEVICE_PROC(cmdPushConstants,vkCmdPushConstants);
        DEVICE_PROC(cmdPushConstants2,vkCmdPushConstants2);
        DEVICE_PROC(cmdPushConstants2Khr,vkCmdPushConstants2KHR);
        DEVICE_PROC(cmdSetViewport,vkCmdSetViewport);
        DEVICE_PROC(cmdSetScissor,vkCmdSetScissor);
        DEVICE_PROC(cmdSetViewportWithCount,vkCmdSetViewportWithCount);
        DEVICE_PROC(cmdSetScissorWithCount,vkCmdSetScissorWithCount);
        DEVICE_PROC(cmdSetViewportWithCountExt,vkCmdSetViewportWithCountEXT);
        DEVICE_PROC(cmdSetScissorWithCountExt,vkCmdSetScissorWithCountEXT);
        DEVICE_PROC(cmdBindVertexBuffers,vkCmdBindVertexBuffers);
        DEVICE_PROC(cmdBindVertexBuffers2,vkCmdBindVertexBuffers2);
        DEVICE_PROC(cmdBindVertexBuffers2Ext,vkCmdBindVertexBuffers2EXT);
        DEVICE_PROC(cmdBindIndexBuffer,vkCmdBindIndexBuffer);
        DEVICE_PROC(cmdBindIndexBuffer2,vkCmdBindIndexBuffer2);
        DEVICE_PROC(cmdBindIndexBuffer2Khr,vkCmdBindIndexBuffer2KHR);
        DEVICE_PROC(cmdDrawIndirectCount,vkCmdDrawIndirectCount);
        DEVICE_PROC(cmdDrawIndirectCountKhr,vkCmdDrawIndirectCountKHR);
        DEVICE_PROC(cmdDrawIndexedIndirectCount,vkCmdDrawIndexedIndirectCount);
        DEVICE_PROC(cmdDrawIndexedIndirectCountKhr,vkCmdDrawIndexedIndirectCountKHR);
        DEVICE_PROC(cmdSetLineWidth,vkCmdSetLineWidth);
        DEVICE_PROC(cmdSetDepthBias,vkCmdSetDepthBias);
        DEVICE_PROC(cmdSetDepthBounds,vkCmdSetDepthBounds);
        DEVICE_PROC(cmdSetStencilCompareMask,vkCmdSetStencilCompareMask);
        DEVICE_PROC(cmdSetStencilWriteMask,vkCmdSetStencilWriteMask);
        DEVICE_PROC(cmdSetStencilReference,vkCmdSetStencilReference);
        DEVICE_PROC(cmdSetEvent,vkCmdSetEvent);
        DEVICE_PROC(cmdResetEvent,vkCmdResetEvent);
        DEVICE_PROC(cmdResetEvent2,vkCmdResetEvent2);
        DEVICE_PROC(cmdResetEvent2Khr,vkCmdResetEvent2KHR);
        DEVICE_PROC(cmdFillBuffer,vkCmdFillBuffer);
        DEVICE_PROC(cmdSetBlendConstants,vkCmdSetBlendConstants);
        DEVICE_PROC(cmdCopyBuffer,vkCmdCopyBuffer);
        DEVICE_PROC(cmdCopyBufferToImage,vkCmdCopyBufferToImage);
        }
#undef DEVICE_PROC
        if(wsiAcquireRequested()) {
            try{state->wsiAcquire=new xess_vk::wsi_acquire::Observer;}catch(...){}
            JsonLine log("wsi_acquire_observer_enabled");log.handle("device",bits(*device));
            log.boolean("allocated",state->wsiAcquire!=nullptr);
            log.boolean("acquire_available",state->acquireNextImage!=nullptr);log.boolean("acquire2_available",state->acquireNextImage2!=nullptr);
            log.boolean("get_status_available",state->getSwapchainStatus!=nullptr);
            log.number("sample_limit_per_api",xess_vk::wsi_acquire::sampleLimitPerApi);
            log.number("summary_interval_calls",xess_vk::wsi_acquire::summaryInterval);
            log.number("periodic_summary_limit",xess_vk::wsi_acquire::periodicSummaryLimit);log.write();
        }
        if constexpr(researchDiagnostics) if (computeRequested && !upscaleConflict) state->compute = xess_vk::compute_observer::create(*device, streamlineLog);
        wchar_t fsrOption[8]{}, upscaleOption[8]{};
        const bool fiMetadataRequested=GetEnvironmentVariableW(L"XESS_FSR_OBSERVER",fsrOption,8)==1 && fsrOption[0]==L'1';
        const bool upscaleMetadataRequested=GetEnvironmentVariableW(L"XESS_FSR_UPSCALE_OBSERVER",upscaleOption,8)==1 && upscaleOption[0]==L'1';
        // Candidate-sync and raw capture are research-only. Keep memory-alias
        // facts and the shader archive: SR replacement consumes both.
        const bool productRoute = upscaleMetadataRequested && !fiMetadataRequested && xess_vk::continuous::requested();
        state->researchDiagnostics = researchDiagnostics&&!productRoute;
        state->srObservationRequested=productRoute&&(xess_vk::diagnostic_flags::srObservation()||xess_vk::diagnostic_flags::srFullGraphObservation()||state->srRuntimeRequested);
        state->srTailDiagnostics=state->srObservationRequested&&(!state->srRuntimeRequested||xess_vk::diagnostic_flags::srObservation()||xess_vk::diagnostic_flags::srFullGraphObservation());
        state->srGpuRequested=state->srObservationRequested&&xess_vk::presentation::gpuTimestampsRequested();
        if (!upscaleConflict && (fiMetadataRequested || upscaleMetadataRequested)) {
            try { state->imageInventory = new xess_vk::image_inventory::Inventory<>; } catch (...) {}
            if (state->imageInventory) {
                try { state->wsiInventory = new xess_vk::wsi_image_inventory::Inventory(*state->imageInventory); } catch (...) {}
            }
            if (!productRoute)
                if constexpr(researchDiagnostics) state->candidateSync = xess_vk::candidate_sync::create(*device, effectiveFeatures.synchronization2, streamlineLog);
            state->renderPass = xess_vk::render_pass_observer::create(*device, streamlineLog,{recordedLayoutInventory,state});
            auto descriptorConfig=descriptorDispatchConfig(productRoute,renderTailRequested(),
                state->srRuntimeRequested&&!xess_vk::diagnostic_flags::srFullGraphObservation()&&xess_vk::diagnostic_flags::fsrPipelineTracking());
            descriptorConfig.measureReferenceConsumption=trackingCpuTimingEnabled();
            state->descriptors = xess_vk::descriptor_image_observer::create(*device, streamlineLog,descriptorConfig);
            if(productRoute){JsonLine selection("tracking_pipeline_selection");selection.handle("device",bits(*device));
                selection.boolean("requested",xess_vk::diagnostic_flags::fsrPipelineTracking());
                selection.boolean("compact_ordinary_dispatch",state->descriptors&&descriptorConfig.compactOrdinaryDispatch);
                selection.boolean("global_descriptor_mutations_retained",true);selection.text("unknown_pipeline_policy","full_typed_evidence");selection.write();}
            if(state->srObservationRequested&&state->descriptors)
                state->srShaders=xess_vk::compute_observer::createShaderArchive(*device,streamlineLog,(xess_vk::diagnostic_flags::srFullGraphObservation()||xess_vk::diagnostic_flags::srNativeReplacementAvailable()));
            if constexpr(researchDiagnostics) if(renderTailRequested()) {
                try {state->renderTail=new xess_vk::render_tail::Store(bits(*device),streamlineLog);}catch(...){}
                if constexpr(researchDiagnostics) if(state->renderTail&&!renderTailArmPath(state)){delete state->renderTail;state->renderTail=nullptr;}
            }
            // Keep early writes and companion sets: a candidate pipeline validates every bound set.
            const auto memoryProperties=reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(gipa(parent->instance,"vkGetPhysicalDeviceMemoryProperties"));
            const auto physicalProperties=reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(gipa(parent->instance,"vkGetPhysicalDeviceProperties"));
            if(memoryProperties)memoryProperties(physical,&state->physicalMemory);
            VkPhysicalDeviceProperties actualProperties{};if(physicalProperties)physicalProperties(physical,&actualProperties);
            if(state->srGpuRequested){
                state->srGpuPeriod=actualProperties.limits.timestampPeriod;
                const auto families=reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(gipa(parent->instance,"vkGetPhysicalDeviceQueueFamilyProperties"));
                std::uint32_t total{};if(families)families(physical,&total,nullptr);state->srGpuFamilyReported=total;
                if(total&&total<=std::size(state->srGpuFamilies)){families(physical,&total,state->srGpuFamilies);
                    if(total<=std::size(state->srGpuFamilies))state->srGpuFamilyCount=total;}
                state->srGpuFunctions.create=reinterpret_cast<PFN_vkCreateQueryPool>(gdpa(*device,"vkCreateQueryPool"));
                state->srGpuFunctions.destroy=reinterpret_cast<PFN_vkDestroyQueryPool>(gdpa(*device,"vkDestroyQueryPool"));
                state->srGpuFunctions.reset=reinterpret_cast<PFN_vkCmdResetQueryPool>(gdpa(*device,"vkCmdResetQueryPool"));
                state->srGpuFunctions.write=reinterpret_cast<PFN_vkCmdWriteTimestamp>(gdpa(*device,"vkCmdWriteTimestamp"));
                state->srGpuFunctions.read=reinterpret_cast<PFN_vkGetQueryPoolResults>(gdpa(*device,"vkGetQueryPoolResults"));
            }
            if(state->srRuntimeRequested){
            state->srQueueFamily=VK_QUEUE_FAMILY_IGNORED;std::uint32_t count{};
            const auto query=reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(gipa(parent->instance,"vkGetPhysicalDeviceQueueFamilyProperties"));
            std::array<VkQueueFamilyProperties,64> families{};if(query)query(physical,&count,nullptr);
            if(count&&count<=families.size()){
                query(physical,&count,families.data());unsigned graphicsQueues{};
                for(std::uint32_t n=0;n<effective->queueCreateInfoCount;++n){const auto& q=effective->pQueueCreateInfos[n];
                    if(q.queueFamilyIndex<count&&(families[q.queueFamilyIndex].queueFlags&(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT))==(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT)){
                        graphicsQueues+=q.queueCount;state->srQueueFamily=q.queueFamilyIndex;
                    }
                }
                state->srSingleQueue=graphicsQueues==1;
            }
            auto* feature=reinterpret_cast<const VkBaseInStructure*>(effective->pNext);
            for(unsigned n=0;feature&&n<64;++n,feature=feature->pNext){
                if(feature->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES)
                    state->srSeparateLayouts=reinterpret_cast<const VkPhysicalDeviceVulkan12Features*>(feature)->separateDepthStencilLayouts!=0;
                if(feature->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SEPARATE_DEPTH_STENCIL_LAYOUTS_FEATURES)
                    state->srSeparateLayouts=reinterpret_cast<const VkPhysicalDeviceSeparateDepthStencilLayoutsFeatures*>(feature)->separateDepthStencilLayouts!=0;
            }
        }
        state->actualBufferImageGranularity=actualProperties.limits.bufferImageGranularity;
            state->nonCoherentAtomSize=actualProperties.limits.nonCoherentAtomSize;
            state->actualDeviceScope=xess_vk::resource_memory_observer::deviceScope(effective);
            xess_vk::resource_memory_facts::Config memoryConfig;memoryConfig.deviceScope=state->actualDeviceScope;
            memoryConfig.bufferImageGranularity=state->actualBufferImageGranularity;
            memoryConfig.bindingCoverage=xess_vk::resource_memory_facts::BindingCoverage::CompleteOrdinaryCalls;
            if(!productRoute||state->renderTail||state->srObservationRequested)state->resourceMemory=xess_vk::resource_memory_observer::create(*device,state->physicalMemory,memoryConfig,streamlineLog);
            const bool rawRequested=rawCaptureRequested(fiMetadataRequested);
            if(rawRequested){
                const auto families=reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(gipa(parent->instance,"vkGetPhysicalDeviceQueueFamilyProperties"));
                std::array<VkQueueFamilyProperties,64> queues{};std::uint32_t total{};
                if(families){families(physical,&total,nullptr);if(total&&total<=queues.size()){families(physical,&total,queues.data());if(total&&total<=queues.size())state->rawQueueFlags=queues[0].queueFlags;}}
                state->rawUniqueQueue=state->rawUniqueQueue&&total&&total<=queues.size()&&queues[0].queueCount>=1&&(state->rawQueueFlags&VK_QUEUE_TRANSFER_BIT);
                const auto formatProperties=reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties>(gipa(parent->instance,"vkGetPhysicalDeviceFormatProperties"));
                const auto imageProperties=reinterpret_cast<PFN_vkGetPhysicalDeviceImageFormatProperties>(gipa(parent->instance,"vkGetPhysicalDeviceImageFormatProperties"));
                VkFormatProperties capabilities{};if(formatProperties)formatProperties(physical,VK_FORMAT_B10G11R11_UFLOAT_PACK32,&capabilities);
                constexpr auto transfer=VK_FORMAT_FEATURE_TRANSFER_SRC_BIT|VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
                const auto formatResult=imageProperties?imageProperties(physical,VK_FORMAT_B10G11R11_UFLOAT_PACK32,VK_IMAGE_TYPE_2D,VK_IMAGE_TILING_OPTIMAL,VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT,0,&state->rawFormatProperties):VK_ERROR_FORMAT_NOT_SUPPORTED;
                state->rawFormatSupported=formatProperties&&(capabilities.optimalTilingFeatures&transfer)==transfer&&formatResult==VK_SUCCESS;
                state->rawEnabled=!state->fakeEnabled&&state->actualDeviceScope==xess_vk::resource_memory_facts::DeviceScope::SinglePhysicalDevice&&state->rawUniqueQueue&&state->rawDefaultAllocators&&state->rawCallbackAbsent&&parent->applicationCallbacksAbsentKnown&&state->rawFormatSupported
                    &&!InterlockedCompareExchange(&rawProcessLease,0,0)&&rawArmPaths(state);
                JsonLine rawLog("rawcolor_probe_device_admission");rawLog.handle("device",bits(*device));rawLog.boolean("enabled",state->rawEnabled);rawLog.boolean("total_device_queue_count_one",state->rawUniqueQueue);rawLog.boolean("default_allocators",state->rawDefaultAllocators);
                rawLog.boolean("memory_report_callback_absent",state->rawCallbackAbsent);rawLog.boolean("instance_callbacks_absent",parent->applicationCallbacksAbsentKnown);rawLog.boolean("format_transfer_supported",state->rawFormatSupported);rawLog.number("format_image_query_result",formatResult);rawLog.boolean("fake_swapchain_conflict",state->fakeEnabled);rawLog.write();
            }
            // These extensions provide binding/dispatch paths outside the ordinary
            // descriptor-set subset. Do not retain seemingly valid stale bindings.
            for (uint32_t i=0; i<effective->enabledExtensionCount; ++i) {
                const auto* extension=effective->ppEnabledExtensionNames[i];
                if (std::strcmp(extension, "VK_EXT_descriptor_buffer")==0 ||
                    std::strcmp(extension, "VK_EXT_descriptor_heap")==0 ||
                    std::strcmp(extension, "VK_EXT_shader_object")==0 ||
                    std::strcmp(extension, "VK_NV_device_generated_commands")==0 ||
                    std::strcmp(extension, "VK_NV_device_generated_commands_compute")==0 ||
                    std::strcmp(extension, "VK_EXT_device_generated_commands")==0)
                    xess_vk::descriptor_image_observer::retire(state->descriptors);
            }
        }
        if (!upscaleConflict && xess_vk::continuous::requested()) {
            try {
                if (!state->presentationRecord || !state->descriptors || !state->renderPass || !state->imageInventory)
                    throw std::invalid_argument("Continuous inputs require admitted presentation and command/descriptor observers");
                VkQueue queue{};
                state->getQueue(*device, state->presentationRecord->family(), 0, &queue);
                bool separate{};
                for (const auto* feature = reinterpret_cast<const VkBaseInStructure*>(effective->pNext);
                     feature; feature = feature->pNext) {
                    if (feature->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES)
                        separate = reinterpret_cast<const VkPhysicalDeviceVulkan12Features*>(feature)->separateDepthStencilLayouts;
                    if (feature->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SEPARATE_DEPTH_STENCIL_LAYOUTS_FEATURES)
                        separate = reinterpret_cast<const VkPhysicalDeviceSeparateDepthStencilLayoutsFeatures*>(feature)->separateDepthStencilLayouts;
                }
                xess_vk::continuous::Config providerConfig{*device,queue,state->presentationRecord->family(),state->physicalMemory,gdpa,separate};
                providerConfig.depthCompute=xess_vk::diagnostic_flags::depthComputePath();
                if(timestampCalibrationRequested){
                    VkPhysicalDeviceProperties properties{};reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(gipa(parent->instance,"vkGetPhysicalDeviceProperties"))(physical,&properties);
                    const auto queueProperties=reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(gipa(parent->instance,"vkGetPhysicalDeviceQueueFamilyProperties"));
                    std::uint32_t count{};queueProperties(physical,&count,nullptr);std::vector<VkQueueFamilyProperties> families(count);queueProperties(physical,&count,families.data());
                    if(providerConfig.queueFamily<families.size()){providerConfig.gpuTimestamps=true;providerConfig.timestampPeriodNs=properties.limits.timestampPeriod;providerConfig.timestampValidBits=families[providerConfig.queueFamily].timestampValidBits;}
                }
                state->continuous = new xess_vk::continuous::Provider(providerConfig);
                state->continuousQueue = queue;
                using Timing = xess_vk::continuous_timing::Callbacks;
                Timing callbacks;
                callbacks.user = state;
                callbacks.contextGeneration = [](void* user) -> std::uint64_t {
                    const auto* device = static_cast<DeviceState*>(user);
                    return device->presentation ? device->presentation->timingContextGeneration() : 0;
                };
                callbacks.begin = [](void* user, std::uint32_t id) {
                    auto* device = static_cast<DeviceState*>(user);
                    return continuousSdkCall([&] {
                        return device->presentation ? device->presentation->beginGameFrame(id) : xess_vk::FrameTimingTicket{};
                    });
                };
                callbacks.simulationEnd = [](void* user, xess_vk::FrameTimingTicket ticket) {
                    continuousSdkCall([&] { static_cast<DeviceState*>(user)->presentation->simulationEnd(ticket); });
                };
                callbacks.renderBegin = [](void* user, xess_vk::FrameTimingTicket ticket) {
                    continuousSdkCall([&] { static_cast<DeviceState*>(user)->presentation->renderSubmitBegin(ticket); });
                };
                callbacks.renderEnd = [](void* user, xess_vk::FrameTimingTicket ticket) {
                    continuousSdkCall([&] { static_cast<DeviceState*>(user)->presentation->renderSubmitEnd(ticket); });
                };
                callbacks.nextSdkId = [](void* user) -> std::uint32_t {
                    const auto* device = static_cast<DeviceState*>(user);
                    return device->presentation ? device->presentation->nextSdkId() : 0;
                };
                callbacks.abandon = [](void* user, xess_vk::FrameTimingTicket ticket) {
                    auto* device = static_cast<DeviceState*>(user);
                    if (device->presentation) device->presentation->abandonFrameTiming(ticket);
                };
                state->continuousTiming = new xess_vk::continuous_timing::Coordinator(callbacks);
                initializePacketLedger(state);
            } catch (const std::exception& error) {
                JsonLine log("continuous_provider_rejected"); log.text("error", error.what()); log.write();
            }
        }
        computeActive = state->compute != nullptr;
        calibrateTrackingCpu(state);
        AcquireSRWLockExclusive(&stateLock);
        if (inventoryDeviceCreations != 0) inventorySessionRetired = true;
        if (inventorySessionRetired) {
            for (auto* previous = devices; previous; previous = previous->next) {
                if constexpr(researchDiagnostics) xess_vk::candidate_sync::retire(previous->candidateSync);
                xess_vk::render_pass_observer::retire(previous->renderPass);
                xess_vk::descriptor_image_observer::retire(previous->descriptors);
            }
            if constexpr(researchDiagnostics) xess_vk::candidate_sync::retire(state->candidateSync);
            xess_vk::render_pass_observer::retire(state->renderPass);
            xess_vk::descriptor_image_observer::retire(state->descriptors);
        }
        if (inventoryDeviceCreations < UINT64_MAX) ++inventoryDeviceCreations;
        state->next = devices; devices = state;
        ReleaseSRWLockExclusive(&stateLock);
    } else { delete state->measurements; delete state->measurementSnapshot; HeapFree(GetProcessHeap(), 0, state); }
    JsonLine log("create_device"); log.number("result", result);
    if(result==VK_SUCCESS&&state&&state->presentationRequested){
        const auto copy=state->fakeEnabled?xess_vk::diagnostic_flags::copyPathSelection():xess_vk::diagnostic_flags::selectCopyPath({});
        log.text("copy_route",copy.async?"async":"sync");log.number("inflight_limit",copy.inflightLimit);log.boolean("shared",copy.shared);
        log.text("copy_route_source",xess_vk::diagnostic_flags::copyPathSourceName(copy.routeSource));
        log.text("inflight_limit_source",xess_vk::diagnostic_flags::copyPathSourceName(copy.inflightSource));
        log.text("shared_source",xess_vk::diagnostic_flags::copyPathSourceName(copy.sharedSource));
    }
    log.handle("physical_device", bits(physical));
    if (result == VK_SUCCESS) log.handle("device", bits(*device));
    // Existing enabled_* fields now always describe what was actually passed to
    // the one downstream create call, not merely the application's initial wish.
    log.names("enabled_extensions", effective->enabledExtensionCount, effective->ppEnabledExtensionNames);
    log.number("queue_create_info_count", effective->queueCreateInfoCount);
    log.names("original_enabled_extensions", info->enabledExtensionCount, info->ppEnabledExtensionNames);
    log.boolean("original_timeline_feature_struct_present", originalFeatures.sawTimeline);
    log.boolean("original_timeline_semaphore_requested", originalFeatures.sawTimeline && originalFeatures.timeline);
    log.boolean("bridge_device_opt_in", bridgeRequested);
    log.boolean("gpu_timestamp_calibration_requested", timestampCalibrationRequested);
    log.boolean("upscale_observation_conflict", upscaleConflict);
    log.boolean("compute_observer_opt_in", computeRequested);
    log.boolean("compute_observer_active", computeActive);
    log.boolean("fake_swapchain_requested", fakeSwapchainRequested());
    log.boolean("fake_swapchain_enabled", result == VK_SUCCESS && state->fakeEnabled);
    log.boolean("image_inventory_active", result == VK_SUCCESS && state->imageInventory);
    log.boolean("image_sync_observer_active", result == VK_SUCCESS && state->candidateSync);
    log.boolean("render_pass_observer_active", result == VK_SUCCESS && state->renderPass);
    log.boolean("descriptor_image_observer_created", result == VK_SUCCESS && state->descriptors);
    log.number("image_inventory_maximum_active", xess_vk::image_inventory::Inventory<>::maximumActive);
    log.text("bridge_device_plan_status", planStatusName(plan.status()));
    log.text("bridge_device_plan_reason", plan.reason());
    if (plan.refusedStructureType() != VK_STRUCTURE_TYPE_MAX_ENUM)
        log.number("bridge_device_refused_structure_type", plan.refusedStructureType());
    log.boolean("bridge_device_plan_applied", plan.ready() && validChain);
    log.boolean("bridge_device_invalid_planned_loader_chain", plan.ready() && !validChain);
    log.number("bridge_device_create_attempts", validChain ? 1 : 0);
    log.boolean("bridge_device_timeline_changed", plan.timelineFeatureChanged());
    log.boolean("bridge_device_synchronous_loader_payload", plan.requiresSynchronousLoaderCall());
    log.key("bridge_device_added_extensions"); log.raw("[");
    for (size_t i = 0; i < plan.addedExtensions().size(); ++i) {
        if (i) log.raw(","); log.quoted(plan.addedExtensions()[i].c_str());
    }
    log.raw("]");
    log.text("bridge_device_query_status", query.status);
    log.number("instance_api_version_requested", parent->requestedApiVersion);
    if (bridgeRequested) {
        log.number("bridge_device_physical_api_version", query.support.physicalDeviceApiVersion);
        log.number("bridge_device_extension_query_result", query.extensionResult);
        log.boolean("bridge_device_extension_query_complete", query.support.extensionQueryComplete);
        log.boolean("bridge_device_timeline_query_complete", query.support.timelineFeatureQueryComplete);
        log.boolean("bridge_device_timeline_supported", query.support.timelineSemaphore == VK_TRUE);
    }
    const auto* node = reinterpret_cast<const VkBaseInStructure*>(effective->pNext);
    log.key("feature_chain_types"); log.raw("[");
    for (uint32_t i = 0; node && i < 256; ++i, node = node->pNext) {
        if (i) log.raw(",");
        char type[24]; std::snprintf(type, sizeof(type), "%u", static_cast<unsigned>(node->sType)); log.raw(type);
    }
    log.raw("]");
    log.boolean("timeline_feature_struct_present", effectiveFeatures.sawTimeline);
    log.boolean("timeline_semaphore_requested", effectiveFeatures.sawTimeline && effectiveFeatures.timeline);
    log.boolean("synchronization2_feature_struct_present", effectiveFeatures.sawSynchronization2);
    log.boolean("synchronization2_requested", effectiveFeatures.sawSynchronization2 && effectiveFeatures.synchronization2);
    log.write();
    if(srCapsRequested)logSrDeviceCapabilities(parent,physical,*info,*effective,query,originalSrRequested,effectiveSrRequested,
        srPlan.ready(),parent->srCreation&&!upscaleConflict);
#ifdef XESS_FSR_DETOUR_AVAILABLE
    // Every successful device boundary is counted, even when the option is off.
    // Do not hold state/log locks across installation; never retry from Present.
    if (result == VK_SUCCESS) {
        if constexpr(researchDiagnostics) xess_vk::fsr_callback_observer::setImageMetadataSink(imageMetadataLog);
        xess_vk::fsr_callback_install::installAtDeviceCreation(bits(*device), streamlineLog, upscaleImageMetadataLog,
            state->continuous ? continuousInvocation : nullptr,
            state->continuousTiming ? continuousTimingEvent : nullptr, state,
            state->continuousTiming ? continuousRegistrationFrame : nullptr,
            state->packets ? continuousEnqueue : nullptr,
            currentProfileReady && !InterlockedCompareExchange(&parent->continuousSurfaceRefused,0,0) && state->presentationRecord && state->continuous && state->continuousTiming && state->packets,
            state->srObservationRequested?srObservationImages:nullptr);
        if (xess_vk::fsr_callback_install::currentContinuousReady(bits(*device)))
            state->continuousUnityBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(L"UnityPlayer.dll"));
    }
#endif
    if(state->srRuntimeRequested)try {state->srRuntime=new GameSrRuntime(state);}
    catch(const std::exception& error){JsonLine line("sr_stage1_refused");line.text("reason",error.what());line.write();}
    if(result==VK_SUCCESS&&continuousRequested&&xess_vk::diagnostic_flags::srReplaceRequested()&&
       (!state->srRuntime||!state->continuousUnityBase)){
        JsonLine log("sr_startup_refused");log.text("reason","pure_SR_or_engine_native_control_unavailable");log.boolean("native_fsr_permitted",false);log.write();
        vkDestroyDevice(*device,allocator);*device=VK_NULL_HANDLE;SetLastError(originalCreateError);return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (result==VK_SUCCESS && continuousRequested) {
        const bool ready=currentContinuousPresentationReady(parent);
        if (!ready) state->presentationRequested=false;
        JsonLine admission("current_continuous_device_admission");
        admission.boolean("profile_preflight_ready",currentProfileReady);
        admission.boolean("surface_first_refused",InterlockedCompareExchange(&parent->continuousSurfaceRefused,0,0)!=0);
        admission.boolean("presentation_ready",ready);
        admission.boolean("bridge_plan_applied",plan.ready()&&validChain);
        admission.boolean("native_parameters_preserved",effective==info);
        admission.write();
    }
    SetLastError(originalCreateError);return result;
}

OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkDestroyDevice(VkDevice device, const VkAllocationCallbacks* allocator) {
    auto* state = findDevice(device); if (!state) return;
    if(state->srSdkPrivate){
        state->destroy(device,allocator);const auto error=GetLastError();
        AcquireSRWLockExclusive(&stateLock);
        for(auto** link=&devices;*link;link=&(*link)->next)if(*link==state){*link=state->next;break;}
        ReleaseSRWLockExclusive(&stateLock);HeapFree(GetProcessHeap(),0,state);SetLastError(error);return;
    }
    const auto entryError=GetLastError();
    if(state->wsiAcquire && !InterlockedCompareExchange(&state->wsiAcquireFinalLogged,1,0))
        wsiAcquireSummary(state,"destroy_device_entry");
    InterlockedExchange(&state->continuousClosing, 1);
    auto* originBinding = presentOriginBinding.load();
    if (originBinding && originBinding->state == state) originBinding->active.store(false);
#ifdef XESS_FSR_DETOUR_AVAILABLE
    // Stop every device-owned FSR source before any healthy native-tail return
    // can bypass final device deletion. Already admitted readers remain gated.
    xess_vk::fsr_callback_install::onDeviceDestroyed(bits(device));
#endif
    if (state->packets) state->packets->stop();
    if (state->continuousTiming) {
        xess_vk::game_timing::stop();
        state->continuousTiming->stop();
        if (xess_vk::game_timing::callbacksInFlight()) {
            if (state->parent) state->parent->presentationQuarantined = true;
            JsonLine log("continuous_timing_destroy_device_deferred");
            log.boolean("original_forwarded", false); log.write();
            SetLastError(entryError); return;
        }
    }
    if ((originBinding && originBinding->state == state && originBinding->inFlight.load()) ||
        (state->packets && state->packets->stats().inFlight) || !TryAcquireSRWLockExclusive(&stateLock)) {
        if (state->parent) state->parent->presentationQuarantined = true;
        JsonLine log("presentation_callback_owners_retained");
        log.boolean("original_forwarded", false);
        log.write();
        SetLastError(entryError);
        return;
    }
    ReleaseSRWLockExclusive(&stateLock);
    bool nativeTailRetained{};
    if (state->presentation && state->presentation->shutdownState().failedQuarantined) {
        if (state->parent) state->parent->presentationQuarantined = true;
        JsonLine log("presentation_failed_shutdown_retained");
        log.boolean("privateCleanupCompleted", false); log.boolean("SDKReleased", false);
        log.boolean("BridgeReleased", false); log.boolean("nativeTailRetained", true);
        log.boolean("failedQuarantined", true); log.boolean("original_forwarded", false); log.write();
        SetLastError(entryError); return;
    }
    // Device destruction is an application-owned external synchronization
    // boundary. This single-queue tail proves private/capture GPU completion,
    // never presentation-engine retirement. No chain-destroy path submits it.
    if ((state->presentation && !state->presentation->shutdownState().privateCleanupCompleted) || state->continuous) {
        if (state->presentation) state->presentation->stopAdmission();
        const auto shutdownQueue = state->presentation ? state->presentation->queue() : state->continuousQueue;
        VkResult drained = VK_SUCCESS;
        if (!shutdownQueue || !state->createFence || !state->submit || !state->waitFences)
            drained = VK_ERROR_UNKNOWN;
        if (drained == VK_SUCCESS && !state->continuousDrainFence) {
            VkFenceCreateInfo info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            drained = state->createFence(device, &info, nullptr, &state->continuousDrainFence);
            if (drained == VK_SUCCESS) {
                VkSubmitInfo tail{VK_STRUCTURE_TYPE_SUBMIT_INFO};
                drained = queueDriver(state, "shutdown_tail_submit", shutdownQueue, [&] { return state->submit(shutdownQueue, 1, &tail, state->continuousDrainFence); });
            }
        }
        if (drained == VK_SUCCESS) {
            JsonLine enter("presentation_shutdown_vulkan_tail_enter"); enter.write();
            drained = state->waitFences(device, 1, &state->continuousDrainFence, VK_TRUE, 10'000'000'000ull);
            JsonLine returned("presentation_shutdown_vulkan_tail_return"); returned.number("result", drained); returned.write();
        }
        if (drained == VK_SUCCESS && state->continuous)
            drained = state->continuous->stop(state->continuousDrainFence);
        if (drained != VK_SUCCESS) {
            if (state->presentation) state->presentation->quarantineShutdown();
            if (state->parent) state->parent->presentationQuarantined = true;
            JsonLine log("presentation_shutdown_gpu_unproven");
            log.number("result", drained); log.boolean("failedQuarantined", true);
            log.boolean("privateCleanupCompleted", false); log.boolean("SDKReleased", false);
            log.boolean("BridgeReleased", false); log.boolean("original_forwarded", false); log.write();
            SetLastError(entryError); return;
        }
    }
    if (state->presentation) {
        const auto result = state->presentation->stop();
        const auto cleanup = state->presentation->shutdownState();
        JsonLine log("presentation_runtime_shutdown");
        log.number("result", result); log.text("stage", state->presentation->stage());
        log.number("frames", state->presentation->frames());
        log.boolean("privateCleanupCompleted", cleanup.privateCleanupCompleted);
        log.boolean("SDKReleased", cleanup.sdkReleased); log.boolean("BridgeReleased", cleanup.bridgeReleased);
        log.boolean("nativeTailRetained", cleanup.nativeTailRetained);
        log.boolean("failedQuarantined", cleanup.failedQuarantined); log.write();
        if (!cleanup.privateCleanupCompleted) {
            if (state->parent) state->parent->presentationQuarantined = true;
            SetLastError(entryError); return;
        }
        nativeTailRetained = cleanup.nativeTailRetained;
        if (!nativeTailRetained) { delete state->presentation; state->presentation = nullptr; }
    }
    if (state->continuousDrainFence) {
        state->destroyFence(device, state->continuousDrainFence, nullptr);
        state->continuousDrainFence = VK_NULL_HANDLE;
    }
    // Late registration/upscale readers hold the shared lock while resolving
    // these CPU owners. Detach under the exclusive lock, delete outside it;
    // failure to obtain the gate retains the owners instead of waiting forever.
    if (!TryAcquireSRWLockExclusive(&stateLock)) {
        if (state->parent) state->parent->presentationQuarantined = true;
        SetLastError(entryError);
        return;
    }
    auto* continuous = state->continuous;
    state->continuous = nullptr;
    auto* timingOwner = state->continuousTiming;
    state->continuousTiming = nullptr;
    auto* packetOwner = state->packets;
    state->packets = nullptr;
    auto* identities = state->packetIdentities;
    state->packetIdentities = nullptr;
    ReleaseSRWLockExclusive(&stateLock);
    delete packetOwner;
    delete continuous;
    delete timingOwner;
    delete identities;
    flushFrameSpikes(state,steadyNowNs(),"device_destroy");
    if (nativeTailRetained) {
        if (state->parent) state->parent->presentationQuarantined = true;
        JsonLine log("presentation_native_tail_retained");
        log.boolean("privateCleanupCompleted", true); log.boolean("SDKReleased", true);
        log.boolean("BridgeReleased", true); log.boolean("nativeTailRetained", true);
        log.boolean("failedQuarantined", false); log.boolean("original_forwarded", false); log.write();
        SetLastError(entryError); return;
    }
    delete state->presentationRecord; state->presentationRecord = nullptr;
    if(InterlockedCompareExchange(&state->rawQuarantined,0,0)){rawQuarantine(state,"DestroyDeviceAlreadyQuarantined",VK_NOT_READY);SetLastError(entryError);return;}
    if constexpr(researchDiagnostics)if(state->rawProbe){RawOperation operation(state);
        if(!operation.acquired){rawQuarantine(state,"DestroyDeviceObservationInProgress",VK_NOT_READY);SetLastError(entryError);return;}
        const auto cleanup=raw::beforeDeviceDestroy(state->rawProbe,2'000'000'000ull);
        if(!cleanup.accepted()){rawQuarantine(state,"DestroyDeviceOwnDrain",cleanup.driver);SetLastError(entryError);return;}
    }
    xess_vk::render_pass_observer::summary(state->renderPass);
    xess_vk::descriptor_image_observer::summary(state->descriptors);
    xess_vk::resource_memory_observer::summary(state->resourceMemory);
    AcquireSRWLockExclusive(&stateLock);
    inventorySessionRetired = true;
    if (state->wsiInventory) {
        const auto changes = state->wsiInventory->stop();
        applyWsiChanges(state, changes);
    }
    for (auto* item = devices; item; item = item->next) {
        if constexpr(researchDiagnostics) xess_vk::candidate_sync::retire(item->candidateSync);
        xess_vk::render_pass_observer::retire(item->renderPass);
        xess_vk::descriptor_image_observer::retire(item->descriptors);
        xess_vk::resource_memory_observer::retire(item->resourceMemory);
    }
    ReleaseSRWLockExclusive(&stateLock);
#ifdef XESS_FSR_DETOUR_AVAILABLE
    xess_vk::fsr_callback_install::onDeviceDestroyed(bits(device));
    if constexpr(researchDiagnostics) xess_vk::fsr_callback_observer::summary();
    xess_vk::upscale_callback_observer::summary();
#endif
    if (state->imageInventory) { try { state->imageInventory->stop(); } catch (...) {} }
    delete state->srRuntime;state->srRuntime=nullptr;
    srGpuBeforeDeviceDestroy(state);
    state->destroy(device, allocator);
    const auto originalError=GetLastError();
    srGpuDeviceTerminated(state);
    if constexpr(researchDiagnostics)if(state->rawProbe){raw::deviceTerminated(state->rawProbe);raw::destroy(state->rawProbe);state->rawProbe=nullptr;}
    AcquireSRWLockExclusive(&stateLock);
    for (auto** link = &devices; *link; link = &(*link)->next) if (*link == state) { *link = state->next; break; }
    ReleaseSRWLockExclusive(&stateLock);
    delete state->wsiInventory;
    delete state->wsiAcquire;
    delete state->queueGate;
    delete state->imageInventory;
    if constexpr(researchDiagnostics) xess_vk::candidate_sync::destroy(state->candidateSync);
    xess_vk::render_pass_observer::destroy(state->renderPass);
    if constexpr(researchDiagnostics) delete state->renderTail;state->renderTail=nullptr;
    xess_vk::descriptor_image_observer::destroy(state->descriptors);
    xess_vk::resource_memory_observer::destroy(state->resourceMemory);
    JsonLine log("destroy_device"); log.handle("device", bits(device));
    log.number("present_calls", InterlockedCompareExchange64(&state->presents, 0, 0));
    log.number("present_negative_results", InterlockedCompareExchange64(&state->presentErrors, 0, 0));
    log.number("image_create_calls", InterlockedCompareExchange64(&state->imageCreates, 0, 0));
    log.number("image_destroy_calls", InterlockedCompareExchange64(&state->imageDestroys, 0, 0));
    log.number("upscale_metadata_calls", InterlockedCompareExchange64(&state->upscaleMetadataCalls, 0, 0));
    log.number("upscale_metadata_lookups", InterlockedCompareExchange64(&state->upscaleMetadataLookups, 0, 0));
    log.number("upscale_metadata_numeric_matches", InterlockedCompareExchange64(&state->upscaleMetadataMatches, 0, 0));
    log.number("queue_submit_calls", InterlockedCompareExchange64(&state->submits, 0, 0)); log.write();
    if constexpr(researchDiagnostics) xess_vk::streamline_observer::flushSummary();
    if (state->compute) {
        // The metadata context emits its final summary before destroying itself.
        if constexpr(researchDiagnostics) xess_vk::compute_observer::destroy(state->compute);
    }
    srTailClose(state,"device_destroy");
    xess_vk::compute_observer::destroyShaderArchive(state->srShaders);
    if(state->measurements){emitMeasurementSummary(state,xess_vk::cpuWallNowNs(),true);JsonLine ended("measurement_device_destroy");ended.number("qpc_ns",xess_vk::cpuWallNowNs());ended.number("writer_failures_total",measurementSummaryLog.failures());ended.number("present_errors",InterlockedCompareExchange64(&state->presentErrors,0,0));ended.boolean("device_destroy_forwarded",true);ended.writeSummary();}
    delete state->srAdmission;
    delete state->measurements; delete state->measurementSnapshot;
    HeapFree(GetProcessHeap(), 0, state);
    SetLastError(originalError);
}

OBS_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR* info,
    const VkAllocationCallbacks* allocator, VkSwapchainKHR* swapchain) {
    auto* state = findDevice(device); if (!state || (!state->fakeEnabled && !state->createSwapchain)) return VK_ERROR_EXTENSION_NOT_PRESENT;
    if (const auto error = fakeRecordingError(state); error != VK_SUCCESS) return error;
    if (state->fakeEnabled && !state->presentationRequested) return VK_ERROR_INITIALIZATION_FAILED;
    if (state->presentationRequested && !state->presentation) {
        try {
            state->presentationSurface = state->parent ? state->parent->presentationSurface : nullptr;
            if (!state->presentationRecord || !state->presentationSurface || !info || info->surface != state->presentationSurface->handle)
                throw std::invalid_argument("No admitted presentation record / primary hidden surface");
            VkQueue queue{}; state->getQueue(device, state->presentationRecord->family(), 0, &queue);
            state->presentation = state->presentationRecord->create(*state->presentationSurface, queue, state->continuous).release();
        } catch (const std::exception& error) {
            JsonLine log("presentation_off_swapchain_rejected"); log.text("error", error.what()); log.write();
            if (state->fakeEnabled) return VK_ERROR_INITIALIZATION_FAILED;
            // No private submission exists. Let the original call own the
            // application's oldSwapchain/pNext/allocator semantics.
            state->presentationRequested = false;
        }
    }
    if (state->fakeEnabled && (!state->presentation || !state->presentation->fakeEnabled())) return VK_ERROR_INITIALIZATION_FAILED;
    xess_vk::wsi_image_inventory::CreateTicket wsiTicket;
    if (state->wsiInventory) {
        xess_vk::wsi_image_inventory::CreateDescription description;
        if (info) {
            description = {info->sType == VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, info->pNext != nullptr, info->flags, info->minImageCount,
                static_cast<std::uint32_t>(info->imageFormat), info->imageExtent.width, info->imageExtent.height,
                info->imageArrayLayers, info->imageUsage, static_cast<std::uint32_t>(info->imageSharingMode)};
        }
        AcquireSRWLockExclusive(&stateLock);
        const auto changes = state->wsiInventory->beforeCreate(info ? bits(info->oldSwapchain) : 0, description);
        wsiTicket = changes.createTicket; applyWsiChanges(state, changes);
        ReleaseSRWLockExclusive(&stateLock);
        reportWsiChanges(state, "before_create", changes);
    }
    const VkResult result = state->presentation ? state->presentation->create(info, allocator, swapchain) : state->createSwapchain(device, info, allocator, swapchain);
    if(result==VK_SUCCESS&&info&&info->oldSwapchain&&state->srRuntime)++state->srRuntime->rebuildRequests;
    if (state->packetIdentities && !state->packetIdentities->swapchainCreated(
            result == VK_SUCCESS && swapchain ? bits(*swapchain) : 0,result)) invalidatePacketCandidates(state);
    if (state->presentation) { JsonLine log("presentation_off_create_swapchain"); log.number("result", result); log.text("stage", state->presentation->stage()); log.boolean("generated_frames", false); log.write(); }
    if (state->wsiInventory) {
        AcquireSRWLockExclusive(&stateLock);
        const auto changes = state->wsiInventory->afterCreate(wsiTicket, result,
            result == VK_SUCCESS && swapchain ? bits(*swapchain) : 0);
        applyWsiChanges(state, changes);
        ReleaseSRWLockExclusive(&stateLock);
        reportWsiChanges(state, "after_create", changes);
    }
    JsonLine log("create_swapchain"); log.handle("device", bits(device)); log.number("result", result);
    if (result == VK_SUCCESS) log.handle("swapchain", bits(*swapchain));
    log.handle("surface", bits(info->surface));
    log.handle("old_swapchain", bits(info->oldSwapchain));
    log.number("format", info->imageFormat); log.number("color_space", info->imageColorSpace);
    log.number("width", info->imageExtent.width); log.number("height", info->imageExtent.height);
    log.number("image_usage", info->imageUsage); log.number("minimum_image_count", info->minImageCount);
    log.number("array_layers", info->imageArrayLayers); log.number("present_mode", info->presentMode);
    log.number("sharing_mode", info->imageSharingMode); log.number("flags", info->flags); log.write();
    return result;
}
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks* allocator) {
    auto* state = findDevice(device); if (!state || (!state->fakeEnabled && !state->destroySwapchain)) return;
    if(state->packetIdentities) state->packetIdentities->swapchainDestroyed(bits(swapchain));
    invalidatePacketCandidates(state);
    if (state->wsiInventory) {
        AcquireSRWLockExclusive(&stateLock);
        const auto changes = state->wsiInventory->beforeDestroy(bits(swapchain));
        applyWsiChanges(state, changes);
        ReleaseSRWLockExclusive(&stateLock);
        reportWsiChanges(state, "before_destroy", changes);
    }
    if (state->presentation) {
        const auto result = state->presentation->destroy(swapchain, allocator);
        if (result != VK_SUCCESS) {
            if (state->parent && (result < 0 || state->presentation->failed() ||
                state->presentation->shutdownState().failedQuarantined))
                state->parent->presentationQuarantined = true;
            JsonLine log("presentation_off_destroy_swapchain_deferred");
            log.number("retained_native_chains", state->presentation->retainedChains());
            log.number("retired_native_chains_destroyed", state->presentation->retiredChainsDestroyed());
            log.number("result", result); log.boolean("original_forwarded", false); log.write();
            return;
        }
    } else if (!state->fakeEnabled) state->destroySwapchain(device, swapchain, allocator);
    JsonLine log("destroy_swapchain"); log.handle("device", bits(device)); log.handle("swapchain", bits(swapchain)); log.write();
}
OBS_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkAcquireNextImageKHR(VkDevice device,VkSwapchainKHR swapchain,
    uint64_t timeout,VkSemaphore semaphore,VkFence fence,uint32_t* imageIndex) {
    auto* state=findDevice(device);if(!state||(!state->fakeEnabled&&!state->acquireNextImage))return VK_ERROR_EXTENSION_NOT_PRESENT;
    if (const auto error = fakeRecordingError(state); error != VK_SUCCESS) return error;
    if(!state->wsiAcquire&&!state->fakeEnabled)return state->acquireNextImage(device,swapchain,timeout,semaphore,fence,imageIndex);
    auto call=wsiAcquireBegin(state,xess_vk::wsi_acquire::Api::Acquire,swapchain);
    call.argumentsKnown=true;call.timeout=timeout;call.semaphore=semaphore!=VK_NULL_HANDLE;call.fence=fence!=VK_NULL_HANDLE;
    call.entryNs=static_cast<std::uint64_t>(steadyNowNs());
    const auto result=state->fakeEnabled ?
        (state->presentation ? state->presentation->acquire(swapchain,timeout,semaphore,fence,imageIndex,&call) : VK_ERROR_OUT_OF_DATE_KHR) :
        state->acquireNextImage(device,swapchain,timeout,semaphore,fence,imageIndex);
    const auto originalError=GetLastError();
    call.elapsedNs=static_cast<std::uint64_t>(steadyNowNs())-call.entryNs;call.result=result;
    call.imageIndexKnown=(result==VK_SUCCESS||result==VK_SUBOPTIMAL_KHR)&&imageIndex;
    if(call.imageIndexKnown)call.imageIndex=*imageIndex;
    if(result<0)fakeAcquireError(state,call);
    fakeAcquireSlow(state,call);if(state->wsiAcquire)wsiAcquireEnd(state,call);SetLastError(originalError);return result;
}
OBS_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkAcquireNextImage2KHR(VkDevice device,const VkAcquireNextImageInfoKHR* info,uint32_t* imageIndex) {
    auto* state=findDevice(device);if(!state||(!state->fakeEnabled&&!state->acquireNextImage2))return VK_ERROR_EXTENSION_NOT_PRESENT;
    if (const auto error = fakeRecordingError(state); error != VK_SUCCESS) return error;
    if(!state->wsiAcquire&&!state->fakeEnabled)return state->acquireNextImage2(device,info,imageIndex);
    auto call=wsiAcquireBegin(state,xess_vk::wsi_acquire::Api::Acquire2,info?info->swapchain:VK_NULL_HANDLE);
    call.argumentsKnown=info&&info->sType==VK_STRUCTURE_TYPE_ACQUIRE_NEXT_IMAGE_INFO_KHR;
    if(call.argumentsKnown){call.timeout=info->timeout;call.semaphore=info->semaphore!=VK_NULL_HANDLE;call.fence=info->fence!=VK_NULL_HANDLE;call.pNext=info->pNext!=nullptr;call.deviceMask=info->deviceMask;}
    call.entryNs=static_cast<std::uint64_t>(steadyNowNs());
    const auto result=state->fakeEnabled ?
        (state->presentation ? state->presentation->acquire2(info,imageIndex,&call) : VK_ERROR_OUT_OF_DATE_KHR) :
        state->acquireNextImage2(device,info,imageIndex);
    const auto originalError=GetLastError();
    call.elapsedNs=static_cast<std::uint64_t>(steadyNowNs())-call.entryNs;call.result=result;
    call.imageIndexKnown=(result==VK_SUCCESS||result==VK_SUBOPTIMAL_KHR)&&imageIndex;
    if(call.imageIndexKnown)call.imageIndex=*imageIndex;
    if(result<0)fakeAcquireError(state,call);
    fakeAcquireSlow(state,call);if(state->wsiAcquire)wsiAcquireEnd(state,call);SetLastError(originalError);return result;
}
OBS_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkGetSwapchainStatusKHR(VkDevice device,VkSwapchainKHR swapchain) {
    auto* state=findDevice(device);if(!state||(!state->fakeEnabled&&!state->getSwapchainStatus))return VK_ERROR_EXTENSION_NOT_PRESENT;
    if (const auto error = fakeRecordingError(state); error != VK_SUCCESS) return error;
    const auto status = [&] { return state->fakeEnabled ?
        (state->presentation ? state->presentation->status(swapchain) : VK_ERROR_OUT_OF_DATE_KHR) :
        state->getSwapchainStatus(device,swapchain); };
    if(!state->wsiAcquire)return status();
    auto call=wsiAcquireBegin(state,xess_vk::wsi_acquire::Api::Status,swapchain);call.argumentsKnown=true;
    call.entryNs=static_cast<std::uint64_t>(steadyNowNs());
    const auto result=status();
    const auto originalError=GetLastError();
    call.elapsedNs=static_cast<std::uint64_t>(steadyNowNs())-call.entryNs;call.result=result;
    wsiAcquireEnd(state,call);SetLastError(originalError);return result;
}
OBS_EXPORT __declspec(noinline) VKAPI_ATTR VkResult VKAPI_CALL vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* info) {
    const auto immediateCaller = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    const auto presentEntryNs = diagnosticLoggingEnabled()?steadyNowNs():0;
    auto* state = findDevice(queue); if (!state || (!state->fakeEnabled && !state->present)) return VK_ERROR_EXTENSION_NOT_PRESENT;
    if(state->srObservationRequested){xess_vk::sr_observation::pollCapture();srTailPoll(state);}
    if (state->fakeEnabled) {
        const auto recordingError = fakeRecordingError(state);
        const auto error = recordingError != VK_SUCCESS ? recordingError : state->presentation ? VK_SUCCESS : VK_ERROR_OUT_OF_DATE_KHR;
        if (error != VK_SUCCESS) {
            if (info && info->pResults) for (std::uint32_t i = 0; i < info->swapchainCount; ++i) info->pResults[i] = error;
            return error;
        }
    }
    // Serialize the ticket claim through its output and snapshot; QueueGate
    // remains local to each queue-driver call, including during SDK waits.
    const auto presentationAdmission=state->presentation ? state->presentation->admitPresent() :
        std::unique_lock<std::recursive_mutex>{};
    xess_vk::measurement::Scope measurementScope(state->measurements);
    if(state->wsiAcquire||state->fakeEnabled)InterlockedExchange(&state->wsiPresentThread,static_cast<LONG>(GetCurrentThreadId()));
    renderTailPresent(state);
    xess_vk::FrameTimingAssociation timing;
    packet::Claim claim;
    const auto snapshot = snapshotPacket(info, presentOriginScope != nullptr);
    auto key = packetKey(state, queue, snapshot);
    const auto origin = borrowPresentOrigin(state, queue, info, snapshot, key);
    const bool direct = origin.present ? origin.direct :
        xess_vk::game_timing::directPresentCaller(state->continuousUnityBase, immediateCaller);
    const char* directRefusal{};
    if (origin.present) {
        directRefusal = origin.refusal;
        if (origin.matched) {
            key = origin.key;
            claim = origin.claim;
        }
        if (direct) InterlockedIncrement64(&state->continuousDirectSeen);
        if (!origin.matched || directRefusal) invalidatePacketCandidates(state);
    } else if (direct) {
        InterlockedIncrement64(&state->continuousDirectSeen);
        if (InterlockedCompareExchange(&state->continuousClosing, 0, 0))
            directRefusal = "device_closing";
        else if (!state->packets || !state->continuousTiming || !state->presentation)
            directRefusal = "context_not_ready";
        else if (queue != state->continuousQueue)
            directRefusal = "queue_mismatch";
        else if (!key.valid)
            directRefusal = "packet_key_invalid";
        else {
            const auto source = state->continuousTiming->packetAssociation(GetCurrentThreadId());
            if (!source.valid)
                directRefusal = "source_scope_or_ticket_missing";
            else if (!state->packets->publish(key, source, state->presentation->timingContextGeneration()))
                directRefusal = "source_publish_refused";
        }
        // A direct call's own source must win before lookup. Do not borrow an
        // old async row when its scope/key is invalid or publication ambiguous.
        if (directRefusal)
            invalidatePacketCandidates(state);
    }
    if (state->packets && !origin.present) {
        if (!directRefusal && key.valid && state->presentation)
            claim = state->packets->claim(key, state->presentation->timingContextGeneration());
        else invalidatePacketCandidates(state);
    }
    if (direct && claim.valid)
        InterlockedIncrement64(&state->continuousDirectAccepted);
    const auto refused = direct && !claim.valid ? InterlockedIncrement64(&state->continuousDirectRefused) : 0;
    const auto unknown = !direct && !origin.present && state->continuousUnityBase ?
        InterlockedIncrement64(&state->continuousUnknownPresentCallers) : 0;
    if ((direct && (refused ? refused <= 4 : InterlockedCompareExchange64(&state->continuousDirectAccepted, 0, 0) <= 3)) ||
        (unknown && unknown <= 4)) {
        JsonLine route("presentation_packet_direct_source");
        route.boolean("recognized_direct", direct);
        route.boolean("claimed", claim.valid);
        route.text("reason", origin.present ? (claim.valid ? "origin_source_claimed" : directRefusal) :
            direct ? (claim.valid ? "current_source_claimed" :
            directRefusal ? directRefusal : "ticket_incomplete_or_not_current") : "unsupported_immediate_caller");
        route.boolean("origin_handoff", origin.present);
        route.boolean("origin_waits_consumed", origin.transformed);
        route.handle("caller", immediateCaller);
        packetCallerFields(route, immediateCaller, "caller_module", "caller_rva");
        route.number("thread_id", GetCurrentThreadId());
        route.boolean("packet_key_valid", key.valid);
        route.boolean("timing_retired", InterlockedCompareExchange(&state->continuousTimingRetired, 0, 0) != 0);
        route.write();
    }
    struct PacketLease {
        packet::Ledger* owner;
        std::uint64_t lease;
        ~PacketLease() {
            if (owner && lease) owner->finish(lease);
        }
    } packetLease{origin.present ? nullptr : state->packets, claim.lease};
    // Retire the observed cycle at entry. SDK output may block while the app
    // legitimately signals a later cycle of the same binary semaphore.
    consumePresentWaits(state, info);
    bool untimedOff{};
    if (state->continuousTiming && state->presentation) {
        timing = claim.valid ? claim.association : xess_vk::FrameTimingAssociation{};
        untimedOff = !timing.valid;
        if (!untimedOff)
            state->presentation->associatePresentTiming(timing.engineFrame, timing.ticket, timing.bootstrapSdkId);
    }
    if(state->presentation)state->presentation->holdOutput(state->srRuntime&&state->srRuntime->holdingOutput());
    const VkResult result = state->presentation ?
        (untimedOff ? state->presentation->presentUntimedOff(queue, info) : state->presentation->present(queue, info)) :
        queueDriver(state, "vkQueuePresentKHR", queue, [&] { return state->present(queue, info); });
    const bool untimedBlocked = state->continuousTiming && state->presentation &&
        state->presentation->lastPrivateCopyCompleted() && state->presentation->lastNativePresentCalled() &&
        state->presentation->lastUntimedTimingUnavailable();
    const bool startupGap = untimedBlocked && direct && key.valid && queue == state->continuousQueue &&
        directRefusal && std::strcmp(directRefusal, "source_scope_or_ticket_missing") == 0 &&
        state->continuousTiming->deferStartupGap(GetCurrentThreadId());
    if (startupGap) {
        JsonLine gap("continuous_startup_direct_gap");
        const auto scope = state->continuousTiming->scopeSnapshot(GetCurrentThreadId());
        gap.number("engine_frame", scope.presentFrame);
        gap.number("context_generation", scope.contextGeneration);
        gap.number("bounded_gap_count", state->continuousTiming->stats().startupGapDeferrals);
        gap.boolean("sdk_output_succeeded", false);
        gap.boolean("timing_retired", false);
        gap.write();
    }
    if (untimedBlocked && !startupGap &&
        InterlockedCompareExchange(&state->continuousTimingRetired,1,0)==0) {
        // The hidden native packet was already returned. Preserve the live SDK
        // owner for visible OFF; only the timing owner may cancel its tickets.
        auto* sourceBinding = presentOriginBinding.load();
        if (sourceBinding && sourceBinding->state == state) sourceBinding->active.store(false);
        xess_vk::game_timing::stop();
        if (state->packets) state->packets->stop();
        state->continuousTiming->stop();
        JsonLine retired("continuous_timing_retired_for_visible_off");
        retired.text("reason","untimed_off_blocked_by_timing_reservations");
        retired.boolean("private_copy_completed",state->presentation->lastPrivateCopyCompleted());
        retired.boolean("native_present_already_called",state->presentation->lastNativePresentCalled());
        retired.boolean("sdk_owner_retained",true);retired.boolean("restart_required_for_timing",true);retired.write();
    }
    if (untimedOff && (InterlockedIncrement64(&state->timingEvents) <= 8 ||
        InterlockedCompareExchange64(&state->timingEvents, 0, 0) % kPresentInterval == 0)) {
        const auto scope = state->continuousTiming->scopeSnapshot(GetCurrentThreadId());
        const auto stats = state->continuousTiming->stats();
        JsonLine log("continuous_present_timing_off_fallback");
        log.number("result", result); log.text("stage", state->presentation->stage());
        log.number("thread_id", GetCurrentThreadId()); log.boolean("scope_in_native_present", scope.inPresent);
        log.number("packet_engine_frame", scope.presentFrame); log.number("context_generation", scope.contextGeneration);
        log.number("copy_progress", static_cast<unsigned>(state->presentation->lastCopyProgress()));
        log.boolean("private_copy_completed", state->presentation->lastPrivateCopyCompleted());
        log.boolean("original_forwarded", state->presentation->lastNativePresentCalled());
        log.boolean("sdk_output_succeeded", state->presentation->lastSdkOutputSucceeded());
        log.boolean("terminal_retirement",InterlockedCompareExchange(&state->continuousTimingRetired,0,0)!=0);
        log.number("timing_occupied", stats.occupied); log.number("timing_pending", stats.pending);
        log.number("timing_saturated", stats.saturated); log.number("timing_rejected", stats.rejected);
        log.number("timing_record_missing", stats.missingRecord); log.number("timing_record_incomplete", stats.incompleteRecord);
        log.number("timing_missing_scope", stats.missingScope); log.number("timing_out_of_order", stats.outOfOrder);
        log.write();
    }
    if (state->continuousTiming && state->presentation && state->presentation->lastSdkOutputSucceeded()) {
        const auto frame = state->presentation->frameStatus();
        if (frame.untimedOff) state->continuousTiming->untimedPresented(frame.sdkPresentId);
    }
    if (continuousWindowActive(state) && InterlockedIncrement64(&state->continuousWindowPresents) >= 16 &&
        InterlockedCompareExchange(&state->continuousWindowState, 2, 1) == 1) {
        JsonLine window("continuous_window_end");
        window.number("sequence", InterlockedIncrement64(&state->continuousDiagnosticSequence));
        window.text("reason", "16_present_window");
        window.number("engine_frame", timing.engineFrame);
        window.number("remaining_record_budget", InterlockedCompareExchange(&state->continuousWindowBudget, 0, 0));
        window.write();
    }
    if(result==VK_ERROR_DEVICE_LOST)xess_vk::measurement::increment(xess_vk::measurement::Counter::DeviceLost);
    const bool sdkSucceeded = !state->presentation || state->presentation->lastSdkOutputSucceeded();
    if (state->continuousTiming && timing.valid)
        state->continuousTiming->presented(timing, sdkSucceeded && (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR));
    if (state->continuous && timing.valid && sdkSucceeded && (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR))
        state->continuous->retireFrame(timing);
    if(!diagnosticLoggingEnabled())return result;
    const auto presentationAttempt = state->presentation ? InterlockedIncrement64(&state->presentationAttempts) : 0;
    if(state->presentation&&(state->fakeEnabled||state->continuous)){
        const auto frame=state->presentation->frameStatus();
        const bool outputFresh=longFrameResetTrace(state,frame,result,presentationAttempt);
        recordMeasurements(state,frame,state->presentation->lastSdkOutputSucceeded(),presentEntryNs,presentationAttempt,measurementScope.commands());
        if(state->continuous)recordFrameSpike(state,frame,outputFresh,presentEntryNs,timing.engineFrame,presentationAttempt);
    }
    if(state->packets && state->presentation && (presentationAttempt<=4 || presentationAttempt%kPresentInterval==0)) {
        const auto stats=state->packets->stats();
        JsonLine carrier("presentation_packet_carrier");
        carrier.boolean("exact_key_valid",key.valid); carrier.boolean("claimed",claim.valid);
        carrier.number("source_published",stats.published); carrier.number("consumer_claimed",stats.claimed);
        carrier.number("hook_enqueue_entered_total",xess_vk::game_timing::counters().enqueueEntrances);
        carrier.number("source_forwarded_active",InterlockedCompareExchange64(&state->continuousEnqueueCalls,0,0));
        carrier.boolean("timing_retired_at_snapshot",InterlockedCompareExchange(&state->continuousTimingRetired,0,0)!=0);
        carrier.number("source_queue_valid",InterlockedCompareExchange64(&state->continuousEnqueueQueueValid,0,0));
        carrier.number("source_queue_matched",InterlockedCompareExchange64(&state->continuousEnqueueQueueMatched,0,0));
        carrier.number("source_packet_shape_valid",InterlockedCompareExchange64(&state->continuousEnqueueShapes,0,0));
        carrier.number("source_packet_key_valid",InterlockedCompareExchange64(&state->continuousEnqueueKeys,0,0));
        carrier.number("source_association_valid",InterlockedCompareExchange64(&state->continuousEnqueueAssociations,0,0));
        carrier.number("source_publish_attempts",InterlockedCompareExchange64(&state->continuousEnqueuePublishAttempts,0,0));
        carrier.number("source_failures",InterlockedCompareExchange64(&state->continuousEnqueueFailures,0,0));
        carrier.number("direct_source_seen",InterlockedCompareExchange64(&state->continuousDirectSeen,0,0));
        carrier.number("direct_source_accepted",InterlockedCompareExchange64(&state->continuousDirectAccepted,0,0));
        carrier.number("direct_source_refused",InterlockedCompareExchange64(&state->continuousDirectRefused,0,0));
        carrier.number("unsupported_immediate_callers",InterlockedCompareExchange64(&state->continuousUnknownPresentCallers,0,0));
        carrier.number("origin_calls",InterlockedCompareExchange64(&state->continuousOriginCalls,0,0));
        carrier.number("origin_consumers",InterlockedCompareExchange64(&state->continuousOriginConsumers,0,0));
        carrier.number("origin_submit_proofs",InterlockedCompareExchange64(&state->continuousOriginSubmitProofs,0,0));
        carrier.number("origin_zero_wait_handoffs",InterlockedCompareExchange64(&state->continuousOriginWaitTransforms,0,0));
        carrier.number("misses",stats.misses); carrier.number("in_flight",stats.inFlight);
        carrier.number("occupied",stats.occupied); carrier.write();
    }
    if (state->presentation && (result < 0 || presentationAttempt <= 4 || presentationAttempt % kPresentInterval == 0)) {
        JsonLine log(state->continuous ? "presentation_continuous_present" : "presentation_off_present");
        log.number("result", result); log.text("stage", state->presentation->stage()); log.text("error", state->presentation->error());
        log.number(state->continuous ? "present_calls" : "xefg_off_frames", state->presentation->frames());
        log.number("output_attempts", presentationAttempt);
        log.number("retained_native_chains", state->presentation->retainedChains());
        log.number("retired_native_chains_destroyed", state->presentation->retiredChainsDestroyed());
        if (!state->continuous) log.boolean("generated_frames", false);
        if (!state->continuous) {
            const auto frame = state->presentation->frameStatus();
            cpuWallFields(log, frame);
            log.boolean("sdk_output_succeeded", state->presentation->lastSdkOutputSucceeded());
            log.boolean("sdk_status_valid", frame.sdkStatusValid);
            log.boolean("output_occluded", frame.outputOccluded);
            log.boolean("queued_off", frame.queuedOff);
            log.boolean("queued_on", frame.queuedOn);
            trackingCpuFields(log, state);
        }
        log.write();
        if (state->continuous) {
            const auto frame = state->presentation->frameStatus();
            const auto inputs = state->continuous->stats();
            JsonLine input("continuous_present");
            input.number("output_attempts", presentationAttempt);
            input.number("present_calls", state->presentation->frames());
            input.number("producer_token", frame.producerToken);
            input.number("recording_generation", frame.recordingGeneration);
            input.number("viewport_generation", frame.viewportGeneration);
            input.number("submission", frame.submission);
            input.number("control_epoch", frame.controlEpoch);
            cpuWallFields(input, frame);
            trackingCpuFields(input, state);
            input.number("engine_frame", frame.engineFrame);
            input.number("input_validity", frame.inputValidity);
            input.boolean("input_matched", frame.inputMatched);
            input.number("input_depth_width", frame.depthExtent.width);
            input.number("input_depth_height", frame.depthExtent.height);
            input.number("input_motion_width", frame.motionExtent.width);
            input.number("input_motion_height", frame.motionExtent.height);
            input.number("constants_render_width", frame.renderSize[0]);
            input.number("constants_render_height", frame.renderSize[1]);
            input.number("constants_upscale_width", frame.upscaleSize[0]);
            input.number("constants_upscale_height", frame.upscaleSize[1]);
            input.boolean("output_region_observed", frame.outputRegion.observed);
            input.boolean("output_region_valid", frame.outputRegion.valid);
            input.boolean("output_region_local_fi_off_verified", frame.outputRegion.localFiOffVerified);
            input.number("output_region_x", frame.outputRegion.rect[0]);
            input.number("output_region_y", frame.outputRegion.rect[1]);
            input.number("output_region_width", frame.outputRegion.rect[2]);
            input.number("output_region_height", frame.outputRegion.rect[3]);
            input.boolean("xefg_low_res_requested", frame.lowResOutputRequested);
            input.boolean("xefg_low_res_active", frame.lowResOutputActive);
            input.boolean("xefg_low_res_held", frame.lowResOutputHeld);
            char xefgGeometry[128];
            std::snprintf(xefgGeometry, sizeof(xefgGeometry), "[%u,%u]", frame.xefgSourceExtent.width, frame.xefgSourceExtent.height);
            input.key("xefg_source_extent"); input.raw(xefgGeometry);
            std::snprintf(xefgGeometry, sizeof(xefgGeometry), "[%u,%u]", frame.xefgOutputExtent.width, frame.xefgOutputExtent.height);
            input.key("xefg_output_extent"); input.raw(xefgGeometry);
            std::snprintf(xefgGeometry, sizeof(xefgGeometry), "[%d,%d,%u,%u]", frame.xefgRegion.offset.x, frame.xefgRegion.offset.y,
                frame.xefgRegion.extent.width, frame.xefgRegion.extent.height);
            input.key("xefg_region"); input.raw(xefgGeometry);
            input.text("xefg_ui_mode", "NONE");
            input.key("xefg_low_res_fallback_reason");
            if (frame.lowResOutputFallbackReason) input.quoted(frame.lowResOutputFallbackReason); else input.raw("null");
            input.boolean("requested_enabled", frame.requestedEnabled);
            input.boolean("menu_requested_enabled", frame.menuRequestedEnabled);
            input.boolean("menu_paired", frame.menuPaired);
            input.boolean("menu_gates_consistent", frame.menuGatesConsistent);
            input.boolean("menu_native_suppressed", frame.menuNativeSuppressed);
            input.boolean("menu_input_observed", frame.menuInputObserved);
            input.boolean("menu_gates_ready", frame.menuGatesReady);
            {
                const auto menuStats = xess_vk::fsr_menu_control::stats();
                const auto menuNow = xess_vk::fsr_menu_control::snapshot();
                input.number("menu_raw_algo", menuNow.rawAlgo);
                input.boolean("menu_setting_enabled", menuNow.rawEnable != 0);
                input.number("menu_render_params_calls", menuStats.renderParamsCalls);
                input.number("menu_forced_off", menuStats.forcedOff);
                input.number("menu_backstop_shadows", menuStats.backstopShadows);
                input.number("menu_compose_off_shadows", menuStats.composeOffShadows);
                input.number("menu_local_algo_preserved", menuStats.localAlgoPreserved);
                input.number("menu_local_compose_matched", menuStats.localComposeMatched);
                input.number("menu_local_compose_refused", menuStats.localComposeRefused);
                input.number("menu_local_unexpected_native_enabled", menuStats.localUnexpectedNativeEnabled);
                input.boolean("diag_local_fi_off_requested", xess_vk::diagnostic_flags::localFiOff());
                input.boolean("diag_local_fi_off_active", menuStats.localFiOffActive);
                input.boolean("diag_keep_native_algo", xess_vk::diagnostic_flags::keepNativeAlgo());
                input.boolean("diag_skip_input_snapshot", xess_vk::diagnostic_flags::skipInputSnapshot());
                input.boolean("diag_keep_native_algo_file", xess_vk::diagnostic_flags::keepNativeAlgoReading().file);
                input.number("diag_keep_native_algo_registry", xess_vk::diagnostic_flags::keepNativeAlgoReading().registry);
                input.boolean("diag_skip_input_snapshot_file", xess_vk::diagnostic_flags::skipInputSnapshotReading().file);
                input.number("diag_skip_input_snapshot_registry", xess_vk::diagnostic_flags::skipInputSnapshotReading().registry);
                input.number("menu_enabled_gate_calls", menuStats.enabledGateCalls);
            }
            input.boolean("latency_transition_admitted", frame.latencyTransitionAdmitted);
            if (frame.menuRequestedEnabled && (!frame.latencyTransitionAdmitted || (frame.onBlockedReason && *frame.onBlockedReason)))
                input.text("on_blocked_reason", frame.onBlockedReason ? frame.onBlockedReason : "ll_mode_change_requires_gpu_quiescence");
            input.boolean("sdk_output_succeeded", state->presentation->lastSdkOutputSucceeded());
            input.boolean("sdk_enabled", frame.sdkEnabled);
            input.boolean("sdk_status_valid", frame.sdkStatusValid);
            input.boolean("output_occluded", frame.outputOccluded);
            input.boolean("queued_off", frame.queuedOff);
            input.boolean("queued_on", frame.queuedOn);
            input.number("sdk_query_result", frame.queryResult);
            input.number("sdk_framegen_result", frame.frameGenResult);
            input.number("sdk_frames_presented", frame.framesPresented);
            input.number("sdk_interpolated_frames", frame.interpolatedFrames);
            input.number("long_frame_resets", static_cast<int64_t>(frame.longFrameResets));
            input.boolean("partial_xell", frame.partialXeLL);
            input.boolean("full_latency_integration", false);
            input.text("timing_policy", frame.timingPolicy ? frame.timingPolicy : "unavailable");
            input.number("sleep_frame_id", frame.sleepFrameId);
            input.number("sleep_calls", frame.sleepCalls);
            input.number("present_marker_calls", frame.presentMarkerCalls);
            input.number("total_marker_calls", frame.totalMarkerCalls);
            input.boolean("complete_frame_timing", frame.completeFrameTiming);
            input.number("consecutive_enabled_presents", frame.consecutiveEnabledPresents);
            input.number("sleep_result", frame.sleepResult);
            input.number("present_start_result", frame.presentStartResult);
            input.number("present_end_result", frame.presentEndResult);
            input.number("captured", inputs.captured);
            input.number("shared_captures",inputs.sharedCaptures);
            input.number("shared_fallbacks",inputs.sharedFallbacks);
            input.number("shared_failures",inputs.sharedFailures);
            input.number("depth_compute_captures",inputs.depthComputeCaptures);
            input.number("depth_transfer_captures",inputs.depthTransferCaptures);
            input.number("depth_compute_fallbacks",inputs.depthComputeFallbacks);
            input.number("depth_compute_failures",inputs.depthComputeFailures);
            for(unsigned i=0;i<inputs.sharedStates.size();++i) {
                char field[32];std::snprintf(field,sizeof(field),"shared_state_%u",i);
                input.number(field,static_cast<unsigned>(inputs.sharedStates[i]));
            }
            input.text("aux_capture_policy", "skip_observed_off");
            input.number("skipped_off_captures", inputs.skippedOffCaptures);
            input.number("invocations", inputs.invocations);
            input.number("rejected_invocations", inputs.rejectedInvocations);
            input.number("missing_invocation_frame", inputs.missingInvocationFrame);
            input.number("camera_match_missing", InterlockedCompareExchange64(&state->continuousCameraMissing, 0, 0));
            input.number("registration_frame_missing", InterlockedCompareExchange64(&state->continuousRegistrationFrameMissing, 0, 0));
            input.number("resolved_ticket_missing", InterlockedCompareExchange64(&state->continuousResolvedTicketMissing, 0, 0));
            input.boolean("frame_rejection_counts_valid_resources_only", true);
            input.number("invocation_capacity_refusals", inputs.invocationCapacityRefusals);
            input.number("matched_dispatches", inputs.matchedDispatches);
            input.number("missing_recording_frame", inputs.missingRecordingFrame);
            input.number("unmatched_dispatches", inputs.unmatchedDispatches);
            input.number("ambiguous_dispatches", inputs.ambiguousDispatches);
            input.number("invalid_recording", inputs.invalidRecording);
            input.number("invalid_sources", inputs.invalidSources);
            input.number("copy_failures", inputs.copyFailures);
            const auto descriptor = xess_vk::descriptor_image_observer::stats(state->descriptors).metadata;
            input.boolean("descriptor_continuous_dispatch_selection", descriptor.continuousDispatchSelection);
            input.number("descriptor_filtered_dispatches", descriptor.filteredDispatches);
            input.number("descriptor_template_updates", descriptor.coreTemplateUpdateCalls+descriptor.khrTemplateUpdateCalls);
            input.number("descriptor_template_read_calls", descriptor.templateReadCalls);
            input.number("descriptor_template_read_bytes", descriptor.templateReadBytes);
            input.number("descriptor_template_read_elements", descriptor.templateReadElements);
            input.number("descriptor_template_span_read_calls", descriptor.templateSpanReadCalls);
            input.number("descriptor_template_span_read_successes", descriptor.templateSpanReadSuccesses);
            input.number("descriptor_template_span_read_fallbacks", descriptor.templateSpanReadFallbacks);
            input.number("descriptor_template_span_read_oversize", descriptor.templateSpanReadOversize);
            input.number("descriptor_template_span_max_bytes", descriptor.templateSpanMaxBytes);
            const auto sets = descriptor.objects[static_cast<std::size_t>(xess_vk::descriptor_image_metadata::ObjectKind::Sets)];
            input.number("descriptor_sets_live", sets.live);
            input.number("descriptor_sets_capacity", sets.capacity);
            input.number("descriptor_sets_capacity_refusals", sets.capacityRefusals);
            input.number("driver_live_sets", descriptor.driverLiveSets);
            input.number("driver_set_high_water", descriptor.driverSetHighWater);
            input.number("declared_pool_max_sets", descriptor.declaredPoolMaxSets);
            input.boolean("driver_set_ledger_complete", descriptor.driverSetLedgerComplete);
            input.number("consumed", inputs.consumed);
            input.number("saturated", inputs.saturated);
            input.number("missing_present", inputs.missingPresent);
            input.number("ambiguous_present", inputs.ambiguousPresent);
            input.write();
        }
    }
    const auto count = InterlockedIncrement64(&state->presents);
    if (count == 1 || count % kHealthSummaryInterval == 0) {
        if constexpr(researchDiagnostics) if (!upscaleOnlyRequested()) xess_vk::streamline_observer::tryInstall(streamlineLog);
        if constexpr(researchDiagnostics) xess_vk::streamline_observer::flushSummary();
        if constexpr(researchDiagnostics) if (state->compute) xess_vk::compute_observer::summary(state->compute);
        if constexpr(researchDiagnostics) xess_vk::candidate_sync::summary(state->candidateSync);
        xess_vk::render_pass_observer::summary(state->renderPass);
#ifdef XESS_FSR_DETOUR_AVAILABLE
        xess_vk::fsr_callback_install::summary();
        if constexpr(researchDiagnostics) xess_vk::fsr_callback_observer::summary();
        xess_vk::upscale_callback_observer::summary();
#endif
    }
    LONG64 errors = 0;
    if (result < 0) errors = InterlockedIncrement64(&state->presentErrors);
    if (count <= 4 || count % kPresentInterval == 0 || (errors > 0 && errors <= 4)) {
        JsonLine log("queue_present_sample"); log.handle("device", bits(state->device)); log.handle("queue", bits(queue));
        log.number("present_calls", count); log.number("result", result); log.number("swapchain_count", info->swapchainCount);
        log.number("wait_semaphore_count", info->waitSemaphoreCount);
        if (state->presentation && count <= 4) {
            packetCallerFields(log, immediateCaller, "immediate_caller_module", "immediate_caller_rva");
            log.handle("immediate_caller", immediateCaller);
            log.handle("packet_info", bits(info));
            log.handle("packet_swapchain", bits(snapshot.chain));
            log.number("packet_image_index", snapshot.image);
            log.handle("packet_first_wait", snapshot.info.waitSemaphoreCount ? snapshot.waits[0] : 0);
            log.boolean("origin_handoff", origin.matched);
            log.boolean("origin_waits_consumed", origin.transformed);
        }
        log.number("upscale_metadata_calls", InterlockedCompareExchange64(&state->upscaleMetadataCalls, 0, 0));
        log.number("upscale_metadata_lookups", InterlockedCompareExchange64(&state->upscaleMetadataLookups, 0, 0));
        log.number("upscale_metadata_numeric_matches", InterlockedCompareExchange64(&state->upscaleMetadataMatches, 0, 0));
        if (state->imageInventory) {
            try {
                const auto inventory = state->imageInventory->lookup(0);
                log.number("image_inventory_active_count", inventory.active);
                log.text("image_inventory_status", inventory.status == xess_vk::image_inventory::Status::NotObserved ? "tracking" : xess_vk::image_inventory::name(inventory.status));
            } catch (...) { log.text("image_inventory_status", "lookup_failed"); }
        }
        log.number("present_negative_results", InterlockedCompareExchange64(&state->presentErrors, 0, 0)); log.write();
    }
    return result;
}

OBS_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateWin32SurfaceKHR(VkInstance instance, const VkWin32SurfaceCreateInfoKHR* info,
    const VkAllocationCallbacks* allocator, VkSurfaceKHR* surface) {
    auto* state = findInstance(instance); if (!state || !state->createWin32Surface) return VK_ERROR_EXTENSION_NOT_PRESENT;
    std::unique_ptr<xess_vk::presentation::Surface> redirected;
    VkWin32SurfaceCreateInfoKHR actual{}; const auto* effective = info;
    const bool continuousRequested=xess_vk::continuous::requested();
    const bool currentReady=continuousRequested && currentContinuousPresentationReady(state);
    if (continuousRequested && !currentReady) {
        InterlockedExchange(&state->continuousSurfaceRefused,1);
        JsonLine rejected("current_continuous_surface_refused");rejected.text("reason","device_not_ready_before_surface");rejected.write();
    }
    if (xess_vk::presentation::enabled() && (currentReady || (!continuousRequested && !upscaleObservationConflict()))) {
        try {
            if (!info || info->sType != VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR || info->pNext || info->flags || allocator || state->presentationSurface)
                throw std::invalid_argument("Presentation OFF accepts one default-allocator Win32 surface without extensions");
            redirected = std::make_unique<xess_vk::presentation::Surface>(info->hwnd);
            actual = *info; actual.hwnd = redirected->hidden(); actual.hinstance = GetModuleHandleW(nullptr); effective = &actual;
        } catch (const std::exception& error) {
            JsonLine rejected("presentation_off_surface_rejected"); rejected.text("error", error.what()); rejected.write();
            // No GPU submission exists at this boundary: preserve the original
            // surface request if the private HWND could not be admitted.
            redirected.reset();
            effective = info;
        }
    }
    const auto result = state->createWin32Surface(instance, effective, allocator, surface);
    if (result == VK_SUCCESS && redirected) {
        redirected->handle = *surface; state->presentationSurface = redirected.release();
        JsonLine log("presentation_off_hidden_surface"); log.handle("original_hwnd", bits(info->hwnd)); log.handle("hidden_hwnd", bits(state->presentationSurface->hidden())); log.write();
    }
    JsonLine log("create_win32_surface"); log.handle("instance", bits(instance)); log.number("result", result);
    if (result == VK_SUCCESS) log.handle("surface", bits(*surface));
    log.handle("hwnd", bits(info->hwnd)); log.number("flags", info->flags); log.write();
    return result;
}
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkDestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface, const VkAllocationCallbacks* allocator) {
    auto* state = findInstance(instance); if (!state || !state->destroySurface) return;
    if (state->presentationQuarantined) {
        JsonLine log("presentation_off_destroy_surface_deferred"); log.boolean("original_forwarded", false); log.write(); return;
    }
    state->destroySurface(instance, surface, allocator);
    if (state->presentationSurface && state->presentationSurface->handle == surface) { delete state->presentationSurface; state->presentationSurface = nullptr; }
    JsonLine log("destroy_surface"); log.handle("instance", bits(instance)); log.handle("surface", bits(surface)); log.write();
}
OBS_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateBuffer(VkDevice device,const VkBufferCreateInfo* info,const VkAllocationCallbacks* allocator,VkBuffer* buffer) {
    auto* state=findDevice(device);if(!state||!state->createBuffer)return VK_ERROR_INITIALIZATION_FAILED;
    const auto result=state->createBuffer(device,info,allocator,buffer);const DWORD originalError=GetLastError();
    VkMemoryRequirements requirements{};bool queried=false;
    if(result==VK_SUCCESS&&state->resourceMemory&&state->bufferMemoryRequirements&&xess_vk::resource_memory_observer::bufferRequirementsQuerySupported(info)){
        state->bufferMemoryRequirements(device,*buffer,&requirements);queried=true;
    }
    SetLastError(originalError);
    xess_vk::resource_memory_observer::bufferCreated(state->resourceMemory,result==VK_SUCCESS?*buffer:VK_NULL_HANDLE,info,result,queried?&requirements:nullptr);
    return result;
}
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkDestroyBuffer(VkDevice device,VkBuffer buffer,const VkAllocationCallbacks* allocator) {
    auto* state=findDevice(device);if(!state||!state->destroyBuffer)return;
    xess_vk::resource_memory_observer::bufferDestroying(state->resourceMemory,buffer);state->destroyBuffer(device,buffer,allocator);
}
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkGetImageMemoryRequirements(VkDevice device,VkImage image,VkMemoryRequirements* requirements) {
    auto* state=findDevice(device);if(state&&state->imageMemoryRequirements)state->imageMemoryRequirements(device,image,requirements);
}
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkGetBufferMemoryRequirements(VkDevice device,VkBuffer buffer,VkMemoryRequirements* requirements) {
    auto* state=findDevice(device);if(state&&state->bufferMemoryRequirements)state->bufferMemoryRequirements(device,buffer,requirements);
}
OBS_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkAllocateMemory(VkDevice device,const VkMemoryAllocateInfo* info,const VkAllocationCallbacks* allocator,VkDeviceMemory* memory) {
    const auto trackingStarted=trackingCpuStart();
    auto* state=findDevice(device);if(!state||!state->allocateMemory)return VK_ERROR_INITIALIZATION_FAILED;
    TrackingCpuScope tracking(state,TrackingCpuGroup::ResourceLifetime,trackingStarted);
    const auto result=tracking.driver([&]{return state->allocateMemory(device,info,allocator,memory);});
    xess_vk::resource_memory_observer::memoryAllocated(state->resourceMemory,result==VK_SUCCESS?*memory:VK_NULL_HANDLE,info,result);return result;
}
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkFreeMemory(VkDevice device,VkDeviceMemory memory,const VkAllocationCallbacks* allocator) {
    const auto trackingStarted=trackingCpuStart();
    auto* state=findDevice(device);if(!state||!state->freeMemory)return;
    TrackingCpuScope tracking(state,TrackingCpuGroup::ResourceLifetime,trackingStarted);
    xess_vk::resource_memory_observer::memoryFreeing(state->resourceMemory,memory);tracking.driver([&]{state->freeMemory(device,memory,allocator);});
}
OBS_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkBindImageMemory(VkDevice device,VkImage image,VkDeviceMemory memory,VkDeviceSize offset) {
    auto* state=findDevice(device);if(!state||!state->bindImageMemory)return VK_ERROR_INITIALIZATION_FAILED;
    VkBindImageMemoryInfo info{VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO};info.image=image;info.memory=memory;info.memoryOffset=offset;
    const auto ticket=xess_vk::resource_memory_observer::beforeImageBind(state->resourceMemory,xess_vk::resource_memory_facts::BindApi::ImageCore,1,&info);
    const auto result=state->bindImageMemory(device,image,memory,offset);xess_vk::resource_memory_observer::afterBind(state->resourceMemory,ticket,result);return result;
}
OBS_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkBindBufferMemory(VkDevice device,VkBuffer buffer,VkDeviceMemory memory,VkDeviceSize offset) {
    auto* state=findDevice(device);if(!state||!state->bindBufferMemory)return VK_ERROR_INITIALIZATION_FAILED;
    VkBindBufferMemoryInfo info{VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO};info.buffer=buffer;info.memory=memory;info.memoryOffset=offset;
    const auto ticket=xess_vk::resource_memory_observer::beforeBufferBind(state->resourceMemory,xess_vk::resource_memory_facts::BindApi::BufferCore,1,&info);
    const auto result=state->bindBufferMemory(device,buffer,memory,offset);xess_vk::resource_memory_observer::afterBind(state->resourceMemory,ticket,result);return result;
}
OBS_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkBindImageMemory2(VkDevice device,std::uint32_t count,const VkBindImageMemoryInfo* infos) {
    auto* state=findDevice(device);if(!state||!state->bindImageMemory2)return VK_ERROR_EXTENSION_NOT_PRESENT;
    const auto ticket=xess_vk::resource_memory_observer::beforeImageBind(state->resourceMemory,xess_vk::resource_memory_facts::BindApi::ImageCore2,count,infos);
    const auto result=state->bindImageMemory2(device,count,infos);xess_vk::resource_memory_observer::afterBind(state->resourceMemory,ticket,result);return result;
}
OBS_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkBindImageMemory2KHR(VkDevice device,std::uint32_t count,const VkBindImageMemoryInfo* infos) {
    auto* state=findDevice(device);if(!state||!state->bindImageMemory2Khr)return VK_ERROR_EXTENSION_NOT_PRESENT;
    const auto ticket=xess_vk::resource_memory_observer::beforeImageBind(state->resourceMemory,xess_vk::resource_memory_facts::BindApi::ImageKhr2,count,infos);
    const auto result=state->bindImageMemory2Khr(device,count,infos);xess_vk::resource_memory_observer::afterBind(state->resourceMemory,ticket,result);return result;
}
OBS_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkBindBufferMemory2(VkDevice device,std::uint32_t count,const VkBindBufferMemoryInfo* infos) {
    auto* state=findDevice(device);if(!state||!state->bindBufferMemory2)return VK_ERROR_EXTENSION_NOT_PRESENT;
    const auto ticket=xess_vk::resource_memory_observer::beforeBufferBind(state->resourceMemory,xess_vk::resource_memory_facts::BindApi::BufferCore2,count,infos);
    const auto result=state->bindBufferMemory2(device,count,infos);xess_vk::resource_memory_observer::afterBind(state->resourceMemory,ticket,result);return result;
}
OBS_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkBindBufferMemory2KHR(VkDevice device,std::uint32_t count,const VkBindBufferMemoryInfo* infos) {
    auto* state=findDevice(device);if(!state||!state->bindBufferMemory2Khr)return VK_ERROR_EXTENSION_NOT_PRESENT;
    const auto ticket=xess_vk::resource_memory_observer::beforeBufferBind(state->resourceMemory,xess_vk::resource_memory_facts::BindApi::BufferKhr2,count,infos);
    const auto result=state->bindBufferMemory2Khr(device,count,infos);xess_vk::resource_memory_observer::afterBind(state->resourceMemory,ticket,result);return result;
}
OBS_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkQueueBindSparse(VkQueue queue,std::uint32_t count,const VkBindSparseInfo* infos,VkFence fence) {
    auto* state=findDevice(queue);if(!state||!state->bindSparse)return VK_ERROR_INITIALIZATION_FAILED;
    invalidatePacketCandidates(state,true);
    xess_vk::resource_memory_observer::coverageLost(state->resourceMemory);
    const auto result=queueDriver(state, "vkQueueBindSparse", queue, [&] { return state->bindSparse(queue,count,infos,fence); });
    const auto error=GetLastError();srGpuAfterSubmit(state,nullptr,result);SetLastError(error);return result;
}
OBS_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateSemaphore(VkDevice device, const VkSemaphoreCreateInfo* info,
    const VkAllocationCallbacks* allocator, VkSemaphore* semaphore) {
    auto* state = findDevice(device);
    if (!state || !state->createSemaphore) return VK_ERROR_INITIALIZATION_FAILED;
    const auto result = state->createSemaphore(device, info, allocator, semaphore);
    const auto originalError = GetLastError();
    if (state->packetIdentities && result == VK_SUCCESS && semaphore) {
        VkSemaphoreCreateInfo create{};
        bool eligible = packetRead(info, &create, sizeof(create)) &&
            create.sType == VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO && !create.flags;
        const auto* next = reinterpret_cast<const VkBaseInStructure*>(create.pNext);
        unsigned count{};
        while (eligible && next && count++ < 8) {
            VkBaseInStructure base{};
            if (!packetRead(next, &base, sizeof(base)) || base.sType != VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO) {
                eligible = false;
                break;
            }
            VkSemaphoreTypeCreateInfo type{};
            if (!packetRead(next, &type, sizeof(type)) || type.semaphoreType != VK_SEMAPHORE_TYPE_BINARY) {
                eligible = false;
                break;
            }
            next = base.pNext;
        }
        if (next) eligible = false;
        if (!state->packetIdentities->semaphoreCreated(bits(*semaphore), result, eligible))
            invalidatePacketCandidates(state);
    }
    SetLastError(originalError);
    return result;
}
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkDestroySemaphore(VkDevice device, VkSemaphore semaphore,
    const VkAllocationCallbacks* allocator) {
    auto* state = findDevice(device);
    if (!state || !state->destroySemaphore) return;
    const auto entryError = GetLastError();
    if (state->packetIdentities) state->packetIdentities->semaphoreDestroyed(bits(semaphore));
    invalidatePacketCandidates(state);
    if (state->presentation && state->presentation->retainsAppSemaphore(semaphore)) {
        JsonLine log("presentation_pending_app_wait_retained");
        log.handle("semaphore", bits(semaphore));
        log.boolean("original_forwarded", false);
        log.boolean("failedQuarantined", true);
        log.write();
        SetLastError(entryError);
        return;
    }
    state->destroySemaphore(device, semaphore, allocator);
}
OBS_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateImage(VkDevice device, const VkImageCreateInfo* info,
    const VkAllocationCallbacks* allocator, VkImage* image) {
    const auto trackingStarted = trackingCpuStart();
    auto* state = findDevice(device); if (!state || !state->createImage) return VK_ERROR_INITIALIZATION_FAILED;
    TrackingCpuScope tracking(state, TrackingCpuGroup::ResourceLifetime, trackingStarted);
    VkImageCreateInfo augmented{};
    if (state->continuous && info && state->continuousImageProperties && !info->pNext &&
        (info->format == VK_FORMAT_D32_SFLOAT_S8_UINT || info->format == VK_FORMAT_R16G16_SFLOAT) &&
        info->imageType == VK_IMAGE_TYPE_2D && info->tiling == VK_IMAGE_TILING_OPTIMAL &&
        !(info->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {
        augmented = *info;
        augmented.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        VkImageFormatProperties properties{};
        if (state->continuousImageProperties(state->physical, augmented.format, augmented.imageType,
            augmented.tiling, augmented.usage, augmented.flags, &properties) == VK_SUCCESS) {
            info = &augmented;
        }
    }
    const VkResult result = tracking.driver([&] { return state->createImage(device, info, allocator, image); });
    const DWORD originalError=GetLastError();VkMemoryRequirements actualRequirements{};bool queriedRequirements=false;
    if(result==VK_SUCCESS&&state->resourceMemory&&state->imageMemoryRequirements&&xess_vk::resource_memory_observer::imageRequirementsQuerySupported(info)){
        state->imageMemoryRequirements(device,*image,&actualRequirements);queriedRequirements=true;
    }
    SetLastError(originalError);
    if (result == VK_SUCCESS && state->imageInventory) {
        // Keep both inventories in the same successful-create observation order.
        AcquireSRWLockExclusive(&stateLock);
        try {
            state->imageInventory->created(bits(*image), {
                static_cast<std::uint32_t>(info->format), info->extent.width, info->extent.height, info->extent.depth,
                info->mipLevels, info->arrayLayers, static_cast<std::uint32_t>(info->samples), info->usage,
                info->flags, static_cast<std::uint32_t>(info->tiling), static_cast<std::uint32_t>(info->sharingMode),
                static_cast<std::uint32_t>(info->initialLayout)});
            const auto snapshot = state->imageInventory->lookup(bits(*image));
            if (snapshot.status == xess_vk::image_inventory::Status::LiveHandleObserved)
                xess_vk::descriptor_image_observer::imageObserved(state->descriptors, snapshot.record);
            else xess_vk::descriptor_image_observer::retire(state->descriptors);
            xess_vk::render_pass_observer::imageCreated(state->renderPass, info, *image, result,
                snapshot.status == xess_vk::image_inventory::Status::LiveHandleObserved ? &snapshot.record : nullptr);
            if(snapshot.status==xess_vk::image_inventory::Status::LiveHandleObserved)
                xess_vk::resource_memory_observer::imageCreated(state->resourceMemory,snapshot.record,info,result,queriedRequirements?&actualRequirements:nullptr);
            else xess_vk::resource_memory_observer::coverageLost(state->resourceMemory);
        } catch (...) {
            try { state->imageInventory->stop(); } catch (...) {}
            xess_vk::render_pass_observer::retire(state->renderPass);
            xess_vk::descriptor_image_observer::retire(state->descriptors);
            xess_vk::resource_memory_observer::coverageLost(state->resourceMemory);
        }
        ReleaseSRWLockExclusive(&stateLock);
    }
    const auto count = InterlockedIncrement64(&state->imageCreates);
    if (count == 4097 && state->researchDiagnostics) {
        JsonLine limit("image_create_log_limit"); limit.handle("device", bits(device)); limit.number("call", count);
        limit.text("remaining_policy", "up_to_4096_large_render_or_storage_images"); limit.write();
    }
    // Startup can allocate thousands of 1x1 placeholders. Retain a separate,
    // bounded budget for later render targets when the player enters a scene.
    const bool lateTarget = count > 4096 && info->extent.width >= 64 && info->extent.height >= 64 &&
        (info->usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT));
    const bool keepLate = lateTarget && InterlockedIncrement64(&state->lateRenderImages) <= 4096;
    if (state->researchDiagnostics && (count <= 4096 || keepLate)) {
        JsonLine log("create_image");
        log.handle("device", bits(device)); log.number("call", count); log.number("result", result);
        log.boolean("late_render_sample", keepLate);
        {
            if (result == VK_SUCCESS) log.handle("image", bits(*image));
            log.number("format", info->format); log.number("width", info->extent.width);
            log.number("height", info->extent.height); log.number("depth", info->extent.depth);
            log.number("mip_levels", info->mipLevels); log.number("array_layers", info->arrayLayers);
            log.number("samples", info->samples); log.number("usage", info->usage);
            log.number("flags", info->flags); log.number("tiling", info->tiling);
            log.number("sharing_mode", info->sharingMode); log.number("initial_layout", info->initialLayout);
        }
        log.write();
    }
    SetLastError(originalError);return result;
}
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkDestroyImage(VkDevice device, VkImage image, const VkAllocationCallbacks* allocator) {
    const auto trackingStarted = trackingCpuStart();
    auto* state = findDevice(device); if (!state || !state->destroyImage) return;
    TrackingCpuScope tracking(state, TrackingCpuGroup::ResourceLifetime, trackingStarted);
    if(state->srRuntime)state->srRuntime->replacementImageRetired(image);
    // Order inventory removal and role retirement against the FSR metadata
    // callback. Never hold this lock across the driver call or diagnostic I/O.
    AcquireSRWLockExclusive(&stateLock);
    xess_vk::render_pass_observer::imageDestroying(state->renderPass, image);
    xess_vk::resource_memory_observer::imageDestroying(state->resourceMemory,image);
    if (state->imageInventory) {
        try {
            const auto snapshot=state->imageInventory->lookup(bits(image));
            if (snapshot.status==xess_vk::image_inventory::Status::LiveHandleObserved)
                xess_vk::descriptor_image_observer::imageRetiring(state->descriptors, {snapshot.record.image,snapshot.record.generation});
            state->imageInventory->destroying(bits(image));
        } catch (...) {
            try { state->imageInventory->stop(); } catch (...) {}
            xess_vk::descriptor_image_observer::retire(state->descriptors);
        }
    }
    if constexpr(researchDiagnostics) xess_vk::candidate_sync::destroyingImage(state->candidateSync, bits(image));
    ReleaseSRWLockExclusive(&stateLock);
    tracking.driver([&] { state->destroyImage(device, image, allocator); });
    const auto count = InterlockedIncrement64(&state->imageDestroys);
    if (!state->researchDiagnostics) return;
    if (count <= 32768) { JsonLine log("destroy_image"); log.handle("device", bits(device)); log.handle("image", bits(image)); log.write(); }
    else if (count == 32769) { JsonLine log("image_destroy_log_limit"); log.handle("device", bits(device)); log.number("call", count); log.write(); }
}
OBS_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateImageView(VkDevice device, const VkImageViewCreateInfo* info,
    const VkAllocationCallbacks* allocator, VkImageView* object) {
    auto* state = findDevice(device); if (!state || !state->createImageView) return VK_ERROR_INITIALIZATION_FAILED;
    const auto result = state->createImageView(device, info, allocator, object);
    xess_vk::render_pass_observer::viewCreated(state->renderPass, info, result == VK_SUCCESS ? *object : VK_NULL_HANDLE, result);
    xess_vk::descriptor_image_observer::viewCreated(state->descriptors, result == VK_SUCCESS ? *object : VK_NULL_HANDLE, info, result);
    return result;
}
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkDestroyImageView(VkDevice device, VkImageView object,
    const VkAllocationCallbacks* allocator) {
    auto* state = findDevice(device); if (!state || !state->destroyImageView) return;
    xess_vk::render_pass_observer::viewDestroying(state->renderPass, object);
    xess_vk::descriptor_image_observer::viewDestroying(state->descriptors, object);
    state->destroyImageView(device, object, allocator);
}
OBS_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateRenderPass(VkDevice device, const VkRenderPassCreateInfo* info,
    const VkAllocationCallbacks* allocator, VkRenderPass* object) {
    const auto trackingStarted = trackingCpuStart();
    auto* state = findDevice(device); if (!state || !state->createRenderPass) return VK_ERROR_INITIALIZATION_FAILED;
    TrackingCpuScope tracking(state, TrackingCpuGroup::Recording, trackingStarted);
    VkRenderPassCreateInfo copy{}; std::vector<VkAttachmentDescription> attachments;
    const VkRenderPassCreateInfo* driverInfo{};
    try { driverInfo = xess_vk::presentation::fake_layout::renderPass(state->fakeEnabled, info, copy, attachments); }
    catch (...) { fakeRecordingFailed(state); return VK_ERROR_OUT_OF_HOST_MEMORY; }
    const auto result = tracking.driver([&] { return state->createRenderPass(device, driverInfo, allocator, object); });
    xess_vk::render_pass_observer::renderPassCreated(state->renderPass, info, result == VK_SUCCESS ? *object : VK_NULL_HANDLE, result);
    return result;
}
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkDestroyRenderPass(VkDevice device, VkRenderPass object,
    const VkAllocationCallbacks* allocator) {
    const auto trackingStarted = trackingCpuStart();
    auto* state = findDevice(device); if (!state || !state->destroyRenderPass) return;
    TrackingCpuScope tracking(state, TrackingCpuGroup::Recording, trackingStarted);
    xess_vk::render_pass_observer::renderPassDestroying(state->renderPass, object);
    tracking.driver([&] { return state->destroyRenderPass(device, object, allocator); });
}
OBS_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateRenderPass2(VkDevice device, const VkRenderPassCreateInfo2* info,
    const VkAllocationCallbacks* allocator, VkRenderPass* object) {
    const auto trackingStarted = trackingCpuStart();
    auto* state = findDevice(device); if (!state || !state->createRenderPass2) return VK_ERROR_INITIALIZATION_FAILED;
    TrackingCpuScope tracking(state, TrackingCpuGroup::Recording, trackingStarted);
    VkRenderPassCreateInfo2 copy{}; std::vector<VkAttachmentDescription2> attachments;
    const VkRenderPassCreateInfo2* driverInfo{};
    try { driverInfo = xess_vk::presentation::fake_layout::renderPass(state->fakeEnabled, info, copy, attachments); }
    catch (...) { fakeRecordingFailed(state); return VK_ERROR_OUT_OF_HOST_MEMORY; }
    const auto result = tracking.driver([&] { return state->createRenderPass2(device, driverInfo, allocator, object); });
    xess_vk::render_pass_observer::renderPass2Created(state->renderPass, info, result == VK_SUCCESS ? *object : VK_NULL_HANDLE, result);
    return result;
}
OBS_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateRenderPass2KHR(VkDevice device, const VkRenderPassCreateInfo2* info,
    const VkAllocationCallbacks* allocator, VkRenderPass* object) {
    const auto trackingStarted = trackingCpuStart();
    auto* state = findDevice(device); if (!state || !state->createRenderPass2Khr) return VK_ERROR_INITIALIZATION_FAILED;
    TrackingCpuScope tracking(state, TrackingCpuGroup::Recording, trackingStarted);
    VkRenderPassCreateInfo2 copy{}; std::vector<VkAttachmentDescription2> attachments;
    const VkRenderPassCreateInfo2* driverInfo{};
    try { driverInfo = xess_vk::presentation::fake_layout::renderPass(state->fakeEnabled, info, copy, attachments); }
    catch (...) { fakeRecordingFailed(state); return VK_ERROR_OUT_OF_HOST_MEMORY; }
    const auto result = tracking.driver([&] { return state->createRenderPass2Khr(device, driverInfo, allocator, object); });
    xess_vk::render_pass_observer::renderPass2Created(state->renderPass, info, result == VK_SUCCESS ? *object : VK_NULL_HANDLE, result);
    return result;
}
OBS_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateFramebuffer(VkDevice device, const VkFramebufferCreateInfo* info,
    const VkAllocationCallbacks* allocator, VkFramebuffer* object) {
    const auto trackingStarted = trackingCpuStart();
    auto* state = findDevice(device); if (!state || !state->createFramebuffer) return VK_ERROR_INITIALIZATION_FAILED;
    TrackingCpuScope tracking(state, TrackingCpuGroup::Recording, trackingStarted);
    const auto result = tracking.driver([&] { return state->createFramebuffer(device, info, allocator, object); });
    xess_vk::render_pass_observer::framebufferCreated(state->renderPass, info, result == VK_SUCCESS ? *object : VK_NULL_HANDLE, result);
    return result;
}
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkDestroyFramebuffer(VkDevice device, VkFramebuffer object,
    const VkAllocationCallbacks* allocator) {
    const auto trackingStarted = trackingCpuStart();
    auto* state = findDevice(device); if (!state || !state->destroyFramebuffer) return;
    TrackingCpuScope tracking(state, TrackingCpuGroup::Recording, trackingStarted);
    xess_vk::render_pass_observer::framebufferDestroying(state->renderPass, object);
    tracking.driver([&] { return state->destroyFramebuffer(device, object, allocator); });
}
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkGetDeviceQueue(VkDevice device, uint32_t family, uint32_t index, VkQueue* queue) {
    auto* state = findDevice(device); if (!state || !state->getQueue) return;
    state->getQueue(device, family, index, queue);
    const auto originalError=GetLastError();rawQueueObserved(state,*queue,family,index,0,true);
    if constexpr(researchDiagnostics) xess_vk::candidate_sync::queue(state->candidateSync, *queue, family);
    xess_vk::render_pass_observer::queue(state->renderPass, *queue, family);
    JsonLine log("get_device_queue"); log.handle("device", bits(device)); log.handle("queue", bits(*queue));
    log.number("family", family); log.number("index", index); log.write();SetLastError(originalError);
}
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkGetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2* info, VkQueue* queue) {
    auto* state = findDevice(device); if (!state || !state->getQueue2) return;
    state->getQueue2(device, info, queue);
    const auto originalError=GetLastError();rawQueueObserved(state,*queue,info->queueFamilyIndex,info->queueIndex,info->flags,info->sType==VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2&&!info->pNext);
    if constexpr(researchDiagnostics) xess_vk::candidate_sync::queue(state->candidateSync, *queue, info->queueFamilyIndex);
    xess_vk::render_pass_observer::queue(state->renderPass, *queue, info->queueFamilyIndex);
    JsonLine log("get_device_queue2"); log.handle("device", bits(device)); log.handle("queue", bits(*queue));
    log.number("family", info->queueFamilyIndex); log.number("index", info->queueIndex); log.number("flags", info->flags); log.write();SetLastError(originalError);
}
OBS_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkGetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR swapchain, uint32_t* count, VkImage* images) {
    auto* state = findDevice(device); if (!state || (!state->fakeEnabled && !state->swapchainImages)) return VK_ERROR_EXTENSION_NOT_PRESENT;
    if (state->fakeEnabled && !state->presentation) return VK_ERROR_OUT_OF_DATE_KHR;
    const uint32_t capacity = images && count ? *count : 0;
    xess_vk::wsi_image_inventory::EnumerationTicket wsiTicket;
    if (state->wsiInventory) {
        AcquireSRWLockExclusive(&stateLock);
        wsiTicket = state->wsiInventory->beforeGetImages(bits(swapchain), images != nullptr, capacity);
        ReleaseSRWLockExclusive(&stateLock);
    }
    const auto result = state->presentation ? state->presentation->images(swapchain, count, images) : state->swapchainImages(device, swapchain, count, images);
    if (state->wsiInventory) {
        xess_vk::wsi_image_inventory::Enumeration enumeration;
        enumeration.driverResult = result;
        std::array<std::uint64_t, xess_vk::wsi_image_inventory::maximumImages> copied{};
        if ((result == VK_SUCCESS || result == VK_INCOMPLETE) && count) {
            enumeration.countCopied = true; enumeration.returnedCount = *count;
            if (images && *count <= capacity && *count <= copied.size()) {
                for (std::uint32_t i = 0; i < *count; ++i) copied[i] = bits(images[i]);
                enumeration.images = {copied.data(), *count};
            }
        }
        AcquireSRWLockExclusive(&stateLock);
        const auto changes = state->wsiInventory->afterGetImages(wsiTicket, enumeration);
        applyWsiChanges(state, changes);
        ReleaseSRWLockExclusive(&stateLock);
        reportWsiChanges(state, "enumerate", changes);
    }
    if ((result == VK_SUCCESS || result == VK_INCOMPLETE) && images && count) {
        JsonLine log("swapchain_images"); log.handle("device", bits(device)); log.handle("swapchain", bits(swapchain));
        log.number("count", *count); log.number("result", result); log.key("images"); log.raw("[");
        for (uint32_t i = 0; i < *count && i < capacity && i < 16; ++i) {
            if (i) log.raw(","); char value[24]; std::snprintf(value, sizeof(value), "0x%llx", static_cast<unsigned long long>(bits(images[i]))); log.quoted(value);
        }
        log.raw("]"); log.write();
    }
    return result;
}
OBS_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo* infos, VkFence fence) {
    const auto trackingStarted = trackingCpuStart();
    auto* state = findDevice(queue); if (!state || !state->submit) return VK_ERROR_INITIALIZATION_FAILED;
    if(srSdkDepth){return state->submit(queue,count,infos,fence);}
    if (const auto error = fakeRecordingError(state); error != VK_SUCCESS) return error;
    TrackingCpuScope tracking(state, TrackingCpuGroup::Recording, trackingStarted);
    const auto entryError=GetLastError();borrowed::SubmitTicket rawTicket;
    {RawOperation operation(state,true);if(operation.acquired)rawTicket=rawBeforeSubmit(state,queue,count,infos,fence);}
    const auto ticket = researchDiagnostics ? xess_vk::candidate_sync::beginSubmit(state->candidateSync, queue) : xess_vk::candidate_sync::SubmitTicket{};
    const auto rpTicket = xess_vk::render_pass_observer::beginSubmit(state->renderPass, queue, count, infos);
    const auto tailTicket=renderTailBeforeSubmit(state,rpTicket);
    bool continuousBatch{};
    if (state->continuous) {
        for (std::uint32_t i = 0; i < rpTicket.commandCount; ++i) {
            const auto& recording = rpTicket.recordingFacts[i];
            continuousBatch |= state->continuous->containsRecording({recording.command, recording.rpRecordingGeneration});
        }
    }
    std::vector<VkSubmitInfo> widened;
    std::vector<std::vector<VkPipelineStageFlags>> stages;
    if (continuousBatch && infos) {
        try {
            widened.assign(infos, infos + count);
            stages.resize(count);
            for (std::uint32_t i = 0; i < count; ++i) {
                stages[i].assign(infos[i].waitSemaphoreCount, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
                widened[i].pWaitDstStageMask = stages[i].data();
            }
        } catch (...) { SetLastError(entryError); return VK_ERROR_OUT_OF_HOST_MEMORY; }
    }
    const auto packetSnapshot = snapshotSubmit(count, infos);
    auto* originSubmit = beginOriginSubmit(state, queue, packetSnapshot);
    const auto packetSubmission = beginPacketSubmit(state, packetSnapshot);
    SetLastError(entryError);
    std::uint64_t sourceEntryNs{},sourceEntryQpc{};xess_vk::measurement::Scope sourceMeasurement(state->measurements);
    std::optional<SrGpuSubmit> srGpuTicket;
    xess_vk::sr_gpu_timestamps::SubmitTicket srRuntimeTiming;
    const auto result = tracking.driver([&] { return queueDriver(state, "vkQueueSubmit", queue, [&] {
        if(state->srRuntime)srRuntimeTiming=state->srRuntime->beforeSubmit(rpTicket);
        if(state->srGpuRequested)srGpuTicket=srGpuBeforeSubmit(state,rpTicket);
        if(state->measurements){LARGE_INTEGER qpc{};QueryPerformanceCounter(&qpc);sourceEntryQpc=qpc.QuadPart;sourceEntryNs=xess_vk::cpuWallNowNs();}
        return state->submit(queue, count, widened.empty() ? infos : widened.data(), fence); }); });
    const auto originalError = GetLastError();
    srGpuAfterSubmit(state,srGpuTicket?&*srGpuTicket:nullptr,result);
    if(state->srRuntime)state->srRuntime->submit(rpTicket,result,srRuntimeTiming);
    if(result==VK_ERROR_DEVICE_LOST)xess_vk::measurement::increment(xess_vk::measurement::Counter::DeviceLost);
    finishOriginSubmit(originSubmit, result);
    if (state->packetIdentities && !state->packetIdentities->completeSignals(packetSubmission, result))
        invalidatePacketCandidates(state);
    if (continuousBatch) {
        const auto serial = InterlockedIncrement64(&state->continuousSubmissions);
        std::uint32_t commandIndex{};
        for (std::uint32_t batch = 0; batch < count; ++batch) {
            const auto& info = infos[batch];
            for (std::uint32_t i = 0; i < info.commandBufferCount && commandIndex < rpTicket.commandCount; ++i, ++commandIndex) {
                const auto& recording = rpTicket.recordingFacts[commandIndex];
                state->continuous->submitted({recording.command, recording.rpRecordingGeneration},
                    {info.pSignalSemaphores, info.signalSemaphoreCount}, queue, serial, result,sourceEntryNs,sourceEntryQpc);
            }
        }
    }
    {RawOperation operation(state,true);if constexpr(researchDiagnostics)if(operation.acquired&&state->rawProbe)raw::afterOriginalResult(state->rawProbe,rawTicket,result,rawWindow(state,queue));}
    xess_vk::render_pass_observer::finishSubmit(state->renderPass, rpTicket, fence, result);
    renderTailAfterSubmit(state,rpTicket,tailTicket,result);
    for (std::uint32_t index = 0; continuousWindowActive(state) && index < rpTicket.commandCount; ++index)
        continuousCommandWindow(state, "continuous_window_submit", rpTicket.recordingFacts[index], result, false);
    if constexpr(researchDiagnostics) xess_vk::candidate_sync::submit(state->candidateSync, ticket, count, infos, fence, result);
    const auto call = InterlockedIncrement64(&state->submits);
    if (call <= 32 || call % 300 == 0) for (uint32_t i = 0; i < count && i < 16; ++i) {
        JsonLine log("queue_submit_sample"); log.handle("device", bits(state->device)); log.handle("queue", bits(queue));
        log.handle("fence", bits(fence)); log.number("call", call); log.number("batch", i); log.number("result", result);
        log.number("wait_count", infos[i].waitSemaphoreCount); log.number("signal_count", infos[i].signalSemaphoreCount);
        log.number("command_buffer_count", infos[i].commandBufferCount); log.key("command_buffers"); log.raw("[");
        for (uint32_t j = 0; j < infos[i].commandBufferCount && j < 16; ++j) {
            if (j) log.raw(","); char value[24]; std::snprintf(value, sizeof(value), "0x%llx", static_cast<unsigned long long>(bits(infos[i].pCommandBuffers[j]))); log.quoted(value);
        }
        log.raw("]"); log.write();
    }
    SetLastError(originalError);return result;
}
namespace {
VkResult submit2Observed(DeviceState* state, PFN_vkQueueSubmit2 submit, const char* api, VkQueue queue, uint32_t count, const VkSubmitInfo2* infos, VkFence fence,
                         xess_vk::CpuWallClock::time_point trackingStarted) noexcept {
    if (!state || !submit) return VK_ERROR_EXTENSION_NOT_PRESENT;
    if(srSdkDepth)return submit(queue,count,infos,fence);
    if (const auto error = fakeRecordingError(state); error != VK_SUCCESS) return error;
    TrackingCpuScope tracking(state, TrackingCpuGroup::Recording, trackingStarted);
    const auto entryError=GetLastError();borrowed::SubmitTicket rawTicket;
    {RawOperation operation(state,true);if(operation.acquired)rawTicket=rawBeforeSubmit(state,queue,count,infos,fence);}
    const auto ticket = researchDiagnostics ? xess_vk::candidate_sync::beginSubmit(state->candidateSync, queue) : xess_vk::candidate_sync::SubmitTicket{};
    const auto rpTicket = xess_vk::render_pass_observer::beginSubmit2(state->renderPass, queue, count, infos);
    const auto tailTicket=renderTailBeforeSubmit(state,rpTicket);
    bool continuousBatch{};
    if (state->continuous) {
        for (std::uint32_t i = 0; i < rpTicket.commandCount; ++i) {
            const auto& recording = rpTicket.recordingFacts[i];
            continuousBatch |= state->continuous->containsRecording({recording.command, recording.rpRecordingGeneration});
        }
    }
    std::vector<VkSubmitInfo2> widened;
    std::vector<std::vector<VkSemaphoreSubmitInfo>> waits;
    std::vector<std::vector<VkSemaphoreSubmitInfo>> signals;
    if (continuousBatch && infos) {
        try {
            widened.assign(infos, infos + count);
            waits.resize(count);
            signals.resize(count);
            for (std::uint32_t i = 0; i < count; ++i) {
                if (infos[i].waitSemaphoreInfoCount) waits[i].assign(infos[i].pWaitSemaphoreInfos, infos[i].pWaitSemaphoreInfos + infos[i].waitSemaphoreInfoCount);
                if (infos[i].signalSemaphoreInfoCount) signals[i].assign(infos[i].pSignalSemaphoreInfos, infos[i].pSignalSemaphoreInfos + infos[i].signalSemaphoreInfoCount);
                for (auto& wait : waits[i]) wait.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                for (auto& signal : signals[i]) signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                widened[i].pWaitSemaphoreInfos = waits[i].data();
                widened[i].pSignalSemaphoreInfos = signals[i].data();
            }
        } catch (...) { SetLastError(entryError); return VK_ERROR_OUT_OF_HOST_MEMORY; }
    }
    const auto packetSubmission = beginPacketSubmit(state, snapshotSubmit(count, infos));
    SetLastError(entryError);
    std::uint64_t sourceEntryNs{},sourceEntryQpc{};xess_vk::measurement::Scope sourceMeasurement(state->measurements);
    std::optional<SrGpuSubmit> srGpuTicket;
    xess_vk::sr_gpu_timestamps::SubmitTicket srRuntimeTiming;
    const auto result = tracking.driver([&] { return queueDriver(state, api, queue, [&] {
        if(state->srRuntime)srRuntimeTiming=state->srRuntime->beforeSubmit(rpTicket);
        if(state->srGpuRequested)srGpuTicket=srGpuBeforeSubmit(state,rpTicket);
        if(state->measurements){LARGE_INTEGER qpc{};QueryPerformanceCounter(&qpc);sourceEntryQpc=qpc.QuadPart;sourceEntryNs=xess_vk::cpuWallNowNs();}
        return submit(queue, count, widened.empty() ? infos : widened.data(), fence); }); });
    const auto originalError = GetLastError();
    srGpuAfterSubmit(state,srGpuTicket?&*srGpuTicket:nullptr,result);
    if(state->srRuntime)state->srRuntime->submit(rpTicket,result,srRuntimeTiming);
    if(result==VK_ERROR_DEVICE_LOST)xess_vk::measurement::increment(xess_vk::measurement::Counter::DeviceLost);
    if (state->packetIdentities && !state->packetIdentities->completeSignals(packetSubmission, result))
        invalidatePacketCandidates(state);
    if (continuousBatch) {
        const auto serial = InterlockedIncrement64(&state->continuousSubmissions);
        std::uint32_t commandIndex{};
        for (std::uint32_t batch = 0; batch < count; ++batch) {
            const auto& info = infos[batch];
            std::array<VkSemaphore, xess_vk::continuous::maximumSignals> signals{};
            const auto signalCount = info.signalSemaphoreInfoCount <= signals.size() ? info.signalSemaphoreInfoCount : 0;
            for (std::uint32_t i = 0; i < signalCount; ++i) signals[i] = info.pSignalSemaphoreInfos[i].semaphore;
            for (std::uint32_t i = 0; i < info.commandBufferInfoCount && commandIndex < rpTicket.commandCount; ++i, ++commandIndex) {
                const auto& recording = rpTicket.recordingFacts[commandIndex];
                state->continuous->submitted({recording.command, recording.rpRecordingGeneration},
                    {signals.data(), signalCount}, queue, serial, result,sourceEntryNs,sourceEntryQpc);
            }
        }
    }
    {RawOperation operation(state,true);if constexpr(researchDiagnostics)if(operation.acquired&&state->rawProbe)raw::afterOriginalResult(state->rawProbe,rawTicket,result,rawWindow(state,queue));}
    xess_vk::render_pass_observer::finishSubmit(state->renderPass, rpTicket, fence, result);
    renderTailAfterSubmit(state,rpTicket,tailTicket,result);
    for (std::uint32_t index = 0; continuousWindowActive(state) && index < rpTicket.commandCount; ++index)
        continuousCommandWindow(state, "continuous_window_submit2", rpTicket.recordingFacts[index], result, false);
    if constexpr(researchDiagnostics) xess_vk::candidate_sync::submit2(state->candidateSync, ticket, count, infos, fence, result);
    const auto call = InterlockedIncrement64(&state->submits);
    if (call <= 32 || call % 300 == 0) for (uint32_t i = 0; i < count && i < 16; ++i) {
        JsonLine log("queue_submit2_sample"); log.handle("device", bits(state->device)); log.handle("queue", bits(queue));
        log.handle("fence", bits(fence)); log.number("call", call); log.number("batch", i); log.number("result", result);
        log.number("wait_count", infos[i].waitSemaphoreInfoCount); log.number("signal_count", infos[i].signalSemaphoreInfoCount);
        log.number("command_buffer_count", infos[i].commandBufferInfoCount); log.key("command_buffers"); log.raw("[");
        for (uint32_t j = 0; j < infos[i].commandBufferInfoCount && j < 16; ++j) {
            if (j) log.raw(","); char value[24]; std::snprintf(value, sizeof(value), "0x%llx", static_cast<unsigned long long>(bits(infos[i].pCommandBufferInfos[j].commandBuffer))); log.quoted(value);
        }
        log.raw("]"); log.write();
    }
    SetLastError(originalError);return result;
}
}
OBS_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit2(VkQueue queue, uint32_t count, const VkSubmitInfo2* infos, VkFence fence) {
    const auto started = trackingCpuStart();
    auto* state = findDevice(queue); return submit2Observed(state, state ? state->submit2 : nullptr, "vkQueueSubmit2", queue, count, infos, fence, started);
}
OBS_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit2KHR(VkQueue queue, uint32_t count, const VkSubmitInfo2* infos, VkFence fence) {
    const auto started = trackingCpuStart();
    auto* state = findDevice(queue); return submit2Observed(state, state ? state->submit2Khr : nullptr, "vkQueueSubmit2KHR", queue, count, infos, fence, started);
}
OBS_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkQueueWaitIdle(VkQueue queue) {
    auto* state=findDevice(queue);if(!state||!state->queueWaitIdle)return VK_ERROR_INITIALIZATION_FAILED;
    if(srSdkDepth)return state->queueWaitIdle(queue);
    const auto result=queueDriver(state, "vkQueueWaitIdle", queue, [&] { return state->queueWaitIdle(queue); });const auto error=GetLastError();RawOperation operation(state,true);
    if constexpr(researchDiagnostics)if(operation.acquired&&state->rawProbe){raw::queueIdle(state->rawProbe,queue,result);raw::poll(state->rawProbe);}SetLastError(error);return result;
}
OBS_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkDeviceWaitIdle(VkDevice device) {
    auto* state=findDevice(device);if(!state||!state->deviceWaitIdle)return VK_ERROR_INITIALIZATION_FAILED;
    if(srSdkDepth)return state->deviceWaitIdle(device);
    const auto result=queueDriver(state, "vkDeviceWaitIdle", VK_NULL_HANDLE, [&] { return state->deviceWaitIdle(device); });const auto error=GetLastError();RawOperation operation(state,true);
    if constexpr(researchDiagnostics)if(operation.acquired&&state->rawProbe){raw::deviceIdle(state->rawProbe,device,result);raw::poll(state->rawProbe);}
    srGpuDeviceIdle(state,result);if(state->srRuntime)state->srRuntime->idle(result);SetLastError(error);return result;
}

// These hooks preserve all downstream arguments and results. The optional
// context observes CPU recording metadata after the application call; it does
// not establish that a command buffer was submitted or executed by the GPU.

// Dedicated FSR-tail CPU observation. No GPU work or whole-device command trace.
namespace {
namespace stc=xess_vk::sr_tail_commands;
struct SrTailInjectedScope {SrTailInjectedScope(){++srTailInjectedDepth;}~SrTailInjectedScope(){--srTailInjectedDepth;}};
struct SrTailErrorGuard {DWORD error{GetLastError()};~SrTailErrorGuard(){SetLastError(error);}};
#ifdef XESS_METADATA_CONSUMER_TEST
int srTailWindowOverride=-1;
void (*srTailTestSink)(const char*){};
const xess_vk::sr_observation::DispatchTicket* srTailDispatchOverride{};
void (*srGpuTestSink)(const char*){};
#endif
namespace sgt=xess_vk::sr_gpu_timestamps;
bool srTailWindowActive(DeviceState*) noexcept;
void srTailUnsigned(JsonLine&,std::uint64_t) noexcept;
struct SrGpuWrite {std::uint64_t window{};sgt::WriteTicket ticket;};
void srGpuWriteLog(JsonLine& line) noexcept {
#ifdef XESS_METADATA_CONSUMER_TEST
    if(srGpuTestSink){line.raw("}\n");if(line.good){line.buffer[line.used]=0;srGpuTestSink(line.buffer);}return;}
#endif
    line.writeObservation();
}
void srGpuStats(DeviceState* state,const char* reason,bool terminated=false) noexcept {
    if(!state->srGpu)return;const auto stats=state->srGpu->stats();
    JsonLine line("sr_gpu_timestamp_stats");line.handle("device",bits(state->device));line.number("window_id",state->srGpuWindow);line.text("reason",reason);
    line.number("recordings",stats.recordings);line.number("passes",stats.passes);line.number("pools_created",stats.creates);line.number("pools_destroyed",stats.destroys);
    line.number("successful_executions",stats.successfulExecutions);line.number("replays",stats.replays);line.number("failed_submits",stats.failedSubmits);
    line.number("query_reads",stats.reads);line.number("samples",stats.samples);line.number("unavailable",stats.unavailable);line.number("duration_unknown",stats.durationUnknown);
    line.number("retired",stats.retired);line.number("discarded_unsubmitted",stats.discardedUnsubmitted);line.number("retained_at_stop",stats.retainedAtStop);
    line.number("rejected_recording_reasons",stats.rejectedRecordingReasons);
    static constexpr const char* reasons[]{"disabled","identity_or_extent_unknown","unsupported_recording","recording_capacity","pass_capacity",
        "query_pool_create_failed","unclosed_pass","stale_ticket","submission_refused","unknown_association"};
    static_assert(std::size(reasons)==std::size_t(sgt::Skip::Count));
    line.key("skip_counts");line.raw("{");for(std::size_t n=0;n<std::size(reasons);++n){if(n)line.raw(",");line.quoted(reasons[n]);line.raw(":");srTailUnsigned(line,stats.skipped[n]);}line.raw("}");
    line.boolean("drained",state->srGpu->drained());line.boolean("device_terminated",terminated);
    line.boolean("native_idle_completion_observed",state->srGpuIdleComplete);
    line.boolean("unread_samples_possible",!state->srGpu->drained());line.boolean("wait_added",false);line.boolean("whole_fsr_coverage_verified",false);srGpuWriteLog(line);
}
void srGpuSample(void* user,const sgt::Sample& sample) noexcept {
    auto* state=static_cast<DeviceState*>(user);const auto& i=sample.identity;const auto& f=sample.recording;
    JsonLine line("sr_gpu_timestamp_sample");line.handle("device",bits(state->device));line.number("window_id",i.window);
    line.text("pass",i.pass==sgt::Pass::Accumulate?"accumulate":"rcas");line.text("association","matched_resource_signature_candidate");
    line.handle("command",f.command);line.number("recording_generation",f.rpRecordingGeneration);line.number("context_incarnation",f.contextIncarnation);
    line.number("callback",i.callback);line.number("record_ordinal",i.ordinal);line.handle("pipeline",i.pipeline);line.number("pipeline_generation",i.pipelineGeneration);
    line.handle("queue",sample.queue);line.number("queue_family",f.queueFamily);line.number("execution",sample.execution);line.boolean("replay",sample.replay);
    line.text("completion",sample.completion==sgt::Completion::RecordingRetired?"actual_recording_retirement":
        sample.completion==sgt::Completion::SequentialReplay?"before_legal_sequential_replay":"native_device_idle_success");
    line.number("engine_frame",i.frame.engineFrame);line.number("sdk_present_id",i.frame.ticket.sdkId);line.number("ticket_generation",i.frame.ticket.contextGeneration);
    line.boolean("frame_known",i.frame.valid&&bool(i.frame.ticket));line.text("frame_scope","original_matched_recording_not_new_replay_frame");
    line.number("render_width",i.render.width);line.number("render_height",i.render.height);line.number("output_width",i.output.width);line.number("output_height",i.output.height);
    line.text("extent_source","matched_148_byte_constants_and_typed_live_images");line.text("extent_scope","original_matched_callback_not_reread_at_execution");
    line.key("groups");line.raw("[");
    for(unsigned n=0;n<3;++n){if(n)line.raw(",");srTailUnsigned(line,i.groups[n]);}line.raw("]");
    gpuTimestampFields(line,"pass_gpu",sample.gpu);line.text("duration_scope","top_bottom_candidate_pass_bracket");
    line.boolean("isolated_shader_cost",false);line.boolean("whole_fsr_coverage_verified",false);line.boolean("execute_permitted",false);srGpuWriteLog(line);
}
void srGpuMaybeFinal(DeviceState* state) noexcept {
    if(state->srGpuClosed&&state->srGpu&&!state->srGpuFinalSummary&&state->srGpu->drained()){
        srGpuStats(state,"closed_recordings_retired");state->srGpuFinalSummary=true;}
}
void srGpuClose(DeviceState* state,const char* reason) noexcept {
    if(!state||!state->srGpuRequested)return;SrTailErrorGuard error;AcquireSRWLockExclusive(&state->srGpuLock);
    if(state->srGpu&&!state->srGpuClosed){state->srGpu->stop();state->srGpuClosed=true;srGpuStats(state,reason);srGpuMaybeFinal(state);}
    ReleaseSRWLockExclusive(&state->srGpuLock);
}
SrGpuWrite srGpuBeforeDispatch(DeviceState* state,const xess_vk::CommandRecordingFacts& recording,
        const std::optional<xess_vk::sr_observation::DispatchTicket>& matched) noexcept {
    if(!state||state->srRuntimeRequested||!state->srGpuRequested||!matched||matched->count!=1||!matched->window||
        matched->containsAccumulate==matched->containsRcas||srTailInjectedDepth)return {};
    SrTailErrorGuard error;if(!srTailWindowActive(state))return {};
    AcquireSRWLockExclusive(&state->srGpuLock);const auto& ticket=*matched;
    if(state->srGpu&&state->srGpuWindow!=ticket.window){
        state->srGpu->stop();state->srGpuClosed=true;
        if(state->srGpu->drained()){srGpuStats(state,"window_replaced_drained");delete state->srGpu;state->srGpu=nullptr;}
    }
    if(!state->srGpu&&recording.queueFamily<state->srGpuFamilyCount){
        const auto& family=state->srGpuFamilies[recording.queueFamily];sgt::Config config;config.device=state->device;
        config.incarnation=recording.contextIncarnation;config.window=ticket.window;config.queueFamily=recording.queueFamily;
        config.queueFlags=family.queueFlags;config.validBits=family.timestampValidBits;config.periodNs=state->srGpuPeriod;
        config.functions=state->srGpuFunctions;config.sink=srGpuSample;config.sinkUser=state;config.enabled=true;
        state->srGpu=new(std::nothrow) sgt::Observer(config);state->srGpuWindow=ticket.window;state->srGpuClosed=state->srGpuFinalSummary=false;
        JsonLine line("sr_gpu_timestamp_begin");line.number("window_id",ticket.window);line.number("queue_family",recording.queueFamily);
        line.number("queue_flags",family.queueFlags);line.number("valid_bits",family.timestampValidBits);line.real("timestamp_period_ns",state->srGpuPeriod);
        line.boolean("active",state->srGpu&&state->srGpu->active());line.number("recording_capacity",sgt::recordingCapacity);line.number("passes_per_recording",sgt::passCapacity);
        line.number("query_capacity",2*sgt::recordingCapacity*sgt::passCapacity);line.boolean("wait_added",false);srGpuWriteLog(line);
    }
    SrGpuWrite result;
    if(state->srGpu&&state->srGpuWindow==ticket.window&&!state->srGpuClosed){
        sgt::Identity identity;identity.window=ticket.window;identity.ordinal=ticket.ordinal;identity.callback=ticket.callbacks[0];
        identity.pipeline=ticket.pipeline;identity.pipelineGeneration=ticket.pipelineGeneration;identity.frame=ticket.frame;
        identity.pass=ticket.containsAccumulate?sgt::Pass::Accumulate:sgt::Pass::Rcas;
        identity.render=ticket.renderExtent;identity.output=ticket.outputExtent;identity.extentsKnown=ticket.extentsKnown;identity.groups=ticket.groups;
        result={ticket.window,state->srGpu->beforeDispatch(recording,identity)};
    }else if(state->srGpuRejectedWindow!=ticket.window){state->srGpuRejectedWindow=ticket.window;
        JsonLine line("sr_gpu_timestamp_refused");line.number("window_id",ticket.window);
        line.text("reason",state->srGpu?"prior_window_recordings_retained":recording.queueFamily==VK_QUEUE_FAMILY_IGNORED?"queue_family_unknown":
            state->srGpuFamilyReported>std::size(state->srGpuFamilies)?"queue_family_properties_capacity":"queue_family_metadata_or_owner_unavailable");
        line.number("recording_queue_family",recording.queueFamily);line.number("reported_family_count",state->srGpuFamilyReported);
        line.number("stored_family_count",state->srGpuFamilyCount);line.number("family_property_capacity",std::size(state->srGpuFamilies));srGpuWriteLog(line);}
    ReleaseSRWLockExclusive(&state->srGpuLock);return result;
}
void srGpuAfterDispatch(DeviceState* state,const SrGpuWrite& ticket) noexcept {
    if(!state||!state->srGpuRequested||!ticket.ticket)return;SrTailErrorGuard error;AcquireSRWLockExclusive(&state->srGpuLock);
    if(state->srGpu&&state->srGpuWindow==ticket.window)state->srGpu->afterDispatch(ticket.ticket);
    ReleaseSRWLockExclusive(&state->srGpuLock);
}
SrGpuSubmit srGpuBeforeSubmit(DeviceState* state,const xess_vk::render_pass_observer::SubmitTicket& ticket) noexcept {
    if(!state||!state->srGpuRequested)return {};SrTailErrorGuard error;AcquireSRWLockExclusive(&state->srGpuLock);SrGpuSubmit result;
    if(state->srGpu){const auto count=std::min<std::size_t>(ticket.commandCount,ticket.recordingFacts.size());result.window=state->srGpuWindow;
        result.ticket=state->srGpu->beforeSubmit(reinterpret_cast<VkQueue>(ticket.queue),ticket.queueFamily,{ticket.recordingFacts.data(),count},
            ticket.queueKnown&&!ticket.issues&&count==ticket.commandCount);}
    ReleaseSRWLockExclusive(&state->srGpuLock);return result;
}
void srGpuAfterSubmit(DeviceState* state,const SrGpuSubmit* ticket,VkResult result) noexcept {
    if(!state||!state->srGpuRequested)return;SrTailErrorGuard error;AcquireSRWLockExclusive(&state->srGpuLock);
    if(result!=VK_ERROR_OUT_OF_HOST_MEMORY&&result!=VK_ERROR_OUT_OF_DEVICE_MEMORY)state->srGpuIdleComplete=false;
    if(state->srGpu&&ticket&&ticket->window==state->srGpuWindow&&ticket->ticket.serial)state->srGpu->submitted(ticket->ticket,result);
    ReleaseSRWLockExclusive(&state->srGpuLock);
}
void srGpuRetire(DeviceState* state,std::uint64_t command,std::uint64_t recording) noexcept {
    if(!state||!state->srGpuRequested)return;SrTailErrorGuard error;AcquireSRWLockExclusive(&state->srGpuLock);
    if(state->srGpu){state->srGpu->retire(command,recording);srGpuMaybeFinal(state);}ReleaseSRWLockExclusive(&state->srGpuLock);
}
void srGpuRetirePool(DeviceState* state,VkCommandPool pool) noexcept {
    if(!state||!state->srGpuRequested)return;SrTailErrorGuard error;AcquireSRWLockExclusive(&state->srGpuLock);
    if(state->srGpu){state->srGpu->retirePool(bits(pool));srGpuMaybeFinal(state);}ReleaseSRWLockExclusive(&state->srGpuLock);
}
void srGpuDeviceIdle(DeviceState* state,VkResult result) noexcept {
    if(!state||!state->srGpuRequested)return;SrTailErrorGuard error;AcquireSRWLockExclusive(&state->srGpuLock);
    state->srGpuIdleComplete=result==VK_SUCCESS;if(state->srGpu)state->srGpu->deviceIdle(result);ReleaseSRWLockExclusive(&state->srGpuLock);
}
void srGpuBeforeDeviceDestroy(DeviceState* state) noexcept {
    if(!state||!state->srGpuRequested)return;SrTailErrorGuard error;AcquireSRWLockExclusive(&state->srGpuLock);
    if(state->srGpu){state->srGpu->stop();state->srGpuClosed=true;
        const bool released=state->srGpuIdleComplete&&state->srGpu->releaseForDeviceDestruction(VK_SUCCESS);
        srGpuStats(state,released?"device_destroy_after_native_idle":"device_destroy_no_completed_idle_samples_may_be_unread");}
    ReleaseSRWLockExclusive(&state->srGpuLock);
}
void srGpuDeviceTerminated(DeviceState* state) noexcept {
    if(!state||!state->srGpuRequested)return;SrTailErrorGuard error;AcquireSRWLockExclusive(&state->srGpuLock);
    if(state->srGpu){srGpuStats(state,"native_device_destroyed_host_metadata_only",true);delete state->srGpu;state->srGpu=nullptr;}
    ReleaseSRWLockExclusive(&state->srGpuLock);
}
bool srTailHooksRequested() noexcept {
#ifdef XESS_METADATA_CONSUMER_TEST
    if(srTailWindowOverride>=0)return srTailWindowOverride!=0;
#endif
    return xess_vk::diagnostic_flags::srObservation()||xess_vk::diagnostic_flags::srFullGraphObservation()||xess_vk::diagnostic_flags::srReplaceRequested();
}
bool srSelectShader(const xess_vk::sr_observation::DispatchTicket& ticket) noexcept {
    return ticket.containsAccumulate||ticket.containsPrepare||
        (xess_vk::diagnostic_flags::srFullGraphObservation()&&(ticket.diagnosticArchiveCandidate||
            (ticket.count==1&&ticket.nativePass!=xess_vk::upscale_tail_probe::Pass::Unknown)));
}
void srDispatchReturned(DeviceState* state,const xess_vk::sr_observation::DispatchTicket& returned,const char* api,
        const xess_vk::CommandRecordingFacts& recording,const xess_vk::descriptor_image_metadata::DispatchEvidence& evidence) noexcept {
    if(srSelectShader(returned))xess_vk::compute_observer::selectArchivedShader(state->srShaders,
        reinterpret_cast<VkPipeline>(returned.pipeline),returned.pipelineGeneration);
    if(!returned.shadowObserved){
        if(xess_vk::sr_observation::consumerActive(bits(state->device))&&xess_vk::sr_observation::consumerDispatch(bits(state->device),returned.consumerWindow,recording,evidence,
            renderTailFrame(state),-1,{},xess_vk::resource_memory_observer::sourceQuery(state->resourceMemory)))
            xess_vk::compute_observer::selectArchivedShader(state->srShaders,reinterpret_cast<VkPipeline>(evidence.pipeline.handle),evidence.pipeline.generation);
        xess_vk::sr_observation::afterDispatch(returned,api);return;
    }
    auto ticket=returned;
    bool known=false;int profileIndex=-1;
    if(ticket.shadowObserved){
        for(const auto& profile:xess_vk::sr_native_shaders::profiles)if(xess_vk::compute_observer::archivedShaderMatches(state->srShaders,
            reinterpret_cast<VkPipeline>(ticket.shadow.pipeline),ticket.shadow.pipelineGeneration,profile.sha256)){
                known=true;ticket.shadow.policy=profile.policy;profileIndex=int(&profile-xess_vk::sr_native_shaders::profiles.data());break;}
        if(ticket.source.valid&&state->parent&&state->parent->gipa){
            const auto query=reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties>(state->parent->gipa(state->parent->instance,"vkGetPhysicalDeviceFormatProperties"));
            const auto& source=ticket.source.images[0].imageDescription;const auto& destination=ticket.source.images[3].imageDescription;
            if(query&&(source.tiling==VK_IMAGE_TILING_LINEAR||source.tiling==VK_IMAGE_TILING_OPTIMAL)&&
               (destination.tiling==VK_IMAGE_TILING_LINEAR||destination.tiling==VK_IMAGE_TILING_OPTIMAL)){
                VkFormatProperties s{},d{};query(state->physical,static_cast<VkFormat>(source.format),&s);query(state->physical,static_cast<VkFormat>(destination.format),&d);
                ticket.shadow.sourceFormatFeatures=source.tiling==VK_IMAGE_TILING_OPTIMAL?s.optimalTilingFeatures:s.linearTilingFeatures;
                ticket.shadow.destinationFormatFeatures=destination.tiling==VK_IMAGE_TILING_OPTIMAL?d.optimalTilingFeatures:d.linearTilingFeatures;
                ticket.shadow.blitFormatsQueried=true;
                ticket.shadow.blitChecks=xess_vk::sr_dispatch_shadow::blitStaticChecks(source,destination,
                    ticket.shadow.sourceFormatFeatures,ticket.shadow.destinationFormatFeatures);
            }
        }
    }
    if(xess_vk::sr_observation::consumerDispatch(bits(state->device),ticket.consumerWindow,recording,evidence,ticket.frame,profileIndex,
        ticket.source.valid?ticket.source.images[3].image:xess_vk::descriptor_image_metadata::Key{},
        xess_vk::resource_memory_observer::sourceQuery(state->resourceMemory)))
        xess_vk::compute_observer::selectArchivedShader(state->srShaders,reinterpret_cast<VkPipeline>(evidence.pipeline.handle),evidence.pipeline.generation);
    xess_vk::sr_observation::afterDispatch(ticket,api,known);
}
bool srTailWindowActive(DeviceState* state) noexcept {
    if(!state||!state->srObservationRequested)return false;
#ifdef XESS_METADATA_CONSUMER_TEST
    if(srTailWindowOverride>=0)return srTailWindowOverride!=0;
#endif
    return xess_vk::sr_observation::active(bits(state->device));
}
void srTailWrite(JsonLine& line) noexcept {
#ifdef XESS_METADATA_CONSUMER_TEST
    if(srTailTestSink){line.raw("}\n");if(line.good){line.buffer[line.used]=0;srTailTestSink(line.buffer);}return;}
#endif
    line.writeObservation();
}
void srTailUnsigned(JsonLine& line,std::uint64_t value) noexcept {
    char text[32];std::snprintf(text,sizeof(text),"%llu",static_cast<unsigned long long>(value));line.raw(text);
}
void srTailHex(JsonLine& line,const char* key,const void* data,std::size_t size) noexcept {
    constexpr char digits[]="0123456789abcdef";const auto* bytes=static_cast<const unsigned char*>(data);
    line.key(key);line.raw("\"");
    for(std::size_t n=0;n<size;++n){char pair[]{digits[bytes[n]>>4],digits[bytes[n]&15],0};line.raw(pair);}
    line.raw("\"");
}
void srTailScopes(JsonLine& line,const stc::MemoryBarrier& value) noexcept {
    line.ticks("src_stage",value.srcStage);line.ticks("dst_stage",value.dstStage);
    line.ticks("src_access",value.srcAccess);line.ticks("dst_access",value.dstAccess);
}
void srTailAttachment(JsonLine& line,const stc::Attachment& value) noexcept {
    line.raw("{\"attachment\":true");line.handle("view",value.view);line.number("layout",value.layout);
    line.handle("resolve_view",value.resolveView);line.number("resolve_layout",value.resolveLayout);line.number("resolve_mode",value.resolveMode);
    line.number("load",value.loadOp);line.number("store",value.storeOp);srTailHex(line,"clear_bytes",&value.clear,sizeof(value.clear));line.raw("}");
}
void srTailCommandFields(JsonLine& line,const stc::Command& command) noexcept {
    line.text("type",stc::name(command.kind));line.number("api_variant",static_cast<unsigned>(command.api));
    line.boolean("truncated",command.truncated);line.boolean("missing_data",command.missingData);line.boolean("extensions_omitted",command.extensionsOmitted);
    line.number("argument_count",command.argumentCount);line.number("stored_arguments",command.storedArguments);
    line.key("arguments_u64");line.raw("[");for(unsigned n=0;n<command.storedArguments;++n){if(n)line.raw(",");srTailUnsigned(line,command.arguments[n]);}line.raw("]");
    if(command.kind==stc::Kind::Other)line.text("api",command.otherName.data());
    if(command.kind==stc::Kind::Barrier){const auto& b=command.barriers;
        line.number("dependency_flags",b.dependencyFlags);line.ticks("src_stage",b.srcStage);line.ticks("dst_stage",b.dstStage);
        line.number("memory_count",b.memoryCount);line.number("buffer_count",b.bufferCount);line.number("image_count",b.imageCount);
        line.number("stored_memory",b.storedMemory);line.number("stored_buffers",b.storedBuffers);line.number("stored_images",b.storedImages);
        line.key("barriers");line.raw("[");bool first=true;
        for(unsigned n=0;n<b.storedMemory;++n){if(!first)line.raw(",");first=false;line.raw("{\"resource\":\"memory\"");srTailScopes(line,b.memory[n]);line.raw("}");}
        for(unsigned n=0;n<b.storedBuffers;++n){if(!first)line.raw(",");first=false;const auto& v=b.buffers[n];line.raw("{\"resource\":\"buffer\"");srTailScopes(line,v.scopes);
            line.handle("buffer",v.buffer);line.ticks("offset",v.offset);line.ticks("size",v.size);line.number("src_family",v.srcQueueFamily);line.number("dst_family",v.dstQueueFamily);line.raw("}");}
        for(unsigned n=0;n<b.storedImages;++n){if(!first)line.raw(",");first=false;const auto& v=b.images[n];line.raw("{\"resource\":\"image\"");srTailScopes(line,v.scopes);
            line.handle("image",v.image);line.number("old_layout",v.oldLayout);line.number("new_layout",v.newLayout);line.number("src_family",v.srcQueueFamily);line.number("dst_family",v.dstQueueFamily);
            line.number("aspect",v.range.aspectMask);line.number("base_mip",v.range.baseMipLevel);line.number("mip_count",v.range.levelCount);line.number("base_layer",v.range.baseArrayLayer);line.number("layer_count",v.range.layerCount);line.raw("}");}
        line.raw("]");
    }
    if(command.kind==stc::Kind::BindDescriptorSets){const auto& d=command.descriptors;
        line.number("bind_point",d.bindPoint);line.number("stages",d.stages);line.handle("layout",d.layout);line.number("first_set",d.firstSet);
        line.number("set_count",d.setCount);line.number("stored_sets",d.storedSets);line.number("dynamic_offset_count",d.dynamicOffsetCount);line.number("stored_offsets",d.storedOffsets);
        line.key("sets");line.raw("[");for(unsigned n=0;n<d.storedSets;++n){if(n)line.raw(",");srTailUnsigned(line,d.sets[n]);}line.raw("]");
        line.key("dynamic_offsets");line.raw("[");for(unsigned n=0;n<d.storedOffsets;++n){if(n)line.raw(",");srTailUnsigned(line,d.offsets[n]);}line.raw("]");
    }
    if(command.kind==stc::Kind::PushConstants){const auto& p=command.push;line.handle("layout",p.layout);line.number("stages",p.stages);
        line.number("offset",p.offset);line.number("size",p.size);line.number("stored_bytes",p.storedBytes);srTailHex(line,"bytes",p.bytes.data(),p.storedBytes);}
    if(command.kind==stc::Kind::BeginRenderPass){const auto& p=command.renderPass;line.handle("render_pass",p.renderPass);line.handle("framebuffer",p.framebuffer);
        line.number("contents",p.contents);line.number("x",p.area.offset.x);line.number("y",p.area.offset.y);line.number("width",p.area.extent.width);line.number("height",p.area.extent.height);
        line.number("clear_count",p.clearCount);line.number("stored_clears",p.storedClears);srTailHex(line,"clear_bytes",p.clears.data(),p.storedClears*sizeof(VkClearValue));}
    if(command.kind==stc::Kind::BeginRendering){const auto& r=command.rendering;line.number("flags",r.flags);line.number("x",r.area.offset.x);line.number("y",r.area.offset.y);
        line.number("width",r.area.extent.width);line.number("height",r.area.extent.height);line.number("layers",r.layerCount);line.number("view_mask",r.viewMask);
        line.number("color_count",r.colorCount);line.number("stored_colors",r.storedColors);line.key("colors");line.raw("[");for(unsigned n=0;n<r.storedColors;++n){if(n)line.raw(",");srTailAttachment(line,r.colors[n]);}line.raw("]");
        if(r.hasDepth){line.key("depth");srTailAttachment(line,r.depth);}if(r.hasStencil){line.key("stencil");srTailAttachment(line,r.stencil);}}
}
// This list is filled from the adapter's actual wrappers, not all Vulkan commands.
static constexpr const char* srTailHookedApis[]={"vkCmdBeginRenderPass","vkCmdBeginRenderPass2","vkCmdBeginRenderPass2KHR","vkCmdBeginRendering","vkCmdBeginRenderingKHR","vkCmdBindDescriptorSets","vkCmdBindDescriptorSets2","vkCmdBindDescriptorSets2KHR","vkCmdBindIndexBuffer","vkCmdBindIndexBuffer2","vkCmdBindIndexBuffer2KHR","vkCmdBindPipeline","vkCmdBindVertexBuffers","vkCmdBindVertexBuffers2","vkCmdBindVertexBuffers2EXT","vkCmdBlitImage","vkCmdBlitImage2","vkCmdBlitImage2KHR","vkCmdCopyBuffer","vkCmdCopyBufferToImage","vkCmdCopyImage","vkCmdCopyImage2","vkCmdCopyImage2KHR","vkCmdCopyImageToBuffer","vkCmdDispatch","vkCmdDispatchBase","vkCmdDispatchBaseKHR","vkCmdDispatchIndirect","vkCmdDraw","vkCmdDrawIndexed","vkCmdDrawIndexedIndirect","vkCmdDrawIndexedIndirectCount","vkCmdDrawIndexedIndirectCountKHR","vkCmdDrawIndirect","vkCmdDrawIndirectCount","vkCmdDrawIndirectCountKHR","vkCmdEndRenderPass","vkCmdEndRenderPass2","vkCmdEndRenderPass2KHR","vkCmdEndRendering","vkCmdEndRenderingKHR","vkCmdExecuteCommands","vkCmdFillBuffer","vkCmdNextSubpass","vkCmdNextSubpass2","vkCmdNextSubpass2KHR","vkCmdPipelineBarrier","vkCmdPipelineBarrier2","vkCmdPipelineBarrier2KHR","vkCmdPushConstants","vkCmdPushConstants2","vkCmdPushConstants2KHR","vkCmdPushDescriptorSet","vkCmdPushDescriptorSet2","vkCmdPushDescriptorSet2KHR","vkCmdPushDescriptorSetKHR","vkCmdPushDescriptorSetWithTemplate","vkCmdPushDescriptorSetWithTemplate2","vkCmdPushDescriptorSetWithTemplate2KHR","vkCmdPushDescriptorSetWithTemplateKHR","vkCmdResetEvent","vkCmdResetEvent2","vkCmdResetEvent2KHR","vkCmdSetBlendConstants","vkCmdSetDepthBias","vkCmdSetDepthBounds","vkCmdSetEvent","vkCmdSetEvent2","vkCmdSetEvent2KHR","vkCmdSetLineWidth","vkCmdSetScissor","vkCmdSetScissorWithCount","vkCmdSetScissorWithCountEXT","vkCmdSetStencilCompareMask","vkCmdSetStencilReference","vkCmdSetStencilWriteMask","vkCmdSetViewport","vkCmdSetViewportWithCount","vkCmdSetViewportWithCountEXT","vkCmdWaitEvents","vkCmdWaitEvents2","vkCmdWaitEvents2KHR"};
void srTailIdentity(JsonLine& line,const stc::Trace& trace) noexcept {
    line.number("window_id",trace.window);line.handle("device",trace.tail.recording.device);line.number("incarnation",trace.tail.recording.incarnation);
    line.handle("command",trace.tail.recording.command);line.number("recording_generation",trace.tail.recording.recording);
    line.text("last_matched_fsr_pass",stc::name(trace.tail.pass));line.number("dispatch_ordinal",trace.tail.dispatchOrdinal);
    line.number("tail_record_order",trace.tail.recordOrder);line.number("callback",trace.tail.callback);
    line.number("engine_frame",trace.tail.frame.engineFrame);line.number("sdk_id",trace.tail.frame.ticket.sdkId);
    line.number("ticket_context",trace.tail.frame.ticket.contextGeneration);line.boolean("frame_known",trace.tail.frame.valid&&bool(trace.tail.frame.ticket));
}
void srTailEmit(DeviceState* state,const stc::Trace& trace,const char* reason) noexcept {
    if(state->srTailEmitted>=128)return;++state->srTailEmitted;
    JsonLine header("sr_tail_commands");srTailIdentity(header,trace);header.text("end",reason);header.number("commands",trace.count);header.number("replacements",trace.replacements);
    header.text("coverage","observed-api-order-only");header.boolean("coverage_closed",false);
    header.text("scalar_encoding","Vulkan signature order; signed integer sign-extended, float IEEE754 bits");
    header.names("hooked_apis",static_cast<std::uint32_t>(std::size(srTailHookedApis)),srTailHookedApis);srTailWrite(header);
    for(unsigned n=0;n<trace.count;++n){JsonLine line("sr_tail_command");srTailIdentity(line,trace);line.number("index",n);
        line.text("coverage","observed-api-order-only");srTailCommandFields(line,trace.commands[n]);srTailWrite(line);}
}
void srTailClose(DeviceState* state,const char* reason) noexcept {
    srGpuClose(state,reason);
    if(!state)return;SrTailErrorGuard error;
    AcquireSRWLockExclusive(&state->srTailLock);auto* observer=state->srTailCommands;state->srTailCommands=nullptr;
    if(observer){stc::Trace trace;while(observer->drain(trace))srTailEmit(state,trace,reason);delete observer;}
    ReleaseSRWLockExclusive(&state->srTailLock);
}
void srTailPoll(DeviceState* state) noexcept {
    if(!state||!state->srObservationRequested||(!state->srTailDiagnostics&&!state->srGpuRequested))return;SrTailErrorGuard error;
    if(!srTailWindowActive(state))srTailClose(state,"window_closed");
}
xess_vk::descriptor_image_metadata::SetImageSnapshot srConsumerSet(void* user,std::uint64_t set) noexcept {
    return xess_vk::descriptor_image_observer::diagnosticSetImages(static_cast<DeviceState*>(user)->descriptors,reinterpret_cast<VkDescriptorSet>(set));
}
xess_vk::sr_observation::ConsumerImages srConsumerAttachments(void* user,const xess_vk::CommandRecordingFacts& facts,const stc::Command& command) noexcept {
    auto* state=static_cast<DeviceState*>(user);xess_vk::sr_observation::ConsumerImages out;
    out.complete=!command.truncated&&!command.missingData&&!command.extensionsOmitted;
    const auto view=[&](std::uint64_t handle){if(!handle)return;
        const auto value=xess_vk::render_pass_observer::diagnosticViewSnapshot(state->renderPass,reinterpret_cast<VkImageView>(handle));
        out.complete&=value.known&&!value.issues&&value.image.handle&&value.image.generation;
        if(out.count==out.images.size()){out.complete=false;return;}
        out.images[out.count++]={value.image.handle,value.image.generation};};
    if(command.kind==stc::Kind::BeginRenderPass){
        VkRenderPassBeginInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        info.renderPass=reinterpret_cast<VkRenderPass>(command.renderPass.renderPass);
        info.framebuffer=reinterpret_cast<VkFramebuffer>(command.renderPass.framebuffer);info.renderArea=command.renderPass.area;
        const auto snapshot=xess_vk::render_pass_observer::diagnosticPassSnapshot(state->renderPass,reinterpret_cast<VkCommandBuffer>(facts.command),&info);
        out.complete&=snapshot.pass.known&&snapshot.framebuffer.known&&!snapshot.pass.issues&&!snapshot.framebuffer.issues&&
            snapshot.count==snapshot.framebuffer.attachmentCount;
        for(std::size_t n=0;n<snapshot.count;++n){out.complete&=snapshot.views[n].known&&snapshot.views[n].key.handle;view(snapshot.views[n].key.handle);}
    }else if(command.kind==stc::Kind::BeginRendering){
        const auto attachment=[&](const stc::Attachment& a){view(a.view);if(a.resolveMode!=VK_RESOLVE_MODE_NONE){out.complete&=a.resolveView!=0;view(a.resolveView);}};
        out.complete&=command.rendering.storedColors<=command.rendering.colors.size()&&command.rendering.colorCount==command.rendering.storedColors;
        for(std::size_t n=0;n<std::min<std::size_t>(command.rendering.storedColors,command.rendering.colors.size());++n)attachment(command.rendering.colors[n]);
        if(command.rendering.hasDepth)attachment(command.rendering.depth);if(command.rendering.hasStencil)attachment(command.rendering.stencil);
    }else out.complete=false;
    return out;
}
template<class Factory>
bool srProductCommand(DeviceState* state,VkCommandBuffer buffer,Factory&& factory) noexcept {
    if(!state||!state->srRuntime||!state->srRuntime->replacementAvailable||srTailInjectedDepth||srSdkDepth)return false;
    SrTailErrorGuard error;const auto facts=xess_vk::render_pass_observer::recordingFacts(state->renderPass,buffer);
    return state->srRuntime->replacementCommand(facts,factory());
}
bool srProductDraw(DeviceState* state,VkCommandBuffer buffer,stc::Kind kind) noexcept {
    return srProductCommand(state,buffer,[&]{return GameSrRuntime::GraphicsCommand{kind};});
}
void srProductBind(DeviceState* state,VkCommandBuffer buffer,VkPipelineBindPoint point,VkPipelineLayout layout,
                   unsigned first,unsigned count,const VkDescriptorSet* sets,unsigned dynamicCount,const std::uint32_t* offsets) noexcept {
    if(point!=VK_PIPELINE_BIND_POINT_GRAPHICS||!state||!state->srRuntime||!state->srRuntime->replacementAvailable||srTailInjectedDepth||srSdkDepth)return;
    SrTailErrorGuard error;state->srRuntime->bindGraphics(xess_vk::render_pass_observer::recordingFacts(state->renderPass,buffer),point,layout,first,count,sets,dynamicCount,offsets);
}
template<class Factory>
bool srTailCommand(DeviceState* state,VkCommandBuffer buffer,Factory&& factory) noexcept {
    if(!state||!state->srTailDiagnostics||srTailInjectedDepth)return false;
    SrTailErrorGuard error;if(!srTailWindowActive(state)){srTailClose(state,"window_closed");return false;}
    const auto auditWindow=xess_vk::sr_observation::consumerWindow(bits(state->device));
    const auto facts=xess_vk::render_pass_observer::recordingFacts(state->renderPass,buffer);const auto recording=stc::key(facts);
    std::optional<stc::Command> captured;
    if(auditWindow){captured=factory();
        xess_vk::sr_observation::consumerCommand(bits(state->device),auditWindow,facts,*captured,
            {srConsumerSet,srConsumerAttachments,xess_vk::resource_memory_observer::sourceQuery(state->resourceMemory),state});}
    AcquireSRWLockExclusive(&state->srTailLock);
    if(state->srTailCommands&&state->srTailCommands->wantsCommand(recording))state->srTailCommands->command(recording,captured?*captured:factory());
    ReleaseSRWLockExclusive(&state->srTailLock);
    return false;
}
template<class Barrier>
void srUndefinedLayoutDiscards(DeviceState* state,VkCommandBuffer command,std::uint32_t count,const Barrier* barriers) noexcept {
    if(!state||!state->srRuntime||srTailInjectedDepth||srSdkDepth||!barriers)return;
    std::uint32_t first{};
    while(first<count&&(barriers[first].oldLayout!=VK_IMAGE_LAYOUT_UNDEFINED||barriers[first].newLayout==VK_IMAGE_LAYOUT_UNDEFINED))++first;
    if(first==count)return;
    SrTailErrorGuard error;
    state->srRuntime->replacementUndefinedLayouts(xess_vk::render_pass_observer::recordingFacts(state->renderPass,command),count-first,barriers+first);
}
void srUndefinedLayoutDiscards2(DeviceState* state,VkCommandBuffer command,std::uint32_t count,const VkDependencyInfo* infos) noexcept {
    if(!state||!state->srRuntime||srTailInjectedDepth||srSdkDepth||!infos)return;
    for(std::uint32_t n=0;n<count;++n)if(infos[n].sType==VK_STRUCTURE_TYPE_DEPENDENCY_INFO&&!infos[n].pNext)
        srUndefinedLayoutDiscards(state,command,infos[n].imageMemoryBarrierCount,infos[n].pImageMemoryBarriers);
}
stc::Command srTailSimple(stc::Kind kind,std::initializer_list<std::uint64_t> args,stc::Api api=stc::Api::Core) noexcept {
    return stc::simple(kind,{args.begin(),args.size()},api);
}
stc::Command srTailOther(const char* api,std::initializer_list<std::uint64_t> args={}) noexcept {
    return stc::other(api,{args.begin(),args.size()});
}
std::uint64_t srTailFloat(float value) noexcept {std::uint32_t out{};std::memcpy(&out,&value,sizeof(out));return out;}
stc::Command srTailViewport(const char* api,std::uint32_t first,std::uint32_t count,const VkViewport* views) noexcept {
    auto out=srTailOther(api,{first,count});out.truncated=count>1;out.missingData=count&&!views;
    if(count&&views)out=srTailOther(api,{first,count,srTailFloat(views[0].x),srTailFloat(views[0].y),srTailFloat(views[0].width),srTailFloat(views[0].height),srTailFloat(views[0].minDepth),srTailFloat(views[0].maxDepth)});
    out.truncated|=count>1;return out;
}
stc::Command srTailScissor(const char* api,std::uint32_t first,std::uint32_t count,const VkRect2D* rects) noexcept {
    auto out=srTailOther(api,{first,count});out.truncated=count>1;out.missingData=count&&!rects;
    if(count&&rects)out=srTailOther(api,{first,count,static_cast<std::uint64_t>(static_cast<std::int64_t>(rects[0].offset.x)),
        static_cast<std::uint64_t>(static_cast<std::int64_t>(rects[0].offset.y)),rects[0].extent.width,rects[0].extent.height});
    out.truncated|=count>1;return out;
}
void srTailMatched(DeviceState* state,const xess_vk::CommandRecordingFacts& recording,const xess_vk::sr_observation::DispatchTicket& ticket) noexcept {
    if(!state||!state->srTailDiagnostics||srTailInjectedDepth||ticket.count!=1||!ticket.window||ticket.containsAccumulate==ticket.containsRcas)return;
    SrTailErrorGuard error;if(!srTailWindowActive(state))return;
    AcquireSRWLockExclusive(&state->srTailLock);
    if(state->srTailCommands&&state->srTailCommands->window()!=ticket.window){stc::Trace trace;
        while(state->srTailCommands->drain(trace))srTailEmit(state,trace,"window_changed");delete state->srTailCommands;state->srTailCommands=nullptr;}
    if(!state->srTailCommands){state->srTailCommands=new(std::nothrow) stc::Observer(ticket.window);state->srTailEmitted=0;}
    if(state->srTailCommands&&state->srTailEmitted<128){stc::Tail tail;tail.recording=stc::key(recording);
        tail.pass=ticket.containsRcas?stc::Pass::Rcas:stc::Pass::Accumulate;tail.dispatchOrdinal=ticket.ordinal;tail.recordOrder=recording.commonRecordOrder;
        tail.callback=ticket.callbacks[0];tail.frame=ticket.frame;state->srTailCommands->tail(tail);}
    ReleaseSRWLockExclusive(&state->srTailLock);
}
void srTailRetire(DeviceState* state,const stc::Key& key) noexcept {
    if(!state||!state->srTailDiagnostics)return;SrTailErrorGuard error;AcquireSRWLockExclusive(&state->srTailLock);
    if(state->srTailCommands)state->srTailCommands->retire(key);ReleaseSRWLockExclusive(&state->srTailLock);
}
void srTailRetirePool(DeviceState* state,VkCommandPool pool) noexcept {
    if(!state||!state->srTailDiagnostics)return;SrTailErrorGuard error;AcquireSRWLockExclusive(&state->srTailLock);
    if(state->srTailCommands)state->srTailCommands->retirePool(bits(pool));ReleaseSRWLockExclusive(&state->srTailLock);
}
void srTailFinish(DeviceState* state,const stc::Key& key,VkResult result) noexcept {
    if(!state||!state->srTailDiagnostics)return;SrTailErrorGuard error;
    xess_vk::sr_observation::consumerEnd(bits(state->device),xess_vk::sr_observation::consumerWindow(bits(state->device)),key,result);AcquireSRWLockExclusive(&state->srTailLock);
    if(state->srTailCommands){if(result==VK_SUCCESS){stc::Trace trace;if(state->srTailCommands->finish(key,trace))srTailEmit(state,trace,"recording_ended");}
        else state->srTailCommands->retire(key);}
    ReleaseSRWLockExclusive(&state->srTailLock);
}
} // namespace
OBS_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo* info,
    const VkAllocationCallbacks* allocator, VkShaderModule* shader) {
    auto* state = findDevice(device); if (!state || !state->createShaderModule) return VK_ERROR_INITIALIZATION_FAILED;
    const auto result = state->createShaderModule(device, info, allocator, shader);
    if constexpr(researchDiagnostics) if (state->compute && result == VK_SUCCESS) xess_vk::compute_observer::shader(state->compute, *shader, info);
    if(state->srShaders&&result==VK_SUCCESS)xess_vk::compute_observer::archiveShader(state->srShaders,*shader,info);
    return result;
}
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkDestroyShaderModule(VkDevice device, VkShaderModule shader, const VkAllocationCallbacks* allocator) {
    auto* state = findDevice(device); if (!state || !state->destroyShaderModule) return;
    if constexpr(researchDiagnostics) if (state->compute) xess_vk::compute_observer::destroyShader(state->compute, shader);
    xess_vk::compute_observer::destroyArchivedShader(state->srShaders,shader);
    state->destroyShaderModule(device, shader, allocator);
}
static void classifyCompactPipeline(DeviceState* state,xess_vk::descriptor_image_metadata::Key key) noexcept {
    const auto hash=xess_vk::compute_observer::archivedShaderFingerprint(state->srShaders,reinterpret_cast<VkPipeline>(key.handle),key.generation);
    if(!xess_vk::sr_native_shaders::needsTypedEvidence({hash.data(),hash.size()}))
        xess_vk::descriptor_image_observer::allowCompactPipeline(state->descriptors,key);
}
OBS_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateComputePipelines(VkDevice device, VkPipelineCache cache, uint32_t count,
    const VkComputePipelineCreateInfo* infos, const VkAllocationCallbacks* allocator, VkPipeline* pipelines) {
    const auto trackingStarted = trackingCpuStart();
    auto* state = findDevice(device); if (!state || !state->createComputePipelines) return VK_ERROR_INITIALIZATION_FAILED;
    TrackingCpuScope tracking(state, TrackingCpuGroup::DescriptorLifetime, trackingStarted);
    const auto result = tracking.driver([&] { return state->createComputePipelines(device, cache, count, infos, allocator, pipelines); });
    // Vulkan may return valid partial pipeline results alongside a failure.
    if constexpr(researchDiagnostics) if (state->compute) xess_vk::compute_observer::pipelines(state->compute, count, infos, pipelines, result);
    if (state->descriptors && infos && pipelines)
        for(uint32_t i=0;i<count;++i) if(pipelines[i]) {
            const auto tracked=xess_vk::descriptor_image_observer::pipelineCreated(state->descriptors,pipelines[i],&infos[i],result);
            if(state->srShaders&&tracked.accepted()){
                xess_vk::compute_observer::archivePipeline(state->srShaders,pipelines[i],tracked.key.generation,&infos[i],result);
                if(xess_vk::diagnostic_flags::fsrPipelineTracking())classifyCompactPipeline(state,tracked.key);
            }
        }
    return result;
}
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkDestroyPipeline(VkDevice device, VkPipeline pipeline, const VkAllocationCallbacks* allocator) {
    const auto trackingStarted = trackingCpuStart();
    auto* state = findDevice(device); if (!state || !state->destroyPipeline) return;
    TrackingCpuScope tracking(state, TrackingCpuGroup::DescriptorLifetime, trackingStarted);
    if constexpr(researchDiagnostics) if (state->compute) xess_vk::compute_observer::destroyPipeline(state->compute, pipeline);
    xess_vk::compute_observer::destroyArchivedPipeline(state->srShaders,pipeline);
    xess_vk::descriptor_image_observer::pipelineDestroying(state->descriptors,pipeline);
    tracking.driver([&] { return state->destroyPipeline(device, pipeline, allocator); });
}
OBS_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkAllocateCommandBuffers(VkDevice device, const VkCommandBufferAllocateInfo* info,
    VkCommandBuffer* buffers) {
    const auto trackingStarted = trackingCpuStart();
    auto* state = findDevice(device); if (!state || !state->allocateCommandBuffers) return VK_ERROR_INITIALIZATION_FAILED;
    TrackingCpuScope tracking(state, TrackingCpuGroup::Recording, trackingStarted);
    const auto result = tracking.driver([&] { return state->allocateCommandBuffers(device, info, buffers); });
    if constexpr(researchDiagnostics) if (state->compute && result == VK_SUCCESS) xess_vk::compute_observer::allocateCommands(state->compute, info, buffers);
    if constexpr(researchDiagnostics) if (result == VK_SUCCESS) xess_vk::candidate_sync::allocate(state->candidateSync, info, buffers);
    xess_vk::render_pass_observer::allocate(state->renderPass, info, buffers, result);
    xess_vk::descriptor_image_observer::allocate(state->descriptors,info,buffers,result);
    return result;
}
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkFreeCommandBuffers(VkDevice device, VkCommandPool pool, uint32_t count,
    const VkCommandBuffer* buffers) {
    const auto trackingStarted = trackingCpuStart();
    auto* state = findDevice(device); if (!state || !state->freeCommandBuffers) return;
    TrackingCpuScope tracking(state, TrackingCpuGroup::Recording, trackingStarted);
    if constexpr(researchDiagnostics) if (state->compute) xess_vk::compute_observer::freeCommands(state->compute, count, buffers);
    for(std::uint32_t n=0;buffers&&n<count;++n)rawRetireRecording(state,rawKey(state,buffers[n]));
    if constexpr(researchDiagnostics) if(state->renderTail&&state->renderTail->active())for(std::uint32_t n=0;buffers&&n<count;++n)state->renderTail->reset(bits(buffers[n]));
    // One facts query per buffer feeds both independent retirements.
    if(state->srRuntime||state->srObservationRequested)for(std::uint32_t n=0;buffers&&n<count;++n){
        const auto facts=xess_vk::render_pass_observer::recordingFacts(state->renderPass,buffers[n]);
        if(state->srRuntime)state->srRuntime->retire(facts,sro::Discard::FreeObserved);
        if(state->srObservationRequested)srTailRetire(state,stc::key(facts));
    }
    if constexpr(researchDiagnostics) xess_vk::candidate_sync::free(state->candidateSync, count, buffers);
    if(!state->srGpuRequested)xess_vk::render_pass_observer::free(state->renderPass, count, buffers);
    xess_vk::descriptor_image_observer::free(state->descriptors,count,buffers);
    tracking.driver([&] { return state->freeCommandBuffers(device, pool, count, buffers); });
    const auto originalError=GetLastError();
    if(state->srGpuRequested){for(std::uint32_t n=0;buffers&&n<count;++n){const auto f=xess_vk::render_pass_observer::recordingFacts(state->renderPass,buffers[n]);
            srGpuRetire(state,f.command,f.rpRecordingGeneration);}
        xess_vk::render_pass_observer::free(state->renderPass,count,buffers);}
    SetLastError(originalError);
}
OBS_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkBeginCommandBuffer(VkCommandBuffer buffer, const VkCommandBufferBeginInfo* info) {
    const auto trackingStarted = trackingCpuStart();
    auto* state = findDevice(buffer); if (!state || !state->beginCommandBuffer) return VK_ERROR_INITIALIZATION_FAILED;
    if(srSdkDepth){return state->beginCommandBuffer(buffer,info);}
    const auto consumerWindow=xess_vk::sr_observation::consumerWindow(bits(state->device));
    TrackingCpuScope tracking(state, TrackingCpuGroup::Recording, trackingStarted);
    const auto previous=rawKey(state,buffer);
    const auto tailPrevious=state->srObservationRequested?stc::key(xess_vk::render_pass_observer::recordingFacts(state->renderPass,buffer)):stc::Key{};
    if(state->srRuntime)state->srRuntime->boundary();
    const auto result = tracking.driver([&] { return state->beginCommandBuffer(buffer, info); });
    const auto originalError=GetLastError();rawRetireRecording(state,previous,result == VK_SUCCESS);
    if(result==VK_SUCCESS){srTailRetire(state,tailPrevious);srGpuRetire(state,tailPrevious.command,tailPrevious.recording);
        if(state->srRuntime){xess_vk::CommandRecordingFacts f;f.device=tailPrevious.device;f.contextIncarnation=tailPrevious.incarnation;f.command=tailPrevious.command;f.rpRecordingGeneration=tailPrevious.recording;
            state->srRuntime->retire(f,sro::Discard::SuccessfulReset);}}
    if constexpr(researchDiagnostics) if (state->compute && result == VK_SUCCESS) xess_vk::compute_observer::beginCommand(state->compute, buffer);
    if constexpr(researchDiagnostics) xess_vk::candidate_sync::begin(state->candidateSync, buffer, info, result);
    xess_vk::render_pass_observer::begin(state->renderPass, buffer, info, result);
    xess_vk::descriptor_image_observer::begin(state->descriptors,buffer,info,result);
    if(consumerWindow&&result==VK_SUCCESS){const auto facts=xess_vk::render_pass_observer::recordingFacts(state->renderPass,buffer);
        if(facts.allocationKnown&&facts.level==VK_COMMAND_BUFFER_LEVEL_PRIMARY)
            xess_vk::sr_observation::consumerBegin(bits(state->device),consumerWindow,stc::key(facts));}
    if constexpr(researchDiagnostics) if(state->renderTail&&state->renderTail->active())state->renderTail->begin(xess_vk::render_pass_observer::recordingFacts(state->renderPass,buffer),result);
    if (continuousWindowActive(state)) {
        const auto recording = xess_vk::render_pass_observer::dispatchPoint(state->renderPass, buffer);
        continuousCommandWindow(state, "continuous_window_command_begin", recording, result, true);
    }
    SetLastError(originalError);return result;
}
OBS_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkResetCommandBuffer(VkCommandBuffer buffer, VkCommandBufferResetFlags flags) {
    const auto trackingStarted = trackingCpuStart();
    auto* state = findDevice(buffer); if (!state || !state->resetCommandBuffer) return VK_ERROR_INITIALIZATION_FAILED;
    if(srSdkDepth){return state->resetCommandBuffer(buffer,flags);}
    TrackingCpuScope tracking(state, TrackingCpuGroup::Recording, trackingStarted);
    const auto previous=rawKey(state,buffer);
    const auto tailPrevious=state->srObservationRequested?stc::key(xess_vk::render_pass_observer::recordingFacts(state->renderPass,buffer)):stc::Key{};
    const auto result = tracking.driver([&] { return state->resetCommandBuffer(buffer, flags); });
    const auto originalError=GetLastError();rawRetireRecording(state,previous,result == VK_SUCCESS);
    if(result==VK_SUCCESS){srTailRetire(state,tailPrevious);srGpuRetire(state,tailPrevious.command,tailPrevious.recording);
        if(state->srRuntime){xess_vk::CommandRecordingFacts f;f.device=tailPrevious.device;f.contextIncarnation=tailPrevious.incarnation;f.command=tailPrevious.command;f.rpRecordingGeneration=tailPrevious.recording;
            state->srRuntime->retire(f,sro::Discard::SuccessfulReset);}}
    if constexpr(researchDiagnostics) xess_vk::candidate_sync::reset(state->candidateSync, buffer, result);
    xess_vk::render_pass_observer::reset(state->renderPass, buffer, result);
    xess_vk::descriptor_image_observer::reset(state->descriptors,buffer,result);
    if constexpr(researchDiagnostics) if(state->renderTail&&state->renderTail->active()&&result==VK_SUCCESS)state->renderTail->reset(bits(buffer));
    if constexpr(researchDiagnostics) if (state->compute && result == VK_SUCCESS) xess_vk::compute_observer::beginCommand(state->compute, buffer);
    SetLastError(originalError);return result;
}
OBS_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkResetCommandPool(VkDevice device, VkCommandPool pool, VkCommandPoolResetFlags flags) {
    const auto trackingStarted = trackingCpuStart();
    auto* state = findDevice(device); if (!state || !state->resetCommandPool) return VK_ERROR_INITIALIZATION_FAILED;
    TrackingCpuScope tracking(state, TrackingCpuGroup::Recording, trackingStarted);
    const auto result = tracking.driver([&] { return state->resetCommandPool(device, pool, flags); });
    const auto originalError=GetLastError();rawRetirePool(state,pool,result == VK_SUCCESS);
    if(result==VK_SUCCESS){srTailRetirePool(state,pool);srGpuRetirePool(state,pool);if(state->srRuntime)state->srRuntime->pool(bits(pool),sro::Discard::SuccessfulPoolReset);}
    if constexpr(researchDiagnostics) if (state->compute && result == VK_SUCCESS) xess_vk::compute_observer::resetPool(state->compute, pool, false);
    if constexpr(researchDiagnostics) if (result == VK_SUCCESS) xess_vk::candidate_sync::resetPool(state->candidateSync, pool, false);
    xess_vk::render_pass_observer::resetPool(state->renderPass, pool, false, result);
    xess_vk::descriptor_image_observer::commandPoolReset(state->descriptors,pool,result);
    if constexpr(researchDiagnostics) if(state->renderTail&&state->renderTail->active()&&result==VK_SUCCESS)state->renderTail->resetPool(bits(pool));
    SetLastError(originalError);return result;
}
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkDestroyCommandPool(VkDevice device, VkCommandPool pool, const VkAllocationCallbacks* allocator) {
    const auto trackingStarted = trackingCpuStart();
    auto* state = findDevice(device); if (!state || !state->destroyCommandPool) return;
    TrackingCpuScope tracking(state, TrackingCpuGroup::Recording, trackingStarted);
    srTailRetirePool(state,pool);
    rawRetirePool(state,pool);
    if constexpr(researchDiagnostics) if (state->compute) xess_vk::compute_observer::resetPool(state->compute, pool, true);
    if constexpr(researchDiagnostics) xess_vk::candidate_sync::resetPool(state->candidateSync, pool, true);
    xess_vk::render_pass_observer::resetPool(state->renderPass, pool, true, VK_SUCCESS);
    xess_vk::descriptor_image_observer::commandPoolDestroying(state->descriptors,pool);
    if constexpr(researchDiagnostics) if(state->renderTail&&state->renderTail->active())state->renderTail->resetPool(bits(pool));
    tracking.driver([&] { return state->destroyCommandPool(device, pool, allocator); });
    const auto originalError=GetLastError();srGpuRetirePool(state,pool);if(state->srRuntime)state->srRuntime->pool(bits(pool),sro::Discard::PoolDestroyObserved);SetLastError(originalError);
}
OBS_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateCommandPool(VkDevice device, const VkCommandPoolCreateInfo* info,
    const VkAllocationCallbacks* allocator, VkCommandPool* pool) {
    const auto trackingStarted = trackingCpuStart();
    auto* state = findDevice(device); if (!state || !state->createCommandPool) return VK_ERROR_INITIALIZATION_FAILED;
    TrackingCpuScope tracking(state, TrackingCpuGroup::Recording, trackingStarted);
    const auto result = tracking.driver([&] { return state->createCommandPool(device, info, allocator, pool); });
    xess_vk::render_pass_observer::poolCreated(state->renderPass,result==VK_SUCCESS ? *pool : VK_NULL_HANDLE,info,result);
    xess_vk::descriptor_image_observer::commandPoolCreated(state->descriptors,result==VK_SUCCESS ? *pool : VK_NULL_HANDLE,info,result);
    if constexpr(researchDiagnostics) if (result == VK_SUCCESS) xess_vk::candidate_sync::pool(state->candidateSync, *pool, info->queueFamilyIndex);
    return result;
}
OBS_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEndCommandBuffer(VkCommandBuffer buffer) {
    const auto trackingStarted = trackingCpuStart();
    auto* state = findDevice(buffer); if (!state || !state->endCommandBuffer) return VK_ERROR_INITIALIZATION_FAILED;
    if(srSdkDepth){return state->endCommandBuffer(buffer);}
    // replacementEnd does not touch render-pass state, so one query serves both.
    const auto endFacts=state->srRuntime||state->srObservationRequested?xess_vk::render_pass_observer::recordingFacts(state->renderPass,buffer):xess_vk::CommandRecordingFacts{};
    if(state->srRuntime)state->srRuntime->replacementEnd(endFacts);
    TrackingCpuScope tracking(state, TrackingCpuGroup::Recording, trackingStarted);
    const auto tailKey=state->srObservationRequested?stc::key(endFacts):stc::Key{};
    const auto result = tracking.driver([&] { return state->endCommandBuffer(buffer); });
    const auto originalError=GetLastError();if(result!=VK_SUCCESS)rawRetireRecording(state,rawKey(state,buffer));
    srTailFinish(state,tailKey,result);
    if constexpr(researchDiagnostics) xess_vk::candidate_sync::end(state->candidateSync, buffer, result);
    xess_vk::render_pass_observer::end(state->renderPass, buffer, result);
    xess_vk::descriptor_image_observer::end(state->descriptors,buffer,result);
    if constexpr(researchDiagnostics) if(state->renderTail&&state->renderTail->active())state->renderTail->end(xess_vk::render_pass_observer::recordingFacts(state->renderPass,buffer),result);
    SetLastError(originalError);return result;
}
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier(VkCommandBuffer buffer, VkPipelineStageFlags src,
    VkPipelineStageFlags dst, VkDependencyFlags flags, uint32_t memoryCount, const VkMemoryBarrier* memory,
    uint32_t bufferCount, const VkBufferMemoryBarrier* buffers, uint32_t imageCount, const VkImageMemoryBarrier* images) {
    const auto trackingStarted = trackingCpuStart();
    auto* state = findDevice(buffer); if (!state || !state->cmdPipelineBarrier) return;
    if(srSdkDepth){state->cmdPipelineBarrier(buffer,src,dst,flags,memoryCount,memory,bufferCount,buffers,imageCount,images);return;}
    srTailCommand(state,buffer,[&]{return stc::barrier(src,dst,flags,memoryCount,memory,bufferCount,buffers,imageCount,images);});
    TrackingCpuScope tracking(state, TrackingCpuGroup::Recording, trackingStarted);
    std::vector<VkImageMemoryBarrier> copy; const VkImageMemoryBarrier* driverImages{};
    try { driverImages = state->fakeEnabled ? xess_vk::presentation::fake_layout::barriers(imageCount, images, fakeImagePredicate(state), copy) : images; }
    catch (...) { fakeRecordingFailed(state); return; }
    tracking.driver([&] { return state->cmdPipelineBarrier(buffer, src, dst, flags, memoryCount, memory, bufferCount, buffers, imageCount, driverImages); });
    if constexpr(researchDiagnostics) xess_vk::candidate_sync::barriers(state->candidateSync, buffer, src, dst, flags, imageCount, images);
    xess_vk::render_pass_observer::explicitImageBarriers(state->renderPass, buffer,src,dst,flags,imageCount,images);
    srUndefinedLayoutDiscards(state,buffer,imageCount,images);
}
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier2(VkCommandBuffer buffer, const VkDependencyInfo* info) {
    const auto trackingStarted = trackingCpuStart();
    auto* state = findDevice(buffer); if (!state || !state->cmdPipelineBarrier2) return;
    if(srSdkDepth){state->cmdPipelineBarrier2(buffer,info);return;}
    srTailCommand(state,buffer,[&]{return stc::barrier2(info);});
    TrackingCpuScope tracking(state, TrackingCpuGroup::Recording, trackingStarted);
    std::vector<VkDependencyInfo> copy; std::vector<std::vector<VkImageMemoryBarrier2>> images; const VkDependencyInfo* driverInfo{};
    try { driverInfo = state->fakeEnabled ? xess_vk::presentation::fake_layout::dependencies(1, info, fakeImagePredicate(state), copy, images) : info; }
    catch (...) { fakeRecordingFailed(state); return; }
    tracking.driver([&] { return state->cmdPipelineBarrier2(buffer, driverInfo); }); if constexpr(researchDiagnostics) xess_vk::candidate_sync::barriers2(state->candidateSync, buffer, info);
    xess_vk::render_pass_observer::explicitImageBarriers2(state->renderPass, buffer,info);
    srUndefinedLayoutDiscards2(state,buffer,1,info);
}
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier2KHR(VkCommandBuffer buffer, const VkDependencyInfo* info) {
    const auto trackingStarted = trackingCpuStart();
    auto* state = findDevice(buffer); if (!state || !state->cmdPipelineBarrier2Khr) return;
    if(srSdkDepth){state->cmdPipelineBarrier2Khr(buffer,info);return;}
    srTailCommand(state,buffer,[&]{return stc::barrier2(info,stc::Api::Khr2);});
    TrackingCpuScope tracking(state, TrackingCpuGroup::Recording, trackingStarted);
    std::vector<VkDependencyInfo> copy; std::vector<std::vector<VkImageMemoryBarrier2>> images; const VkDependencyInfo* driverInfo{};
    try { driverInfo = state->fakeEnabled ? xess_vk::presentation::fake_layout::dependencies(1, info, fakeImagePredicate(state), copy, images) : info; }
    catch (...) { fakeRecordingFailed(state); return; }
    tracking.driver([&] { return state->cmdPipelineBarrier2Khr(buffer, driverInfo); }); if constexpr(researchDiagnostics) xess_vk::candidate_sync::barriers2(state->candidateSync, buffer, info);
    xess_vk::render_pass_observer::explicitImageBarriers2(state->renderPass, buffer,info);
    srUndefinedLayoutDiscards2(state,buffer,1,info);
}
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdWaitEvents(VkCommandBuffer buffer, uint32_t eventCount, const VkEvent* events,
    VkPipelineStageFlags src, VkPipelineStageFlags dst, uint32_t memoryCount, const VkMemoryBarrier* memory,
    uint32_t bufferCount, const VkBufferMemoryBarrier* buffers, uint32_t imageCount, const VkImageMemoryBarrier* images) {
    const auto trackingStarted = trackingCpuStart();
    auto* state = findDevice(buffer); if (!state || !state->cmdWaitEvents) return;
    srTailCommand(state,buffer,[&]{auto snapshot=srTailOther("vkCmdWaitEvents",{eventCount,src,dst,memoryCount,bufferCount,imageCount});snapshot.truncated=true;return snapshot;});
    TrackingCpuScope tracking(state, TrackingCpuGroup::Recording, trackingStarted);
    std::vector<VkImageMemoryBarrier> copy; const VkImageMemoryBarrier* driverImages{};
    try { driverImages = state->fakeEnabled ? xess_vk::presentation::fake_layout::barriers(imageCount, images, fakeImagePredicate(state), copy) : images; }
    catch (...) { fakeRecordingFailed(state); return; }
    tracking.driver([&] { return state->cmdWaitEvents(buffer, eventCount, events, src, dst, memoryCount, memory, bufferCount, buffers, imageCount, driverImages); });
    srUndefinedLayoutDiscards(state,buffer,imageCount,images);
    xess_vk::render_pass_observer::unsupported(state->renderPass, buffer, xess_vk::render_pass_metadata::Issue::UnsupportedExtension);
    if constexpr(researchDiagnostics) xess_vk::candidate_sync::opaque(state->candidateSync, buffer, xess_vk::image_sync::Issue::Events);
}
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdWaitEvents2(VkCommandBuffer buffer, uint32_t count, const VkEvent* events,
    const VkDependencyInfo* infos) {
    const auto trackingStarted = trackingCpuStart();
    auto* state = findDevice(buffer); if (!state || !state->cmdWaitEvents2) return;
    srTailCommand(state,buffer,[&]{auto snapshot=srTailOther("vkCmdWaitEvents2",{count});snapshot.truncated=true;return snapshot;});
    TrackingCpuScope tracking(state, TrackingCpuGroup::Recording, trackingStarted);
    std::vector<VkDependencyInfo> copy; std::vector<std::vector<VkImageMemoryBarrier2>> images; const VkDependencyInfo* driverInfos{};
    try { driverInfos = state->fakeEnabled ? xess_vk::presentation::fake_layout::dependencies(count, infos, fakeImagePredicate(state), copy, images) : infos; }
    catch (...) { fakeRecordingFailed(state); return; }
    tracking.driver([&] { return state->cmdWaitEvents2(buffer, count, events, driverInfos); });
    srUndefinedLayoutDiscards2(state,buffer,count,infos);
    xess_vk::render_pass_observer::unsupported(state->renderPass, buffer, xess_vk::render_pass_metadata::Issue::UnsupportedExtension);
    if constexpr(researchDiagnostics) xess_vk::candidate_sync::opaque(state->candidateSync, buffer, xess_vk::image_sync::Issue::Events);
}
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdWaitEvents2KHR(VkCommandBuffer buffer, uint32_t count, const VkEvent* events,
    const VkDependencyInfo* infos) {
    const auto trackingStarted = trackingCpuStart();
    auto* state = findDevice(buffer); if (!state || !state->cmdWaitEvents2Khr) return;
    srTailCommand(state,buffer,[&]{auto snapshot=srTailOther("vkCmdWaitEvents2KHR",{count});snapshot.truncated=true;return snapshot;});
    TrackingCpuScope tracking(state, TrackingCpuGroup::Recording, trackingStarted);
    std::vector<VkDependencyInfo> copy; std::vector<std::vector<VkImageMemoryBarrier2>> images; const VkDependencyInfo* driverInfos{};
    try { driverInfos = state->fakeEnabled ? xess_vk::presentation::fake_layout::dependencies(count, infos, fakeImagePredicate(state), copy, images) : infos; }
    catch (...) { fakeRecordingFailed(state); return; }
    tracking.driver([&] { return state->cmdWaitEvents2Khr(buffer, count, events, driverInfos); });
    srUndefinedLayoutDiscards2(state,buffer,count,infos);
    xess_vk::render_pass_observer::unsupported(state->renderPass, buffer, xess_vk::render_pass_metadata::Issue::UnsupportedExtension);
    if constexpr(researchDiagnostics) xess_vk::candidate_sync::opaque(state->candidateSync, buffer, xess_vk::image_sync::Issue::Events);
}
namespace {
void setEvent2Mapped(DeviceState* state, PFN_vkCmdSetEvent2 set, VkCommandBuffer buffer, VkEvent event,
    const VkDependencyInfo* info, xess_vk::CpuWallClock::time_point started) {
    if (!state || !set) return;
    if (!state->fakeEnabled) { set(buffer, event, info); return; }
    TrackingCpuScope tracking(state, TrackingCpuGroup::Recording, started);
    std::vector<VkDependencyInfo> copy; std::vector<std::vector<VkImageMemoryBarrier2>> images; const VkDependencyInfo* driverInfo{};
    try { driverInfo = state->fakeEnabled ? xess_vk::presentation::fake_layout::dependencies(1, info, fakeImagePredicate(state), copy, images) : info; }
    catch (...) { fakeRecordingFailed(state); return; }
    tracking.driver([&] { return set(buffer, event, driverInfo); });
    xess_vk::render_pass_observer::unsupported(state->renderPass, buffer, xess_vk::render_pass_metadata::Issue::UnsupportedExtension);
    if constexpr(researchDiagnostics) xess_vk::candidate_sync::opaque(state->candidateSync, buffer, xess_vk::image_sync::Issue::Events);
}
}
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdSetEvent2(VkCommandBuffer buffer, VkEvent event, const VkDependencyInfo* info) {
    const auto started = trackingCpuStart();auto* state = findDevice(buffer);
    srTailCommand(state,buffer,[&]{auto snapshot=srTailOther("vkCmdSetEvent2",{bits(event)});snapshot.truncated=true;return snapshot;});
    setEvent2Mapped(state, state ? state->cmdSetEvent2 : nullptr, buffer, event, info, started);
}
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdSetEvent2KHR(VkCommandBuffer buffer, VkEvent event, const VkDependencyInfo* info) {
    const auto started = trackingCpuStart();auto* state = findDevice(buffer);
    srTailCommand(state,buffer,[&]{auto snapshot=srTailOther("vkCmdSetEvent2KHR",{bits(event)});snapshot.truncated=true;return snapshot;});
    setEvent2Mapped(state, state ? state->cmdSetEvent2Khr : nullptr, buffer, event, info, started);
}
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdBeginRenderPass(VkCommandBuffer buffer, const VkRenderPassBeginInfo* info,
    VkSubpassContents contents) {
    const auto trackingStarted = trackingCpuStart();
    auto* state = findDevice(buffer); if (!state || !state->cmdBeginRenderPass) return;
    srProductCommand(state,buffer,[&]{return stc::beginRenderPass(info,contents);});
    srTailCommand(state,buffer,[&]{return stc::beginRenderPass(info,contents);});
    TrackingCpuScope tracking(state, TrackingCpuGroup::Recording, trackingStarted);
    tracking.driver([&] { return state->cmdBeginRenderPass(buffer, info, contents); });
    xess_vk::render_pass_observer::beginRenderPass(state->renderPass, buffer, info, contents);
    renderTailPass(state,buffer,info);
    if constexpr(researchDiagnostics) xess_vk::candidate_sync::opaque(state->candidateSync, buffer, xess_vk::image_sync::Issue::RenderPass);
}
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdBeginRenderPass2(VkCommandBuffer buffer, const VkRenderPassBeginInfo* info,
    const VkSubpassBeginInfo* begin) {
    const auto trackingStarted = trackingCpuStart();
    auto* state = findDevice(buffer); if (!state || !state->cmdBeginRenderPass2) return;
    srProductCommand(state,buffer,[&]{return stc::beginRenderPass2(info,begin);});
    srTailCommand(state,buffer,[&]{return stc::beginRenderPass2(info,begin);});
    TrackingCpuScope tracking(state, TrackingCpuGroup::Recording, trackingStarted);
    tracking.driver([&] { return state->cmdBeginRenderPass2(buffer, info, begin); });
    xess_vk::render_pass_observer::beginRenderPass2(state->renderPass, buffer, info, begin);
    renderTailPass(state,buffer,info);
    if constexpr(researchDiagnostics) xess_vk::candidate_sync::opaque(state->candidateSync, buffer, xess_vk::image_sync::Issue::RenderPass);
}
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdBeginRenderPass2KHR(VkCommandBuffer buffer, const VkRenderPassBeginInfo* info,
    const VkSubpassBeginInfo* begin) {
    const auto trackingStarted = trackingCpuStart();
    auto* state = findDevice(buffer); if (!state || !state->cmdBeginRenderPass2Khr) return;
    srProductCommand(state,buffer,[&]{return stc::beginRenderPass2(info,begin,stc::Api::Khr2);});
    srTailCommand(state,buffer,[&]{return stc::beginRenderPass2(info,begin,stc::Api::Khr2);});
    TrackingCpuScope tracking(state, TrackingCpuGroup::Recording, trackingStarted);
    tracking.driver([&] { return state->cmdBeginRenderPass2Khr(buffer, info, begin); });
    xess_vk::render_pass_observer::beginRenderPass2(state->renderPass, buffer, info, begin);
    renderTailPass(state,buffer,info);
    if constexpr(researchDiagnostics) xess_vk::candidate_sync::opaque(state->candidateSync, buffer, xess_vk::image_sync::Issue::RenderPass);
}
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdNextSubpass(VkCommandBuffer buffer, VkSubpassContents contents) {
    const auto trackingStarted = trackingCpuStart();
    auto* state = findDevice(buffer); if (!state || !state->cmdNextSubpass) return;
    srTailCommand(state,buffer,[&]{auto snapshot=srTailOther("vkCmdNextSubpass",{contents});snapshot.truncated=true;return snapshot;});
    TrackingCpuScope tracking(state, TrackingCpuGroup::Recording, trackingStarted);
    tracking.driver([&] { return state->cmdNextSubpass(buffer, contents); });
    xess_vk::render_pass_observer::nextSubpass(state->renderPass, buffer);
    if constexpr(researchDiagnostics) xess_vk::candidate_sync::opaque(state->candidateSync, buffer, xess_vk::image_sync::Issue::RenderPass);
}
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdNextSubpass2(VkCommandBuffer buffer, const VkSubpassBeginInfo* begin, const VkSubpassEndInfo* end) {
    const auto trackingStarted = trackingCpuStart();
    auto* state = findDevice(buffer); if (!state || !state->cmdNextSubpass2) return;
    srTailCommand(state,buffer,[&]{auto snapshot=srTailOther("vkCmdNextSubpass2",{});snapshot.truncated=true;return snapshot;});
    TrackingCpuScope tracking(state, TrackingCpuGroup::Recording, trackingStarted);
    tracking.driver([&] { return state->cmdNextSubpass2(buffer, begin, end); });
    xess_vk::render_pass_observer::nextSubpass(state->renderPass, buffer);
    if constexpr(researchDiagnostics) xess_vk::candidate_sync::opaque(state->candidateSync, buffer, xess_vk::image_sync::Issue::RenderPass);
}
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdNextSubpass2KHR(VkCommandBuffer buffer, const VkSubpassBeginInfo* begin, const VkSubpassEndInfo* end) {
    const auto trackingStarted = trackingCpuStart();
    auto* state = findDevice(buffer); if (!state || !state->cmdNextSubpass2Khr) return;
    srTailCommand(state,buffer,[&]{auto snapshot=srTailOther("vkCmdNextSubpass2KHR",{});snapshot.truncated=true;return snapshot;});
    TrackingCpuScope tracking(state, TrackingCpuGroup::Recording, trackingStarted);
    tracking.driver([&] { return state->cmdNextSubpass2Khr(buffer, begin, end); });
    xess_vk::render_pass_observer::nextSubpass(state->renderPass, buffer);
    if constexpr(researchDiagnostics) xess_vk::candidate_sync::opaque(state->candidateSync, buffer, xess_vk::image_sync::Issue::RenderPass);
}
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdEndRenderPass(VkCommandBuffer buffer) {
    const auto trackingStarted = trackingCpuStart();
    auto* state = findDevice(buffer); if (!state || !state->cmdEndRenderPass) return;
    srTailCommand(state,buffer,[&]{auto snapshot=srTailOther("vkCmdEndRenderPass",{});snapshot.truncated=true;return snapshot;});
    TrackingCpuScope tracking(state, TrackingCpuGroup::Recording, trackingStarted);
    tracking.driver([&] { return state->cmdEndRenderPass(buffer); });
    xess_vk::render_pass_observer::endRenderPass(state->renderPass, buffer);
    if constexpr(researchDiagnostics) if(state->renderTail&&state->renderTail->active())state->renderTail->endPass(xess_vk::render_pass_observer::recordingFacts(state->renderPass,buffer));
    if constexpr(researchDiagnostics) xess_vk::candidate_sync::opaque(state->candidateSync, buffer, xess_vk::image_sync::Issue::RenderPass);
}
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdEndRenderPass2(VkCommandBuffer buffer, const VkSubpassEndInfo* end) {
    const auto trackingStarted = trackingCpuStart();
    auto* state = findDevice(buffer); if (!state || !state->cmdEndRenderPass2) return;
    srTailCommand(state,buffer,[&]{auto snapshot=srTailOther("vkCmdEndRenderPass2",{});snapshot.truncated=true;return snapshot;});
    TrackingCpuScope tracking(state, TrackingCpuGroup::Recording, trackingStarted);
    tracking.driver([&] { return state->cmdEndRenderPass2(buffer, end); });
    xess_vk::render_pass_observer::endRenderPass2(state->renderPass, buffer, end);
    if constexpr(researchDiagnostics) if(state->renderTail&&state->renderTail->active())state->renderTail->endPass(xess_vk::render_pass_observer::recordingFacts(state->renderPass,buffer));
    if constexpr(researchDiagnostics) xess_vk::candidate_sync::opaque(state->candidateSync, buffer, xess_vk::image_sync::Issue::RenderPass);
}
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdEndRenderPass2KHR(VkCommandBuffer buffer, const VkSubpassEndInfo* end) {
    const auto trackingStarted = trackingCpuStart();
    auto* state = findDevice(buffer); if (!state || !state->cmdEndRenderPass2Khr) return;
    srTailCommand(state,buffer,[&]{auto snapshot=srTailOther("vkCmdEndRenderPass2KHR",{});snapshot.truncated=true;return snapshot;});
    TrackingCpuScope tracking(state, TrackingCpuGroup::Recording, trackingStarted);
    tracking.driver([&] { return state->cmdEndRenderPass2Khr(buffer, end); });
    xess_vk::render_pass_observer::endRenderPass2(state->renderPass, buffer, end);
    if constexpr(researchDiagnostics) if(state->renderTail&&state->renderTail->active())state->renderTail->endPass(xess_vk::render_pass_observer::recordingFacts(state->renderPass,buffer));
    if constexpr(researchDiagnostics) xess_vk::candidate_sync::opaque(state->candidateSync, buffer, xess_vk::image_sync::Issue::RenderPass);
}
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdBeginRendering(VkCommandBuffer buffer, const VkRenderingInfo* info) {
    const auto trackingStarted = trackingCpuStart();
    auto* state = findDevice(buffer); if (!state || !state->cmdBeginRendering) return;
    srProductCommand(state,buffer,[&]{return stc::beginRendering(info);});
    srTailCommand(state,buffer,[&]{return stc::beginRendering(info);});
    TrackingCpuScope tracking(state, TrackingCpuGroup::Recording, trackingStarted);
    tracking.driver([&] { return state->cmdBeginRendering(buffer, info); });
    xess_vk::render_pass_observer::beginDynamicRendering(state->renderPass, buffer, info);
    renderTailDynamic(state,buffer,info);
    if constexpr(researchDiagnostics) xess_vk::candidate_sync::opaque(state->candidateSync, buffer, xess_vk::image_sync::Issue::RenderPass);
}
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdBeginRenderingKHR(VkCommandBuffer buffer, const VkRenderingInfo* info) {
    const auto trackingStarted = trackingCpuStart();
    auto* state = findDevice(buffer); if (!state || !state->cmdBeginRenderingKhr) return;
    srProductCommand(state,buffer,[&]{return stc::beginRendering(info,stc::Api::Khr);});
    srTailCommand(state,buffer,[&]{return stc::beginRendering(info,stc::Api::Khr);});
    TrackingCpuScope tracking(state, TrackingCpuGroup::Recording, trackingStarted);
    tracking.driver([&] { return state->cmdBeginRenderingKhr(buffer, info); });
    xess_vk::render_pass_observer::beginDynamicRendering(state->renderPass, buffer, info);
    renderTailDynamic(state,buffer,info);
    if constexpr(researchDiagnostics) xess_vk::candidate_sync::opaque(state->candidateSync, buffer, xess_vk::image_sync::Issue::RenderPass);
}
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdEndRendering(VkCommandBuffer buffer) {
    const auto trackingStarted = trackingCpuStart();
    auto* state = findDevice(buffer); if (!state || !state->cmdEndRendering) return;
    srTailCommand(state,buffer,[&]{auto snapshot=srTailOther("vkCmdEndRendering",{});snapshot.truncated=true;return snapshot;});
    TrackingCpuScope tracking(state, TrackingCpuGroup::Recording, trackingStarted);
    tracking.driver([&] { return state->cmdEndRendering(buffer); });
    xess_vk::render_pass_observer::endDynamicRendering(state->renderPass, buffer);
    if constexpr(researchDiagnostics) if(state->renderTail&&state->renderTail->active())state->renderTail->endPass(xess_vk::render_pass_observer::recordingFacts(state->renderPass,buffer));
    if constexpr(researchDiagnostics) xess_vk::candidate_sync::opaque(state->candidateSync, buffer, xess_vk::image_sync::Issue::RenderPass);
}
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdEndRenderingKHR(VkCommandBuffer buffer) {
    const auto trackingStarted = trackingCpuStart();
    auto* state = findDevice(buffer); if (!state || !state->cmdEndRenderingKhr) return;
    srTailCommand(state,buffer,[&]{auto snapshot=srTailOther("vkCmdEndRenderingKHR",{});snapshot.truncated=true;return snapshot;});
    TrackingCpuScope tracking(state, TrackingCpuGroup::Recording, trackingStarted);
    tracking.driver([&] { return state->cmdEndRenderingKhr(buffer); });
    xess_vk::render_pass_observer::endDynamicRendering(state->renderPass, buffer);
    if constexpr(researchDiagnostics) if(state->renderTail&&state->renderTail->active())state->renderTail->endPass(xess_vk::render_pass_observer::recordingFacts(state->renderPass,buffer));
    if constexpr(researchDiagnostics) xess_vk::candidate_sync::opaque(state->candidateSync, buffer, xess_vk::image_sync::Issue::RenderPass);
}
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdExecuteCommands(VkCommandBuffer buffer, uint32_t count, const VkCommandBuffer* buffers) {
    const auto trackingStarted = trackingCpuStart();
    auto* state = findDevice(buffer); if (!state || !state->cmdExecuteCommands) return;
    srProductCommand(state,buffer,[&]{auto snapshot=srTailOther("vkCmdExecuteCommands",{count});snapshot.truncated=true;return snapshot;});
    srTailCommand(state,buffer,[&]{auto snapshot=srTailOther("vkCmdExecuteCommands",{count});snapshot.truncated=true;return snapshot;});
    TrackingCpuScope tracking(state, TrackingCpuGroup::Recording, trackingStarted);
    xess_vk::descriptor_image_observer::commandUnsupported(state->descriptors,buffer,xess_vk::descriptor_image_observer::Issue::Secondary);
    if constexpr(researchDiagnostics) if(state->renderTail&&state->renderTail->active()) {
        xess_vk::render_tail::Event event;event.kind=xess_vk::render_tail::Kind::Secondary;
        event.recording=xess_vk::render_pass_observer::recordingFacts(state->renderPass,buffer);event.frame=renderTailFrame(state);event.issues=1;
        if(buffers)for(std::size_t n=0;n<count;++n){if(event.secondaryCount==event.secondary.size()){event.secondaryDrops=count-event.secondaryCount;break;}
            const auto child=xess_vk::render_pass_observer::recordingFacts(state->renderPass,buffers[n]);event.secondary[event.secondaryCount++]={bits(buffers[n]),child.rpRecordingGeneration};}
        state->renderTail->record(event);
    }
    tracking.driver([&] { return state->cmdExecuteCommands(buffer, count, buffers); });
    xess_vk::render_pass_observer::unsupported(state->renderPass, buffer, xess_vk::render_pass_metadata::Issue::Secondary);
    if constexpr(researchDiagnostics) xess_vk::candidate_sync::opaque(state->candidateSync, buffer, xess_vk::image_sync::Issue::SecondaryCommands);
}
namespace {
void srImageTouch(DeviceState* state,VkCommandBuffer command,VkImage image,
                  xess_vk::RecordedImageTouch kind=xess_vk::RecordedImageTouch::Transfer,bool discard=false) noexcept {
    if(!state||!(state->srRuntimeRequested||state->srObservationRequested)||srTailInjectedDepth)return;
    SrTailErrorGuard error;
    if(state->srRuntime)state->srRuntime->replacementImage(image,discard,xess_vk::render_pass_observer::recordingFacts(state->renderPass,command));
    const auto auditWindow=xess_vk::sr_observation::consumerWindow(bits(state->device));
    xess_vk::render_pass_observer::imageTouched(state->renderPass,command,image,kind);
    if(auditWindow){
        const auto resolved=renderTailImage(state,bits(image),0);
        xess_vk::sr_observation::consumerImage(bits(state->device),auditWindow,xess_vk::render_pass_observer::recordingFacts(state->renderPass,command),
            {resolved.record.image,resolved.record.generation},xess_vk::resource_memory_observer::sourceQuery(state->resourceMemory));}
}
xess_vk::render_tail::Event tailTransfer(DeviceState* s,VkCommandBuffer command,VkImage src,VkImageLayout srcLayout,VkImage dst,VkImageLayout dstLayout,xess_vk::render_tail::Kind kind) noexcept {
    if constexpr(!researchDiagnostics)return {};
    xess_vk::render_tail::Event event;event.kind=kind;event.recording=xess_vk::render_pass_observer::recordingFacts(s->renderPass,command);
    event.frame=renderTailFrame(s);event.sourceLayout=srcLayout;event.destinationLayout=dstLayout;event.imageCount=2;
    event.images[0]=renderTailImage(s,bits(src),xess_vk::render_tail::TransferSource);event.images[0].layout=srcLayout;
    event.images[1]=renderTailImage(s,bits(dst),xess_vk::render_tail::TransferDestination);event.images[1].layout=dstLayout;return event;
}
void tailTransferLayouts(DeviceState* s,xess_vk::render_tail::Event& event) noexcept {
    if constexpr(!researchDiagnostics)return;
    if(!event.regionCount)return;
    const VkImageSubresourceLayers layers[]{event.regions[0].source,event.regions[0].destination};
    for(std::size_t n=0;n<2;++n) {
        auto& image=event.images[n];image.range={layers[n].aspectMask,layers[n].mipLevel,1,layers[n].baseArrayLayer,layers[n].layerCount};
        if(event.regionCount!=1||event.regionDrops||!image.record.generation)continue;
        const auto result=xess_vk::render_pass_observer::resolveSourceAt(s->renderPass,event.recording,{image.record.image,image.record.generation},image.range,image.layout);
        image.layoutQueried=result.queried;image.layoutResolved=result.recordedLayoutResolved;image.layoutPrefixSupported=result.recordedPrefixSupported;
        image.layoutPrefix=result.prefixOrder;image.layoutIssues=result.issues;image.recordedLayout=result.recordedLayout;
        image.layoutLastOrder=result.lastEvent.order;image.barrierOldLayout=result.lastEvent.oldLayout;image.barrierNewLayout=result.lastEvent.newLayout;
        image.srcStage=result.lastEvent.srcStage;image.dstStage=result.lastEvent.dstStage;image.srcAccess=result.lastEvent.srcAccess;image.dstAccess=result.lastEvent.dstAccess;
    }
}
template<class Region>void tailCopies(DeviceState* s,VkCommandBuffer command,VkImage src,VkImageLayout sl,VkImage dst,VkImageLayout dl,std::uint32_t count,const Region* regions) noexcept {
    if constexpr(!researchDiagnostics)return;
    if(!s->renderTail||!s->renderTail->active())return;const ContinuousDiagnosticErrorGuard guard;
    auto event=tailTransfer(s,command,src,sl,dst,dl,xess_vk::render_tail::Kind::Copy);
    if(regions)for(std::size_t n=0;n<count;++n) {
        if(event.regionCount==event.regions.size()){event.regionDrops=count-event.regionCount;break;}
        const auto& in=regions[n];auto& out=event.regions[event.regionCount++];out.source=in.srcSubresource;out.destination=in.dstSubresource;
        out.sourceOffsets[0]=in.srcOffset;out.destinationOffsets[0]=in.dstOffset;out.extent=in.extent;
    }
    if(count&&!regions)event.issues|=1;tailTransferLayouts(s,event);s->renderTail->record(event);
}
template<class Region>void tailBlits(DeviceState* s,VkCommandBuffer command,VkImage src,VkImageLayout sl,VkImage dst,VkImageLayout dl,std::uint32_t count,const Region* regions,VkFilter filter) noexcept {
    if constexpr(!researchDiagnostics)return;
    if(!s->renderTail||!s->renderTail->active())return;const ContinuousDiagnosticErrorGuard guard;
    auto event=tailTransfer(s,command,src,sl,dst,dl,xess_vk::render_tail::Kind::Blit);event.filter=filter;
    if(regions)for(std::size_t n=0;n<count;++n) {
        if(event.regionCount==event.regions.size()){event.regionDrops=count-event.regionCount;break;}
        const auto& in=regions[n];auto& out=event.regions[event.regionCount++];out.source=in.srcSubresource;out.destination=in.dstSubresource;
        for(unsigned i=0;i<2;++i){out.sourceOffsets[i]=in.srcOffsets[i];out.destinationOffsets[i]=in.dstOffsets[i];}
    }
    if(count&&!regions)event.issues|=1;tailTransferLayouts(s,event);s->renderTail->record(event);
}
}
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdCopyImage(VkCommandBuffer b,VkImage src,VkImageLayout sl,VkImage dst,VkImageLayout dl,uint32_t count,const VkImageCopy* regions) {
    auto* s=findDevice(b);if(!s||!s->cmdCopyImage)return;
    srImageTouch(s,b,src);srImageTouch(s,b,dst);
    srTailCommand(s,b,[&]{auto snapshot=srTailOther("vkCmdCopyImage",{bits(src),sl,bits(dst),dl,count});snapshot.truncated=true;return snapshot;});tailCopies(s,b,src,sl,dst,dl,count,regions);s->cmdCopyImage(b,src,sl,dst,dl,count,regions);
}
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdCopyImage2(VkCommandBuffer b,const VkCopyImageInfo2* info) {
    auto* s=findDevice(b);if(!s||!s->cmdCopyImage2)return;
    srImageTouch(s,b,info?info->srcImage:VK_NULL_HANDLE);if(info)srImageTouch(s,b,info->dstImage);
    srTailCommand(s,b,[&]{auto snapshot=srTailOther("vkCmdCopyImage2",{});snapshot.truncated=true;return snapshot;});if(info)tailCopies(s,b,info->srcImage,info->srcImageLayout,info->dstImage,info->dstImageLayout,info->regionCount,info->pRegions);s->cmdCopyImage2(b,info);
}
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdCopyImage2KHR(VkCommandBuffer b,const VkCopyImageInfo2* info) {
    auto* s=findDevice(b);if(!s||!s->cmdCopyImage2Khr)return;
    srImageTouch(s,b,info?info->srcImage:VK_NULL_HANDLE);if(info)srImageTouch(s,b,info->dstImage);
    srTailCommand(s,b,[&]{auto snapshot=srTailOther("vkCmdCopyImage2KHR",{});snapshot.truncated=true;return snapshot;});if(info)tailCopies(s,b,info->srcImage,info->srcImageLayout,info->dstImage,info->dstImageLayout,info->regionCount,info->pRegions);s->cmdCopyImage2Khr(b,info);
}
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdBlitImage(VkCommandBuffer b,VkImage src,VkImageLayout sl,VkImage dst,VkImageLayout dl,uint32_t count,const VkImageBlit* regions,VkFilter filter) {
    auto* s=findDevice(b);if(!s||!s->cmdBlitImage)return;
    srImageTouch(s,b,src);srImageTouch(s,b,dst);
    srTailCommand(s,b,[&]{auto snapshot=srTailOther("vkCmdBlitImage",{bits(src),sl,bits(dst),dl,count,filter});snapshot.truncated=true;return snapshot;});tailBlits(s,b,src,sl,dst,dl,count,regions,filter);s->cmdBlitImage(b,src,sl,dst,dl,count,regions,filter);
}
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdBlitImage2(VkCommandBuffer b,const VkBlitImageInfo2* info) {
    auto* s=findDevice(b);if(!s||!s->cmdBlitImage2)return;
    srImageTouch(s,b,info?info->srcImage:VK_NULL_HANDLE);if(info)srImageTouch(s,b,info->dstImage);
    srTailCommand(s,b,[&]{auto snapshot=srTailOther("vkCmdBlitImage2",{});snapshot.truncated=true;return snapshot;});if(info)tailBlits(s,b,info->srcImage,info->srcImageLayout,info->dstImage,info->dstImageLayout,info->regionCount,info->pRegions,info->filter);s->cmdBlitImage2(b,info);
}
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdBlitImage2KHR(VkCommandBuffer b,const VkBlitImageInfo2* info) {
    auto* s=findDevice(b);if(!s||!s->cmdBlitImage2Khr)return;
    srImageTouch(s,b,info?info->srcImage:VK_NULL_HANDLE);if(info)srImageTouch(s,b,info->dstImage);
    srTailCommand(s,b,[&]{auto snapshot=srTailOther("vkCmdBlitImage2KHR",{});snapshot.truncated=true;return snapshot;});if(info)tailBlits(s,b,info->srcImage,info->srcImageLayout,info->dstImage,info->dstImageLayout,info->regionCount,info->pRegions,info->filter);s->cmdBlitImage2Khr(b,info);
}
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdDraw(VkCommandBuffer b,uint32_t vertices,uint32_t instances,uint32_t firstVertex,uint32_t firstInstance) {
    const auto trackingStarted=trackingCpuStart();
    auto* s=findDevice(b);if(!s||!s->cmdDraw)return;
    TrackingCpuScope tracking(s,TrackingCpuGroup::Draw,trackingStarted);
    if(srProductDraw(s,b,stc::Kind::Draw))return;
    if(srTailCommand(s,b,[&]{return srTailSimple(stc::Kind::Draw,{vertices,instances,firstVertex,firstInstance},stc::Api::Core);}))return;if(vertices&&instances)renderTailDraw(s,b);tracking.driver([&]{s->cmdDraw(b,vertices,instances,firstVertex,firstInstance);});
}
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndexed(VkCommandBuffer b,uint32_t indices,uint32_t instances,uint32_t firstIndex,int32_t vertexOffset,uint32_t firstInstance) {
    const auto trackingStarted=trackingCpuStart();
    auto* s=findDevice(b);if(!s||!s->cmdDrawIndexed)return;
    TrackingCpuScope tracking(s,TrackingCpuGroup::Draw,trackingStarted);
    if(srProductDraw(s,b,stc::Kind::DrawIndexed))return;
    if(srTailCommand(s,b,[&]{return srTailSimple(stc::Kind::DrawIndexed,{indices,instances,firstIndex,static_cast<std::uint64_t>(static_cast<std::int64_t>(vertexOffset)),firstInstance},stc::Api::Core);}))return;if(indices&&instances)renderTailDraw(s,b);tracking.driver([&]{s->cmdDrawIndexed(b,indices,instances,firstIndex,vertexOffset,firstInstance);});
}
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndirect(VkCommandBuffer b,VkBuffer indirect,VkDeviceSize offset,uint32_t count,uint32_t stride) {
    const auto trackingStarted=trackingCpuStart();
    auto* s=findDevice(b);if(!s||!s->cmdDrawIndirect)return;
    TrackingCpuScope tracking(s,TrackingCpuGroup::Draw,trackingStarted);
    if(srProductDraw(s,b,stc::Kind::DrawIndirect))return;
    if(srTailCommand(s,b,[&]{return srTailSimple(stc::Kind::DrawIndirect,{bits(indirect),offset,count,stride},stc::Api::Core);}))return;if(count)renderTailDraw(s,b);tracking.driver([&]{s->cmdDrawIndirect(b,indirect,offset,count,stride);});
}
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndexedIndirect(VkCommandBuffer b,VkBuffer indirect,VkDeviceSize offset,uint32_t count,uint32_t stride) {
    const auto trackingStarted=trackingCpuStart();
    auto* s=findDevice(b);if(!s||!s->cmdDrawIndexedIndirect)return;
    TrackingCpuScope tracking(s,TrackingCpuGroup::Draw,trackingStarted);
    if(srProductDraw(s,b,stc::Kind::DrawIndexedIndirect))return;
    if(srTailCommand(s,b,[&]{return srTailSimple(stc::Kind::DrawIndexedIndirect,{bits(indirect),offset,count,stride},stc::Api::Core);}))return;if(count)renderTailDraw(s,b);tracking.driver([&]{s->cmdDrawIndexedIndirect(b,indirect,offset,count,stride);});
}
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdBindPipeline(VkCommandBuffer buffer, VkPipelineBindPoint bindPoint, VkPipeline pipeline) {
    const auto trackingStarted = trackingCpuStart();
    auto* state = findDevice(buffer); if (!state || !state->cmdBindPipeline) return;
    if(srSdkDepth){state->cmdBindPipeline(buffer,bindPoint,pipeline);return;}
    srTailCommand(state,buffer,[&]{return srTailSimple(stc::Kind::BindPipeline,{bindPoint,bits(pipeline)},stc::Api::Core);});
    TrackingCpuScope tracking(state, TrackingCpuGroup::Bind, trackingStarted);
    tracking.driver([&] { return state->cmdBindPipeline(buffer, bindPoint, pipeline); });
    if constexpr(researchDiagnostics) if (state->compute) xess_vk::compute_observer::bindPipeline(state->compute, buffer, bindPoint, pipeline);
    xess_vk::descriptor_image_observer::bindPipeline(state->descriptors,buffer,bindPoint,pipeline);
    if constexpr(researchDiagnostics) if(state->renderTail&&state->renderTail->active()&&bindPoint==VK_PIPELINE_BIND_POINT_GRAPHICS)
        state->renderTail->bindPipeline(xess_vk::render_pass_observer::recordingFacts(state->renderPass,buffer),bits(pipeline));
}
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdDispatch(VkCommandBuffer buffer, uint32_t x, uint32_t y, uint32_t z) {
    const auto applicationError=GetLastError();
    const auto trackingStarted = trackingCpuStart();
    auto* state = findDevice(buffer); if (!state || !state->cmdDispatch) return;
    if(srSdkDepth){state->cmdDispatch(buffer,x,y,z);return;}
    srTailCommand(state,buffer,[&]{return srTailSimple(stc::Kind::Dispatch,{x,y,z},stc::Api::Core);});
    TrackingCpuScope tracking(state, TrackingCpuGroup::Dispatch, trackingStarted);
    const auto recording = xess_vk::render_pass_observer::dispatchPoint(state->renderPass,buffer);

    std::optional<xess_vk::sr_observation::DispatchTicket> srTicket;
    xess_vk::descriptor_image_metadata::DispatchEvidence srEvidence;
    if(state->descriptors) {
        const auto evidence = rawPreDispatch(state,buffer,x,y,z,xess_vk::descriptor_image_observer::DispatchKind::Direct,recording);
        if(state->srRuntime||xess_vk::sr_observation::consumerActive(bits(state->device)))srEvidence=evidence;
        renderTailDispatch(state,recording,evidence);
        if(!evidence.diagnosticSelectionBypassed)continuousRecord(state, recording, evidence);
        if(state->srObservationRequested&&xess_vk::sr_observation::active(bits(state->device)))srTicket=srObservationDispatch(state,recording,evidence);
    }
#ifdef XESS_METADATA_CONSUMER_TEST
    if(srTailDispatchOverride)srTicket=*srTailDispatchOverride;
#endif
    if(state->srRuntime&&srTicket)state->srRuntime->prepare(recording,*srTicket);
    const bool suppressed=srInterceptDispatch(state,recording,srEvidence,srTicket?*srTicket:xess_vk::sr_observation::DispatchTicket{},true);
    const auto srGpuTicket=suppressed?SrGpuWrite{}:srGpuBeforeDispatch(state,recording,srTicket);
    if(!suppressed)tracking.driver([&] { return state->cmdDispatch(buffer, x, y, z); });
    const auto originalError=suppressed?applicationError:GetLastError();
    if(state->srRuntime)state->srRuntime->replacementFullWriteReturned(recording,!suppressed);
    srGpuAfterDispatch(state,srGpuTicket);
    if(srTicket) {
        if(state->srRuntime)state->srRuntime->replacementReturned(*srTicket);
        srTailMatched(state,recording,*srTicket);
        if(!suppressed)srDispatchReturned(state,*srTicket,"vkCmdDispatch",recording,srEvidence);
    }
    if constexpr(researchDiagnostics) if (state->compute) xess_vk::compute_observer::dispatch(state->compute, buffer, x, y, z, "dispatch");
    SetLastError(originalError);
}
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdDispatchIndirect(VkCommandBuffer buffer, VkBuffer indirect, VkDeviceSize offset) {
    const auto applicationError=GetLastError();
    const auto trackingStarted = trackingCpuStart();
    auto* state = findDevice(buffer); if (!state || !state->cmdDispatchIndirect) return;
    if(srSdkDepth){state->cmdDispatchIndirect(buffer,indirect,offset);return;}
    srTailCommand(state,buffer,[&]{return srTailSimple(stc::Kind::DispatchIndirect,{bits(indirect),offset},stc::Api::Core);});
    TrackingCpuScope tracking(state, TrackingCpuGroup::Dispatch, trackingStarted);
    const auto recording = xess_vk::render_pass_observer::dispatchPoint(state->renderPass,buffer);

    std::optional<xess_vk::sr_observation::DispatchTicket> srTicket;
    xess_vk::descriptor_image_metadata::DispatchEvidence srEvidence;
    if(state->descriptors) {
        const auto evidence=rawPreDispatch(state,buffer,0,0,0,xess_vk::descriptor_image_observer::DispatchKind::Indirect,recording);
        if(state->srRuntime||xess_vk::sr_observation::consumerActive(bits(state->device)))srEvidence=evidence;
        renderTailDispatch(state,recording,evidence);
        if(state->srObservationRequested&&xess_vk::sr_observation::active(bits(state->device)))srTicket=srObservationDispatch(state,recording,evidence);
    }
#ifdef XESS_METADATA_CONSUMER_TEST
    if(srTailDispatchOverride)srTicket=*srTailDispatchOverride;
#endif
    if(state->srRuntime&&srTicket)state->srRuntime->prepare(recording,*srTicket);
    const bool suppressed=srInterceptDispatch(state,recording,srEvidence,srTicket?*srTicket:xess_vk::sr_observation::DispatchTicket{},false);
    const auto srGpuTicket=suppressed?SrGpuWrite{}:srGpuBeforeDispatch(state,recording,srTicket);
    if(!suppressed)tracking.driver([&] { return state->cmdDispatchIndirect(buffer, indirect, offset); });
    const auto originalError=suppressed?applicationError:GetLastError();
    if(state->srRuntime)state->srRuntime->replacementFullWriteReturned(recording,!suppressed);
    srGpuAfterDispatch(state,srGpuTicket);
    if(srTicket) {
        if(state->srRuntime)state->srRuntime->replacementReturned(*srTicket);
        srTailMatched(state,recording,*srTicket);
        if(!suppressed)srDispatchReturned(state,*srTicket,"vkCmdDispatchIndirect",recording,srEvidence);
    }
    if constexpr(researchDiagnostics) if (state->compute) xess_vk::compute_observer::dispatch(state->compute, buffer, 0, 0, 0, "dispatch_indirect", indirect, offset);
    SetLastError(originalError);
}
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdDispatchBase(VkCommandBuffer buffer, uint32_t baseX, uint32_t baseY, uint32_t baseZ,
    uint32_t x, uint32_t y, uint32_t z) {
    const auto applicationError=GetLastError();
    const auto trackingStarted = trackingCpuStart();
    auto* state = findDevice(buffer); if (!state || !state->cmdDispatchBase) return;
    if(srSdkDepth){state->cmdDispatchBase(buffer,baseX,baseY,baseZ,x,y,z);return;}
    srTailCommand(state,buffer,[&]{return srTailSimple(stc::Kind::DispatchBase,{baseX,baseY,baseZ,x,y,z},stc::Api::Core);});
    TrackingCpuScope tracking(state, TrackingCpuGroup::Dispatch, trackingStarted);
    const auto recording = xess_vk::render_pass_observer::dispatchPoint(state->renderPass,buffer);

    std::optional<xess_vk::sr_observation::DispatchTicket> srTicket;
    xess_vk::descriptor_image_metadata::DispatchEvidence srEvidence;
    if(state->descriptors) {
        const auto evidence = rawPreDispatch(state,buffer,x,y,z,(baseX||baseY||baseZ)?xess_vk::descriptor_image_observer::DispatchKind::Base:xess_vk::descriptor_image_observer::DispatchKind::Direct,recording);
        if(state->srRuntime||xess_vk::sr_observation::consumerActive(bits(state->device)))srEvidence=evidence;
        renderTailDispatch(state,recording,evidence);
        if(!evidence.diagnosticSelectionBypassed)continuousRecord(state, recording, evidence);
        if(state->srObservationRequested&&xess_vk::sr_observation::active(bits(state->device)))srTicket=srObservationDispatch(state,recording,evidence);
    }
#ifdef XESS_METADATA_CONSUMER_TEST
    if(srTailDispatchOverride)srTicket=*srTailDispatchOverride;
#endif
    if(state->srRuntime&&srTicket)state->srRuntime->prepare(recording,*srTicket);
    const bool suppressed=srInterceptDispatch(state,recording,srEvidence,srTicket?*srTicket:xess_vk::sr_observation::DispatchTicket{},baseX==0&&baseY==0&&baseZ==0);
    const auto srGpuTicket=suppressed?SrGpuWrite{}:srGpuBeforeDispatch(state,recording,srTicket);
    if(!suppressed)tracking.driver([&] { return state->cmdDispatchBase(buffer, baseX, baseY, baseZ, x, y, z); });
    const auto originalError=suppressed?applicationError:GetLastError();
    if(state->srRuntime)state->srRuntime->replacementFullWriteReturned(recording,!suppressed);
    srGpuAfterDispatch(state,srGpuTicket);
    if(srTicket) {
        if(state->srRuntime)state->srRuntime->replacementReturned(*srTicket);
        srTailMatched(state,recording,*srTicket);
        if(!suppressed)srDispatchReturned(state,*srTicket,"vkCmdDispatchBase",recording,srEvidence);
    }
    // Base coordinates are forwarded unchanged, but are not in this metadata API.
    if constexpr(researchDiagnostics) if (state->compute) xess_vk::compute_observer::dispatch(state->compute, buffer, x, y, z, "dispatch_base");
    SetLastError(originalError);
}
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdDispatchBaseKHR(VkCommandBuffer buffer, uint32_t baseX, uint32_t baseY, uint32_t baseZ,
    uint32_t x, uint32_t y, uint32_t z) {
    const auto applicationError=GetLastError();
    const auto trackingStarted = trackingCpuStart();
    auto* state = findDevice(buffer); if (!state || !state->cmdDispatchBaseKhr) return;
    if(srSdkDepth){state->cmdDispatchBaseKhr(buffer,baseX,baseY,baseZ,x,y,z);return;}
    srTailCommand(state,buffer,[&]{return srTailSimple(stc::Kind::DispatchBase,{baseX,baseY,baseZ,x,y,z},stc::Api::Khr);});
    TrackingCpuScope tracking(state, TrackingCpuGroup::Dispatch, trackingStarted);
    const auto recording = xess_vk::render_pass_observer::dispatchPoint(state->renderPass,buffer);

    std::optional<xess_vk::sr_observation::DispatchTicket> srTicket;
    xess_vk::descriptor_image_metadata::DispatchEvidence srEvidence;
    if(state->descriptors) {
        const auto evidence = rawPreDispatch(state,buffer,x,y,z,(baseX||baseY||baseZ)?xess_vk::descriptor_image_observer::DispatchKind::Base:xess_vk::descriptor_image_observer::DispatchKind::Direct,recording);
        if(state->srRuntime||xess_vk::sr_observation::consumerActive(bits(state->device)))srEvidence=evidence;
        renderTailDispatch(state,recording,evidence);
        if(!evidence.diagnosticSelectionBypassed)continuousRecord(state, recording, evidence);
        if(state->srObservationRequested&&xess_vk::sr_observation::active(bits(state->device)))srTicket=srObservationDispatch(state,recording,evidence);
    }
#ifdef XESS_METADATA_CONSUMER_TEST
    if(srTailDispatchOverride)srTicket=*srTailDispatchOverride;
#endif
    if(state->srRuntime&&srTicket)state->srRuntime->prepare(recording,*srTicket);
    const bool suppressed=srInterceptDispatch(state,recording,srEvidence,srTicket?*srTicket:xess_vk::sr_observation::DispatchTicket{},baseX==0&&baseY==0&&baseZ==0);
    const auto srGpuTicket=suppressed?SrGpuWrite{}:srGpuBeforeDispatch(state,recording,srTicket);
    if(!suppressed)tracking.driver([&] { return state->cmdDispatchBaseKhr(buffer, baseX, baseY, baseZ, x, y, z); });
    const auto originalError=suppressed?applicationError:GetLastError();
    if(state->srRuntime)state->srRuntime->replacementFullWriteReturned(recording,!suppressed);
    srGpuAfterDispatch(state,srGpuTicket);
    if(srTicket) {
        if(state->srRuntime)state->srRuntime->replacementReturned(*srTicket);
        srTailMatched(state,recording,*srTicket);
        if(!suppressed)srDispatchReturned(state,*srTicket,"vkCmdDispatchBaseKHR",recording,srEvidence);
    }
    if constexpr(researchDiagnostics) if (state->compute) xess_vk::compute_observer::dispatch(state->compute, buffer, x, y, z, "dispatch_base_khr");
    SetLastError(originalError);
}


#define SR_TAIL_FORWARD(NAME,MEMBER,PARAMETERS,ARGUMENTS,SNAPSHOT) \
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL NAME PARAMETERS { \
    auto* state=findDevice(buffer);if(!state||!state->MEMBER)return; \
    if(srSdkDepth){state->MEMBER ARGUMENTS;return;} \
    if constexpr(std::string_view(#NAME)=="vkCmdDrawIndirectCount"||std::string_view(#NAME)=="vkCmdDrawIndirectCountKHR"||std::string_view(#NAME)=="vkCmdDrawIndexedIndirectCount"||std::string_view(#NAME)=="vkCmdDrawIndexedIndirectCountKHR") \
        if(srProductDraw(state,buffer,std::string_view(#NAME).find("Indexed")!=std::string_view::npos?stc::Kind::DrawIndexedIndirectCount:stc::Kind::DrawIndirectCount))return; \
    if(srTailCommand(state,buffer,[&]{return SNAPSHOT;}))return; \
    state->MEMBER ARGUMENTS; \
}
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdPushConstants(VkCommandBuffer buffer,VkPipelineLayout layout,VkShaderStageFlags stages,uint32_t offset,uint32_t size,const void* values) {
    auto* state=findDevice(buffer);if(!state||!state->cmdPushConstants)return;
    if(srSdkDepth){state->cmdPushConstants(buffer,layout,stages,offset,size,values);return;}
    // push() only tracks compute stages; skip the facts query for graphics-only pushes.
    if(state->srRuntime&&(stages&VK_SHADER_STAGE_COMPUTE_BIT))state->srRuntime->push(xess_vk::render_pass_observer::recordingFacts(state->renderPass,buffer),stages);
    srTailCommand(state,buffer,[&]{return stc::pushConstants(layout,stages,offset,size,values);});
    state->cmdPushConstants(buffer,layout,stages,offset,size,values);
}
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdPushConstants2(VkCommandBuffer buffer,const VkPushConstantsInfo* info) {
    auto* state=findDevice(buffer);if(!state||!state->cmdPushConstants2)return;
    if(srSdkDepth){state->cmdPushConstants2(buffer,info);return;}
    if(state->srRuntime&&info&&(info->stageFlags&VK_SHADER_STAGE_COMPUTE_BIT))state->srRuntime->push(xess_vk::render_pass_observer::recordingFacts(state->renderPass,buffer),info->stageFlags);
    srTailCommand(state,buffer,[&]{return stc::pushConstants2(info);});state->cmdPushConstants2(buffer,info);
}
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdPushConstants2KHR(VkCommandBuffer buffer,const VkPushConstantsInfo* info) {
    auto* state=findDevice(buffer);if(!state||!state->cmdPushConstants2Khr)return;
    if(srSdkDepth){state->cmdPushConstants2Khr(buffer,info);return;}
    if(state->srRuntime&&info&&(info->stageFlags&VK_SHADER_STAGE_COMPUTE_BIT))state->srRuntime->push(xess_vk::render_pass_observer::recordingFacts(state->renderPass,buffer),info->stageFlags);
    srTailCommand(state,buffer,[&]{return stc::pushConstants2(info);});state->cmdPushConstants2Khr(buffer,info);
}
SR_TAIL_FORWARD(vkCmdSetViewport,cmdSetViewport,(VkCommandBuffer buffer,uint32_t first,uint32_t count,const VkViewport* views),(buffer,first,count,views),(srTailViewport("vkCmdSetViewport",first,count,views)))
SR_TAIL_FORWARD(vkCmdSetScissor,cmdSetScissor,(VkCommandBuffer buffer,uint32_t first,uint32_t count,const VkRect2D* rects),(buffer,first,count,rects),(srTailScissor("vkCmdSetScissor",first,count,rects)))
SR_TAIL_FORWARD(vkCmdSetViewportWithCount,cmdSetViewportWithCount,(VkCommandBuffer buffer,uint32_t count,const VkViewport* views),(buffer,count,views),(srTailViewport("vkCmdSetViewportWithCount",0,count,views)))
SR_TAIL_FORWARD(vkCmdSetScissorWithCount,cmdSetScissorWithCount,(VkCommandBuffer buffer,uint32_t count,const VkRect2D* rects),(buffer,count,rects),(srTailScissor("vkCmdSetScissorWithCount",0,count,rects)))
SR_TAIL_FORWARD(vkCmdSetViewportWithCountEXT,cmdSetViewportWithCountExt,(VkCommandBuffer buffer,uint32_t count,const VkViewport* views),(buffer,count,views),(srTailViewport("vkCmdSetViewportWithCountEXT",0,count,views)))
SR_TAIL_FORWARD(vkCmdSetScissorWithCountEXT,cmdSetScissorWithCountExt,(VkCommandBuffer buffer,uint32_t count,const VkRect2D* rects),(buffer,count,rects),(srTailScissor("vkCmdSetScissorWithCountEXT",0,count,rects)))
SR_TAIL_FORWARD(vkCmdBindVertexBuffers,cmdBindVertexBuffers,(VkCommandBuffer buffer,uint32_t first,uint32_t count,const VkBuffer* buffers,const VkDeviceSize* offsets),(buffer,first,count,buffers,offsets),(([&]{auto snapshot=srTailOther("vkCmdBindVertexBuffers",{first,count,count&&buffers?bits(buffers[0]):0,count&&offsets?offsets[0]:0});snapshot.truncated=true;return snapshot;}())))
SR_TAIL_FORWARD(vkCmdBindVertexBuffers2,cmdBindVertexBuffers2,(VkCommandBuffer buffer,uint32_t first,uint32_t count,const VkBuffer* buffers,const VkDeviceSize* offsets,const VkDeviceSize* sizes,const VkDeviceSize* strides),(buffer,first,count,buffers,offsets,sizes,strides),(([&]{auto snapshot=srTailOther("vkCmdBindVertexBuffers2",{first,count,count&&buffers?bits(buffers[0]):0,count&&offsets?offsets[0]:0,count&&sizes?sizes[0]:0,count&&strides?strides[0]:0});snapshot.truncated=true;return snapshot;}())))
SR_TAIL_FORWARD(vkCmdBindVertexBuffers2EXT,cmdBindVertexBuffers2Ext,(VkCommandBuffer buffer,uint32_t first,uint32_t count,const VkBuffer* buffers,const VkDeviceSize* offsets,const VkDeviceSize* sizes,const VkDeviceSize* strides),(buffer,first,count,buffers,offsets,sizes,strides),(([&]{auto snapshot=srTailOther("vkCmdBindVertexBuffers2EXT",{first,count,count&&buffers?bits(buffers[0]):0,count&&offsets?offsets[0]:0,count&&sizes?sizes[0]:0,count&&strides?strides[0]:0});snapshot.truncated=true;return snapshot;}())))
SR_TAIL_FORWARD(vkCmdBindIndexBuffer,cmdBindIndexBuffer,(VkCommandBuffer buffer,VkBuffer indices,VkDeviceSize offset,VkIndexType type),(buffer,indices,offset,type),(srTailOther("vkCmdBindIndexBuffer",{bits(indices),offset,type})))
SR_TAIL_FORWARD(vkCmdBindIndexBuffer2,cmdBindIndexBuffer2,(VkCommandBuffer buffer,VkBuffer indices,VkDeviceSize offset,VkDeviceSize size,VkIndexType type),(buffer,indices,offset,size,type),(srTailOther("vkCmdBindIndexBuffer2",{bits(indices),offset,size,type})))
SR_TAIL_FORWARD(vkCmdBindIndexBuffer2KHR,cmdBindIndexBuffer2Khr,(VkCommandBuffer buffer,VkBuffer indices,VkDeviceSize offset,VkDeviceSize size,VkIndexType type),(buffer,indices,offset,size,type),(srTailOther("vkCmdBindIndexBuffer2KHR",{bits(indices),offset,size,type})))
SR_TAIL_FORWARD(vkCmdDrawIndirectCount,cmdDrawIndirectCount,(VkCommandBuffer buffer,VkBuffer indirect,VkDeviceSize offset,VkBuffer counts,VkDeviceSize countOffset,uint32_t maximum,uint32_t stride),(buffer,indirect,offset,counts,countOffset,maximum,stride),(srTailSimple(stc::Kind::DrawIndirectCount,{bits(indirect),offset,bits(counts),countOffset,maximum,stride},stc::Api::Core)))
SR_TAIL_FORWARD(vkCmdDrawIndirectCountKHR,cmdDrawIndirectCountKhr,(VkCommandBuffer buffer,VkBuffer indirect,VkDeviceSize offset,VkBuffer counts,VkDeviceSize countOffset,uint32_t maximum,uint32_t stride),(buffer,indirect,offset,counts,countOffset,maximum,stride),(srTailSimple(stc::Kind::DrawIndirectCount,{bits(indirect),offset,bits(counts),countOffset,maximum,stride},stc::Api::Khr)))
SR_TAIL_FORWARD(vkCmdDrawIndexedIndirectCount,cmdDrawIndexedIndirectCount,(VkCommandBuffer buffer,VkBuffer indirect,VkDeviceSize offset,VkBuffer counts,VkDeviceSize countOffset,uint32_t maximum,uint32_t stride),(buffer,indirect,offset,counts,countOffset,maximum,stride),(srTailSimple(stc::Kind::DrawIndexedIndirectCount,{bits(indirect),offset,bits(counts),countOffset,maximum,stride},stc::Api::Core)))
SR_TAIL_FORWARD(vkCmdDrawIndexedIndirectCountKHR,cmdDrawIndexedIndirectCountKhr,(VkCommandBuffer buffer,VkBuffer indirect,VkDeviceSize offset,VkBuffer counts,VkDeviceSize countOffset,uint32_t maximum,uint32_t stride),(buffer,indirect,offset,counts,countOffset,maximum,stride),(srTailSimple(stc::Kind::DrawIndexedIndirectCount,{bits(indirect),offset,bits(counts),countOffset,maximum,stride},stc::Api::Khr)))
SR_TAIL_FORWARD(vkCmdSetLineWidth,cmdSetLineWidth,(VkCommandBuffer buffer,float width),(buffer,width),(srTailOther("vkCmdSetLineWidth",{srTailFloat(width)})))
SR_TAIL_FORWARD(vkCmdSetDepthBias,cmdSetDepthBias,(VkCommandBuffer buffer,float constant,float clamp,float slope),(buffer,constant,clamp,slope),(srTailOther("vkCmdSetDepthBias",{srTailFloat(constant),srTailFloat(clamp),srTailFloat(slope)})))
SR_TAIL_FORWARD(vkCmdSetDepthBounds,cmdSetDepthBounds,(VkCommandBuffer buffer,float minimum,float maximum),(buffer,minimum,maximum),(srTailOther("vkCmdSetDepthBounds",{srTailFloat(minimum),srTailFloat(maximum)})))
SR_TAIL_FORWARD(vkCmdSetStencilCompareMask,cmdSetStencilCompareMask,(VkCommandBuffer buffer,VkStencilFaceFlags face,uint32_t mask),(buffer,face,mask),(srTailOther("vkCmdSetStencilCompareMask",{face,mask})))
SR_TAIL_FORWARD(vkCmdSetStencilWriteMask,cmdSetStencilWriteMask,(VkCommandBuffer buffer,VkStencilFaceFlags face,uint32_t mask),(buffer,face,mask),(srTailOther("vkCmdSetStencilWriteMask",{face,mask})))
SR_TAIL_FORWARD(vkCmdSetStencilReference,cmdSetStencilReference,(VkCommandBuffer buffer,VkStencilFaceFlags face,uint32_t reference),(buffer,face,reference),(srTailOther("vkCmdSetStencilReference",{face,reference})))
SR_TAIL_FORWARD(vkCmdSetEvent,cmdSetEvent,(VkCommandBuffer buffer,VkEvent event,VkPipelineStageFlags stages),(buffer,event,stages),(srTailOther("vkCmdSetEvent",{bits(event),stages})))
SR_TAIL_FORWARD(vkCmdResetEvent,cmdResetEvent,(VkCommandBuffer buffer,VkEvent event,VkPipelineStageFlags stages),(buffer,event,stages),(srTailOther("vkCmdResetEvent",{bits(event),stages})))
SR_TAIL_FORWARD(vkCmdResetEvent2,cmdResetEvent2,(VkCommandBuffer buffer,VkEvent event,VkPipelineStageFlags2 stages),(buffer,event,stages),(srTailOther("vkCmdResetEvent2",{bits(event),stages})))
SR_TAIL_FORWARD(vkCmdResetEvent2KHR,cmdResetEvent2Khr,(VkCommandBuffer buffer,VkEvent event,VkPipelineStageFlags2 stages),(buffer,event,stages),(srTailOther("vkCmdResetEvent2KHR",{bits(event),stages})))
SR_TAIL_FORWARD(vkCmdFillBuffer,cmdFillBuffer,(VkCommandBuffer buffer,VkBuffer destination,VkDeviceSize offset,VkDeviceSize size,uint32_t data),(buffer,destination,offset,size,data),(srTailOther("vkCmdFillBuffer",{bits(destination),offset,size,data})))
SR_TAIL_FORWARD(vkCmdSetBlendConstants,cmdSetBlendConstants,(VkCommandBuffer buffer,const float values[4]),(buffer,values),(srTailOther("vkCmdSetBlendConstants",{values?srTailFloat(values[0]):0,values?srTailFloat(values[1]):0,values?srTailFloat(values[2]):0,values?srTailFloat(values[3]):0})))
SR_TAIL_FORWARD(vkCmdCopyBuffer,cmdCopyBuffer,(VkCommandBuffer buffer,VkBuffer source,VkBuffer destination,uint32_t count,const VkBufferCopy* regions),(buffer,source,destination,count,regions),(([&]{auto snapshot=srTailOther("vkCmdCopyBuffer",{bits(source),bits(destination),count});snapshot.truncated=true;return snapshot;}())))
#undef SR_TAIL_FORWARD
#define SR_IMAGE_FORWARD(NAME,MEMBER,PARAMETERS,ARGUMENTS,FIRST,SECOND,KIND) \
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL NAME PARAMETERS { \
    auto* state=findDevice(buffer);if(!state||!state->MEMBER)return; \
    if(srSdkDepth){state->MEMBER ARGUMENTS;return;} \
    srImageTouch(state,buffer,FIRST,KIND);if(SECOND)srImageTouch(state,buffer,SECOND,KIND); \
    srTailCommand(state,buffer,[&]{auto snapshot=srTailOther(#NAME,{bits(FIRST),bits(SECOND)});snapshot.truncated=true;return snapshot;}); \
    state->MEMBER ARGUMENTS; \
}
SR_IMAGE_FORWARD(vkCmdCopyBufferToImage,cmdCopyBufferToImage,
    (VkCommandBuffer buffer,VkBuffer source,VkImage destination,VkImageLayout layout,uint32_t count,const VkBufferImageCopy* regions),
    (buffer,source,destination,layout,count,regions),destination,VK_NULL_HANDLE,xess_vk::RecordedImageTouch::Transfer)
SR_IMAGE_FORWARD(vkCmdCopyImageToBuffer,cmdCopyImageToBuffer,
    (VkCommandBuffer buffer,VkImage source,VkImageLayout layout,VkBuffer destination,uint32_t count,const VkBufferImageCopy* regions),
    (buffer,source,layout,destination,count,regions),source,VK_NULL_HANDLE,xess_vk::RecordedImageTouch::Transfer)
SR_IMAGE_FORWARD(vkCmdClearDepthStencilImage,cmdClearDepthStencilImage,
    (VkCommandBuffer buffer,VkImage image,VkImageLayout layout,const VkClearDepthStencilValue* value,uint32_t count,const VkImageSubresourceRange* ranges),
    (buffer,image,layout,value,count,ranges),image,VK_NULL_HANDLE,xess_vk::RecordedImageTouch::Clear)
SR_IMAGE_FORWARD(vkCmdResolveImage,cmdResolveImage,
    (VkCommandBuffer buffer,VkImage source,VkImageLayout sourceLayout,VkImage destination,VkImageLayout destinationLayout,uint32_t count,const VkImageResolve* regions),
    (buffer,source,sourceLayout,destination,destinationLayout,count,regions),source,destination,xess_vk::RecordedImageTouch::Transfer)
SR_IMAGE_FORWARD(vkCmdResolveImage2,cmdResolveImage2,(VkCommandBuffer buffer,const VkResolveImageInfo2* info),
    (buffer,info),(info?info->srcImage:VK_NULL_HANDLE),(info?info->dstImage:VK_NULL_HANDLE),xess_vk::RecordedImageTouch::Transfer)
SR_IMAGE_FORWARD(vkCmdResolveImage2KHR,cmdResolveImage2Khr,(VkCommandBuffer buffer,const VkResolveImageInfo2* info),
    (buffer,info),(info?info->srcImage:VK_NULL_HANDLE),(info?info->dstImage:VK_NULL_HANDLE),xess_vk::RecordedImageTouch::Transfer)
SR_IMAGE_FORWARD(vkCmdCopyBufferToImage2,cmdCopyBufferToImage2,(VkCommandBuffer buffer,const VkCopyBufferToImageInfo2* info),
    (buffer,info),(info?info->dstImage:VK_NULL_HANDLE),VK_NULL_HANDLE,xess_vk::RecordedImageTouch::Transfer)
SR_IMAGE_FORWARD(vkCmdCopyBufferToImage2KHR,cmdCopyBufferToImage2Khr,(VkCommandBuffer buffer,const VkCopyBufferToImageInfo2* info),
    (buffer,info),(info?info->dstImage:VK_NULL_HANDLE),VK_NULL_HANDLE,xess_vk::RecordedImageTouch::Transfer)
SR_IMAGE_FORWARD(vkCmdCopyImageToBuffer2,cmdCopyImageToBuffer2,(VkCommandBuffer buffer,const VkCopyImageToBufferInfo2* info),
    (buffer,info),(info?info->srcImage:VK_NULL_HANDLE),VK_NULL_HANDLE,xess_vk::RecordedImageTouch::Transfer)
SR_IMAGE_FORWARD(vkCmdCopyImageToBuffer2KHR,cmdCopyImageToBuffer2Khr,(VkCommandBuffer buffer,const VkCopyImageToBufferInfo2* info),
    (buffer,info),(info?info->srcImage:VK_NULL_HANDLE),VK_NULL_HANDLE,xess_vk::RecordedImageTouch::Transfer)
#undef SR_IMAGE_FORWARD
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdClearColorImage(VkCommandBuffer buffer,VkImage image,VkImageLayout layout,
    const VkClearColorValue* value,uint32_t count,const VkImageSubresourceRange* ranges) {
    auto* state=findDevice(buffer);if(!state||!state->cmdClearColorImage)return;
    if(srSdkDepth){state->cmdClearColorImage(buffer,image,layout,value,count,ranges);return;}
    const bool discard=state->srRuntime&&state->srRuntime->replacementFullClear(image,count,ranges);
    srImageTouch(state,buffer,image,xess_vk::RecordedImageTouch::Clear,discard);
    srTailCommand(state,buffer,[&]{auto snapshot=srTailOther("vkCmdClearColorImage",{bits(image),0});snapshot.truncated=true;return snapshot;});
    state->cmdClearColorImage(buffer,image,layout,value,count,ranges);
}
// Ordinary descriptor evidence. Original Vulkan calls are always made exactly
// once, with unchanged arguments and no metadata lock held across the driver.
namespace {
std::size_t descriptorTemplateRead(void*,const void* source,void* destination,std::size_t bytes) {
    SIZE_T received{};
    if(!ReadProcessMemory(GetCurrentProcess(),source,destination,bytes,&received))return 0;
    return static_cast<std::size_t>(received);
}
template<class Info>
bool descriptorStagesGraphicsOnly(const Info* info,VkStructureType expected) noexcept {
    constexpr VkShaderStageFlags graphics=VK_SHADER_STAGE_ALL_GRAPHICS|VK_SHADER_STAGE_TASK_BIT_EXT|VK_SHADER_STAGE_MESH_BIT_EXT;
    return info && info->sType==expected && !info->pNext && info->stageFlags && !(info->stageFlags&~graphics);
}
}
OBS_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateDescriptorSetLayout(VkDevice device, const VkDescriptorSetLayoutCreateInfo* info,
    const VkAllocationCallbacks* allocator, VkDescriptorSetLayout* object) {
    const auto trackingStarted = trackingCpuStart();
    auto* state=findDevice(device); if(!state || !state->createDescriptorSetLayout) return VK_ERROR_INITIALIZATION_FAILED;
    TrackingCpuScope tracking(state, TrackingCpuGroup::DescriptorLifetime, trackingStarted);
    const auto result=tracking.driver([&] { return state->createDescriptorSetLayout(device,info,allocator,object); });
    const auto tracked=xess_vk::descriptor_image_observer::setLayoutCreated(state->descriptors,result==VK_SUCCESS ? *object : VK_NULL_HANDLE,info,result);
    if(state->srShaders&&result==VK_SUCCESS&&tracked.accepted())
        xess_vk::compute_observer::archiveSetLayout(state->srShaders,*object,tracked.key.generation,info,result);
    return result;
}
OBS_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreatePipelineLayout(VkDevice device, const VkPipelineLayoutCreateInfo* info,
    const VkAllocationCallbacks* allocator, VkPipelineLayout* object) {
    const auto trackingStarted = trackingCpuStart();
    auto* state=findDevice(device); if(!state || !state->createPipelineLayout) return VK_ERROR_INITIALIZATION_FAILED;
    TrackingCpuScope tracking(state, TrackingCpuGroup::DescriptorLifetime, trackingStarted);
    const auto result=tracking.driver([&] { return state->createPipelineLayout(device,info,allocator,object); });
    const auto tracked=xess_vk::descriptor_image_observer::pipelineLayoutCreated(state->descriptors,result==VK_SUCCESS ? *object : VK_NULL_HANDLE,info,result);
    if(state->srShaders&&result==VK_SUCCESS&&tracked.accepted())
        xess_vk::compute_observer::archivePipelineLayout(state->srShaders,*object,tracked.key.generation,info,result);
    return result;
}
OBS_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateDescriptorPool(VkDevice device, const VkDescriptorPoolCreateInfo* info,
    const VkAllocationCallbacks* allocator, VkDescriptorPool* object) {
    const auto trackingStarted = trackingCpuStart();
    auto* state=findDevice(device); if(!state || !state->createDescriptorPool) return VK_ERROR_INITIALIZATION_FAILED;
    TrackingCpuScope tracking(state, TrackingCpuGroup::DescriptorLifetime, trackingStarted);
    const auto result=tracking.driver([&] { return state->createDescriptorPool(device,info,allocator,object); });
    xess_vk::descriptor_image_observer::poolCreated(state->descriptors,result==VK_SUCCESS ? *object : VK_NULL_HANDLE,info,result);
    return result;
}
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkDestroyDescriptorSetLayout(VkDevice device, VkDescriptorSetLayout object,const VkAllocationCallbacks* allocator) {
    const auto trackingStarted = trackingCpuStart();
    auto* state=findDevice(device); if(!state || !state->destroyDescriptorSetLayout) return;
    TrackingCpuScope tracking(state, TrackingCpuGroup::DescriptorLifetime, trackingStarted);
    xess_vk::descriptor_image_observer::setLayoutDestroying(state->descriptors,object);
    tracking.driver([&] { return state->destroyDescriptorSetLayout(device,object,allocator); });
    xess_vk::compute_observer::destroyArchivedSetLayout(state->srShaders,object);
}
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkDestroyPipelineLayout(VkDevice device, VkPipelineLayout object,const VkAllocationCallbacks* allocator) {
    const auto trackingStarted = trackingCpuStart();
    auto* state=findDevice(device); if(!state || !state->destroyPipelineLayout) return;
    TrackingCpuScope tracking(state, TrackingCpuGroup::DescriptorLifetime, trackingStarted);
    xess_vk::descriptor_image_observer::pipelineLayoutDestroying(state->descriptors,object);
    tracking.driver([&] { return state->destroyPipelineLayout(device,object,allocator); });
    xess_vk::compute_observer::destroyArchivedPipelineLayout(state->srShaders,object);
}
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkDestroyDescriptorPool(VkDevice device, VkDescriptorPool object,const VkAllocationCallbacks* allocator) {
    const auto trackingStarted = trackingCpuStart();
    auto* state=findDevice(device); if(!state || !state->destroyDescriptorPool) return;
    TrackingCpuScope tracking(state, TrackingCpuGroup::DescriptorLifetime, trackingStarted);
    xess_vk::descriptor_image_observer::poolDestroying(state->descriptors,object);
    tracking.driver([&] { return state->destroyDescriptorPool(device,object,allocator); });
}
OBS_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkResetDescriptorPool(VkDevice device,VkDescriptorPool pool,VkDescriptorPoolResetFlags flags) {
    const auto trackingStarted = trackingCpuStart();
    auto* state=findDevice(device); if(!state || !state->resetDescriptorPool) return VK_ERROR_INITIALIZATION_FAILED;
    TrackingCpuScope tracking(state, TrackingCpuGroup::DescriptorLifetime, trackingStarted);
    const auto result=tracking.driver([&] { return state->resetDescriptorPool(device,pool,flags); });
    xess_vk::descriptor_image_observer::poolReset(state->descriptors,pool,result); return result;
}
OBS_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkAllocateDescriptorSets(VkDevice device,const VkDescriptorSetAllocateInfo* info,VkDescriptorSet* sets) {
    const auto trackingStarted = trackingCpuStart();
    auto* state=findDevice(device); if(!state || !state->allocateDescriptorSets) return VK_ERROR_INITIALIZATION_FAILED;
    TrackingCpuScope tracking(state, TrackingCpuGroup::DescriptorLifetime, trackingStarted);
    const auto result=tracking.driver([&] { return state->allocateDescriptorSets(device,info,sets); });
    xess_vk::descriptor_image_observer::setsAllocated(state->descriptors,info,sets,result); return result;
}
OBS_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkFreeDescriptorSets(VkDevice device,VkDescriptorPool pool,uint32_t count,const VkDescriptorSet* sets) {
    const auto trackingStarted = trackingCpuStart();
    auto* state=findDevice(device); if(!state || !state->freeDescriptorSets) return VK_ERROR_INITIALIZATION_FAILED;
    TrackingCpuScope tracking(state, TrackingCpuGroup::DescriptorLifetime, trackingStarted);
    const auto result=tracking.driver([&] { return state->freeDescriptorSets(device,pool,count,sets); });
    xess_vk::descriptor_image_observer::setsFreed(state->descriptors,pool,count,sets,result); return result;
}
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkUpdateDescriptorSets(VkDevice device,uint32_t writeCount,const VkWriteDescriptorSet* writes,
    uint32_t copyCount,const VkCopyDescriptorSet* copies) {
    const auto trackingStarted = trackingCpuStart();
    auto* state=findDevice(device); if(!state || !state->updateDescriptorSets) return;
    TrackingCpuScope tracking(state, TrackingCpuGroup::DescriptorUpdate, trackingStarted);
    tracking.driver([&] { return state->updateDescriptorSets(device,writeCount,writes,copyCount,copies); });
    xess_vk::descriptor_image_observer::updateDescriptors(state->descriptors,writeCount,writes,copyCount,copies);
}
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdBindDescriptorSets(VkCommandBuffer buffer,VkPipelineBindPoint point,VkPipelineLayout layout,
    uint32_t first,uint32_t count,const VkDescriptorSet* sets,uint32_t dynamicCount,const uint32_t* offsets) {
    const auto trackingStarted = trackingCpuStart();
    auto* state=findDevice(buffer); if(!state || !state->cmdBindDescriptorSets) return;
    if(srSdkDepth){state->cmdBindDescriptorSets(buffer,point,layout,first,count,sets,dynamicCount,offsets);return;}
    srProductBind(state,buffer,point,layout,first,count,sets,dynamicCount,offsets);
    srTailCommand(state,buffer,[&]{return stc::bindDescriptorSets(point,layout,first,count,sets,dynamicCount,offsets);});
    TrackingCpuScope tracking(state, TrackingCpuGroup::Bind, trackingStarted);
    tracking.driver([&] { return state->cmdBindDescriptorSets(buffer,point,layout,first,count,sets,dynamicCount,offsets); });
    xess_vk::descriptor_image_observer::bindSets(state->descriptors,buffer,point,layout,first,count,sets,dynamicCount,offsets);
    if constexpr(researchDiagnostics) if(state->renderTail&&state->renderTail->active()&&point==VK_PIPELINE_BIND_POINT_GRAPHICS) {
        std::array<std::uint64_t,xess_vk::descriptor_image_metadata::maxBoundSets> copied{};
        const auto facts=xess_vk::render_pass_observer::recordingFacts(state->renderPass,buffer);
        if(count<=copied.size()&&(!count||sets)){for(std::size_t n=0;n<count;++n)copied[n]=bits(sets[n]);state->renderTail->bindSets(facts,bits(layout),first,{copied.data(),count});}
        else state->renderTail->bindSets(facts,0,UINT32_MAX,{});
    }
}
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkUpdateDescriptorSetWithTemplate(VkDevice device,VkDescriptorSet set,VkDescriptorUpdateTemplate update,const void* data) {
    const auto trackingStarted = trackingCpuStart();
    auto* state=findDevice(device); if(!state || !state->updateDescriptorSetWithTemplate) return;
    TrackingCpuScope tracking(state, TrackingCpuGroup::DescriptorUpdate, trackingStarted);
    tracking.driver([&] { return state->updateDescriptorSetWithTemplate(device,set,update,data); });
    xess_vk::descriptor_image_observer::updateWithTemplate(state->descriptors,set,update,data,
        xess_vk::descriptor_image_metadata::TemplateUpdateKind::Core,{descriptorTemplateRead,nullptr,true});
}
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkUpdateDescriptorSetWithTemplateKHR(VkDevice device,VkDescriptorSet set,VkDescriptorUpdateTemplate update,const void* data) {
    const auto trackingStarted = trackingCpuStart();
    auto* state=findDevice(device); if(!state || !state->updateDescriptorSetWithTemplateKhr) return;
    TrackingCpuScope tracking(state, TrackingCpuGroup::DescriptorUpdate, trackingStarted);
    tracking.driver([&] { return state->updateDescriptorSetWithTemplateKhr(device,set,update,data); });
    xess_vk::descriptor_image_observer::updateWithTemplate(state->descriptors,set,update,data,
        xess_vk::descriptor_image_metadata::TemplateUpdateKind::Khr,{descriptorTemplateRead,nullptr,true});
}
OBS_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateDescriptorUpdateTemplate(VkDevice device,const VkDescriptorUpdateTemplateCreateInfo* info,
    const VkAllocationCallbacks* allocator,VkDescriptorUpdateTemplate* object) {
    const auto trackingStarted = trackingCpuStart();
    auto* state=findDevice(device);if(!state||!state->createDescriptorUpdateTemplate)return VK_ERROR_INITIALIZATION_FAILED;
    TrackingCpuScope tracking(state, TrackingCpuGroup::DescriptorLifetime, trackingStarted);
    const auto result=tracking.driver([&] { return state->createDescriptorUpdateTemplate(device,info,allocator,object); });
    xess_vk::descriptor_image_observer::templateCreated(state->descriptors,result==VK_SUCCESS&&object?*object:VK_NULL_HANDLE,info,result);
    return result;
}
OBS_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateDescriptorUpdateTemplateKHR(VkDevice device,const VkDescriptorUpdateTemplateCreateInfo* info,
    const VkAllocationCallbacks* allocator,VkDescriptorUpdateTemplate* object) {
    const auto trackingStarted = trackingCpuStart();
    auto* state=findDevice(device);if(!state||!state->createDescriptorUpdateTemplateKhr)return VK_ERROR_INITIALIZATION_FAILED;
    TrackingCpuScope tracking(state, TrackingCpuGroup::DescriptorLifetime, trackingStarted);
    const auto result=tracking.driver([&] { return state->createDescriptorUpdateTemplateKhr(device,info,allocator,object); });
    xess_vk::descriptor_image_observer::templateCreated(state->descriptors,result==VK_SUCCESS&&object?*object:VK_NULL_HANDLE,info,result);
    return result;
}
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkDestroyDescriptorUpdateTemplate(VkDevice device,VkDescriptorUpdateTemplate object,const VkAllocationCallbacks* allocator) {
    const auto trackingStarted = trackingCpuStart();
    auto* state=findDevice(device);if(!state||!state->destroyDescriptorUpdateTemplate)return;
    TrackingCpuScope tracking(state, TrackingCpuGroup::DescriptorLifetime, trackingStarted);
    xess_vk::descriptor_image_observer::templateDestroying(state->descriptors,object);
    tracking.driver([&] { return state->destroyDescriptorUpdateTemplate(device,object,allocator); });
}
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkDestroyDescriptorUpdateTemplateKHR(VkDevice device,VkDescriptorUpdateTemplate object,const VkAllocationCallbacks* allocator) {
    const auto trackingStarted = trackingCpuStart();
    auto* state=findDevice(device);if(!state||!state->destroyDescriptorUpdateTemplateKhr)return;
    TrackingCpuScope tracking(state, TrackingCpuGroup::DescriptorLifetime, trackingStarted);
    xess_vk::descriptor_image_observer::templateDestroying(state->descriptors,object);
    tracking.driver([&] { return state->destroyDescriptorUpdateTemplateKhr(device,object,allocator); });
}
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdPushDescriptorSet(VkCommandBuffer buffer,VkPipelineBindPoint point, VkPipelineLayout layout, uint32_t set, uint32_t count, const VkWriteDescriptorSet* writes) {
    const auto trackingStarted = trackingCpuStart();
    auto* state=findDevice(buffer); if(!state || !state->cmdPushDescriptorSet) return;
    srTailCommand(state,buffer,[&]{auto snapshot=srTailOther("vkCmdPushDescriptorSet",{point,bits(layout),set,count});snapshot.truncated=true;return snapshot;});
    TrackingCpuScope tracking(state, TrackingCpuGroup::Bind, trackingStarted);
    if(point==VK_PIPELINE_BIND_POINT_COMPUTE) xess_vk::descriptor_image_observer::commandUnsupported(state->descriptors,buffer);
    tracking.driver([&] { return state->cmdPushDescriptorSet(buffer,point, layout, set, count, writes); });
}
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdPushDescriptorSetWithTemplate(VkCommandBuffer buffer,VkDescriptorUpdateTemplate update, VkPipelineLayout layout, uint32_t set, const void* data) {
    const auto trackingStarted = trackingCpuStart();
    auto* state=findDevice(buffer); if(!state || !state->cmdPushDescriptorSetWithTemplate) return;
    srTailCommand(state,buffer,[&]{auto snapshot=srTailOther("vkCmdPushDescriptorSetWithTemplate",{bits(update),bits(layout),set});snapshot.truncated=true;return snapshot;});
    TrackingCpuScope tracking(state, TrackingCpuGroup::Bind, trackingStarted);
    xess_vk::descriptor_image_observer::commandUnsupported(state->descriptors,buffer);
    tracking.driver([&] { return state->cmdPushDescriptorSetWithTemplate(buffer,update, layout, set, data); });
}
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdBindDescriptorSets2(VkCommandBuffer buffer,const VkBindDescriptorSetsInfo* info) {
    const auto trackingStarted = trackingCpuStart();
    auto* state=findDevice(buffer); if(!state || !state->cmdBindDescriptorSets2) return;
    if(srSdkDepth){state->cmdBindDescriptorSets2(buffer,info);return;}
    srProductCommand(state,buffer,[&]{return stc::bindDescriptorSets2(info);});
    srTailCommand(state,buffer,[&]{return stc::bindDescriptorSets2(info);});
    TrackingCpuScope tracking(state, TrackingCpuGroup::Bind, trackingStarted);
    if(!descriptorStagesGraphicsOnly(info,VK_STRUCTURE_TYPE_BIND_DESCRIPTOR_SETS_INFO)) xess_vk::descriptor_image_observer::commandUnsupported(state->descriptors,buffer);
    tracking.driver([&] { return state->cmdBindDescriptorSets2(buffer,info); });
}
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdPushDescriptorSet2(VkCommandBuffer buffer,const VkPushDescriptorSetInfo* info) {
    const auto trackingStarted = trackingCpuStart();
    auto* state=findDevice(buffer); if(!state || !state->cmdPushDescriptorSet2) return;
    srTailCommand(state,buffer,[&]{auto snapshot=srTailOther("vkCmdPushDescriptorSet2",{});snapshot.truncated=true;return snapshot;});
    TrackingCpuScope tracking(state, TrackingCpuGroup::Bind, trackingStarted);
    if(!descriptorStagesGraphicsOnly(info,VK_STRUCTURE_TYPE_PUSH_DESCRIPTOR_SET_INFO)) xess_vk::descriptor_image_observer::commandUnsupported(state->descriptors,buffer);
    tracking.driver([&] { return state->cmdPushDescriptorSet2(buffer,info); });
}
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdPushDescriptorSetWithTemplate2(VkCommandBuffer buffer,const VkPushDescriptorSetWithTemplateInfo* info) {
    const auto trackingStarted = trackingCpuStart();
    auto* state=findDevice(buffer); if(!state || !state->cmdPushDescriptorSetWithTemplate2) return;
    srTailCommand(state,buffer,[&]{auto snapshot=srTailOther("vkCmdPushDescriptorSetWithTemplate2",{});snapshot.truncated=true;return snapshot;});
    TrackingCpuScope tracking(state, TrackingCpuGroup::Bind, trackingStarted);
    xess_vk::descriptor_image_observer::commandUnsupported(state->descriptors,buffer);
    tracking.driver([&] { return state->cmdPushDescriptorSetWithTemplate2(buffer,info); });
}
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdPushDescriptorSetKHR(VkCommandBuffer buffer,VkPipelineBindPoint point, VkPipelineLayout layout, uint32_t set, uint32_t count, const VkWriteDescriptorSet* writes) {
    const auto trackingStarted = trackingCpuStart();
    auto* state=findDevice(buffer); if(!state || !state->cmdPushDescriptorSetKHR) return;
    srTailCommand(state,buffer,[&]{auto snapshot=srTailOther("vkCmdPushDescriptorSetKHR",{point,bits(layout),set,count});snapshot.truncated=true;return snapshot;});
    TrackingCpuScope tracking(state, TrackingCpuGroup::Bind, trackingStarted);
    if(point==VK_PIPELINE_BIND_POINT_COMPUTE) xess_vk::descriptor_image_observer::commandUnsupported(state->descriptors,buffer);
    tracking.driver([&] { return state->cmdPushDescriptorSetKHR(buffer,point, layout, set, count, writes); });
}
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdPushDescriptorSetWithTemplateKHR(VkCommandBuffer buffer,VkDescriptorUpdateTemplate update, VkPipelineLayout layout, uint32_t set, const void* data) {
    const auto trackingStarted = trackingCpuStart();
    auto* state=findDevice(buffer); if(!state || !state->cmdPushDescriptorSetWithTemplateKHR) return;
    srTailCommand(state,buffer,[&]{auto snapshot=srTailOther("vkCmdPushDescriptorSetWithTemplateKHR",{bits(update),bits(layout),set});snapshot.truncated=true;return snapshot;});
    TrackingCpuScope tracking(state, TrackingCpuGroup::Bind, trackingStarted);
    xess_vk::descriptor_image_observer::commandUnsupported(state->descriptors,buffer);
    tracking.driver([&] { return state->cmdPushDescriptorSetWithTemplateKHR(buffer,update, layout, set, data); });
}
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdBindDescriptorSets2KHR(VkCommandBuffer buffer,const VkBindDescriptorSetsInfo* info) {
    const auto trackingStarted = trackingCpuStart();
    auto* state=findDevice(buffer); if(!state || !state->cmdBindDescriptorSets2KHR) return;
    if(srSdkDepth){state->cmdBindDescriptorSets2KHR(buffer,info);return;}
    srProductCommand(state,buffer,[&]{return stc::bindDescriptorSets2(info,stc::Api::Khr2);});
    srTailCommand(state,buffer,[&]{return stc::bindDescriptorSets2(info,stc::Api::Khr2);});
    TrackingCpuScope tracking(state, TrackingCpuGroup::Bind, trackingStarted);
    if(!descriptorStagesGraphicsOnly(info,VK_STRUCTURE_TYPE_BIND_DESCRIPTOR_SETS_INFO)) xess_vk::descriptor_image_observer::commandUnsupported(state->descriptors,buffer);
    tracking.driver([&] { return state->cmdBindDescriptorSets2KHR(buffer,info); });
}
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdPushDescriptorSet2KHR(VkCommandBuffer buffer,const VkPushDescriptorSetInfo* info) {
    const auto trackingStarted = trackingCpuStart();
    auto* state=findDevice(buffer); if(!state || !state->cmdPushDescriptorSet2KHR) return;
    srTailCommand(state,buffer,[&]{auto snapshot=srTailOther("vkCmdPushDescriptorSet2KHR",{});snapshot.truncated=true;return snapshot;});
    TrackingCpuScope tracking(state, TrackingCpuGroup::Bind, trackingStarted);
    if(!descriptorStagesGraphicsOnly(info,VK_STRUCTURE_TYPE_PUSH_DESCRIPTOR_SET_INFO)) xess_vk::descriptor_image_observer::commandUnsupported(state->descriptors,buffer);
    tracking.driver([&] { return state->cmdPushDescriptorSet2KHR(buffer,info); });
}
OBS_EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdPushDescriptorSetWithTemplate2KHR(VkCommandBuffer buffer,const VkPushDescriptorSetWithTemplateInfo* info) {
    const auto trackingStarted = trackingCpuStart();
    auto* state=findDevice(buffer); if(!state || !state->cmdPushDescriptorSetWithTemplate2KHR) return;
    srTailCommand(state,buffer,[&]{auto snapshot=srTailOther("vkCmdPushDescriptorSetWithTemplate2KHR",{});snapshot.truncated=true;return snapshot;});
    TrackingCpuScope tracking(state, TrackingCpuGroup::Bind, trackingStarted);
    xess_vk::descriptor_image_observer::commandUnsupported(state->descriptors,buffer);
    tracking.driver([&] { return state->cmdPushDescriptorSetWithTemplate2KHR(buffer,info); });
}
namespace {
PFN_vkVoidFunction instanceHook(const char* name) noexcept {
#define HOOK(function) if (std::strcmp(name, #function) == 0) return reinterpret_cast<PFN_vkVoidFunction>(function)
    if(srTailHooksRequested()) {
        HOOK(vkCmdClearColorImage);HOOK(vkCmdClearDepthStencilImage);
        HOOK(vkCmdResolveImage);HOOK(vkCmdResolveImage2);HOOK(vkCmdResolveImage2KHR);
        HOOK(vkCmdCopyBufferToImage2);HOOK(vkCmdCopyBufferToImage2KHR);
        HOOK(vkCmdCopyImageToBuffer2);HOOK(vkCmdCopyImageToBuffer2KHR);
    }
    if(renderTailRequested()||srTailHooksRequested()) {
        HOOK(vkCmdCopyImage);HOOK(vkCmdCopyImage2);HOOK(vkCmdCopyImage2KHR);
        HOOK(vkCmdBlitImage);HOOK(vkCmdBlitImage2);HOOK(vkCmdBlitImage2KHR);
        HOOK(vkCmdDraw);HOOK(vkCmdDrawIndexed);HOOK(vkCmdDrawIndirect);HOOK(vkCmdDrawIndexedIndirect);
    }
    HOOK(vkQueueWaitIdle);HOOK(vkDeviceWaitIdle);
    HOOK(vkCreateSemaphore);HOOK(vkDestroySemaphore);
    HOOK(vkCreateBuffer);HOOK(vkDestroyBuffer);HOOK(vkAllocateMemory);HOOK(vkFreeMemory);
    HOOK(vkBindImageMemory);HOOK(vkBindImageMemory2);HOOK(vkBindImageMemory2KHR);HOOK(vkBindBufferMemory);HOOK(vkBindBufferMemory2);HOOK(vkBindBufferMemory2KHR);
    HOOK(vkGetImageMemoryRequirements);HOOK(vkGetBufferMemoryRequirements);HOOK(vkQueueBindSparse);
    HOOK(vkCreateDescriptorSetLayout);
    HOOK(vkDestroyDescriptorSetLayout);
    HOOK(vkCreatePipelineLayout);
    HOOK(vkDestroyPipelineLayout);
    HOOK(vkCreateDescriptorPool);
    HOOK(vkResetDescriptorPool);
    HOOK(vkDestroyDescriptorPool);
    HOOK(vkAllocateDescriptorSets);
    HOOK(vkFreeDescriptorSets);
    HOOK(vkUpdateDescriptorSets);
    HOOK(vkCmdBindDescriptorSets);
    HOOK(vkCreateDescriptorUpdateTemplate);
    HOOK(vkDestroyDescriptorUpdateTemplate);
    HOOK(vkCreateDescriptorUpdateTemplateKHR);
    HOOK(vkDestroyDescriptorUpdateTemplateKHR);
    HOOK(vkUpdateDescriptorSetWithTemplate);
    HOOK(vkUpdateDescriptorSetWithTemplateKHR);
    HOOK(vkCmdPushDescriptorSet);
    HOOK(vkCmdPushDescriptorSetWithTemplate);
    HOOK(vkCmdBindDescriptorSets2);
    HOOK(vkCmdPushDescriptorSet2);
    HOOK(vkCmdPushDescriptorSetWithTemplate2);
    HOOK(vkCmdPushDescriptorSetKHR);
    HOOK(vkCmdPushDescriptorSetWithTemplateKHR);
    HOOK(vkCmdBindDescriptorSets2KHR);
    HOOK(vkCmdPushDescriptorSet2KHR);
    HOOK(vkCmdPushDescriptorSetWithTemplate2KHR);
    HOOK(vkCreateImageView); HOOK(vkDestroyImageView); HOOK(vkCreateRenderPass);
    HOOK(vkDestroyRenderPass); HOOK(vkCreateRenderPass2); HOOK(vkCreateRenderPass2KHR);
    HOOK(vkCreateFramebuffer); HOOK(vkDestroyFramebuffer); HOOK(vkCmdNextSubpass);
    HOOK(vkCmdNextSubpass2); HOOK(vkCmdNextSubpass2KHR); HOOK(vkCmdEndRenderPass);
    HOOK(vkCmdEndRenderPass2); HOOK(vkCmdEndRenderPass2KHR); HOOK(vkCmdBeginRendering);
    HOOK(vkCmdBeginRenderingKHR); HOOK(vkCmdEndRendering); HOOK(vkCmdEndRenderingKHR);
    HOOK(vkGetInstanceProcAddr); HOOK(vkGetDeviceProcAddr);
    HOOK(vkCreateInstance); HOOK(vkDestroyInstance); HOOK(vkCreateDevice);
    HOOK(vkCreateWin32SurfaceKHR); HOOK(vkDestroySurfaceKHR);
    HOOK(vkEnumeratePhysicalDevices); HOOK(vkEnumeratePhysicalDeviceGroups); HOOK(vkEnumeratePhysicalDeviceGroupsKHR);
    HOOK(vkGetPhysicalDeviceProperties); HOOK(vkGetPhysicalDeviceProperties2); HOOK(vkGetPhysicalDeviceProperties2KHR);
    HOOK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR); HOOK(vkGetPhysicalDeviceSurfaceCapabilities2KHR);
    HOOK(vkDestroyDevice); HOOK(vkCreateSwapchainKHR); HOOK(vkDestroySwapchainKHR); HOOK(vkQueuePresentKHR);
    HOOK(vkCreateImage); HOOK(vkDestroyImage); HOOK(vkGetDeviceQueue); HOOK(vkGetDeviceQueue2);
    HOOK(vkGetSwapchainImagesKHR); HOOK(vkAcquireNextImageKHR); HOOK(vkAcquireNextImage2KHR); HOOK(vkGetSwapchainStatusKHR);
    HOOK(vkQueueSubmit); HOOK(vkQueueSubmit2); HOOK(vkQueueSubmit2KHR);
    HOOK(vkCreateShaderModule); HOOK(vkDestroyShaderModule); HOOK(vkCreateComputePipelines); HOOK(vkDestroyPipeline);
    HOOK(vkAllocateCommandBuffers); HOOK(vkFreeCommandBuffers); HOOK(vkBeginCommandBuffer); HOOK(vkResetCommandBuffer);
    HOOK(vkResetCommandPool); HOOK(vkDestroyCommandPool); HOOK(vkCmdBindPipeline);
    HOOK(vkCreateCommandPool); HOOK(vkEndCommandBuffer);
    HOOK(vkCmdPipelineBarrier); HOOK(vkCmdPipelineBarrier2); HOOK(vkCmdPipelineBarrier2KHR);
    HOOK(vkCmdWaitEvents); HOOK(vkCmdWaitEvents2); HOOK(vkCmdWaitEvents2KHR); HOOK(vkCmdSetEvent2); HOOK(vkCmdSetEvent2KHR);
    HOOK(vkCmdBeginRenderPass); HOOK(vkCmdBeginRenderPass2); HOOK(vkCmdBeginRenderPass2KHR); HOOK(vkCmdExecuteCommands);
    if(srTailHooksRequested()) {
        HOOK(vkCmdPushConstants);
        HOOK(vkCmdPushConstants2);
        HOOK(vkCmdPushConstants2KHR);
        HOOK(vkCmdSetViewport);
        HOOK(vkCmdSetScissor);
        HOOK(vkCmdSetViewportWithCount);
        HOOK(vkCmdSetScissorWithCount);
        HOOK(vkCmdSetViewportWithCountEXT);
        HOOK(vkCmdSetScissorWithCountEXT);
        HOOK(vkCmdBindVertexBuffers);
        HOOK(vkCmdBindVertexBuffers2);
        HOOK(vkCmdBindVertexBuffers2EXT);
        HOOK(vkCmdBindIndexBuffer);
        HOOK(vkCmdBindIndexBuffer2);
        HOOK(vkCmdBindIndexBuffer2KHR);
        HOOK(vkCmdDrawIndirectCount);
        HOOK(vkCmdDrawIndirectCountKHR);
        HOOK(vkCmdDrawIndexedIndirectCount);
        HOOK(vkCmdDrawIndexedIndirectCountKHR);
        HOOK(vkCmdSetLineWidth);
        HOOK(vkCmdSetDepthBias);
        HOOK(vkCmdSetDepthBounds);
        HOOK(vkCmdSetStencilCompareMask);
        HOOK(vkCmdSetStencilWriteMask);
        HOOK(vkCmdSetStencilReference);
        HOOK(vkCmdSetEvent);
        HOOK(vkCmdResetEvent);
        HOOK(vkCmdResetEvent2);
        HOOK(vkCmdResetEvent2KHR);
        HOOK(vkCmdFillBuffer);
        HOOK(vkCmdSetBlendConstants);
        HOOK(vkCmdCopyBuffer);
        HOOK(vkCmdCopyBufferToImage);
        HOOK(vkCmdCopyImageToBuffer);
    }
    HOOK(vkCmdDispatch); HOOK(vkCmdDispatchIndirect); HOOK(vkCmdDispatchBase); HOOK(vkCmdDispatchBaseKHR);
#undef HOOK
    return nullptr;
}
PFN_vkVoidFunction deviceHook(const char* name) noexcept {
#define HOOK(function) if (std::strcmp(name, #function) == 0) return reinterpret_cast<PFN_vkVoidFunction>(function)
    if(srTailHooksRequested()) {
        HOOK(vkCmdClearColorImage);HOOK(vkCmdClearDepthStencilImage);
        HOOK(vkCmdResolveImage);HOOK(vkCmdResolveImage2);HOOK(vkCmdResolveImage2KHR);
        HOOK(vkCmdCopyBufferToImage2);HOOK(vkCmdCopyBufferToImage2KHR);
        HOOK(vkCmdCopyImageToBuffer2);HOOK(vkCmdCopyImageToBuffer2KHR);
    }
    if(renderTailRequested()||srTailHooksRequested()) {
        HOOK(vkCmdCopyImage);HOOK(vkCmdCopyImage2);HOOK(vkCmdCopyImage2KHR);
        HOOK(vkCmdBlitImage);HOOK(vkCmdBlitImage2);HOOK(vkCmdBlitImage2KHR);
        HOOK(vkCmdDraw);HOOK(vkCmdDrawIndexed);HOOK(vkCmdDrawIndirect);HOOK(vkCmdDrawIndexedIndirect);
    }
    HOOK(vkQueueWaitIdle);HOOK(vkDeviceWaitIdle);
    HOOK(vkCreateSemaphore);HOOK(vkDestroySemaphore);
    HOOK(vkCreateBuffer);HOOK(vkDestroyBuffer);HOOK(vkAllocateMemory);HOOK(vkFreeMemory);
    HOOK(vkBindImageMemory);HOOK(vkBindImageMemory2);HOOK(vkBindImageMemory2KHR);HOOK(vkBindBufferMemory);HOOK(vkBindBufferMemory2);HOOK(vkBindBufferMemory2KHR);
    HOOK(vkGetImageMemoryRequirements);HOOK(vkGetBufferMemoryRequirements);HOOK(vkQueueBindSparse);
    HOOK(vkCreateDescriptorSetLayout);
    HOOK(vkDestroyDescriptorSetLayout);
    HOOK(vkCreatePipelineLayout);
    HOOK(vkDestroyPipelineLayout);
    HOOK(vkCreateDescriptorPool);
    HOOK(vkResetDescriptorPool);
    HOOK(vkDestroyDescriptorPool);
    HOOK(vkAllocateDescriptorSets);
    HOOK(vkFreeDescriptorSets);
    HOOK(vkUpdateDescriptorSets);
    HOOK(vkCmdBindDescriptorSets);
    HOOK(vkCreateDescriptorUpdateTemplate);
    HOOK(vkDestroyDescriptorUpdateTemplate);
    HOOK(vkCreateDescriptorUpdateTemplateKHR);
    HOOK(vkDestroyDescriptorUpdateTemplateKHR);
    HOOK(vkUpdateDescriptorSetWithTemplate);
    HOOK(vkUpdateDescriptorSetWithTemplateKHR);
    HOOK(vkCmdPushDescriptorSet);
    HOOK(vkCmdPushDescriptorSetWithTemplate);
    HOOK(vkCmdBindDescriptorSets2);
    HOOK(vkCmdPushDescriptorSet2);
    HOOK(vkCmdPushDescriptorSetWithTemplate2);
    HOOK(vkCmdPushDescriptorSetKHR);
    HOOK(vkCmdPushDescriptorSetWithTemplateKHR);
    HOOK(vkCmdBindDescriptorSets2KHR);
    HOOK(vkCmdPushDescriptorSet2KHR);
    HOOK(vkCmdPushDescriptorSetWithTemplate2KHR);
    HOOK(vkCreateImageView); HOOK(vkDestroyImageView); HOOK(vkCreateRenderPass);
    HOOK(vkDestroyRenderPass); HOOK(vkCreateRenderPass2); HOOK(vkCreateRenderPass2KHR);
    HOOK(vkCreateFramebuffer); HOOK(vkDestroyFramebuffer); HOOK(vkCmdNextSubpass);
    HOOK(vkCmdNextSubpass2); HOOK(vkCmdNextSubpass2KHR); HOOK(vkCmdEndRenderPass);
    HOOK(vkCmdEndRenderPass2); HOOK(vkCmdEndRenderPass2KHR); HOOK(vkCmdBeginRendering);
    HOOK(vkCmdBeginRenderingKHR); HOOK(vkCmdEndRendering); HOOK(vkCmdEndRenderingKHR);
    HOOK(vkGetDeviceProcAddr); HOOK(vkDestroyDevice);
    HOOK(vkCreateSwapchainKHR); HOOK(vkDestroySwapchainKHR); HOOK(vkQueuePresentKHR);
    HOOK(vkCreateImage); HOOK(vkDestroyImage); HOOK(vkGetDeviceQueue); HOOK(vkGetDeviceQueue2);
    HOOK(vkGetSwapchainImagesKHR); HOOK(vkAcquireNextImageKHR); HOOK(vkAcquireNextImage2KHR); HOOK(vkGetSwapchainStatusKHR);
    HOOK(vkQueueSubmit); HOOK(vkQueueSubmit2); HOOK(vkQueueSubmit2KHR);
    HOOK(vkCreateShaderModule); HOOK(vkDestroyShaderModule); HOOK(vkCreateComputePipelines); HOOK(vkDestroyPipeline);
    HOOK(vkAllocateCommandBuffers); HOOK(vkFreeCommandBuffers); HOOK(vkBeginCommandBuffer); HOOK(vkResetCommandBuffer);
    HOOK(vkResetCommandPool); HOOK(vkDestroyCommandPool); HOOK(vkCmdBindPipeline);
    HOOK(vkCreateCommandPool); HOOK(vkEndCommandBuffer);
    HOOK(vkCmdPipelineBarrier); HOOK(vkCmdPipelineBarrier2); HOOK(vkCmdPipelineBarrier2KHR);
    HOOK(vkCmdWaitEvents); HOOK(vkCmdWaitEvents2); HOOK(vkCmdWaitEvents2KHR); HOOK(vkCmdSetEvent2); HOOK(vkCmdSetEvent2KHR);
    HOOK(vkCmdBeginRenderPass); HOOK(vkCmdBeginRenderPass2); HOOK(vkCmdBeginRenderPass2KHR); HOOK(vkCmdExecuteCommands);
    if(srTailHooksRequested()) {
        HOOK(vkCmdPushConstants);
        HOOK(vkCmdPushConstants2);
        HOOK(vkCmdPushConstants2KHR);
        HOOK(vkCmdSetViewport);
        HOOK(vkCmdSetScissor);
        HOOK(vkCmdSetViewportWithCount);
        HOOK(vkCmdSetScissorWithCount);
        HOOK(vkCmdSetViewportWithCountEXT);
        HOOK(vkCmdSetScissorWithCountEXT);
        HOOK(vkCmdBindVertexBuffers);
        HOOK(vkCmdBindVertexBuffers2);
        HOOK(vkCmdBindVertexBuffers2EXT);
        HOOK(vkCmdBindIndexBuffer);
        HOOK(vkCmdBindIndexBuffer2);
        HOOK(vkCmdBindIndexBuffer2KHR);
        HOOK(vkCmdDrawIndirectCount);
        HOOK(vkCmdDrawIndirectCountKHR);
        HOOK(vkCmdDrawIndexedIndirectCount);
        HOOK(vkCmdDrawIndexedIndirectCountKHR);
        HOOK(vkCmdSetLineWidth);
        HOOK(vkCmdSetDepthBias);
        HOOK(vkCmdSetDepthBounds);
        HOOK(vkCmdSetStencilCompareMask);
        HOOK(vkCmdSetStencilWriteMask);
        HOOK(vkCmdSetStencilReference);
        HOOK(vkCmdSetEvent);
        HOOK(vkCmdResetEvent);
        HOOK(vkCmdResetEvent2);
        HOOK(vkCmdResetEvent2KHR);
        HOOK(vkCmdFillBuffer);
        HOOK(vkCmdSetBlendConstants);
        HOOK(vkCmdCopyBuffer);
        HOOK(vkCmdCopyBufferToImage);
        HOOK(vkCmdCopyImageToBuffer);
    }
    HOOK(vkCmdDispatch); HOOK(vkCmdDispatchIndirect); HOOK(vkCmdDispatchBase); HOOK(vkCmdDispatchBaseKHR);
#undef HOOK
    return nullptr;
}
}

OBS_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char* name) {
    if (!name) return nullptr;
    // The loader queries these while constructing the chain, including with a
    // not-yet-created instance handle. Do not look up dispatch state first.
    if (std::strcmp(name, "vkCreateInstance") == 0) return reinterpret_cast<PFN_vkVoidFunction>(vkCreateInstance);
    if (std::strcmp(name, "vkGetInstanceProcAddr") == 0) return reinterpret_cast<PFN_vkVoidFunction>(vkGetInstanceProcAddr);
    if (std::strcmp(name, "vkGetDeviceProcAddr") == 0) return reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceProcAddr);
    // Interface v2 also exposes this callback through GIPA so a layer above us
    // can retrieve the physical-device chain without bypassing this observer.
    if (std::strcmp(name, "vk_layerGetPhysicalDeviceProcAddr") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(vk_layerGetPhysicalDeviceProcAddr);
    if (!instance) return nullptr;
    auto* state = findInstance(instance); if (!state) return nullptr;
    const auto downstream = state->gipa(instance, name);
    // Preserve availability of core versions/extensions instead of advertising a
    // hook when the next layer/driver does not expose the requested command.
    if (!downstream) return nullptr;
    if(state->srSdkPrivate&&std::strcmp(name,"vkCreateDevice")&&std::strcmp(name,"vkDestroyDevice")&&std::strcmp(name,"vkDestroyInstance"))return downstream;
    if (const auto hook = instanceHook(name)) return hook;
    return downstream;
}
OBS_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char* name) {
    if (!name) return nullptr;
    if (std::strcmp(name, "vkGetDeviceProcAddr") == 0) return reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceProcAddr);
    if (!device) return nullptr;
    auto* state = findDevice(device); if (!state) return nullptr;
    const auto downstream = state->gdpa(device, name); if (!downstream) return nullptr;
    if(state->srSdkPrivate&&std::strcmp(name,"vkDestroyDevice"))return downstream;
    if (const auto hook = deviceHook(name)) return hook;
    return downstream;
}
OBS_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_layerGetPhysicalDeviceProcAddr(VkInstance instance, const char* name) {
    if (!name || !instance) return nullptr;
    auto* state = findInstance(instance); if (!state || !state->gpdpa) return nullptr;
    const auto downstream = state->gpdpa(instance, name); if (!downstream) return nullptr;
    if(state->srSdkPrivate&&std::strcmp(name,"vkCreateDevice"))return downstream;
    if (std::strcmp(name, "vkCreateDevice") == 0) return reinterpret_cast<PFN_vkVoidFunction>(vkCreateDevice);
    if (std::strcmp(name, "vkGetPhysicalDeviceProperties") == 0) return reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceProperties);
    if (std::strcmp(name, "vkGetPhysicalDeviceProperties2") == 0) return reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceProperties2);
    if (std::strcmp(name, "vkGetPhysicalDeviceProperties2KHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceProperties2KHR);
    if (std::strcmp(name, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
    if (std::strcmp(name, "vkGetPhysicalDeviceSurfaceCapabilities2KHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceSurfaceCapabilities2KHR);
    return downstream;
}
OBS_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* info) {
    if (!info || info->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT || info->loaderLayerInterfaceVersion < 2)
        return VK_ERROR_INITIALIZATION_FAILED;
    info->loaderLayerInterfaceVersion = 2;
    info->pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
    info->pfnGetDeviceProcAddr = vkGetDeviceProcAddr;
    info->pfnGetPhysicalDeviceProcAddr = vk_layerGetPhysicalDeviceProcAddr;
    return VK_SUCCESS;
}

extern "C" BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID reserved) {
    // FreeLibrary can unload a layer before process termination. No Vulkan calls,
    // heap teardown, or waits here while the Windows loader lock is held.
    if (reason == DLL_PROCESS_DETACH && !reserved && logFile != INVALID_HANDLE_VALUE) {
        CloseHandle(logFile);
        logFile = INVALID_HANDLE_VALUE;
    }
    return TRUE;
}
