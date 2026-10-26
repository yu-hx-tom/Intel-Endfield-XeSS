#pragma once
#include <string_view>

namespace xess_vk::sr_status {
inline const char* category(std::string_view reason) noexcept {
    if(reason=="none")return "none";
    if(reason=="initializing")return "initialization";
    if(reason=="recovery_pending_output")return "recovery";
    if(reason.find("sdk")!=reason.npos||reason=="overlay_record")return "sdk";
    if(reason.find("submission")!=reason.npos)return "submission";
    if(reason.find("consumer")!=reason.npos||reason.find("bindings_unknown")!=reason.npos||reason=="execute_secondary_commands")return "consumer";
    if(reason.find("allocation")!=reason.npos||reason.find("untracked")!=reason.npos)return "tracking";
    if(reason.find("shader")!=reason.npos||reason.find("native_aa")!=reason.npos||reason.find("extent")!=reason.npos)return "configuration";
    if(reason.find("recording")!=reason.npos||reason.find("restore")!=reason.npos||reason.find("push_constant")!=reason.npos)return "recording";
    if(reason.find("input")!=reason.npos||reason.find("source")!=reason.npos||reason.find("color")!=reason.npos||reason=="frame_constants")return "input";
    return "unknown";
}
struct Recovery {
    bool trackingLost{},pending{},owners{},writes{},open{},missing{},foreignThread{},sdkRestartFailed{};
    const char* blocked()const noexcept {
        if(trackingLost)return "tracking_lost";
        if(pending)return "pending_sdk_recordings";
        if(owners)return "recording_owners_alive";
        if(writes)return "pending_write_receipts";
        if(open)return "transaction_open";
        if(missing)return "omitted_contents_alive";
        if(foreignThread)return "sdk_owner_thread";
        if(sdkRestartFailed)return "sdk_restart_failed";
        return "none";
    }
};
}
