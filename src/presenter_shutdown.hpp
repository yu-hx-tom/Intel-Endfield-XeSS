#pragma once
#include <chrono>
#include <condition_variable>
#include <mutex>
namespace xess_vk::detail {
enum class ShutdownAdmission { Start, AlreadyCompleted, Retain };
inline ShutdownAdmission admitPresenterShutdown(bool completed, bool &inProgress, bool failed) {
    if (completed) return ShutdownAdmission::AlreadyCompleted;
    if (inProgress || failed) return ShutdownAdmission::Retain;
    inProgress = true;
    return ShutdownAdmission::Start;
}
// The production caller provides actual completion waits. Neither successful
// queue submission nor a wakeup is completion proof. Failed proof never enters
// proxy release or vendor destruction; synchronous vendor calls are not timed
// out or cancelled by this helper.
template<class GpuProof, class ReleaseProxy, class DestroyContexts>
bool runPresenterShutdownStages(bool &failed, GpuProof &&proveGpu, ReleaseProxy &&releaseProxy,
                                DestroyContexts &&destroyContexts) {
    if (failed) return false;
    try {
        proveGpu();
        releaseProxy();
        if (!destroyContexts()) { failed = true; return false; }
        return true;
    } catch (...) {
        failed = true;
        throw;
    }
}
inline bool waitForPresenterOperations(std::unique_lock<std::mutex> &lock,
                                      std::condition_variable &completed,
                                      const unsigned &operations,
                                      std::chrono::milliseconds timeout,
                                      bool &failed) {
    if (failed)
        return false;
    if (completed.wait_for(lock, timeout, [&] { return operations == 0; }))
        return true;
    failed = true;
    return false;
}
// SDK destruction failure is sticky. The context remains live, and subsequent
// shutdown attempts cannot turn that failure into apparent cleanup success.
template <class Sdk, class FgContext, class LatencyContext>
bool destroyPresenterContexts(Sdk &sdk, FgContext &context, LatencyContext &latency, bool &failed,
                              void (*trace)(const char*) = nullptr) {
    if (failed)
        return false;
    if (context) {
        if(trace) trace("FGDestroy enter");
        const auto result=sdk.xefgSwapChainDestroy(context);
        if(trace) trace("FGDestroy return");
        if (result < 0) {
            failed = true;
            return false;
        }
        context = nullptr;
    }
    if (latency) {
        if(trace) trace("LLDestroy enter");
        const auto result=sdk.xellDestroyContext(latency);
        if(trace) trace("LLDestroy return");
        if (result != 0) {
            failed = true;
            return false;
        }
        latency = nullptr;
    }
    return true;
}
} // namespace xess_vk::detail
