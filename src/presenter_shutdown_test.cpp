#include "presenter_shutdown.hpp"
#include "presentation_window_shutdown.hpp"
#include <iostream>
#include <stdexcept>
#include <vector>
#include <thread>
#include <atomic>

namespace presenter_shutdown_fixture {
unsigned checks{};
void require(bool value, const char* why) {
    ++checks;
    if (!value) throw std::runtime_error(why);
}
struct FakeSdk {
    int fgResult{}, latencyResult{};
    unsigned fgCalls{}, latencyCalls{};
    int* fgSeen{};
    int* latencySeen{};
    std::vector<char> order;
    int xefgSwapChainDestroy(int* context) {
        ++fgCalls; fgSeen = context; order.push_back('F'); return fgResult;
    }
    int xellDestroyContext(int* context) {
        ++latencyCalls; latencySeen = context; order.push_back('L'); return latencyResult;
    }
};
void actualHelperFailuresAndSuccess() {
    using xess_vk::detail::destroyPresenterContexts;
    int fgObject{}, latencyObject{};
    {
        FakeSdk sdk; sdk.fgResult = -1;
        int* fg = &fgObject; int* latency = &latencyObject; bool failed{};
        require(!destroyPresenterContexts(sdk, fg, latency, failed) && failed && fg == &fgObject && latency == &latencyObject &&
            sdk.fgCalls == 1 && sdk.latencyCalls == 0 && sdk.fgSeen == &fgObject,
            "FG destroy failure retains both contexts and does not destroy latency dependency");
        sdk.fgResult = 0;
        require(!destroyPresenterContexts(sdk, fg, latency, failed) && fg == &fgObject && latency == &latencyObject &&
            sdk.fgCalls == 1 && sdk.latencyCalls == 0,
            "second shutdown remains failed without retrying failed FG or claiming cleanup");
    }
    {
        FakeSdk sdk; sdk.latencyResult = 1;
        int* fg = &fgObject; int* latency = &latencyObject; bool failed{};
        require(!destroyPresenterContexts(sdk, fg, latency, failed) && failed && !fg && latency == &latencyObject &&
            sdk.fgCalls == 1 && sdk.latencyCalls == 1 && sdk.latencySeen == &latencyObject && sdk.order == std::vector<char>({'F', 'L'}),
            "latency destroy failure retains failed latency after successful FG destruction in dependency order");
        sdk.latencyResult = 0;
        require(!destroyPresenterContexts(sdk, fg, latency, failed) && !fg && latency == &latencyObject &&
            sdk.fgCalls == 1 && sdk.latencyCalls == 1,
            "second shutdown keeps latency failure sticky without double destroying either context");
    }
    {
        FakeSdk sdk; int* fg = &fgObject; int* latency = &latencyObject; bool failed{};
        require(destroyPresenterContexts(sdk, fg, latency, failed) && !failed && !fg && !latency &&
            sdk.fgCalls == 1 && sdk.latencyCalls == 1 && sdk.order == std::vector<char>({'F', 'L'}),
            "successful shutdown clears both actual helper contexts after ordered SDK destruction");
        require(destroyPresenterContexts(sdk, fg, latency, failed) && !failed && sdk.fgCalls == 1 && sdk.latencyCalls == 1,
            "repeated successful shutdown is idempotent without extra SDK calls");
    }
}
void boundedOperationDrain() {
    using xess_vk::detail::waitForPresenterOperations;
    std::mutex mutex;
    std::condition_variable completed;
    unsigned operations = 1;
    bool failed{};
    std::atomic<bool> release{}, mutexAvailable{};
    std::thread inFlight([&] {
        {
            std::lock_guard lock(mutex);
            mutexAvailable = true;
        }
        while (!release.load())
            std::this_thread::yield();
        std::lock_guard lock(mutex);
        operations = 0;
        completed.notify_all();
    });
    std::unique_lock lock(mutex);
    const bool drained = waitForPresenterOperations(lock, completed, operations,
                                                    std::chrono::milliseconds(20), failed);
    const bool operationWasRetained = operations == 1;
    const bool mutexReleasedDuringWait = mutexAvailable.load();
    lock.unlock();
    release = true;
    inFlight.join();
    require(!drained && failed && operationWasRetained && mutexReleasedDuringWait,
            "bounded production drain releases its mutex and reports an active operation without false completion");
    FakeSdk sdk;
    int fgObject{}, latencyObject{};
    int *fg = &fgObject;
    int *latency = &latencyObject;
    require(sdk.fgCalls == 0 && sdk.latencyCalls == 0 && fg && latency,
            "timed-out operation leaves every SDK dependency alive without destroy calls");
    lock.lock();
    require(!waitForPresenterOperations(lock, completed, operations,
                                       std::chrono::milliseconds(20), failed) && failed &&
            operations == 0 && sdk.fgCalls == 0 && sdk.latencyCalls == 0,
            "shutdown timeout remains sticky when the previously active callback later returns");
    bool cleanFailure{};
    require(waitForPresenterOperations(lock, completed, operations,
                                      std::chrono::milliseconds(20), cleanFailure) && !cleanFailure,
            "already drained operations complete immediately without a false timeout");
}
void actualShutdownStagesAndReentry() {
    using namespace xess_vk::detail;
    bool completed{}, inProgress{}, failed{};
    require(admitPresenterShutdown(completed, inProgress, failed) == ShutdownAdmission::Start && inProgress,
            "healthy shutdown admits exactly one owner");
    require(admitPresenterShutdown(completed, inProgress, failed) == ShutdownAdmission::Retain,
            "reentrant shutdown cannot repeat synchronous vendor destruction");
    std::vector<char> order;
    require(runPresenterShutdownStages(failed, [&] { order.push_back('G'); },
                [&] { order.push_back('P'); }, [&] { order.push_back('F'); order.push_back('L'); return true; }) &&
            order == std::vector<char>({'G', 'P', 'F', 'L'}),
            "actual GPU proof precedes proxy release and ordered FG/LL destruction");
    completed = true;
    require(admitPresenterShutdown(completed, inProgress, failed) == ShutdownAdmission::AlreadyCompleted,
            "completed shutdown is idempotent even while its ownership gate remains closed");
    failed = false; order.clear();
    try {
        runPresenterShutdownStages(failed, [&] { order.push_back('G'); throw std::runtime_error("GPU proof timeout"); },
                [&] { order.push_back('P'); }, [&] { order.push_back('F'); return true; });
    } catch (const std::runtime_error&) {}
    require(failed && order == std::vector<char>({'G'}),
            "unproven GPU completion retains proxy and vendor contexts");
    require(!runPresenterShutdownStages(failed, [&] { order.push_back('G'); },
                [&] { order.push_back('P'); }, [&] { order.push_back('F'); return true; }) && order.size() == 1,
            "later completion cannot silently retry a quarantined shutdown");
}
void boundedWindowThreadClose() {
    using xess_vk::presentation::detail::closeWindowThread;
    unsigned requests{}, waits{}, joins{};
    auto request = [&] { ++requests; };
    require(!closeWindowThread(request, [&](std::uint32_t timeout) { ++waits; return timeout != 10'000; },
                [&] { ++joins; }, 10'000) && requests == 1 && waits == 1 && joins == 0,
            "posting window close is not thread completion proof and timeout never joins");
    require(closeWindowThread(request, [&](std::uint32_t timeout) { ++waits; return timeout == 10'000; },
                [&] { ++joins; }, 10'000) && requests == 2 && waits == 2 && joins == 1,
            "join occurs only after observing actual window thread completion");
    require(!closeWindowThread([] { throw std::runtime_error("request failed"); },
                [&](std::uint32_t) { ++waits; return true; }, [&] { ++joins; }, 10'000) && waits == 2 && joins == 1,
            "failed window close retains its owner without an unbounded join");
}
}
int main() {
    using namespace presenter_shutdown_fixture;
    try {
        actualHelperFailuresAndSuccess();
        boundedOperationDrain();
        actualShutdownStagesAndReentry();
        boundedWindowThreadClose();
        std::cout << "{\"stage\":\"presenter-production-shutdown-helper\",\"passed\":true,\"checks\":" << checks
            << ",\"gpuTouched\":false,\"gameTouched\":false,\"sdkLoaded\":false}\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
