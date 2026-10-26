#pragma once
#include <atomic>
#include <cstdint>
namespace xess_vk::sr_native_resolution {
enum class Reason:unsigned {None,InputUnavailable,SdkFailure,UnsupportedExtent,RuntimeUnavailable,ConsumerUnproven};
inline const char* name(Reason r) noexcept {
    switch(r){case Reason::None:return "none";case Reason::InputUnavailable:return "input_unavailable";
    case Reason::SdkFailure:return "sdk_failure";case Reason::UnsupportedExtent:return "unsupported_extent";
    case Reason::ConsumerUnproven:return "consumer_unproven";default:return "runtime_unavailable";}
}
// Rendering threads request only. Engine object changes happen in the pinned
// HGCamera.UpdateRenderingScale callback on the engine's own update thread.
struct Control {
    std::atomic<bool> installed{};
    std::atomic<Reason> reason{Reason::None};
    std::atomic<std::uint64_t> applied{},targetExtent{},configuredExtent{};
    std::atomic<unsigned> thread{};
    std::atomic<std::uint64_t> recoveries{},failures{},reentries{},foreignThreads{};
    // Only the recording boundary clears a request, after observed owner/content retirement.
    void recoverAfterRetirement() noexcept {if(reason.exchange(Reason::None)!=Reason::None)++recoveries;}
    void request(Reason r) noexcept {auto expected=Reason::None;if(r!=Reason::None)reason.compare_exchange_strong(expected,r);}
    bool requested()const noexcept{return reason.load()!=Reason::None;}
};
inline Control control;
inline bool validExtent(std::uint64_t p) noexcept {
    const auto w=std::uint32_t(p),h=std::uint32_t(p>>32);return w&&h&&w<=32768&&h<=32768;
}
}
