#pragma once
#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <mutex>
#include <string_view>

namespace xess_vk::release_log {
using View=std::string_view;
// Bounded top-level JSON token scan; preserves original values, including
// nested objects and escaped strings. All producers emit complete JSON lines.
template<class F> bool fields(View s,F field) noexcept {
    std::size_t i=s.find('{');if(i==s.npos)return false;++i;
    const auto ws=[&]{while(i<s.size()&&(s[i]==' '||s[i]=='\r'||s[i]=='\n'||s[i]=='\t'))++i;};
    while(i<s.size()){
        ws();if(i>=s.size())return false;if(s[i]=='}')return true;if(s[i]==','){++i;ws();}
        if(i>=s.size()||s[i++]!='"')return false;const auto begin=i;
        while(i<s.size()&&s[i]!='"'){if(s[i]=='\\')++i;++i;}if(i>=s.size())return false;
        const auto key=s.substr(begin,i-begin);++i;ws();if(i>=s.size()||s[i++]!=':')return false;ws();
        const auto valueBegin=i;int nesting{};bool quoted{},escape{};
        for(;i<s.size();++i){const char c=s[i];
            if(quoted){if(escape)escape=false;else if(c=='\\')escape=true;else if(c=='"')quoted=false;continue;}
            if(c=='"')quoted=true;else if(c=='{'||c=='[')++nesting;
            else if(c==']'||c=='}'){if(!nesting)break;--nesting;}
            else if(c==','&&!nesting)break;
        }
        if(i>=s.size()||quoted||nesting)return false;
        auto v=s.substr(valueBegin,i-valueBegin);while(!v.empty()&&(v.back()==' '||v.back()=='\r'||v.back()=='\n'))v.remove_suffix(1);
        field(key,v);
    }return false;
}
inline View value(View s,View name) noexcept {View out;fields(s,[&](View k,View v){if(k==name)out=v;});return out;}
inline View text(View s,View name) noexcept {auto v=value(s,name);return v.size()>=2&&v.front()=='"'&&v.back()=='"'?v.substr(1,v.size()-2):View{};}
inline bool keepStatus(View k) noexcept {
    if(k.starts_with("fg_"))return k!="fg_settings_policy"&&k!="fg_multiplier_change_policy"&&k!="fg_threshold_attempted"&&k!="fg_debug_attempted"&&k!="fg_debug_applied"&&k!="fg_debug_result";
    if(k.starts_with("status_")||k.starts_with("game_frame_cap_"))return true;
    if(k.starts_with("sr_"))return !k.starts_with("sr_guard_")&&!k.starts_with("sr_hot_path_")&&k!="sr_tail_diagnostics_enabled";
    constexpr View names[]{"event","pid","uptime_ms","window","qpc_ns","build_flavor","release_status_schema","writer_failures_total",
        "async_log_active","async_log_pending","async_log_sink_failures","async_log_start_failures","async_log_dropped_verbose","async_log_dropped_detail","async_log_dropped_summary","async_log_dropped_observation",
        "attempt","engine_frame","sdk_id","sdk_query_result","sdk_framegen_result","sdk_frames","output_fresh","sdk_result_known","control_epoch","inflight_limit","async",
        "shared_copy_path","shared_color_direct","shared_auxiliary_direct","depth_snapshot_compute","shared_fallback_reason","copy_route",
        "menu_paired","menu_requested_enabled","menu_gates_consistent","input_matched","input_validity","history_reset_known","history_reset_reasons",
        "input_captured","input_consumed","copy_failures","submission_interval_resets"};
    return std::find(std::begin(names),std::end(names),k)!=std::end(names);
}
inline constexpr View stateKeys[]{"sr_runtime_connected","sr_state","sr_output_mode","sr_active_reason","sr_reason_category","sr_recovery_blocked","sr_fault","sr_output_held","sr_currently_stopped","sr_engine_native_request","sr_successful_recoveries","sr_last_issue_reason","sr_fallback_state","menu_requested_enabled","sdk_frames","fg_multiplier_actual"};
inline std::size_t projectStatus(View input,char* output,std::size_t capacity,bool stateOnly=false) noexcept {
    std::size_t used{};bool good=true,first=true;
    const auto append=[&](View v){if(v.size()>capacity-used){good=false;return;}std::memcpy(output+used,v.data(),v.size());used+=v.size();};
    if(stateOnly){append("{\"event\":\"runtime_state_changed\"");first=false;}else append("{");
    const bool valid=fields(input,[&](View k,View v){
        if(stateOnly){if(k!="pid"&&k!="uptime_ms"&&k!="qpc_ns"&&std::find(std::begin(stateKeys),std::end(stateKeys),k)==std::end(stateKeys))return;}
        else if(!keepStatus(k))return;
        if(!first)append(",");first=false;append("\"");append(k);append("\":");append(v);
    });append("}\n");
    return good&&valid?used:0;
}
enum class Rule { Off, Keep, Change, Fault, Evidence };
inline Rule rule(View line) noexcept {
    const auto e=text(line,"event");
    if(e=="measurement_status"||e=="measurement_summary"||e=="measurement_device_destroy"||e=="diagnostic_writer_drained"||e=="runtime_state_changed")return Rule::Keep;
    if(e.ends_with("_install")||e.ends_with("_install_summary")||e.ends_with("_plan")||e.ends_with("_admission"))return Rule::Change;
    constexpr View life[]{"observer_loaded","loader_negotiate","create_instance","destroy_instance","create_device","destroy_device","create_swapchain","destroy_swapchain","create_win32_surface","destroy_surface","physical_device_properties","get_device_queue","presentation_off_create_swapchain","presentation_off_hidden_surface"};
    if(std::find(std::begin(life),std::end(life),e)!=std::end(life))return Rule::Keep;
    // Probe rejections are diagnostic evidence, not product failures.
    if(e.starts_with("descriptor_")||e.starts_with("upscale_")||e.starts_with("rp_")||e.starts_with("tracking_")||e.starts_with("continuous_window")||e.starts_with("continuous_registration")||e.starts_with("sr_dispatch")||e.starts_with("sr_callback")||e.starts_with("sr_raw_")||e.starts_with("sr_following"))return Rule::Off;
    const auto result=value(line,"result"),error=text(line,"error"),state=text(line,"state");
    const auto status=text(line,"status"),fault=value(line,"fault");
    // Targeted SR evidence: diagnostic mode only, quota-bounded by its producer and here.
    if(e=="sr_backend_runtime"||e=="sr_menu_mask_overwrite"||e=="sr_watched_image_use"||e=="sr_prepare_outputs"||e=="sr_photo_overwrite"||e=="sr_auxiliary_overwrite"||
       e=="sr_native_replacement_consumer"||e.starts_with("sr_native_fallback_"))return Rule::Evidence;
    if(e.find("error")!=e.npos||e.find("failed")!=e.npos||e.find("fatal")!=e.npos||e.ends_with("_too_large")||(!result.empty()&&result.front()=='-')||(!error.empty()&&error!="none")||state.starts_with("refused")||status.find("fail")!=status.npos||value(line,"stopped")=="true"||(!fault.empty()&&fault!="0"))return Rule::Fault;
    return Rule::Off;
}
inline std::uint64_t hash(View v,std::uint64_t h=1469598103934665603ULL) noexcept {for(unsigned char c:v){h^=c;h*=1099511628211ULL;}return h;}
class Policy {
    struct Stamp {std::uint64_t key{},state{};bool live{};};
    std::array<Stamp,128> stamps_{};unsigned next_{};std::mutex mutex_;
    unsigned srFaultBytes_{},srEvidenceBytes_{};
    bool stateKnown_{};std::uint64_t stateHash_{};
public:
    bool stateChanged(View line) noexcept {
        std::lock_guard lock(mutex_);auto h=1469598103934665603ULL;
        for(auto key:stateKeys){h=hash(key,h);h=hash(value(line,key),h);}
        if(stateKnown_&&stateHash_==h)return false;stateKnown_=true;stateHash_=h;return true;
    }
    bool accept(unsigned& stream,View line) noexcept {
        const auto r=rule(line);if(r==Rule::Off)return false;
        if(stream==1&&text(line,"event")!="measurement_status"){if(r==Rule::Fault)stream=0;else return false;}
        try{
            std::lock_guard lock(mutex_);
            if(r==Rule::Evidence){
                // Main log only. At most 256 KiB of targeted SR evidence per session.
                if(line.size()>262144-srEvidenceBytes_)return false;srEvidenceBytes_+=unsigned(line.size());stream=0;return true;
            }
            if(stream==3){
                if(r!=Rule::Change&&r!=Rule::Fault)return false;
                // No observation file. At most 32 KiB of SR fault snapshots per session.
                if(r==Rule::Fault){if(line.size()>32768-srFaultBytes_)return false;srFaultBytes_+=unsigned(line.size());}
                stream=0;
            }
            if(r!=Rule::Change)return true;
            auto key=hash(text(line,"event"));key=hash(value(line,"device"),key);key=hash(value(line,"target_rva"),key);
            std::uint64_t state=1469598103934665603ULL;
            constexpr View keys[]{"state","stage","status","requested","installed","active","hook_created","enable_succeeded","sampling_allowed","stop_requested","forwarder_published","transaction_stage","transaction_failure","hook_bytes_still_active","target_protection_still_restored","result","error","reason"};
            for(auto k:keys){state=hash(k,state);state=hash(value(line,k),state);}
            for(auto& s:stamps_)if(s.live&&s.key==key){if(s.state==state)return false;s.state=state;return true;}
            stamps_[next_]={key,state,true};next_=(next_+1)%stamps_.size();return true;
        }catch(...){return r==Rule::Fault;}
    }
};
} // namespace xess_vk::release_log
