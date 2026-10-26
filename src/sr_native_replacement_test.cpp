#include "sr_native_replacement.hpp"
#include <cstdio>
#include <cstdlib>
namespace r=xess_vk::sr_replacement;
unsigned checks{};
void check(bool v,const char* why){++checks;if(!v){std::fprintf(stderr,"FAIL %s\n",why);std::exit(1);}}
void frame(r::Transaction& t,unsigned id){
    t.begin(id,10,id);
    for(unsigned pass=2;pass<=8;++pass)check(t.core(pass,id,10,id),"every recognised core is suppressed");
    t.complete(true);
}
int main(){
    {VkImageSubresourceRange v{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};check(r::fullColorRange(v,1,1),"full image");
     v.levelCount=VK_REMAINING_MIP_LEVELS;v.layerCount=VK_REMAINING_ARRAY_LAYERS;check(r::fullColorRange(v,2,3),"remaining ranges");
     v.baseMipLevel=1;check(!r::fullColorRange(v,2,3),"partial range never discards old contents");}
    {r::Transaction t;for(unsigned id=1;id<=1000;++id)frame(t,id);
     check(t.counts().replacementFrames==1000&&!t.stopped(),"first frame and every later frame use one policy, without warmup");
     for(auto v:t.counts().skipped)check(v==1000,"all seven skipped");for(auto v:t.counts().forwarded)check(!v,"no native FSR core execution");}
    {r::Transaction t;t.begin(1,10,1);check(!t.core(0,1,10,1)&&!t.core(1,1,10,1),"required MV and transparency dependencies retained");
     check(t.core(2,1,10,1)&&t.core(4,1,10,1)&&t.fatal(),"bad sequence never falls through to native dispatch");}
    {r::Transaction t;t.begin(1,10,1);t.core(2,1,10,1);t.begin(2,10,2);check(t.fault()==r::Fault::IncompleteFrame,"incomplete frame cannot be hidden");}
    {r::Transaction t;t.begin(1,10,1);check(t.core(2,1,11,1)&&t.fatal(),"foreign recording cannot suppress under another identity");}
    {r::Transaction t;t.begin(1,10,1);for(unsigned p=2;p<=8;++p)t.core(p,1,10,1);t.complete(false);check(t.fault()==r::Fault::Terminal,"missing output is explicit, never restores FSR");}
    {r::Transaction t;frame(t,1);t.produce({100,4},true,2);t.produce({200,7},false,5);
     check(!t.use({100,5}),"image generation is part of identity");check(!t.use({200,7})&&!t.stopped(),"valid content reads do not reset or stop SR");
     check(t.omitted({100,4})&&t.producer({100,4})==2&&t.missingContents()==1,"unrelated valid reads preserve missing-content provenance");
     t.produce({100,4},false,5);check(t.omitted({100,4})&&t.producer({100,4})==2,"partial write cannot repair missing content");
     check(t.use({100,4})&&t.fatal(),"actual omitted read remains rejected");check(!t.use({100,4})&&t.counts().guards==1,"first fault retained");}
    {r::Transaction t;t.produce({100,4},true,2);check(!t.use({100,4},true)&&!t.missingContents(),"verified whole-image overwrite retires missing content");
     t.produce({100,5},true,2);t.retire({100,4});check(t.omitted({100,5}),"retirement is generation specific");t.retire({100,5});check(!t.hasContents(),"destroyed images no longer watched");}
    {r::Transaction t;for(unsigned n=0;n<257;++n)t.produce({100+n,1},true,2);
     check(!t.stopped()&&!t.counts().capacity&&t.missingContents()==257,"live resources beyond the old 64-slot budget do not stop SR");
     for(unsigned n=0;n<257;++n)t.retire({100+n,1});
     check(!t.hasContents(),"all live identities retire without dropping unretired contents");}
    {r::Transaction t;t.begin(1,10,1);t.core(2,1,10,1);t.produce({100,4},true,2);t.stop(r::Fault::Consumer);
     check(!t.recover(),"an open recording cannot be recovered");t.retired(10,2);check(t.open(),"foreign retirement cannot close transaction ownership");
     t.retired(10,1);check(!t.open()&&t.omitted({100,4})&&!t.recover(),"retiring commands cannot silently erase omitted live images");
     t.retire({100,4});const auto guards=t.counts().guards;
     check(t.recover()&&!t.stopped()&&t.counts().guards==guards,"retired ownership and contents allow recovery while retaining cumulative fault evidence");}
    std::printf("{\"passed\":true,\"checks\":%u,\"gpuTouched\":false}\n",checks);
}
