#include "measurement_window.hpp"
#include "measurement_log.hpp"
#include "cpu_wall_time.hpp"
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace xess_vk;
using namespace xess_vk::measurement;
unsigned checks{};
void require(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
std::string read(const std::filesystem::path& path){
    std::ifstream input(path,std::ios::binary);
    return {std::istreambuf_iterator<char>(input),{}};
}
void detailAndScope(){
    auto window=std::make_unique<Window>(1);
    auto snapshot=std::make_unique<Snapshot>();
    {Scope scope(window.get());
        observe(Metric::CapWait,15'000'000,7,9);observe(Metric::CapWait,40'000'001,8,9);
        increment(Counter::MenuEnabledFrames);addCommands(CopyRole::Color,Commands{1});
        command(Command::ImageCopy);
        {Scope disabled(nullptr);observe(Metric::CapWait,99);command(Command::ImageCopy);
            increment(Counter::MenuEnabledFrames);addCommands(CopyRole::Color,Commands{99});}
        require(scope.commands()[0]==1,"Nested scope corrupted command identity");
        CpuWallTime wall;{CpuWallScope sample(wall,Metric::PresentCall);}require(wall.count==1,"Existing CPU counter lost");
    }
    require(!active&&!activeCommands,"Thread-local owner retained beyond wrapper");
    require(!window->take(1'000'000'000,*snapshot),"Window emitted early");
    require(window->take(1'000'000'001,*snapshot),"Window failed to emit");
    auto& cap=snapshot->metrics[static_cast<unsigned>(Metric::CapWait)];
    require(cap.count==2&&cap.over15Ms==1&&cap.over40Ms==1&&cap.stored==2,"Threshold or complete sample accounting wrong");
    require(cap.values[1].key==8&&cap.values[1].owner==9,"Sample source identity lost");
    for(unsigned i=0;i<sampleCapacity+3;++i)window->add(Metric::CapWait,i);
    require(window->take(2'000'000'001,*snapshot),"Second window missing");
    require(snapshot->metrics[3].count==sampleCapacity+3&&snapshot->metrics[3].dropped==3,"Overflow hidden");
    std::thread first([&]{Scope scope(window.get());for(unsigned i=0;i<100;++i){
        observe(Metric::PresentCall,i);increment(Counter::SdkDoubleFrames);addCommands(CopyRole::Sdk,Commands{0,0,0,0,0,1});}});
    std::thread second([&]{Scope scope(window.get());for(unsigned i=0;i<100;++i){
        observe(Metric::PresentCall,i);increment(Counter::SdkDoubleFrames);addCommands(CopyRole::Sdk,Commands{0,0,0,0,0,1});}});
    first.join();second.join();require(window->take(3'000'000'001,*snapshot),"Concurrent window missing");
    require(snapshot->metrics[1].count==200&&snapshot->metrics[1].dropped==0,"Concurrent samples lost");
    SummarySnapshot summary;
    require(!window->takeSummary(summaryDurationNs,summary),"Summary emitted before 60 seconds");
    require(window->takeSummary(summaryDurationNs+1,summary),"Independent 60-second summary missing");
    require(summary.sequence==0&&summary.metrics[3].count==sampleCapacity+5,"Detail resets erased summary observations");
    require(summary.metrics[1].count==201,"Concurrent summary samples lost");
    require(summary.counters[static_cast<unsigned>(Counter::MenuEnabledFrames)]==1,"Disabled scope changed summary counters");
    require(summary.counters[static_cast<unsigned>(Counter::SdkDoubleFrames)]==200,"Concurrent summary counters lost");
    window->increment(Counter::SdkTripleFrames);window->increment(Counter::SdkQuadFrames);

    require(summary.copyCommands[static_cast<unsigned>(CopyRole::Color)][0]==1&&
        summary.copyCommands[static_cast<unsigned>(CopyRole::Sdk)][5]==200,"Copy roles or disabled scope mixed command counts");
    require(window->takeSummary(summaryDurationNs+2,summary,true)&&summary.sequence==1&&summary.metrics[3].count==0&&
        summary.counters[static_cast<unsigned>(Counter::SdkTripleFrames)]==1&&summary.counters[static_cast<unsigned>(Counter::SdkQuadFrames)]==1,
        "Forced final summary did not reset or advance independently");
    require(!window->takeSummary(summaryDurationNs+1,summary,true),"Backwards clock emitted a summary");
    window->add(Metric::Count,99);window->increment(Counter::Count);window->addCommands(CopyRole::Count,Commands{99});
    require(window->takeSummary(2*summaryDurationNs+2,summary)&&summary.counters[0]==0,"Invalid enum affected summary state");
}
void fullWindowPercentiles(){
    auto window=std::make_unique<Window>(1);
    auto detail=std::make_unique<Snapshot>();
    std::vector<std::uint64_t> values;
    for(unsigned i=0;i<8192;++i){
        const std::uint64_t value=i<sampleCapacity?1'000'000:10'000'000+(i%1000)*100'003ull;
        values.push_back(value);window->add(Metric::CapWait,value);
        if(i%1000==999)require(window->take(1+(i/1000+1)*1'000'000'000ull,*detail),"Detail cadence changed while accumulating summary");
    }
    SummarySnapshot summary;
    require(window->takeSummary(summaryDurationNs+1,summary),"Full-window summary missing");
    const auto& metric=summary.metrics[static_cast<unsigned>(Metric::CapWait)];
    std::sort(values.begin(),values.end());
    require(metric.count==values.size(),"Summary dropped observations beyond detail capacity");
    std::uint64_t total{};for(auto value:values)total+=value;
    require(metric.totalNs==total&&metric.maximumNs==values.back()&&!metric.totalSaturated,"Full-window total or maximum wrong");
    const std::array<unsigned,5> percentages{10,50,90,95,99};
    const std::array<Percentile,5> percentiles{metric.p10,metric.p50,metric.p90,metric.p95,metric.p99};
    for(std::size_t i=0;i<percentiles.size();++i){
        const auto exact=values[(values.size()*percentages[i]+99)/100-1];const auto p=percentiles[i];
        require(p.lowerNs<=exact&&exact<=p.upperNs,"Histogram percentile does not enclose full sorted nearest rank");
        require(p.upperNs-p.lowerNs<=exact/256,"Histogram exceeded documented precision");
    }
    require(metric.p50.lowerNs>10'000'000,"Summary percentile used only the first 256 detail samples");
    for(const auto [threshold,count]:std::array<std::pair<std::uint64_t,std::uint64_t>,4>{{
        {15'000'000,metric.over15Ms},{25'000'000,metric.over25Ms},{40'000'000,metric.over40Ms},{100'000'000,metric.over100Ms}}})
        require(count==static_cast<std::uint64_t>(std::count_if(values.begin(),values.end(),[&](auto value){return value>threshold;})),
            "Full-window threshold count wrong");
    auto histogram=std::make_unique<Histogram>();
    for(auto value:std::array<std::uint64_t,8>{15'000'000,15'000'001,25'000'000,25'000'001,40'000'000,40'000'001,100'000'000,100'000'001})histogram->add(value);
    const auto thresholds=histogram->summary();
    require(thresholds.over15Ms==7&&thresholds.over25Ms==5&&thresholds.over40Ms==3&&thresholds.over100Ms==1,
        "Summary threshold comparisons are not strictly greater than");
    const auto one=[&](std::uint64_t value){
        histogram->clear();histogram->add(value);const auto result=histogram->summary();
        for(auto p:std::array{result.p10,result.p50,result.p90,result.p95,result.p99})
            require(p.lowerNs<=value&&value<=p.upperNs,"Histogram power-of-two boundary out of range");
    };
    one(0);for(unsigned bit=0;bit<64;++bit){one(std::uint64_t{1}<<bit);one((std::uint64_t{1}<<bit)-1);}
    one(UINT64_MAX);histogram->add(1);
    require(histogram->summary().totalSaturated&&histogram->summary().totalNs==UINT64_MAX,"Summary sum overflow wrapped silently");
}
void independentRotation(const std::filesystem::path& folder){
    const auto prefix=(folder/L"rotation-observer.jsonl").wstring();
    {std::ofstream verbose{std::filesystem::path(prefix)};verbose<<"full verbose log";}
    {Log detail(8);SummaryLog summary(8);Log observation(8,LogStream::Observation);
        require(detail.write(prefix.c_str(),"123456\n",7),"Independent detail writer failed");
        require(summary.write(prefix.c_str(),"first!\n",7),"Independent summary writer failed");
        require(observation.write(prefix.c_str(),"sr-one\n",7),"Independent SR observation writer failed");
        require(detail.write(prefix.c_str(),"abcdef\n",7),"First detail rotation failed");
        require(summary.write(prefix.c_str(),"second\n",7),"First summary rotation failed");
        require(observation.write(prefix.c_str(),"sr-two\n",7),"First SR observation rotation failed");
        require(detail.write(prefix.c_str(),"987654\n",7),"Second detail rotation failed");
        require(summary.write(prefix.c_str(),"third!\n",7),"Second summary rotation failed");
        require(observation.write(prefix.c_str(),"sr-end\n",7),"Second SR observation rotation failed");
        require(!detail.write(prefix.c_str(),"oversized",9)&&detail.failures()==1,"Oversized detail write not reported");
        require(summary.failures()==0&&summary.rotations()==2&&summary.writtenBytes()==21,"Detail failure affected summary budget");
        require(observation.failures()==0&&observation.rotations()==2&&observation.writtenBytes()==21,"Other log budgets do not stop SR observations");
    }
    require(read(prefix+L".measurements-0.jsonl")=="987654\n","Recent detail file not bounded/retained");
    require(read(prefix+L".measurements-1.jsonl")=="abcdef\n","Previous detail rotation lost");
    require(read(prefix+L".measurements-summary-0.jsonl")=="third!\n","Recent summary file not bounded/retained");
    require(read(prefix+L".measurements-summary-1.jsonl")=="second\n","Previous summary rotation lost");
    require(read(prefix+L".sr-observation-0.jsonl")=="sr-end\n"&&read(prefix+L".sr-observation-1.jsonl")=="sr-two\n","SR runtime and timing stream retains two bounded recent files");
    require(read(prefix)=="full verbose log","Statistics altered original log");
}
void longDetailFlood(const std::filesystem::path& folder){
    const auto prefix=(folder/L"long-observer.jsonl").wstring();
    auto window=std::make_unique<Window>(1);auto detail=std::make_unique<Snapshot>();
    std::vector<std::string> expected;
    std::string detailLine="{\"event\":\"detail_fixture\",\"padding\":\"";
    detailLine.append(4093-detailLine.size(),'x');detailLine+="\"}\n";
    require(detailLine.size()==4096,"Detail flood line size wrong");
    std::uint64_t detailBytes{},detailRotations{};
    {Log detailed;SummaryLog summaries;
        for(unsigned second=1;second<=360;++second){
            window->add(Metric::CapWait,(second%100)*1'000'000ull);
            window->increment(second%2?Counter::MenuEnabledFrames:Counter::MenuDisabledFrames);
            window->increment(second%3?Counter::SdkSingleFrames:Counter::SdkDoubleFrames);
            window->addCommands(CopyRole::Color,Commands{1});
            if(second>240){window->add(Metric::SrGpu,3'000'000+second);window->increment(Counter::SrExecutes);}
            for(unsigned line=0;line<32;++line)
                require(detailed.write(prefix.c_str(),detailLine.data(),static_cast<DWORD>(detailLine.size())),"Long detail write failed");
            if(second==180)require(!detailed.write(prefix.c_str(),"x",16*1024*1024+1),"Detail failure injection was not rejected");
            const auto now=1+second*1'000'000'000ull;
            require(window->take(now,*detail)&&detail->sequence==second-1,"One-second detail sequence changed");
            SummarySnapshot summary;
            const bool emitted=window->takeSummary(now,summary);
            require(emitted==(second%60==0),"Summary cadence was coupled to detail or omitted a window");
            if(!emitted)continue;
            require(summary.sequence==expected.size()&&summary.beginNs==1+(second-60)*1'000'000'000ull&&summary.endNs==now,
                "Summary sequence or time coverage has a gap");
            require(summary.metrics[static_cast<unsigned>(Metric::CapWait)].count==60,"Minute did not retain all one-second-reset observations");
            require(summary.counters[static_cast<unsigned>(Counter::MenuEnabledFrames)]==30&&
                summary.counters[static_cast<unsigned>(Counter::SdkDoubleFrames)]==20&&summary.copyCommands[0][0]==60,
                "Window counters or actual command totals lost across detail resets");
            require(summary.metrics[static_cast<unsigned>(Metric::SrGpu)].count==(second>240?60u:0u),"SR metrics contaminated inactive windows");
            char line[512]{};
            const auto bytes=std::snprintf(line,sizeof(line),"{\"event\":\"measurement_summary\",\"window\":%llu,\"begin_ns\":%llu,\"end_ns\":%llu,\"cap_count\":%llu,\"cap_p95_lower_ns\":%llu,\"cap_p95_upper_ns\":%llu}\n",
                static_cast<unsigned long long>(summary.sequence),static_cast<unsigned long long>(summary.beginNs),static_cast<unsigned long long>(summary.endNs),
                static_cast<unsigned long long>(summary.metrics[3].count),static_cast<unsigned long long>(summary.metrics[3].p95.lowerNs),
                static_cast<unsigned long long>(summary.metrics[3].p95.upperNs));
            require(bytes>0&&bytes<static_cast<int>(sizeof(line))&&summaries.write(prefix.c_str(),line,static_cast<DWORD>(bytes)),"Long summary write failed");
            expected.emplace_back(line,static_cast<std::size_t>(bytes));
        }
        detailBytes=detailed.writtenBytes();detailRotations=detailed.rotations();
        require(detailBytes>32ull*1024*1024&&detailRotations>=2&&detailed.failures()==1,"Fixture did not cross the exhausted-detail condition");
        require(summaries.failures()==0&&summaries.rotations()==0,"Summary budget was exhausted by detail");
    }
    require(expected.size()==6,"Long fixture did not generate six full minute summaries");
    std::string all;for(const auto& line:expected)all+=line;
    require(read(prefix+L".measurements-summary-0.jsonl")==all,"Persisted summary windows are discontinuous after 32 MiB detail");
    for(unsigned i=0;i<2;++i)
        require(std::filesystem::file_size(prefix+L".measurements-"+std::to_wstring(i)+L".jsonl")<=16ull*1024*1024,"Detail rotation exceeded its disk bound");
    std::ofstream record(folder/L"long-detail-verification.json");
    record<<"{\"detail_bytes_written\":"<<detailBytes<<",\"detail_rotations\":"<<detailRotations
        <<",\"injected_detail_failures\":1,\"summary_failures\":0,\"summary_windows\":6,\"first_sequence\":0,\"last_sequence\":5,\"simulated_seconds\":360,\"gpu_calls\":0}\n";
    require(static_cast<bool>(record),"Fixture verification record could not be saved");
    std::printf("detail bytes=%llu, rotations=%llu; persisted 60-second summaries=6, sequence=0..5, gaps=0\n",
        static_cast<unsigned long long>(detailBytes),static_cast<unsigned long long>(detailRotations));
}
int main(int argc,char** argv){try{
    detailAndScope();fullWindowPercentiles();
    const bool preserve=argc==2;
    const auto folder=preserve?std::filesystem::absolute(argv[1]):std::filesystem::temp_directory_path()/
        (L"xess-measurements-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    require(std::filesystem::create_directory(folder),"Test directory collision");
    independentRotation(folder);longDetailFlood(folder);
    if(!preserve){for(const auto& entry:std::filesystem::directory_iterator(folder))std::filesystem::remove(entry.path());std::filesystem::remove(folder);}
    std::printf("measurement window: %u checks passed; detail compatibility, whole-window percentile bounds, thresholds, counters, roles, TLS, concurrency and independent rotation; GPU calls=0\n",checks);
    return 0;
}catch(const std::exception& e){std::fprintf(stderr,"measurement test: %s\n",e.what());return 1;}}
