#include "wsi_retirement.hpp"
#include <iostream>
#include <stdexcept>
namespace {
using Ledger=xess_vk::testing::WsiRetirement;
unsigned checks=0;
void require(bool value){++checks;if(!value)throw std::runtime_error("retirement assertion");}
template<class F>void rejects(F action){bool failed=false;try{action();}catch(const std::logic_error&){failed=true;}require(failed);}
void acquire(Ledger& ledger,unsigned index){ledger.acquiredAfterFence(ledger.current(),index,ledger.stats().acquires+1,VK_SUCCESS,VK_SUCCESS);}
void present(Ledger& ledger,unsigned index){ledger.presented(ledger.current(),index,ledger.stats().presents+1,VK_SUCCESS,VK_SUCCESS);}
void cycle(Ledger& ledger,unsigned index){acquire(ledger,index);present(ledger,index);require(ledger.conserved());}
void initialAndFailures(){
    Ledger l;rejects([&]{l.created(0);});rejects([&]{l.created(33);});l.created(3);
    rejects([&]{l.created(3);});rejects([&]{present(l,0);});cycle(l,0);
    for(auto result:{VK_TIMEOUT,VK_NOT_READY,VK_ERROR_DEVICE_LOST,VK_ERROR_OUT_OF_DATE_KHR}){
        rejects([&]{l.acquiredAfterFence(1,0,2,VK_SUCCESS,result);});
        rejects([&]{l.acquiredAfterFence(1,0,2,result,VK_SUCCESS);});
        require(l.stats().pending==1&&l.stats().retiredByAcquire==0&&l.stats().acquires==1);
    }
    rejects([&]{l.acquiredAfterFence(2,0,2,VK_SUCCESS,VK_SUCCESS);});
    rejects([&]{l.acquiredAfterFence(1,3,2,VK_SUCCESS,VK_SUCCESS);});
    rejects([&]{l.acquiredAfterFence(1,0,3,VK_SUCCESS,VK_SUCCESS);});
    acquire(l,1);require(l.stats().pending==1);rejects([&]{acquire(l,1);});
    rejects([&]{l.retiringForReplacement();});
    rejects([&]{l.presented(1,1,2,VK_SUCCESS,VK_ERROR_OUT_OF_DATE_KHR);});
    require(l.generation(1).images[1].acquired&&l.stats().pending==1);present(l,1);
    l.retiringForReplacement();require(l.current()==0&&l.generation(1).retired);
    // Models a failed driver create: no created() occurs, old cannot resume.
    rejects([&]{l.acquiredAfterFence(1,0,3,VK_SUCCESS,VK_SUCCESS);});
    rejects([&]{l.destroyed(1);});require(l.stats().pending==2&&l.conserved());
}
void replacementAndCarry(){
    Ledger l;l.created(3);for(unsigned i=0;i<3;++i)cycle(l,i);
    l.retiringForReplacement();l.created(3);
    acquire(l,0);require(!l.canDestroy(1));present(l,0);
    cycle(l,1);require(!l.canDestroy(1)); // Another new image is insufficient.
    rejects([&]{l.acquiredAfterFence(2,0,6,VK_SUCCESS,VK_TIMEOUT);});require(!l.canDestroy(1));
    acquire(l,0);require(l.canDestroy(1)&&l.stats().retiredBySuccessor==3&&l.stats().retiredByAcquire==1);
    present(l,0);l.destroyed(1);rejects([&]{l.destroyed(1);});require(l.stats().destroyedGenerations==1&&l.conserved());
    // Two quick recreations: no successful presentation in generation 3.
    l.retiringForReplacement();l.created(2);l.retiringForReplacement();l.created(4);
    cycle(l,2);require(!l.canDestroy(2)&&!l.canDestroy(3));cycle(l,0);require(!l.canDestroy(2));
    acquire(l,2);require(l.canDestroy(2)&&l.canDestroy(3));present(l,2);
    l.destroyed(2);l.destroyed(3);require(l.stats().destroyedGenerations==3&&l.conserved());
    // A successor with an unresolved first present, itself replaced, is carried.
    l.retiringForReplacement();l.created(2);cycle(l,0);l.retiringForReplacement();l.created(2);
    cycle(l,1);require(!l.canDestroy(4)&&!l.canDestroy(5));acquire(l,1);
    require(l.canDestroy(4)&&l.canDestroy(5));present(l,1);l.destroyed(4);l.destroyed(5);require(l.conserved());
}
void boundedAndLongRun(){
    Ledger l;l.created(32);
    for(unsigned n=0;n<10000;++n)cycle(l,n%32);
    require(l.stats().pending==32&&l.stats().retiredByAcquire==9968);
    for(unsigned n=1;n<8;++n){l.retiringForReplacement();l.created(32);cycle(l,0);}
    require(l.stats().generations==8&&l.stats().pending==39);
    rejects([&]{l.retiringForReplacement();});require(l.current()==8&&!l.generation(8).retired);
    acquire(l,0);present(l,0);require(l.stats().pending==1&&l.conserved());
    for(unsigned n=1;n<8;++n){require(l.canDestroy(n));l.destroyed(n);}
    require(l.stats().destroyedGenerations==7);
}
}
int main(){try{initialAndFailures();replacementAndCarry();boundedAndLongRun();
    std::cout<<"{\"stage\":\"pure-wsi-generational-retirement\",\"testPassed\":true,\"checks\":"<<checks
        <<",\"maxGenerations\":8,\"maxImagesPerGeneration\":32,\"ledgerBytes\":"<<sizeof(Ledger)
        <<",\"gpuTouched\":false,\"gameTouched\":false}\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
