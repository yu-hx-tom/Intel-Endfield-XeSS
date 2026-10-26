#pragma once
#include <cstdint>
namespace xess_vk::presentation::detail {
// A close request is not thread-completion proof. The owner retains the whole
// captured-this object on timeout/error, and joins only a completed thread.
template<class RequestClose, class WaitComplete, class JoinComplete>
bool closeWindowThread(RequestClose &&request, WaitComplete &&wait, JoinComplete &&join,
                       std::uint32_t timeoutMs) noexcept {
    try {
        request();
        if (!wait(timeoutMs)) return false;
        join();
        return true;
    } catch (...) { return false; }
}
}
