#include "release_logging.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <vector>
using namespace xess_vk::release_log;
int main(int argc,char** argv){try{
    if(argc==4&&View(argv[1])=="--replay"){
        const std::filesystem::path input=argv[2],output=argv[3];std::filesystem::create_directories(output);
        Policy policy;std::array<std::ofstream,4> sinks;std::array<std::uint64_t,4> bytes{};unsigned rows{},kept{},statusRows{};std::size_t maxStatus{};
        std::map<std::string,unsigned> before,after;std::vector<std::filesystem::path> files;
        for(const auto& f:std::filesystem::directory_iterator(input))if(f.is_regular_file()&&f.path().filename().string().starts_with("observer.jsonl"))files.push_back(f.path());
        std::sort(files.begin(),files.end());
        for(const auto& path:files){const auto name=path.filename().string();unsigned original=name.find(".sr-observation-")!=name.npos?3:name.find(".measurements-summary-")!=name.npos?2:name.find(".measurements-")!=name.npos?1:0;
            std::ifstream src(path,std::ios::binary);std::string line;
            while(std::getline(src,line)){line+='\n';const auto event=std::string(text(line,"event"));if(event.empty())continue;++rows;++before[event];unsigned stream=original;if(!policy.accept(stream,line))continue;
                std::array<char,32768> compact{};View record=line;
                if(stream==1){const auto n=projectStatus(line,compact.data(),compact.size());if(!n)throw std::runtime_error("projection failed");record={compact.data(),n};maxStatus=std::max(maxStatus,n);++statusRows;}
                if(!sinks[stream].is_open()){constexpr const char* names[]{"observer.jsonl","measurements.jsonl","summary.jsonl","observation.jsonl"};sinks[stream].open(output/names[stream],std::ios::binary);}
                sinks[stream].write(record.data(),record.size());bytes[stream]+=record.size();++kept;++after[event];
                if(stream==1&&policy.stateChanged(record)){
                    std::array<char,4096> changed{};const auto length=projectStatus(record,changed.data(),changed.size(),true);
                    if(length){if(!sinks[0].is_open())sinks[0].open(output/"observer.jsonl",std::ios::binary);sinks[0].write(changed.data(),length);bytes[0]+=length;++after["runtime_state_changed"];}
                }
            }
        }
        std::ofstream events(output/"events.tsv");events<<"event\tbefore\tafter\n";for(const auto& [name,n]:before)events<<name<<'\t'<<n<<'\t'<<after[name]<<'\n';
        std::cout<<"{\"source_rows\":"<<rows<<",\"kept_rows\":"<<kept<<",\"status_rows\":"<<statusRows<<",\"max_status_bytes\":"<<maxStatus<<",\"main_bytes\":"<<bytes[0]<<",\"status_bytes\":"<<bytes[1]<<",\"summary_bytes\":"<<bytes[2]<<",\"observation_bytes\":"<<bytes[3]<<",\"state_changes_reconstructed\":true}\n";return 0;
    }
    unsigned checks{};auto check=[&](bool v){++checks;if(!v)throw std::runtime_error("release logging regression");};
    for(auto e:{"descriptor_image_binding","descriptor_pre_dispatch","upscale_image_metadata_match","continuous_registration_frame","upscale_source_snapshot","sr_dispatch_candidate","continuous_present"}){
        std::string row="{\"event\":\""+std::string(e)+"\",\"result\":0}";check(rule(row)==Rule::Off);
    }
    Policy p;unsigned stream=3;check(!p.accept(stream,R"({"event":"sr_callback_snapshot"})"));
    const View a=R"({"event":"sr_native_job_observer_install_summary","device":"d","target_rva":1,"state":"installed","attempt":1})";
    stream=3;check(p.accept(stream,a)&&stream==0);stream=3;check(!p.accept(stream,a));
    stream=3;check(p.accept(stream,R"({"event":"sr_native_job_observer_install_summary","device":"d","target_rva":1,"state":"refused","attempt":2})"));
    stream=0;check(p.accept(stream,R"({"event":"create_device","result":0})"));check(p.accept(stream,R"({"event":"present_failed","result":-4})"));
    const View refusal=R"({"event":"sr_native_replacement_fault","origin":"compute_dispatch","fault":1,"image":"0x1234","pipeline":"0x5678","producer_profile":4,"content_omitted":true})";
    stream=0;check(p.accept(stream,refusal)&&stream==0);
    check(text(refusal,"origin")=="compute_dispatch"&&text(refusal,"image")=="0x1234"&&value(refusal,"producer_profile")=="4");
    std::array<char,2048> output{};const auto n=projectStatus(R"({"event":"measurement_status","qpc_ns":42,"sr_state":"xess_replacement","sdk_frames":3,"status_interval_attempts":50,"provider_depth_gpu":{"requested":false},"sr_guard_draw_checks":999,"unknown":"escaped \"value\""})",output.data(),output.size());
    check(n>0);View compact{output.data(),n};check(value(compact,"status_interval_attempts")=="50");check(value(compact,"provider_depth_gpu").empty());check(value(compact,"sr_guard_draw_checks").empty());check(text(compact,"sr_state")=="xess_replacement");
    check(p.stateChanged(compact));check(!p.stateChanged(compact));check(!projectStatus("{   ",output.data(),output.size()));check(!projectStatus(compact,output.data(),1));
    stream=3;std::string large="{\"event\":\"sr_sdk_failed\",\"error\":\""+std::string(33000,'a')+"\"}";check(!p.accept(stream,large));
    {Policy evidence;unsigned s=0;check(rule(R"({"event":"sr_native_fallback_presenting","transaction_stopped":true})")==Rule::Evidence);
        check(evidence.accept(s,R"({"event":"sr_native_fallback_presenting","presents":9})")&&s==0);
        s=3;check(evidence.accept(s,R"({"event":"sr_watched_image_use","binding":4})")&&s==0);
        const std::string big="{\"event\":\"sr_prepare_outputs\",\"x\":\""+std::string(200000,'a')+"\"}";
        s=0;check(evidence.accept(s,big));s=0;check(!evidence.accept(s,big));}
    std::cout<<"{\"passed\":true,\"checks\":"<<checks<<"}\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
