#undef NDEBUG
#include "presentation_output_geometry.hpp"
#include <cassert>
#include <iostream>

using namespace xess_vk;
using namespace xess_vk::presentation::detail;
static frame_input::FrameConstantsSample sample(int width=1920,int height=1080) {
    frame_input::FrameConstantsSample s;
    s.validityMask=frame_input::Extents;s.renderSize={1128,634};s.upscaleSize={width,height};
    s.outputRegion.observed=s.outputRegion.valid=true;s.outputRegion.rect={0,90,2880,1620};return s;
}
int main() {
    auto s=sample();auto g=lowResOutputGeometry({2880,1800},s,{1128,634},{1128,634});
    assert(g&&g.canvas.width==1920&&g.canvas.height==1200&&g.region.offset.y==60&&g.region.extent.height==1080);
    for(const auto extent:{VkExtent2D{1600,900},VkExtent2D{2560,1440}}) {
        s=sample(int(extent.width),int(extent.height));g=lowResOutputGeometry({2880,1800},s,{1128,634},{1128,634});
        assert(g&&g.canvas.width==extent.width&&g.canvas.height==extent.height*10/9);
    }
    s=sample();s.outputRegion.rect={440,0,2560,1440};g=lowResOutputGeometry({3440,1440},s,{1128,634},{1128,634});
    assert(g&&g.canvas.width==2580&&g.canvas.height==1080&&g.region.offset.x==330);
    s.outputRegion.rect={0,0,2560,1440};g=lowResOutputGeometry({2560,1440},s,{1128,634},{1128,634});
    assert(g&&g.canvas.width==1920&&g.canvas.height==1080&&!g.region.offset.x&&!g.region.offset.y);
    const auto invalid=[](VkExtent2D source,frame_input::FrameConstantsSample value,VkExtent2D depth={1128,634},VkExtent2D motion={1128,634}) {
        const auto result=lowResOutputGeometry(source,value,depth,motion);assert(!result&&result.fallbackReason);
    };
    s=sample();s.outputRegion.rect[1]=91;invalid({2880,1800},s);
    s=sample();s.outputRegion.rect[2]=2879;invalid({2880,1800},s);
    s=sample();s.outputRegion.rect[0]=-1;invalid({2880,1800},s);
    s=sample();s.outputRegion.rect[3]=1801;invalid({2880,1800},s);
    s=sample();s.outputRegion.valid=false;invalid({2880,1800},s);
    s=sample();s.validityMask=0;invalid({2880,1800},s);
    s=sample();s.upscaleSize={2880,1620};invalid({2880,1800},s);
    s=sample();s.upscaleSize={3840,2160};invalid({2880,1800},s);
    s=sample();invalid({2880,1800},s,{1921,1080},{1921,1080});
    invalid({2880,1800},s,{1128,634},{1129,634});
    invalid({UINT32_MAX,1800},s);invalid({0,1800},s);
    std::cout<<"low-res output geometry: exact full-WSI/asymmetric/size/bounds/nonintegral/aspect/input guards passed; GPU untouched\n";
}
