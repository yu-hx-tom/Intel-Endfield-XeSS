#include "presentation_runtime.hpp"
#include "build_flavor.hpp"
#include <limits>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <algorithm>
#include <stdexcept>
namespace xess_vk::presentation {
struct Runtime::FakeState {
    enum class Use { Available, Reserved, Acquired, CopyPending };
    struct Slot {
        VkImage image{};
        Use use{Use::Available};
        std::uint64_t copyToken{};
    };
    struct Pool {
        std::uint64_t generation{};
        VkExtent2D extent{};
        VkFormat format{};
        bool retired{}, destroying{};
        unsigned acquireSubmissions{};
        std::vector<Slot> slots;
    };
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::map<VkSwapchainKHR, std::shared_ptr<Pool>> pools;
    VkSwapchainKHR current{};
    VkExtent2D preparedExtent{};
    VkFormat preparedFormat{};
    VkResult terminal{VK_SUCCESS};
    bool stopping{};
};
bool enabled() noexcept {
    wchar_t value[8]{};
    return GetEnvironmentVariableW(L"XESS_PRESENTATION_OFF", value, 8) == 1 && value[0] == L'1';
}
bool gpuTimestampsRequested() noexcept {
    if constexpr(releaseMinimal)return false;
    wchar_t value[2]{};
    return GetEnvironmentVariableW(L"XESS_GPU_TIMESTAMPS", value, 2) == 1 && value[0] == L'1';
}
Runtime::Runtime(VkDevice d, VkSurfaceKHR s, VkQueue q, Dispatch dispatch, Backend &b, bool fake)
    : device_(d), surface_(s), queue_(q), dispatch_(dispatch), backend_(&b), fake_(fake), fakeState_(fake?std::make_unique<FakeState>():nullptr) {}
Runtime::Runtime(VkDevice d, VkSurfaceKHR s, VkQueue q, Dispatch dispatch, std::unique_ptr<Backend> b, bool fake)
    : device_(d), surface_(s), queue_(q), dispatch_(dispatch), owned_(std::move(b)), backend_(owned_.get()), fake_(fake), fakeState_(fake?std::make_unique<FakeState>():nullptr) {}
Runtime::~Runtime() {
    // The layer also retains the Vulkan parents on unproven cleanup.
    if (owned_ && !cleanupProven_) owned_.release();
}
VkResult Runtime::fail(VkResult result, const char *stage) {
    failed_ = true;
    shutdown_.failedQuarantined = true;
    stage_ = stage;
    if(fakeState_){std::lock_guard lock(fakeState_->mutex);fakeState_->terminal=VK_ERROR_DEVICE_LOST;fakeState_->changed.notify_all();}
    try { if (backend_) backend_->requestTimingStop(); } catch (...) {}
    return result < 0 ? result : VK_ERROR_UNKNOWN;
}
VkResult Runtime::create(const VkSwapchainCreateInfoKHR *info, const VkAllocationCallbacks *a, VkSwapchainKHR *out) {
    if(fake_)return createFake(info,a,out);
    VkSurfaceFullScreenExclusiveInfoEXT exclusive{VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT};
    VkSurfaceFullScreenExclusiveWin32InfoEXT monitor{VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_WIN32_INFO_EXT};
    bool hasExclusive{}, hasMonitor{}, supported = true;
    if (backend_ && info && info->surface == surface_ && info->pNext) {
        auto* node = static_cast<const VkBaseInStructure*>(info->pNext);
        for (unsigned n = 0; node && supported; ++n, node = node->pNext) {
            if (n >= 2) { supported = false; break; }
            if (node->sType == VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT && !hasExclusive) {
                const auto* value = reinterpret_cast<const VkSurfaceFullScreenExclusiveInfoEXT*>(node);
                hasExclusive = true;
                supported = value->fullScreenExclusive == VK_FULL_SCREEN_EXCLUSIVE_DEFAULT_EXT ||
                    value->fullScreenExclusive == VK_FULL_SCREEN_EXCLUSIVE_ALLOWED_EXT ||
                    value->fullScreenExclusive == VK_FULL_SCREEN_EXCLUSIVE_DISALLOWED_EXT;
            } else if (node->sType == VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_WIN32_INFO_EXT && !hasMonitor) {
                monitor = *reinterpret_cast<const VkSurfaceFullScreenExclusiveWin32InfoEXT*>(node);
                monitor.pNext = nullptr; hasMonitor = true;
            } else supported = false;
        }
        if (!supported || !hasExclusive) {
            stage_ = "unsupported_fullscreen_chain";
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }
        exclusive.fullScreenExclusive = VK_FULL_SCREEN_EXCLUSIVE_DISALLOWED_EXT;
        exclusive.pNext = hasMonitor ? &monitor : nullptr;
    }
    bool managed = backend_ && !shutdown_.privateCleanupCompleted && !failed_ && info && out && !a &&
        info->sType == VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR && (!info->pNext || hasExclusive) &&
        !info->flags && info->surface == surface_ &&
        (info->imageFormat == VK_FORMAT_R8G8B8A8_UNORM || info->imageFormat == VK_FORMAT_B8G8R8A8_UNORM) &&
        info->imageColorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR &&
        info->imageExtent.width && info->imageExtent.height && info->imageArrayLayers == 1 &&
        info->imageSharingMode == VK_SHARING_MODE_EXCLUSIVE && !info->queueFamilyIndexCount &&
        (info->presentMode == VK_PRESENT_MODE_FIFO_KHR || info->presentMode == VK_PRESENT_MODE_IMMEDIATE_KHR) &&
        (!current_ || info->oldSwapchain == current_) &&
        (current_ || !info->oldSwapchain) && chainGeneration_ != std::numeric_limits<std::uint64_t>::max();
    if (managed) {
        cleanupProven_ = false;
        try {
            if (backend_->prepare(*info) != VK_SUCCESS) {
                fail(VK_ERROR_UNKNOWN, "prepare_passthrough");
                managed = false;
            }
        } catch (...) {
            fail(VK_ERROR_UNKNOWN, "prepare_exception_passthrough");
            managed = false;
        }
    } else stage_ = "create_passthrough";
    VkSwapchainKHR nativeOld = info ? info->oldSwapchain : VK_NULL_HANDLE;
    if (managed && !current_ && !nativeOld) {
        for (const auto &entry : chains_) {
            if (!entry.second.managed || !entry.second.destroyRequested || entry.second.nativeRetired) continue;
            if (nativeOld) { nativeOld = VK_NULL_HANDLE; break; }
            nativeOld = entry.first;
        }
    }
    std::uint64_t replacementFrom{}, replacementThrough{};
    // The native call owns oldSwapchain retirement even when creation fails.
    if (nativeOld) {
        const auto old = chains_.find(nativeOld);
        if (old != chains_.end()) {
            old->second.retired = true;
            old->second.nativeRetired = true;
            replacementFrom = old->second.replacementFrom ? old->second.replacementFrom : old->second.generation;
            replacementThrough = old->second.generation;
        }
        if (current_ == nativeOld) current_ = VK_NULL_HANDLE;
    }
    VkSwapchainCreateInfoKHR actual{};
    if (managed) {
        actual = *info;
        actual.oldSwapchain = nativeOld;
        if (hasExclusive) actual.pNext = &exclusive;
        actual.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        actual.clipped = VK_FALSE;
    }
    const auto result = dispatch_.create(device_, managed ? &actual : info, a, out);
    if (result != VK_SUCCESS) { stage_ = "native_create"; return result; }
    if (!managed) {
        stage_ = "created_passthrough";
        return result; // Untracked native chains need no private metadata budget.
    }
    try {
        auto &chain = chains_[*out];
        chain.generation = ++chainGeneration_;
        chain.replacementFrom = replacementFrom;
        chain.replacementThrough = replacementThrough;
        chain.managed = managed;
        current_ = *out;
        if (managed) {
            std::uint32_t count{};
            auto r = dispatch_.images(device_, *out, &count, nullptr);
            if (r != VK_SUCCESS || !count || count > 32) {
                chain.managed = false;
                fail(r, "native_images_passthrough");
            } else {
                chain.images.resize(count);
                r = dispatch_.images(device_, *out, &count, chain.images.data());
                if (r != VK_SUCCESS || !count || count > chain.images.size()) {
                    chain.managed = false;
                    fail(r, "native_images_passthrough");
                } else chain.images.resize(count);
            }
        }
    } catch (...) {
        // The application already owns a successful native chain.
        fail(VK_ERROR_UNKNOWN, "chain_bookkeeping_passthrough");
    }
    if (!failed_) stage_ = managed ? "created" : "created_passthrough";
    return result;
}
VkResult Runtime::images(VkSwapchainKHR chain, std::uint32_t *count, VkImage *images) {
    if(fake_){
        if(!count)return VK_ERROR_INITIALIZATION_FAILED;
        std::lock_guard lock(fakeState_->mutex);const auto found=fakeState_->pools.find(chain);
        if(found==fakeState_->pools.end()||found->second->destroying)return VK_ERROR_OUT_OF_DATE_KHR;
        const auto& slots=found->second->slots;const auto available=static_cast<std::uint32_t>(slots.size());
        if(!images){*count=available;return VK_SUCCESS;}
        const auto copied=std::min(*count,available);for(std::uint32_t i=0;i<copied;++i)images[i]=slots[i].image;
        *count=copied;return copied<available?VK_INCOMPLETE:VK_SUCCESS;
    }
    return dispatch_.images(device_, chain, count, images);
}
VkResult Runtime::forward(VkQueue q, const VkPresentInfoKHR *info, const char *stage) {
    if(fake_)return fail(VK_ERROR_DEVICE_LOST,"fake_handle_never_forwarded");
    stage_ = stage;
    lastNativePresentCalled_ = true;
    const auto invalidateRetirement = [&](Chain &chain) {
        chain.nativePresentCalled = true;
        if (!chain.retired) return;
        ++retirementEpoch_;
        chain.presentCompleted = false;
        const auto current = chains_.find(current_);
        if (current != chains_.end() && chain.generation >= current->second.replacementFrom &&
            chain.generation <= current->second.replacementThrough)
            current->second.presentAnchor = VK_NULL_HANDLE;
    };
    if (info && info->pSwapchains) {
        if (info->swapchainCount > 16) {
            for (auto &item : chains_) invalidateRetirement(item.second);
        } else for (std::uint32_t n = 0; n < info->swapchainCount; ++n) {
            const auto chain = chains_.find(info->pSwapchains[n]);
            if (chain != chains_.end()) invalidateRetirement(chain->second);
        }
    }
    if (backend_) backend_->nativePresentIssued(0, false);
    if (backend_ && shutdown_.privateCleanupCompleted)
        shutdown_.nativeTailRetained |= backend_->shutdownState().nativeTailRetained;
    return dispatch_.present(q, info);
}
VkResult Runtime::present(VkQueue q, const VkPresentInfoKHR *info) {
    const auto admission = admitPresent();
    return presentPacket(q, info);
}
VkResult Runtime::presentUntimedOff(VkQueue q, const VkPresentInfoKHR *info) {
    const auto admission = admitPresent();
    if (backend_) backend_->associatePresentTiming(0, {}, 0);
    return presentPacket(q, info);
}
VkResult Runtime::presentPacket(VkQueue q, const VkPresentInfoKHR *info) {
    if(fake_)return presentFake(q,info);
    lastPrivateCopyCompleted_ = lastNativePresentCalled_ = lastSdkOutputSucceeded_ = false;
    lastSdkOutputAttempted_ = false;
    lastUntimedTimingUnavailable_ = false;
    lastCopyProgress_ = CopyProgress::NotSubmitted;
    if (!backend_ || shutdown_.privateCleanupCompleted)
        return forward(q, info, "private_released_native_passthrough");
    if (failed_) return forward(q, info, "failed_native_passthrough");
    if (q != queue_ || !info || info->sType != VK_STRUCTURE_TYPE_PRESENT_INFO_KHR || info->pNext ||
        info->swapchainCount != 1 || !info->pSwapchains || !info->pImageIndices ||
        (info->waitSemaphoreCount && !info->pWaitSemaphores) || info->waitSemaphoreCount > 16)
        return forward(q, info, "present_contract_passthrough");
    const auto found = chains_.find(info->pSwapchains[0]);
    if (found == chains_.end() || !found->second.managed || found->second.retired ||
        info->pSwapchains[0] != current_ || info->pImageIndices[0] >= found->second.images.size())
        return forward(q, info, "present_chain_passthrough");
    auto &chain = found->second;
    cleanupProven_ = false;
    VkResult finished = VK_ERROR_UNKNOWN;
    VkResult copied = VK_ERROR_UNKNOWN;
    bool copyEntered{};
    {
        CpuWallScope elapsed(copyCpu_);
        try {
            finished = backend_->finishQueuedCopy();
            if (finished == VK_SUCCESS) {
                if (queuedProof_.generation && queuedProof_.retirementEpoch == retirementEpoch_) {
                    for (auto &entry : chains_) {
                        if (entry.second.generation == queuedProof_.generation) {
                            completeReplacementPresent(entry.second, queuedProof_.image);
                            break;
                        }
                    }
                }
                queuedProof_ = {};
                copyEntered = true;
                copied = backend_->copy(*info, chain.images[info->pImageIndices[0]], chain.generation);
            }
        } catch (...) {
            // Submission ownership must still be checked after an exception.
        }
    }
    if (finished != VK_SUCCESS) {
        // The new packet's waits were never submitted; preserve their original ownership.
        fail(finished, "previous_queued_copy_incomplete_passthrough");
        return forward(q, info, "previous_queued_copy_incomplete_passthrough");
    }
    if (!copyEntered) {
        fail(VK_ERROR_UNKNOWN, "queued_retirement_failed_passthrough");
        return forward(q, info, "queued_retirement_failed_passthrough");
    }
    lastCopyProgress_ = backend_->copyProgress();
    const bool queued = lastCopyProgress_ == CopyProgress::QueuedOff || lastCopyProgress_ == CopyProgress::QueuedOn;
    lastPrivateCopyCompleted_ = lastCopyProgress_ == CopyProgress::Completed;
    chain.privateSubmitted |= lastCopyProgress_ != CopyProgress::NotSubmitted;
    if (lastCopyProgress_ == CopyProgress::NotSubmitted) {
        fail(copied, "copy_not_submitted_passthrough");
        return forward(q, info, "copy_not_submitted_passthrough");
    }
    if(lastCopyProgress_==CopyProgress::SubmissionUncertain) {
        if(info->pResults) info->pResults[0]=copied;
        return fail(copied,"copy_submission_uncertain_retained");
    }
    const auto complete = lastPrivateCopyCompleted_ ? VK_NULL_HANDLE : backend_->copyCompletionSemaphore();
    if ((lastCopyProgress_ == CopyProgress::SubmittedPending || queued) && !complete)
        return fail(copied, "copy_pending_without_completion_sync");
    if (copied != VK_SUCCESS || (!lastPrivateCopyCompleted_ && !queued))
        fail(copied, "copy_pending_native_synchronized");
    VkPresentInfoKHR native = *info;
    native.waitSemaphoreCount = complete ? 1 : 0;
    native.pWaitSemaphores = complete ? &complete : nullptr;
    lastNativePresentCalled_ = true;
    chain.nativePresentCalled = true;
    backend_->nativePresentIssued(chain.generation, lastCopyProgress_ == CopyProgress::SubmittedPending);
    const auto result = [&] {
        CpuWallScope elapsed(nativePresentCpu_);
        return dispatch_.present(q, &native);
    }();
    const auto individual = info->pResults ? info->pResults[0] : result;
    const auto accepted = [](VkResult r) { return r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR; };
    if (!accepted(result) || !accepted(individual)) {
        if (lastPrivateCopyCompleted_ || queued) {
            bool discarded{};
            try { discarded = backend_->discardOutput(); } catch (...) {}
            if (!discarded) fail(result, "unused_output_resources_retained");
        }
        stage_ = "native_present";
        if ((!accepted(result) && result != VK_ERROR_OUT_OF_DATE_KHR) ||
            (!accepted(individual) && individual != VK_ERROR_OUT_OF_DATE_KHR))
            fail(result, "native_present");
        return result;
    }
    if (copied != VK_SUCCESS || (!lastPrivateCopyCompleted_ && !queued)) {
        stage_ = "copy_pending_native_synchronized";
        return result;
    }
    if (queued)
        queuedProof_ = {chain.generation, retirementEpoch_, chain.images[info->pImageIndices[0]]};
    else
        completeReplacementPresent(chain, chain.images[info->pImageIndices[0]]);
    VkResult output = VK_ERROR_UNKNOWN;
    try {
        CpuWallScope elapsed(outputCpu_);
        lastSdkOutputAttempted_ = true;
        output = backend_->output();
    } catch (...) {}
    if (output != VK_SUCCESS) {
        if (output == VK_NOT_READY) {
            lastUntimedTimingUnavailable_ = backend_->frameStatus().untimedTimingUnavailable;
            stage_ = "sdk_output_temporarily_unavailable";
            return result;
        }
        fail(output, "sdk_output_failed_native_preserved");
        // Native already consumed this packet: preserve its results and never replay it.
        return result;
    }
    lastSdkOutputSucceeded_ = true;
    ++frames_;
    stage_ = "presented";
    return result;
}
void Runtime::completeReplacementPresent(Chain &chain, VkImage image) {
    if (!chain.presentAnchor) {
        chain.presentAnchor = image;
        return;
    }
    if (chain.presentAnchor != image || !chain.replacementThrough) return;
    // Valid acquire -> app render -> our completed copy of the same image
    // proves its earlier present finished, and thus the replaced WSI tail.
    for (auto &entry : chains_)
        if (entry.second.retired && entry.second.generation >= chain.replacementFrom &&
            entry.second.generation <= chain.replacementThrough)
            entry.second.presentCompleted = true;
    destroyCompletedRequests();
}
void Runtime::destroyCompletedRequests() {
    for (auto it = chains_.begin(); it != chains_.end();) {
        auto &chain = it->second;
        if (!chain.destroyRequested || !chain.presentCompleted) { ++it; continue; }
        if (backend_) backend_->retireChain(chain.generation);
        dispatch_.destroy(device_, it->first, nullptr);
        ++retiredChainsDestroyed_;
        it = chains_.erase(it);
    }
}
VkResult Runtime::destroy(VkSwapchainKHR chain, const VkAllocationCallbacks *a) {
    if(fake_)return destroyFake(chain,a);
    const auto found = chains_.find(chain);
    if (found == chains_.end()) {
        dispatch_.destroy(device_, chain, a);
        return VK_SUCCESS;
    }
    // Managed creation accepts only the default allocator. Never retain a
    // caller's callback pointer beyond a destroy call.
    found->second.destroyRequested = a == nullptr;
    if (current_ && chain != current_ && found->second.retired) {
        const bool neverUsed = !found->second.nativePresentCalled && !found->second.privateSubmitted;
        if (!neverUsed && !found->second.presentCompleted &&
            (!backend_ || !backend_->canDestroyRetiredChain(found->second.generation))) {
            stage_ = "old_chain_retained_live_backend";
            return VK_NOT_READY;
        }
        if (backend_) backend_->retireChain(found->second.generation);
        dispatch_.destroy(device_, chain, a);
        ++retiredChainsDestroyed_;
        chains_.erase(found);
        return VK_SUCCESS;
    }
    // A chain destroy is not device shutdown: never close the backend shared
    // with a new chain or submit an unowned queue tail here. Completion proof
    // and private cleanup belong to the externally synchronized DeviceDestroy.
    const bool neverUsed = !found->second.nativePresentCalled && !found->second.privateSubmitted;
    const bool retired = found->second.presentCompleted ||
        (backend_ && backend_->canDestroyRetiredChain(found->second.generation));
    if (!neverUsed && !retired && !shutdown_.privateCleanupCompleted) {
        found->second.retired = true;
        if (chain == current_) current_ = VK_NULL_HANDLE;
        stage_ = "chain_retained_until_device_shutdown";
        return VK_NOT_READY;
    }
    if (shutdown_.nativeTailRetained && found->second.nativePresentCalled && !found->second.presentCompleted) {
        stage_ = "private_clean_native_tail_retained";
        return VK_NOT_READY;
    }
    if (backend_) backend_->retireChain(found->second.generation);
    dispatch_.destroy(device_, chain, a);
    if (found->second.retired) ++retiredChainsDestroyed_;
    chains_.erase(chain);
    if (chain == current_) current_ = VK_NULL_HANDLE;
    return VK_SUCCESS;
}
VkResult Runtime::stop() {
    if (fakeState_) {
        std::lock_guard lock(fakeState_->mutex);
        fakeState_->stopping = true;
        fakeState_->changed.notify_all();
        if (fakeState_->terminal != VK_SUCCESS) {
            shutdown_.failedQuarantined = true;
            stage_ = "fake_terminal_resources_retained";
            return fakeState_->terminal;
        }
    }
    if (shutdown_.privateCleanupCompleted)
        return shutdown_.nativeTailRetained ? VK_NOT_READY : VK_SUCCESS;
    if (shutdownBlocked_) return VK_ERROR_UNKNOWN;
    VkResult result = VK_ERROR_UNKNOWN;
    try {
        backend_->stopAdmission();
        result = backend_->drain();
    } catch (...) {}
    shutdown_ = backend_->shutdownState();
    if (result != VK_SUCCESS) {
        shutdownBlocked_ = true;
        shutdown_.failedQuarantined = true;
        stage_ = "cleanup_retained";
        return result < 0 ? result : VK_ERROR_UNKNOWN;
    }
    if (!shutdown_.privateCleanupCompleted) {
        shutdownBlocked_ = shutdown_.failedQuarantined = true;
        stage_ = "private_cleanup_unproven";
        return VK_ERROR_UNKNOWN;
    }
    if (fake_) {
        for (;;) {
            VkSwapchainKHR handle;
            {
                std::lock_guard lock(fakeState_->mutex);
                if (fakeState_->pools.empty()) break;
                handle = fakeState_->pools.begin()->first;
            }
            if (destroyFake(handle, nullptr) != VK_SUCCESS) {
                shutdownBlocked_ = shutdown_.failedQuarantined = true;
                shutdown_.privateCleanupCompleted = false;
                stage_ = "fake_pool_cleanup_retained";
                return VK_ERROR_UNKNOWN;
            }
        }
        shutdown_.nativeTailRetained = false;
    }
    cleanupProven_ = true;
    // Keep the small backend shell stable for callback methods which already
    // entered on another thread. Its SDK, bridge and private Vulkan children
    // are gone, and stopAdmission makes all later borrowing return empty.
    stage_ = shutdown_.nativeTailRetained ? "private_clean_native_tail_retained" : "private_cleanup_completed";
    return shutdown_.nativeTailRetained ? VK_NOT_READY : VK_SUCCESS;
}
std::size_t Runtime::retainedChains() const {
    if (!fake_)
        return chains_.size();
    std::lock_guard lock(fakeState_->mutex);
    return fakeState_->pools.size();
}
bool Runtime::failed() const {
    if (!fake_)
        return failed_;
    std::lock_guard lock(fakeState_->mutex);
    return fakeState_->terminal != VK_SUCCESS;
}
bool Runtime::ownsFakeChain(VkSwapchainKHR chain) const {
    if (!fake_)
        return false;
    std::lock_guard lock(fakeState_->mutex);
    return fakeState_->pools.contains(chain);
}
bool Runtime::isFakeImage(VkImage image) const {
    if (!fake_ || !image)
        return false;
    std::lock_guard lock(fakeState_->mutex);
    for (const auto &entry : fakeState_->pools)
        for (const auto &slot : entry.second->slots)
            if (slot.image == image)
                return true;
    return false;
}
VkResult Runtime::createFake(const VkSwapchainCreateInfoKHR *info,
                             const VkAllocationCallbacks *allocator, VkSwapchainKHR *out) {
    {
        std::lock_guard lock(fakeState_->mutex);
        if (info && info->oldSwapchain && !fakeState_->pools.contains(info->oldSwapchain))
            return VK_ERROR_OUT_OF_DATE_KHR;
        if (fakeState_->current && (!info || info->oldSwapchain != fakeState_->current))
            return VK_ERROR_NATIVE_WINDOW_IN_USE_KHR;
        if (info && info->oldSwapchain) {
            const auto old = fakeState_->pools.find(info->oldSwapchain);
            if (old != fakeState_->pools.end())
                old->second->retired = true;
        }
        if (info && fakeState_->current == info->oldSwapchain)
            fakeState_->current = VK_NULL_HANDLE;
        fakeState_->changed.notify_all();
        if (fakeState_->terminal != VK_SUCCESS)
            return fakeState_->terminal;
        if (fakeState_->stopping)
            return VK_ERROR_OUT_OF_DATE_KHR;
    }
    if (!backend_ || !info || !out || allocator ||
        info->sType != VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR || info->flags ||
        info->surface != surface_ || !info->imageExtent.width || !info->imageExtent.height ||
        info->imageArrayLayers != 1 ||
        (info->imageFormat != VK_FORMAT_R8G8B8A8_UNORM &&
         info->imageFormat != VK_FORMAT_B8G8R8A8_UNORM) ||
        info->imageColorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR ||
        info->imageSharingMode != VK_SHARING_MODE_EXCLUSIVE || info->queueFamilyIndexCount ||
        !info->minImageCount || info->minImageCount > 32 ||
        (info->presentMode != VK_PRESENT_MODE_FIFO_KHR &&
         info->presentMode != VK_PRESENT_MODE_IMMEDIATE_KHR))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    bool exclusive{}, monitor{};
    for (auto *node = static_cast<const VkBaseInStructure *>(info->pNext); node;
         node = node->pNext) {
        if (node->sType == VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT && !exclusive) {
            const auto mode = reinterpret_cast<const VkSurfaceFullScreenExclusiveInfoEXT *>(node)
                                  ->fullScreenExclusive;
            if (mode != VK_FULL_SCREEN_EXCLUSIVE_DEFAULT_EXT &&
                mode != VK_FULL_SCREEN_EXCLUSIVE_ALLOWED_EXT &&
                mode != VK_FULL_SCREEN_EXCLUSIVE_DISALLOWED_EXT)
                return VK_ERROR_FEATURE_NOT_PRESENT;
            exclusive = true;
        } else if (node->sType == VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_WIN32_INFO_EXT &&
                   !monitor)
            monitor = true;
        else
            return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    if (monitor && !exclusive)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    std::uint64_t generation{};
    {
        std::lock_guard lock(fakeState_->mutex);
        if (chainGeneration_ == UINT64_MAX)
            return VK_ERROR_TOO_MANY_OBJECTS;
        generation = ++chainGeneration_;
    }
    try {
        const auto prepared = backend_->prepare(*info);
        if (prepared != VK_SUCCESS)
            return prepared;
        {
            std::lock_guard lock(fakeState_->mutex);
            fakeState_->preparedExtent = info->imageExtent;
            fakeState_->preparedFormat = info->imageFormat;
        }
        auto pool = std::make_shared<FakeState::Pool>();
        pool->generation = generation;
        pool->extent = info->imageExtent;
        pool->format = info->imageFormat;
        pool->slots.resize(std::max<std::uint32_t>(3, info->minImageCount));
        const auto handle = reinterpret_cast<VkSwapchainKHR>(pool.get());
        // Reserve all CPU bookkeeping before publishing any real image allocation.
        {
            std::lock_guard lock(fakeState_->mutex);
            fakeState_->pools.emplace(handle, pool);
        }
        std::vector<VkImage> images;
        VkResult created;
        try {
            images.reserve(pool->slots.size());
            created = backend_->createFakeImages(*info, generation, images);
        } catch (...) {
            std::lock_guard lock(fakeState_->mutex);
            fakeState_->pools.erase(handle);
            throw;
        }
        if (created != VK_SUCCESS) {
            std::lock_guard lock(fakeState_->mutex);
            fakeState_->pools.erase(handle);
            return created;
        }
        if (images.size() != pool->slots.size()) {
            fail(VK_ERROR_UNKNOWN, "fake_pool_contract_failed");
            return VK_ERROR_DEVICE_LOST;
        }
        {
            std::lock_guard lock(fakeState_->mutex);
            for (std::size_t i = 0; i < images.size(); ++i)
                pool->slots[i].image = images[i];
            fakeState_->current = handle;
        }
        cleanupProven_ = false;
        *out = handle;
        stage_ = "fake_created";
        return VK_SUCCESS;
    } catch (const std::bad_alloc &) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    } catch (...) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
}
VkResult Runtime::status(VkSwapchainKHR chain) {
    if (!fake_)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    try {
        VkExtent2D extent;
        {
            std::lock_guard lock(fakeState_->mutex);
            if (fakeState_->terminal != VK_SUCCESS)
                return fakeState_->terminal;
            const auto found = fakeState_->pools.find(chain);
            if (fakeState_->stopping || found == fakeState_->pools.end() ||
                found->second->retired || found->second->destroying)
                return VK_ERROR_OUT_OF_DATE_KHR;
            extent = found->second->extent;
        }
        return backend_->windowStatus(extent);
    } catch (...) {
        std::lock_guard lock(fakeState_->mutex);
        fakeState_->terminal = VK_ERROR_DEVICE_LOST;
        fakeState_->changed.notify_all();
        return VK_ERROR_DEVICE_LOST;
    }
}
VkResult Runtime::acquire2(const VkAcquireNextImageInfoKHR *info, std::uint32_t *index, wsi_acquire::Call* timing) {
    if (!info || info->sType != VK_STRUCTURE_TYPE_ACQUIRE_NEXT_IMAGE_INFO_KHR || info->pNext ||
        info->deviceMask != 1)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    return acquire(info->swapchain, info->timeout, info->semaphore, info->fence, index, timing);
}
VkResult Runtime::acquire(VkSwapchainKHR chain, std::uint64_t timeout, VkSemaphore semaphore,
                          VkFence fence, std::uint32_t *index, wsi_acquire::Call* timing) {
    if (!fake_)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    if (!index || (!semaphore && !fence))
        return VK_ERROR_INITIALIZATION_FAILED;
    const auto began = std::chrono::steady_clock::now();
    CpuWallTime ignored;
    try {
        for (;;) {
            const auto ready = status(chain);
            if (ready != VK_SUCCESS)
                return ready;
            std::shared_ptr<FakeState::Pool> pool;
            std::array<std::pair<std::size_t, std::uint64_t>, 32> pending{};
            std::size_t pendingCount{}, selected = SIZE_MAX;
            bool pendingSelection{};
            std::uint64_t selectedToken{};
            {
                std::lock_guard lock(fakeState_->mutex);
                const auto found = fakeState_->pools.find(chain);
                if (found == fakeState_->pools.end() || found->second->retired ||
                    found->second->destroying)
                    return VK_ERROR_OUT_OF_DATE_KHR;
                pool = found->second;
                if (timing) timing->generation = pool->generation;
                for (std::size_t i = 0; i < pool->slots.size(); ++i) {
                    auto &slot = pool->slots[i];
                    if (slot.use == FakeState::Use::Available && selected == SIZE_MAX) {
                        selected = i;
                        slot.use = FakeState::Use::Reserved;
                    } else if (slot.use == FakeState::Use::CopyPending)
                        pending[pendingCount++] = {i, slot.copyToken};
                }
                if(selected==SIZE_MAX&&backend_->asyncAcquireEnabled()&&pendingCount) {
                    for(std::size_t i=0;i<pendingCount;++i) if(pending[i].second) {
                        selected=pending[i].first; selectedToken=pending[i].second; pendingSelection=true;
                        pool->slots[selected].use=FakeState::Use::Reserved; break;
                    }
                }
                if(selected!=SIZE_MAX)++pool->acquireSubmissions;
            }
            if (selected != SIZE_MAX) {
                const auto signalled = pendingSelection ?
                    backend_->signalAcquireAfterCopy(pool->slots[selected].image,pool->generation,selectedToken,semaphore,fence,timing) :
                    backend_->signalAcquire(semaphore, fence, timing);
                {
                    std::lock_guard lock(fakeState_->mutex);
                    --pool->acquireSubmissions;
                    auto &slot = pool->slots[selected];
                    if (signalled == VK_SUCCESS)
                        slot.use = FakeState::Use::Acquired;
                    else if (signalled == VK_ERROR_OUT_OF_HOST_MEMORY ||
                             signalled == VK_ERROR_OUT_OF_DEVICE_MEMORY)
                        slot.use = pendingSelection ? FakeState::Use::CopyPending : FakeState::Use::Available;
                    if (signalled != VK_SUCCESS && signalled != VK_ERROR_OUT_OF_HOST_MEMORY &&
                        signalled != VK_ERROR_OUT_OF_DEVICE_MEMORY)
                        fakeState_->terminal = VK_ERROR_DEVICE_LOST;
                    fakeState_->changed.notify_all();
                }
                if (signalled == VK_SUCCESS)
                    *index = static_cast<std::uint32_t>(selected);
                return signalled;
            }
            bool completed{};
            {
            CpuWallScope elapsed(timing ? timing->noSlotPollCpu : ignored);
            for (std::size_t i = 0; i < pendingCount; ++i) {
                const auto [slotIndex, token] = pending[i];
                const auto polled = backend_->pollCopyCompletion(token);
                if (polled == VK_NOT_READY)
                    continue;
                if (polled != VK_SUCCESS) {
                    std::lock_guard lock(fakeState_->mutex);
                    fakeState_->terminal = VK_ERROR_DEVICE_LOST;
                    fakeState_->changed.notify_all();
                    return VK_ERROR_DEVICE_LOST;
                }
                std::lock_guard lock(fakeState_->mutex);
                auto &slot = pool->slots[slotIndex];
                if (slot.use == FakeState::Use::CopyPending && slot.copyToken == token) {
                    slot.use = FakeState::Use::Available;
                    completed = true;
                }
            }
            }
            if (completed)
                continue;
            if (!timeout)
                return VK_NOT_READY;
            const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now() - began)
                                     .count();
            if (timeout != UINT64_MAX && static_cast<std::uint64_t>(elapsed) >= timeout)
                return VK_TIMEOUT;
            const auto waitNs = timeout == UINT64_MAX
                                    ? 1'000'000ull
                                    : std::min<std::uint64_t>(
                                          1'000'000, timeout - static_cast<std::uint64_t>(elapsed));
            {
                CpuWallScope elapsed(timing ? timing->noSlotWaitCpu : ignored);
                std::unique_lock lock(fakeState_->mutex);
                fakeState_->changed.wait_for(lock, std::chrono::nanoseconds(waitNs));
            }
        }
    } catch (...) {
        std::lock_guard lock(fakeState_->mutex);
        fakeState_->terminal = VK_ERROR_DEVICE_LOST;
        fakeState_->changed.notify_all();
        return VK_ERROR_DEVICE_LOST;
    }
}
VkResult Runtime::presentFake(VkQueue queue, const VkPresentInfoKHR *info) {
    lastPrivateCopyCompleted_ = lastNativePresentCalled_ = lastSdkOutputSucceeded_ = false;
    lastSdkOutputAttempted_ = false;
    lastUntimedTimingUnavailable_ = false;
    lastCopyProgress_ = CopyProgress::NotSubmitted;
    if (queue != queue_ || !info || info->sType != VK_STRUCTURE_TYPE_PRESENT_INFO_KHR ||
        info->pNext || info->swapchainCount != 1 || !info->pSwapchains || !info->pImageIndices ||
        (info->waitSemaphoreCount && !info->pWaitSemaphores) || info->waitSemaphoreCount > 16)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    auto finish = [&](VkResult result) {
        if (info->pResults)
            info->pResults[0] = result;
        return result;
    };
    try {
        std::shared_ptr<FakeState::Pool> pool;
        const auto index = info->pImageIndices[0];
        bool compatible{};
        {
            std::lock_guard lock(fakeState_->mutex);
            if (fakeState_->terminal != VK_SUCCESS)
                return finish(fakeState_->terminal);
            const auto found = fakeState_->pools.find(info->pSwapchains[0]);
            if (found == fakeState_->pools.end() || found->second->destroying ||
                index >= found->second->slots.size() ||
                found->second->slots[index].use != FakeState::Use::Acquired)
                return finish(VK_ERROR_OUT_OF_DATE_KHR);
            pool = found->second;
            pool->slots[index].use = FakeState::Use::Reserved;
            compatible = pool->extent.width == fakeState_->preparedExtent.width &&
                         pool->extent.height == fakeState_->preparedExtent.height &&
                         pool->format == fakeState_->preparedFormat;
        }
        VkResult copied = VK_ERROR_UNKNOWN;
        VkResult windowResult = VK_SUCCESS;
        {
            CpuWallScope elapsed(copyCpu_);
            try {
                const auto previous = backend_->finishQueuedCopy();
                if (previous != VK_SUCCESS) {
                    fail(previous, "fake_previous_copy_failed");
                    return finish(VK_ERROR_DEVICE_LOST);
                }
                windowResult =
                    compatible ? backend_->windowStatus(pool->extent) : VK_ERROR_OUT_OF_DATE_KHR;
                if (windowResult != VK_SUCCESS && windowResult != VK_ERROR_OUT_OF_DATE_KHR &&
                    windowResult != VK_ERROR_SURFACE_LOST_KHR) {
                    fail(windowResult, "fake_window_failed");
                    return finish(VK_ERROR_DEVICE_LOST);
                }
                copied = (holdOutput_ || windowResult != VK_SUCCESS)
                             ? backend_->consumePresentWaits(*info, pool->slots[index].image, pool->generation)
                             : backend_->copy(*info, pool->slots[index].image, pool->generation);
            } catch (...) {
                fail(VK_ERROR_UNKNOWN, "fake_copy_exception");
                return finish(VK_ERROR_DEVICE_LOST);
            }
        }
        lastCopyProgress_ = backend_->copyProgress();
        lastPrivateCopyCompleted_ = lastCopyProgress_ == CopyProgress::Completed;
        if (copied == VK_ERROR_OUT_OF_HOST_MEMORY || copied == VK_ERROR_OUT_OF_DEVICE_MEMORY) {
            if (lastCopyProgress_ == CopyProgress::NotSubmitted) {
                std::lock_guard lock(fakeState_->mutex);
                pool->slots[index].use = FakeState::Use::Acquired;
                return finish(copied);
            }
        }
        const auto token = backend_->copyCompletionToken();
        if (copied != VK_SUCCESS || !token ||
            lastCopyProgress_ == CopyProgress::SubmissionUncertain) {
            fail(copied, "fake_submission_failed");
            return finish(VK_ERROR_DEVICE_LOST);
        }
        {
            std::lock_guard lock(fakeState_->mutex);
            auto &slot = pool->slots[index];
            slot.copyToken = token;
            slot.use = FakeState::Use::CopyPending;
            fakeState_->changed.notify_all();
        }
        if(holdOutput_&&windowResult==VK_SUCCESS){stage_="sr_output_held_waits_consumed";return finish(VK_SUCCESS);}
        if (windowResult != VK_SUCCESS) {
            stage_ = "fake_window_error_waits_consumed";
            return finish(windowResult);
        }
        VkResult output = VK_ERROR_UNKNOWN;
        try {
            CpuWallScope elapsed(outputCpu_);
            lastSdkOutputAttempted_ = true;
            output = backend_->output();
        } catch (...) {
        }
        if (output == VK_NOT_READY) {
            lastUntimedTimingUnavailable_ = backend_->frameStatus().untimedTimingUnavailable;
            stage_ = "sdk_output_temporarily_unavailable";
            return finish(VK_SUCCESS);
        }
        if (output != VK_SUCCESS) {
            fail(output, "fake_sdk_output_failed");
            return finish(VK_ERROR_DEVICE_LOST);
        }
        lastSdkOutputSucceeded_ = true;
        ++frames_;
        stage_ = "fake_presented";
        return finish(VK_SUCCESS);
    } catch (...) {
        fail(VK_ERROR_UNKNOWN, "fake_present_exception");
        return finish(VK_ERROR_DEVICE_LOST);
    }
}
VkResult Runtime::destroyFake(VkSwapchainKHR chain, const VkAllocationCallbacks *allocator) {
    if (allocator)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    std::shared_ptr<FakeState::Pool> pool;
    {
        std::lock_guard lock(fakeState_->mutex);
        const auto found = fakeState_->pools.find(chain);
        if (found == fakeState_->pools.end())
            return VK_SUCCESS;
        pool = found->second;
        if(pool->acquireSubmissions)return VK_NOT_READY;
        pool->retired = pool->destroying = true;
        if (fakeState_->current == chain)
            fakeState_->current = VK_NULL_HANDLE;
        fakeState_->changed.notify_all();
    }
    VkResult destroyed;
    try {
        destroyed = backend_->destroyFakeImages(pool->generation);
    } catch (...) {
        return fail(VK_ERROR_DEVICE_LOST, "fake_destroy_exception");
    }
    if (destroyed != VK_SUCCESS)
        return destroyed;
    {
        std::lock_guard lock(fakeState_->mutex);
        fakeState_->pools.erase(chain);
        ++retiredChainsDestroyed_;
    }
    return VK_SUCCESS;
}
} // namespace xess_vk::presentation
