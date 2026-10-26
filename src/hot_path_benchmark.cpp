// Per-command observer lookup microbenchmark. Not registered with ctest:
// timings are machine dependent. Each loop also asserts the guard result so a
// faster path that changes behaviour cannot report a number.
// Build with XESS_HOT_PATH_BASELINE against the previous sources to measure the
// old implementation with the same workloads (only shared APIs are used then).
#define main descriptor_layer_original_main
#include "descriptor_image_layer_test.cpp"
#undef main
#include <chrono>

namespace hot_path_benchmark {
using namespace descriptor_layer_fixture;
namespace ro=xess_vk::render_pass_observer;
using Clock=std::chrono::steady_clock;
#if defined(XESS_HOT_PATH_BASELINE)
constexpr const char* implementation="baseline";
#elif defined(XESS_RP_FACTS_HOT_STATE)
constexpr const char* implementation="hot_state";
#else
constexpr const char* implementation="hints";
#endif
volatile std::uint64_t consumed{}; // written by the main thread only
template<class F> double nsPerOp(std::size_t ops,F&& run){
    const auto start=Clock::now();run();
    return std::chrono::duration<double,std::nano>(Clock::now()-start).count()/double(ops);
}
struct Commands {
    ro::Context* c{};std::vector<VkCommandBuffer> buffers;VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    Commands(std::size_t count,std::uint64_t base){
        c=ro::create(device,+[](const char*){});require(c!=nullptr,"render-pass context created");
        const auto pool=handle<VkCommandPool>(0x7000);VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        ro::poolCreated(c,pool,&pi,VK_SUCCESS);buffers.resize(count);
        for(std::size_t n=0;n<count;++n)buffers[n]=handle<VkCommandBuffer>(base+0x40*n);
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool=pool;ai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ai.commandBufferCount=std::uint32_t(count);
        ro::allocate(c,&ai,buffers.data(),VK_SUCCESS);
        begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;for(const auto b:buffers)ro::begin(c,b,&begin,VK_SUCCESS);
    }
    ~Commands(){ro::destroy(c);}
};
void recordingFacts(){
    for(const std::size_t live:{std::size_t{1},std::size_t{16},std::size_t{64},std::size_t{128}}){
        Commands set(live,0x710000);const auto last=set.buffers.back();const auto expected=ro::recordingFacts(set.c,last);
        require(expected.recording&&expected.stateKnown&&expected.command==bits(last),"benchmark command is recording");
        constexpr std::size_t ops=2'000'000;std::uint64_t sum{};
        const auto ns=nsPerOp(ops,[&]{for(std::size_t n=0;n<ops;++n)sum+=ro::recordingFacts(set.c,last).rpRecordingGeneration;});consumed=sum;
        const auto after=ro::recordingFacts(set.c,last);
        require(after.rpRecordingGeneration==expected.rpRecordingGeneration&&after.commonRecordOrder==expected.commonRecordOrder&&after.recording,
            "repeated queries return the same recording facts");
        std::cout<<"{\"bench\":\"rp_recording_facts\",\"implementation\":\""<<implementation<<"\",\"live_command_buffers\":"<<live<<",\"ns_per_call\":"<<ns<<"}\n";
    }
}
// Recording workload with the observer's write side included: each thread owns
// one command and repeats begin, then `points` x (one recording point followed
// by `queries` facts queries), then end. Unity worker threads record this way;
// the per-point query count approximates bind+draw pairs between barriers.
// Reported time is per query, so write-side publication cost is included.
// `work` busy iterations stand in for the driver call and other hooks around
// each observed command; 0 is the worst-case back-to-back contention.
std::uint64_t driverWork(std::uint64_t iterations,std::uint64_t seed) noexcept {
    for(std::uint64_t n=0;n<iterations;++n)seed=seed*6364136223846793005ull+1442695040888963407ull;
    return seed;
}
void recordingWorkload(){
    constexpr std::size_t points=32,cycles=4000;
    struct Case {std::size_t threads,queries;std::uint64_t work;};
    for(const auto [threads,queries,work]:{Case{1,0,0},Case{1,4,0},Case{1,16,0},Case{4,0,0},Case{4,4,0},Case{4,16,0},Case{1,16,150},Case{4,16,150},Case{4,4,150}}){
        Commands set(threads,0x720000);
        std::atomic<bool> go{false};std::atomic<std::uint64_t> wrong{};std::vector<std::thread> workers;
        std::vector<std::uint64_t> sums(threads); // one slot per thread, summed after join
        for(std::size_t t=0;t<threads;++t)workers.emplace_back([&,t]{
            while(!go.load(std::memory_order_acquire)){}
            const auto b=set.buffers[t];std::uint64_t sum{};
            for(std::size_t cycle=0;cycle<cycles;++cycle){
                ro::begin(set.c,b,&set.begin,VK_SUCCESS);
                for(std::uint64_t point=1;point<=points;++point){ro::explicitBarrier(set.c,b);sum+=driverWork(work,point);
                    for(std::size_t q=0;q<queries;++q){const auto f=ro::recordingFacts(set.c,b);sum+=driverWork(work,q);
                        if(f.command!=bits(b)||f.commonRecordOrder!=point||!f.recording)wrong.fetch_add(1,std::memory_order_relaxed);
                        sum+=f.rpRecordingGeneration;}}
                ro::end(set.c,b,VK_SUCCESS);
            }
            sums[t]=sum;
        });
        const auto start=Clock::now();go.store(true,std::memory_order_release);for(auto& w:workers)w.join();
        const auto elapsed=std::chrono::duration<double,std::nano>(Clock::now()-start).count();
        std::uint64_t total{};for(const auto s:sums)total+=s;consumed=total;
        require(wrong==0,"every query returns its own command's current order");
        const auto perThreadQueries=double(cycles*points*(queries?queries:1));
        std::cout<<"{\"bench\":\"rp_recording_workload\",\"implementation\":\""<<implementation<<"\",\"threads\":"<<threads
                 <<",\"queries_per_recording_point\":"<<queries<<",\"driver_work_iterations\":"<<work
                 <<",\"wall_ns_per_"<<(queries?"query":"recording_point")<<"\":"<<elapsed/perThreadQueries<<"}\n";
    }
}
void replacementCommand(){
    namespace stc=xess_vk::sr_tail_commands;
    for(const unsigned live:{1u,16u,48u}){
        Fixture f;const auto r=setup(f);GameSrRuntime runtime(&f.state,true);
        runtime.nativeTransaction.produce({0xF0F0,9},true,2);
        const auto bind=stc::bindDescriptorSets(VK_PIPELINE_BIND_POINT_GRAPHICS,r.pipelineLayout,0,1,&r.set,0,nullptr);
        const auto draw=stc::simple(stc::Kind::Draw);
        xess_vk::CommandRecordingFacts facts;facts.rpRecordingGeneration=1;
        for(unsigned n=0;n<live;++n){facts.command=0xE00000+0x40*n;runtime.replacementCommand(facts,bind);runtime.replacementCommand(facts,draw);}
        constexpr std::size_t ops=500'000;std::uint64_t sum{};
        const auto drawNs=nsPerOp(ops,[&]{for(std::size_t n=0;n<ops;++n)sum+=runtime.replacementCommand(facts,draw);});
        const auto bindNs=nsPerOp(ops,[&]{for(std::size_t n=0;n<ops;++n)sum+=runtime.replacementCommand(facts,bind);});consumed=sum;
        require(!runtime.nativeTransaction.stopped()&&runtime.replacementMaskFallbacks==0&&!runtime.guardSuppressedCommands,
            "benchmark draws stay on the bitmap path without suppressing commands");
        std::cout<<"{\"bench\":\"sr_replacement_command\",\"implementation\":\""<<implementation<<"\",\"live_command_buffers\":"<<live
                 <<",\"draw_ns_per_call\":"<<drawNs<<",\"bind_ns_per_call\":"<<bindNs<<",\"binding_record_bytes\":"<<sizeof(GameSrRuntime::GraphicsBindings)<<"}\n";
    }
}
} // namespace hot_path_benchmark

int main(){
    try{hot_path_benchmark::recordingFacts();hot_path_benchmark::recordingWorkload();hot_path_benchmark::replacementCommand();return 0;}
    catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
}
