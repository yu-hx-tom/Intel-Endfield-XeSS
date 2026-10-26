#pragma once
#include <windows.h>
#include <d3d12.h>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace xess_vk::detail {
class FenceWaitTimeout final : public std::runtime_error {
public:
    explicit FenceWaitTimeout(const std::string& message) : std::runtime_error(message) {}
};

// An event can contain a late SetEventOnCompletion notification from another
// fence/target. Only this fence's completed value proves completion. The event
// merely wakes the polling loop, including when reused after an earlier timeout.
inline void waitForD3D12Fence(ID3D12Fence* fence, std::uint64_t target, HANDLE event,
                             std::uint32_t timeoutMs, const char* description) {
    const auto isComplete = [&] {
        const auto completed = fence->GetCompletedValue();
        if (completed == UINT64_MAX) throw std::runtime_error(std::string(description) + ": D3D12 device removed");
        return completed >= target;
    };
    if (isComplete()) return;
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    const auto hr = fence->SetEventOnCompletion(target, event);
    if (FAILED(hr)) throw std::runtime_error(std::string(description) + ": SetEventOnCompletion failed, HRESULT " + std::to_string(hr));
    for (;;) {
        if (isComplete()) return;
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) {
            // Include completion racing with the final deadline check.
            if (isComplete()) return;
            throw FenceWaitTimeout(std::string(description) + " timed out before requested fence value completed");
        }
        const DWORD result = WaitForSingleObject(event, static_cast<DWORD>(deadline - now));
        if (result != WAIT_OBJECT_0 && result != WAIT_TIMEOUT) {
            if (isComplete()) return;
            throw std::runtime_error(std::string(description) + ": event wait failed, result " + std::to_string(result));
        }
        // Both wakeup and timeout recheck the actual target in the next iteration.
    }
}
} // namespace xess_vk::detail
