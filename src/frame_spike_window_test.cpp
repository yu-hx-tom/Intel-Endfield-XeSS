#include "frame_spike_window.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>

int main(){try{
    using W=xess_vk::FrameSpikeWindow;W w;unsigned checks{};
    const auto need=[&](bool v){++checks;if(!v)throw std::runtime_error("frame-spike window check failed");};
    need(!w.due(1));w.observe(100,0);need(!w.intervals&&!w.spikes&&!w.due(100+W::periodNs-1));
    for(const auto n:{30,80,50,20,60,100}){
        w.observe(100+n,n);const auto slot=w.consider(n);
        if(slot>=0){std::string text=std::to_string(n);w.retain(slot,n,text.data(),text.size());text.assign(text.size(),'x');}
    }
    std::array<std::int64_t,3> top{};for(unsigned n=0;n<3;++n){const auto& d=w.worst[n];top[n]=d.intervalNs;need(std::string(d.text.data(),d.bytes)==std::to_string(d.intervalNs));}
    std::sort(top.begin(),top.end());need(top==std::array<std::int64_t,3>{60,80,100});
    need(w.intervals==6&&w.spikes==6&&w.maximumNs==100&&w.due(100+W::periodNs)&&!w.due(99));
    need(w.consider(60)==-1);w.retain(-1,1,"a",1);need(w.droppedDetails==1);
    w.retain(0,1,"a",W::detailBytes);need(w.droppedDetails==2&&w.worst[0].intervalNs>1);
    w.reset(200);need(!w.intervals&&!w.spikes&&!w.maximumNs&&!w.droppedDetails&&!w.due(200));
    for(const auto& d:w.worst)need(!d.bytes&&!d.intervalNs);
    w.observe(201,25'000'000);need(w.intervals==1&&w.maximumNs==25'000'000&&!w.spikes);
    std::cout<<"{\"passed\":true,\"checks\":"<<checks<<",\"fixedStorageBytes\":"<<sizeof(w)<<",\"detailLimit\":3}\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
