#include "fsr4_command_map.hpp"
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace xess_vk;
namespace {
unsigned checks{};
void require(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
VkCommandBuffer command(std::uintptr_t value){return reinterpret_cast<VkCommandBuffer>(value);}
void generations(){
    fsr4::CommandMap map;const sr_overlay::RecordingKey first{1,2,0x100,3},second{1,2,0x100,4};
    require(map.empty()&&map.command(command(first.command))==command(first.command),"Empty map preserves command");
    map.publish(map.reserve(),first,command(0x200));
    require(!map.empty()&&map.command(command(first.command))==command(0x200),"Exact command redirects");
    map.erase(second);require(map.command(command(first.command))==command(0x200),"Wrong generation cannot retire mapping");
    map.erase(first);require(map.empty(),"Retired mapping leaves fast path empty");
    map.publish(map.reserve(),second,command(0x300));map.erase(first);
    require(map.command(command(second.command))==command(0x300),"Stale retirement preserves a reused command");
}
void bounds(){
    fsr4::CommandMap map;
    for(unsigned n=0;n<map.capacity;++n)map.publish(map.reserve(),{1,2,0x100+n,1},command(0x1000+n));
    require(map.reserve()==map.capacity,"Mapping capacity refuses before recording mutations");
    for(unsigned n=0;n<map.capacity;++n)require(map.command(command(0x100+n))==command(0x1000+n),"All admitted mappings remain visible");
    require(map.command(command(0x999))==command(0x999),"Unrelated command is untouched");
    for(unsigned n=0;n<map.capacity;++n)map.erase({1,2,0x100+n,1});
    require(map.empty()&&map.reserve()==0,"All exact retirements restore empty fast path");
}
void concurrency(){
    fsr4::CommandMap map;std::atomic<bool> done{},bad{};std::atomic<unsigned> reads{};
    auto reader=[&]{while(!done.load(std::memory_order_acquire)){
        const auto a=map.command(command(0x100)),b=map.command(command(0x200));
        if((a!=command(0x100)&&a!=command(0x1000))||(b!=command(0x200)&&b!=command(0x2000)))bad.store(true);
        reads.fetch_add(1,std::memory_order_relaxed);
    }};
    std::thread one(reader),two(reader);
    for(unsigned n=1;n<=200'000;++n){
        const sr_overlay::RecordingKey a{1,2,0x100,n},b{1,2,0x200,n};
        map.publish(map.reserve(),a,command(0x1000));map.erase(a);
        map.publish(map.reserve(),b,command(0x2000));map.erase(b);
    }
    done.store(true,std::memory_order_release);one.join();two.join();
    require(reads.load()>0&&!bad.load()&&map.empty(),"Concurrent readers never borrow another command's tail");
}
}
int main(){try{generations();bounds();concurrency();std::cout<<"{\"passed\":true,\"checks\":"<<checks<<",\"driverCalls\":0}\n";return 0;}
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
