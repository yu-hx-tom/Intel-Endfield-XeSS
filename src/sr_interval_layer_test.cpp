#define main descriptor_layer_original_main
#include "descriptor_image_layer_test.cpp"
#undef main

namespace sr_interval_fixture {
using namespace descriptor_layer_fixture;
namespace ro=xess_vk::render_pass_observer;
namespace sr=xess_vk::sr_observation;
unsigned calls{};
const VkImage depth=handle<VkImage>(0x9010),other=handle<VkImage>(0x9020);
const void* expectedInfo{};
void called(VkCommandBuffer buffer){
    require(buffer==command,"interval wrapper preserves command buffer");
    require(TryAcquireSRWLockExclusive(&stateLock)!=0,"no outer metadata lock spans driver call");
    ReleaseSRWLockExclusive(&stateLock);++calls;SetLastError(43210);
}
void VKAPI_PTR copy(VkCommandBuffer b,VkImage s,VkImageLayout sl,VkImage d,VkImageLayout dl,uint32_t n,const VkImageCopy* r){
    require(s==depth&&d==other&&sl==VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL&&dl==VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL&&!n&&!r,"copy arguments unchanged");called(b);
}
void VKAPI_PTR clear(VkCommandBuffer b,VkImage i,VkImageLayout l,const VkClearDepthStencilValue* v,uint32_t n,const VkImageSubresourceRange* r){
    require(i==depth&&l==VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL&&v==expectedInfo&&!n&&!r,"clear arguments unchanged");called(b);
}
void VKAPI_PTR color(VkCommandBuffer b,VkImage i,VkImageLayout l,const VkClearColorValue* v,uint32_t n,const VkImageSubresourceRange* r){
    require(i==other&&l==VK_IMAGE_LAYOUT_GENERAL&&v==expectedInfo&&!n&&!r,"unrelated color clear unchanged");called(b);
}
template<class Info>void modern(VkCommandBuffer b,const Info* info){require(info==expectedInfo,"modern transfer preserves literal info pointer");called(b);}
void VKAPI_PTR resolve(VkCommandBuffer b,VkImage s,VkImageLayout,VkImage d,VkImageLayout,uint32_t n,const VkImageResolve* r){
    require(s==other&&d==depth&&!n&&!r,"resolve images unchanged");called(b);
}
void VKAPI_PTR bufferToImage(VkCommandBuffer b,VkBuffer,VkImage i,VkImageLayout,uint32_t n,const VkBufferImageCopy* r){require(i==depth&&!n&&!r,"buffer destination unchanged");called(b);}
void VKAPI_PTR imageToBuffer(VkCommandBuffer b,VkImage i,VkImageLayout,VkBuffer,uint32_t n,const VkBufferImageCopy* r){require(i==depth&&!n&&!r,"buffer source unchanged");called(b);}
void consumerArchive(){
    namespace co=xess_vk::compute_observer;
    Fixture f(false);sr::resetForTest();
    struct Cleanup {DeviceState& state;~Cleanup(){co::destroyShaderArchive(state.srShaders);state.srShaders=nullptr;sr::resetForTest();}} cleanup{f.state};
    f.state.srShaders=co::createShaderArchive(device,+[](const char* line){sr::summary();sink(line);},true);
    require(f.state.srShaders!=nullptr,"consumer shader archive created without compute trace or GPU");
    const std::uint32_t code[]{0x07230203,0x00010000,0,1024,0,(5u<<16)|15,5,3,0x6e69616d,0,
        (4u<<16)|71,10,34,0,(4u<<16)|71,10,33,0};
    const auto shader=handle<VkShaderModule>(0x9100);
    VkShaderModuleCreateInfo si{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};si.codeSize=sizeof(code);si.pCode=code;
    co::archiveShader(f.state.srShaders,shader,&si);
    VkComputePipelineCreateInfo pi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pi.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_COMPUTE_BIT,shader,"main",nullptr};
    xess_vk::FrameTimingAssociation frame{1,{1,1,false},0,true};
    const auto readFrame=+[](void* user,std::uint32_t){return *static_cast<const xess_vk::FrameTimingAssociation*>(user);};
    const auto resolver=+[](std::uint64_t,const xess_vk::upscale_tail_probe::Result&,sr::Images&) noexcept {};
    require(sr::configure(0x100000,bits(device),sink,readFrame,&frame,resolver,false,true)&&sr::activate(),
        "read-only consumer window configured with copied own-process facts");
    sr::captureForTest(1);const auto window=sr::consumerWindow(bits(device));require(window!=0,"consumer window armed without marker files");
    xess_vk::CommandRecordingFacts facts;facts.device=bits(device);facts.contextIncarnation=1;facts.command=bits(command);facts.rpRecordingGeneration=1;facts.commonRecordOrder=280;
    dm::DispatchEvidence evidence;evidence.metadataAccepted=true;evidence.count=1;evidence.pipeline={0x9200,1};
    evidence.bindings[0].image={0x9300,1};evidence.bindings[0].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    sr::consumerDispatch(bits(device),window,facts,evidence,frame,2,{},{});
    evidence.bindings[0].descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    for(bool shadow:{false,true}){
        evidence.pipeline={shadow?0x9401u:0x9400u,shadow?2u:1u};
        co::archivePipeline(f.state.srShaders,handle<VkPipeline>(evidence.pipeline.handle),evidence.pipeline.generation,&pi,VK_SUCCESS);
        sr::DispatchTicket ticket;ticket.consumerWindow=window;ticket.shadowObserved=shadow;ticket.frame=frame;
        SetLastError(54321);srDispatchReturned(&f.state,ticket,"vkCmdDispatch",facts,evidence);
        require(GetLastError()==54321,"real return adapter archives outside observer lock and preserves caller error");
    }
    require(std::count_if(lines.begin(),lines.end(),[](const auto& line){return line.find("\"event\":\"sr_shader_archive_begin\"")!=std::string::npos;})==2,
        "both normal and shadow return branches archive unknown referenced pipelines through the production adapter");
    require(driverCalls==0,"consumer archive fixture issues no Vulkan driver calls");
}
void replacementWrapperOrder(){
    Fixture f;srTailWindowOverride=0;
    std::unique_ptr<ro::Context,decltype(&ro::destroy)> context(ro::create(device,nullptr),ro::destroy);f.state.renderPass=context.get();
    const auto pool=handle<VkCommandPool>(0xD800);VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};ro::poolCreated(context.get(),pool,&pi,VK_SUCCESS);
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ai.commandPool=pool;ai.commandBufferCount=1;ro::allocate(context.get(),&ai,&command,VK_SUCCESS);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};ro::begin(context.get(),command,&bi,VK_SUCCESS);
    GameSrRuntime runtime(&f.state,true);f.state.srRuntime=&runtime;
    unsigned snapshots{},decisions{};bool suppress=true;const dm::Key image{0xD810,3};
    struct State {unsigned* snapshots;unsigned* decisions;bool* suppress;GameSrRuntime* runtime;dm::Key image;}
        state{&snapshots,&decisions,&suppress,&runtime,image};
    static State* selected;selected=&state;
    srContinuousRecordForTest=+[](const xess_vk::CommandRecordingFacts&){++*selected->snapshots;};
    srReplacementInterceptForTest=+[](const xess_vk::CommandRecordingFacts& facts,bool direct){
        ++*selected->decisions;
        require(!direct||*selected->snapshots>0,"T3 snapshot call precedes Prepare dispatch suppression");
        require(facts.command&&facts.rpRecordingGeneration,"wrapper receipt uses the real current recording identity");
        selected->runtime->nativeTransaction.produce(selected->image,true,7);
        selected->runtime->pendingFullWrites.push_back({facts,selected->image,{19,2},false,true,true,24,32,{3,4,1}});
        return *selected->suppress;
    };
    const auto driver=+[](VkCommandBuffer b){
        require(b==command&&selected->runtime->nativeTransaction.omitted(selected->image),"all writers still have omitted content when entering the original driver");
        ++driverCalls;SetLastError(54321);
    };
    static void(*driverCall)(VkCommandBuffer);driverCall=driver;
    f.state.cmdDispatch=+[](VkCommandBuffer b,unsigned,unsigned,unsigned){driverCall(b);};
    f.state.cmdDispatchIndirect=+[](VkCommandBuffer b,VkBuffer,VkDeviceSize){driverCall(b);};
    f.state.cmdDispatchBase=f.state.cmdDispatchBaseKhr=+[](VkCommandBuffer b,unsigned,unsigned,unsigned,unsigned,unsigned,unsigned){driverCall(b);};
    for(unsigned api=0;api<4;++api){
        snapshots=decisions=0;const auto before=driverCalls;
        const auto invoke=[&]{switch(api){
            case 0:vkCmdDispatch(command,3,4,1);break;
            case 1:vkCmdDispatchIndirect(command,handle<VkBuffer>(0x1010),16);break;
            case 2:vkCmdDispatchBase(command,0,0,0,3,4,1);break;
            default:vkCmdDispatchBaseKHR(command,0,0,0,3,4,1);break;
        }};
        suppress=true;invoke();require(decisions==1&&driverCalls==before&&runtime.nativeTransaction.omitted(image),"all four production wrappers leave suppressed writers omitted without a driver call");
        require(snapshots==unsigned(api!=1),"direct grids retain T3 snapshot insertion when native dispatch is omitted");
        suppress=false;SetLastError(12345);invoke();
        require(decisions==2&&driverCalls==before+1&&!runtime.nativeTransaction.omitted(image)&&GetLastError()==54321,"only the returned driver call permits full-write retirement and preserves its last error");
    }
    require(runtime.replacementPhotoFullWrites==4&&runtime.replacementPhotoFullWritesSkipped==4&&!runtime.fullWriteRetiredWhileSuppressed,
        "production wrappers distinguish every forwarded/skipped receipt without inventing GPU completion");
    f.state.srRuntime=nullptr;f.state.renderPass=nullptr;
    srReplacementInterceptForTest=nullptr;srContinuousRecordForTest=nullptr;srTailWindowOverride=-1;
}
void replacementR32Attachments(){
    for(unsigned variant=0;variant<5;++variant){
        Fixture f;
        std::unique_ptr<ro::Context,decltype(&ro::destroy)> context(ro::create(device,nullptr),ro::destroy);
        f.state.renderPass=context.get();
        VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};image.imageType=VK_IMAGE_TYPE_2D;image.format=VK_FORMAT_R32_UINT;
        image.extent=variant==0?VkExtent3D{1,1,1}:VkExtent3D{1128,634,1};image.mipLevels=image.arrayLayers=1;
        image.samples=VK_SAMPLE_COUNT_1_BIT;image.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_STORAGE_BIT;
        image.tiling=VK_IMAGE_TILING_OPTIMAL;image.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        const auto img=handle<VkImage>(0xA100);f.inventory->created(bits(img),{std::uint32_t(image.format),image.extent.width,image.extent.height,1,1,1,
            std::uint32_t(image.samples),image.usage,image.flags,std::uint32_t(image.tiling),std::uint32_t(image.sharingMode),std::uint32_t(image.initialLayout)});
        const auto record=f.inventory->lookup(bits(img)).record;ro::imageCreated(context.get(),&image,img,VK_SUCCESS,&record);
        const auto view=handle<VkImageView>(0xA200);VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image=img;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=image.format;vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        if(variant==1){vi.subresourceRange.levelCount=VK_REMAINING_MIP_LEVELS;vi.subresourceRange.layerCount=VK_REMAINING_ARRAY_LAYERS;}
        ro::viewCreated(context.get(),&vi,view,VK_SUCCESS);
        VkAttachmentDescription attachment{};attachment.format=image.format;attachment.samples=VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp=variant==3?VK_ATTACHMENT_LOAD_OP_LOAD:VK_ATTACHMENT_LOAD_OP_CLEAR;attachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
        attachment.initialLayout=attachment.finalLayout=VK_IMAGE_LAYOUT_GENERAL;
        VkAttachmentReference reference{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};VkSubpassDescription sub{};
        sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;sub.colorAttachmentCount=1;sub.pColorAttachments=&reference;
        const auto pass=handle<VkRenderPass>(0xA300);VkRenderPassCreateInfo pi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        pi.attachmentCount=pi.subpassCount=1;pi.pAttachments=&attachment;pi.pSubpasses=&sub;ro::renderPassCreated(context.get(),&pi,pass,VK_SUCCESS);
        const auto fb=handle<VkFramebuffer>(0xA400);VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fi.renderPass=pass;fi.attachmentCount=1;fi.pAttachments=&view;fi.width=image.extent.width;fi.height=image.extent.height;fi.layers=1;
        ro::framebufferCreated(context.get(),&fi,fb,VK_SUCCESS);
        const auto pool=handle<VkCommandPool>(0xA500);VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        ro::poolCreated(context.get(),pool,&poolInfo,VK_SUCCESS);
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ai.commandPool=pool;ai.commandBufferCount=1;
        ai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ro::allocate(context.get(),&ai,&command,VK_SUCCESS);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        ro::begin(context.get(),command,&bi,VK_SUCCESS);
        VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};begin.renderPass=pass;begin.framebuffer=fb;
        begin.renderArea.extent={image.extent.width,image.extent.height};if(variant==2)--begin.renderArea.extent.width;
        auto facts=ro::recordingFacts(context.get(),command);
        GameSrRuntime runtime(&f.state,true);runtime.nativeTransaction.produce({record.image,record.generation},true,2);
        auto call=xess_vk::sr_tail_commands::beginRenderPass(&begin,VK_SUBPASS_CONTENTS_INLINE);
        if(variant==4)call.extensionsOmitted=true;
        runtime.replacementCommand(facts,call);
        const bool safe=variant<2;
        require(runtime.nativeTransaction.stopped()!=safe,"production R32 attachment guard accepts full clear, blocks partial/LOAD/unknown");
        require(runtime.nativeTransaction.counts().discards==unsigned(safe),"R32 clear retires exactly the old core content, without granting a read");
        if(!safe){
            require(std::string_view(runtime.replacementFault.origin)=="attachment"&&runtime.replacementFault.image.handle==record.image&&
                runtime.replacementFault.load==attachment.loadOp&&runtime.replacementFault.area.extent.width==begin.renderArea.extent.width,
                "first attachment fault retains the exact identity, load and area for later periodic reports");
            runtime.replacementStop(xess_vk::sr_replacement::Fault::Capacity,"later_unrelated_failure",facts,77);
            require(std::string_view(runtime.replacementFault.origin)=="attachment"&&runtime.replacementFault.issues!=77&&runtime.nativeTransaction.counts().guards==1,
                "later failures cannot overwrite the original stopped-session evidence");
        }
        f.state.renderPass=nullptr;
    }
}
void replacementKeptMotionWriter(){
    for(const auto profile:{0,xess_vk::sr_native_shaders::auxiliaryWriterProfile,xess_vk::sr_native_shaders::photoWriterProfile})for(unsigned variant=0;variant<10;++variant){
        Fixture f;GameSrRuntime runtime(&f.state,true);
        xess_vk::CommandRecordingFacts facts;facts.command=bits(command);facts.rpRecordingGeneration=1;facts.commonRecordOrder=2;
        dm::DispatchEvidence evidence;evidence.pipeline={0xC100,1};evidence.metadataAccepted=true;evidence.count=evidence.guardImageCount=2;evidence.guardImagesKnown=true;
        evidence.bindings[0].setIndex=0;evidence.bindings[0].binding=0;evidence.bindings[0].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        evidence.bindings[0].image={0xC200,1};evidence.bindings[1].setIndex=0;evidence.bindings[1].binding=1;
        evidence.bindings[1].descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;evidence.bindings[1].image={variant==1?0xC200u:0xC300u,1};
        // Cache entries represent the already fingerprinted exact shader identity.
        runtime.replacementPipelines[0]={evidence.pipeline,variant==2?-1:profile};
        if(variant==3){evidence.metadataAccepted=false;evidence.issues=dm::bit(dm::Issue::UnsupportedDescriptor);evidence.count=0;}
        if(variant==4)evidence.bindings[0].setIndex=1;
        if(variant==5)evidence.bindings[0].binding=4;
        if(variant==6)evidence.bindings[0].descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        if(variant==8)++runtime.replacementPipelines[0].key.generation;
        if(variant==9){evidence.bindings[1].binding=2;evidence.bindings[1].image=evidence.bindings[0].image;}
        runtime.nativeTransaction.produce(evidence.bindings[0].image,true,7);
        sr::DispatchTicket ticket;ticket.frame={1,{1,1,false},0,true};
        require(runtime.intercept(facts,evidence,ticket,variant!=7)==(variant!=0),"only a proven write-only producer forwards; an unsafe consumer is omitted, not the command buffer");
        require(runtime.nativeTransaction.stopped()==(variant!=0),"only exact verified write-only destination avoids a consumer fault");
        require(runtime.nativeTransaction.watches(evidence.bindings[0].image)&&runtime.nativeTransaction.omitted(evidence.bindings[0].image)&&!runtime.nativeTransaction.counts().discards,
            "write-only classification does not invent full coverage or retire the old-content watch");
    }
}
void replacementCompactConsumers(){
    for(bool compact:{false,true})for(unsigned variant=0;variant<5;++variant){
        Fixture f;GameSrRuntime runtime(&f.state,true);
        xess_vk::CommandRecordingFacts facts;facts.command=bits(command);facts.rpRecordingGeneration=1;facts.commonRecordOrder=2;
        dm::DispatchEvidence e;e.pipeline={0xCC00,1};e.guardImageCount=1;e.guardImagesKnown=variant!=2;
        e.membershipOnly=e.filteredOut=compact;e.metadataAccepted=!compact;e.count=compact?0:1;
        e.bindings[0].image={variant==1?0xCC11u:0xCC10u,1};e.bindings[0].descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        runtime.replacementPipelines[0]={e.pipeline,-1};runtime.nativeTransaction.produce({0xCC10,1},variant!=4,2);
        if(variant==3)runtime.nativeTransaction.retire({0xCC10,1});
        const bool suppressed=runtime.intercept(facts,e,{},true);require(suppressed==runtime.nativeTransaction.stopped(),"compact/full consumers forward only when no guard rejects their inputs");
        require(runtime.nativeTransaction.stopped()==(variant==0||variant==2),"compact/full guards identically block omitted content and unknown membership");
        require(runtime.nativeTransaction.counts().guards==unsigned(variant==0||variant==2),"valid native reads never reset admission");
        require(!srObservationDispatch(&f.state,facts,e).containsPrepare,"ordinary guard identities cannot become a Prepare input ticket");
    }
}
void replacementShaderAccess(){
    for(bool compact:{false,true})for(unsigned variant=0;variant<6;++variant){
        Fixture f;GameSrRuntime runtime(&f.state,true);
        xess_vk::CommandRecordingFacts facts;facts.command=bits(command);facts.rpRecordingGeneration=1;facts.commonRecordOrder=2;
        dm::DispatchEvidence e;e.pipeline={0xCD00,1};e.guardImageCount=1;e.guardImagesKnown=variant!=4;
        e.membershipOnly=e.filteredOut=compact;e.metadataAccepted=!compact;e.count=compact?0:1;
        auto& b=e.bindings[0];b.image={0xCD10,1};b.setIndex=2;b.binding=3;
        b.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        auto& pipeline=runtime.replacementPipelines[0];pipeline.key=e.pipeline;pipeline.profile=-1;
        pipeline.reads.known=variant!=2;
        if(variant==1){pipeline.reads.bindings[0]={2,3};pipeline.reads.count=1;}
        if(variant==3)e.issues=dm::bit(dm::Issue::UnsupportedDispatch);
        runtime.nativeTransaction.produce(b.image,variant!=5,5);
        const bool suppressed=runtime.intercept(facts,e,{},true);require(suppressed==runtime.nativeTransaction.stopped(),"proven non-reading bindings forward; missing contents never reach a rejected consumer");
        const bool checked=variant>=1&&variant<=4;
        require(runtime.nativeTransaction.stopped()==checked,"real reads and incomplete access facts retain the consumer guard");
        require(runtime.nativeTransaction.watches(b.image)&&runtime.nativeTransaction.omitted(b.image)==(variant!=5),
            "not reading an image never erases its possibly missing old contents");
        require(runtime.replacementNonReadingReferences==unsigned(!checked),"only proven non-reading bindings bypass consumer checks");
        require(runtime.nativeTransaction.counts().guards==unsigned(checked),"only a real or unverified read produces a guard");
    }
}
void replacementLateShaderAccess(){
    namespace co=xess_vk::compute_observer;
    for(unsigned mode=0;mode<3;++mode){const bool nonReadable=mode!=0;
        Fixture f;GameSrRuntime runtime(&f.state,true);
        f.state.srShaders=co::createShaderArchive(device,sink,true);
        struct Cleanup{DeviceState& state;~Cleanup(){co::destroyShaderArchive(state.srShaders);state.srShaders=nullptr;}} cleanup{f.state};
        std::vector<std::uint32_t> code{0x07230203,0x00010000,0,64,0,(5u<<16)|15,5,3,0x6e69616d,0,
            (4u<<16)|71,10,34,0,(4u<<16)|71,10,33,0,(4u<<16)|59,7,10,0};
        if(mode==1)code.insert(code.end(),{(3u<<16)|71,10,25});
        if(mode==2)code.insert(code.end(),{(5u<<16)|54,1,3,0,2,(2u<<16)|248,4,
            (4u<<16)|61,8,20,10,(4u<<16)|99,20,30,31,(1u<<16)|253,(1u<<16)|56});
        VkShaderModuleCreateInfo si{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};si.codeSize=code.size()*4;si.pCode=code.data();
        for(unsigned n=0;n<co::shaderArchiveFullGraphModuleLimit;++n){
            const auto module=handle<VkShaderModule>(0xDD00+n);co::archiveShader(f.state.srShaders,module,&si);co::destroyArchivedShader(f.state.srShaders,module);
        }
        const auto shader=handle<VkShaderModule>(0xDE00);const auto pipeline=handle<VkPipeline>(0xDE01);
        co::archiveShader(f.state.srShaders,shader,&si);
        VkComputePipelineCreateInfo pi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pi.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_COMPUTE_BIT,shader,"main",nullptr};
        co::archivePipeline(f.state.srShaders,pipeline,1,&pi,VK_SUCCESS);co::destroyArchivedShader(f.state.srShaders,shader);
        dm::DispatchEvidence e;e.pipeline={bits(pipeline),1};e.metadataAccepted=e.guardImagesKnown=true;e.count=e.guardImageCount=1;
        auto& b=e.bindings[0];b.image={0xDE02,1};b.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        xess_vk::CommandRecordingFacts facts;facts.command=bits(command);facts.rpRecordingGeneration=1665;facts.commonRecordOrder=260;
        runtime.nativeTransaction.produce(b.image,true,2);
        require(runtime.intercept(facts,e,{},true)==!nonReadable,"late shader forwards only a proven non-reading binding after archive exhaustion");
        require(runtime.nativeTransaction.stopped()==!nonReadable,"late real reads still trigger the consumer guard");
        require(runtime.nativeTransaction.omitted(b.image),"non-reading classification never invents initialized image content");
    }
}
void replacementMaskOverwrite(){
    for(unsigned variant=0;variant<10;++variant){
        Fixture f;GameSrRuntime runtime(&f.state,true);
        xess_vk::CommandRecordingFacts facts;facts.command=bits(command);facts.rpRecordingGeneration=1581;facts.commonRecordOrder=141;
        dm::DispatchEvidence e;e.pipeline={0xCE00,1};e.metadataAccepted=e.guardImagesKnown=true;e.count=e.guardImageCount=3;e.groups={141,80,1};
        runtime.replacementPipelines[0]={e.pipeline,1};
        for(unsigned n=0;n<2;++n){auto& b=e.bindings[n];b.binding=n;b.image={0xCE10+n,1};b.view={0xCE20+n,1};
            b.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;b.viewType=VK_IMAGE_VIEW_TYPE_2D;b.viewFormat=VK_FORMAT_R8_UNORM;
            b.declaredLayout=VK_IMAGE_LAYOUT_GENERAL;b.range={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            b.imageDescription={VK_FORMAT_R8_UNORM,1128,634,1,1,1,VK_SAMPLE_COUNT_1_BIT,VK_IMAGE_USAGE_STORAGE_BIT};
            if(variant==4){b.viewFormat=VK_FORMAT_R16_SFLOAT;b.imageDescription.format=VK_FORMAT_R16_SFLOAT;}
            runtime.nativeTransaction.produce(b.image,true,6);
        }
        auto& input=e.bindings[2];input.binding=2;input.descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;input.image={variant==1?0xCE10u:0xCE30u,1};
        if(variant==2)--e.groups[0];
        sr::DispatchTicket ticket;ticket.nativePass=xess_vk::upscale_tail_probe::Pass::Transparency;ticket.extentsKnown=true;ticket.frame={1,{1,1,false},0,true};
        ticket.pipeline=e.pipeline.handle;ticket.pipelineGeneration=e.pipeline.generation;ticket.command=facts.command;ticket.recording=facts.rpRecordingGeneration;
        ticket.groups=e.groups;ticket.renderExtent={1128,634};
        if(variant==5)--ticket.renderExtent.width;if(variant==6)ticket.nativePass=xess_vk::upscale_tail_probe::Pass::Unknown;
        if(variant==7)ticket.frame.valid=false;if(variant==8)++ticket.pipelineGeneration;if(variant==9)++ticket.recording;
        const bool suppressed=runtime.intercept(facts,e,ticket,true);
        require(suppressed==(variant==1),"mask writes do not consume old contents, but an aliased sampled input still rejects the dispatch");
        require(runtime.nativeTransaction.omitted(e.bindings[0].image)&&runtime.nativeTransaction.omitted(e.bindings[1].image),
            "intended writes never retire contents before the native recording receipt");
        runtime.replacementFullWriteReturned(facts,!suppressed&&variant!=3);
        for(unsigned n=0;n<2;++n)require(runtime.nativeTransaction.omitted(e.bindings[n].image)==(variant!=0),
            ("only a returned full-extent R8 mask dispatch retires both destinations: variant="+std::to_string(variant)+" binding="+std::to_string(n)).c_str());
    }
}
void replacementUndefinedLayoutDiscards(){
    for(bool modern:{false,true})for(unsigned variant=0;variant<24;++variant){
        Fixture f;GameSrRuntime runtime(&f.state,true);xess_vk::CommandRecordingFacts facts;
        facts.command=bits(command);facts.rpRecordingGeneration=1;facts.commonRecordOrder=3;
        facts.device=bits(runtime.identity.device);facts.contextIncarnation=runtime.identity.incarnation;
        facts.scopeKnown=true;facts.queueFamily=f.state.srQueueFamily;facts.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        if(variant==19)facts.scopeKnown=false;
        if(variant==20)++facts.queueFamily;
        if(variant==21)runtime.ownerThread=GetCurrentThreadId()+1;
        if(variant==22)++facts.contextIncarnation;
        xess_vk::image_inventory::Description d{};d.format=VK_FORMAT_R16_SFLOAT;d.width=64;d.height=32;
        d.depth=d.mipLevels=d.arrayLayers=1;
        if(variant==2||variant==3||variant==14)d.mipLevels=2;
        if(variant==13||variant==14)d.arrayLayers=2;
        if(variant==12)d.depth=2;
        const auto image=handle<VkImage>(0xCF00);f.inventory->created(bits(image),d);
        const auto r=f.inventory->lookup(bits(image)).record;const dm::Key watched{r.image,r.generation},other{0xCF01,2};
        runtime.nativeTransaction.produce(watched,variant!=17,7);runtime.nativeTransaction.produce(other,true,2);
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.image=image;
        b.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;b.newLayout=VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        switch(variant){
        case 1:b.subresourceRange.levelCount=VK_REMAINING_MIP_LEVELS;b.subresourceRange.layerCount=VK_REMAINING_ARRAY_LAYERS;break;
        case 3:b.subresourceRange.baseMipLevel=1;break;case 4:b.subresourceRange.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT;break;
        case 5:b.oldLayout=VK_IMAGE_LAYOUT_GENERAL;break;case 6:b.newLayout=VK_IMAGE_LAYOUT_UNDEFINED;break;
        case 7:b.newLayout=VK_IMAGE_LAYOUT_PREINITIALIZED;break;case 8:b.pNext=reinterpret_cast<void*>(1);break;
        case 9:b.srcQueueFamilyIndex=0;b.dstQueueFamilyIndex=1;break;case 10:b.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;break;
        case 11:b.image=handle<VkImage>(0xCF02);break;case 13:b.subresourceRange.baseArrayLayer=1;break;
        case 14:b.subresourceRange.levelCount=b.subresourceRange.layerCount=2;break;
        case 15:b.oldLayout=VK_IMAGE_LAYOUT_GENERAL;break;
        case 23:b.newLayout=VK_IMAGE_LAYOUT_ZERO_INITIALIZED_EXT;break;
        }
        std::array<VkImageMemoryBarrier,12> old{};old.back()=b;
        std::array<VkImageMemoryBarrier2,12> newer{};auto& b2=newer.back();b2={VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b2.pNext=b.pNext;b2.image=b.image;b2.oldLayout=b.oldLayout;b2.newLayout=b.newLayout;
        b2.srcQueueFamilyIndex=b.srcQueueFamilyIndex;b2.dstQueueFamilyIndex=b.dstQueueFamilyIndex;b2.subresourceRange=b.subresourceRange;
        if(variant==10)b2.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        if(variant==16)++srSdkDepth;
        if(modern)runtime.replacementUndefinedLayouts(facts,variant==18?12:1,variant==18?newer.data():&b2);
        else runtime.replacementUndefinedLayouts(facts,variant==18?12:1,variant==18?old.data():&b);
        if(variant==16)--srSdkDepth;
        const bool discarded=variant<2||variant==14||variant==17||variant==18;
        require(runtime.nativeTransaction.watches(watched)!=discarded&&runtime.replacementUndefinedLayoutDiscards==unsigned(discarded&&variant!=17),
            "only full ordinary image layout discards retire old contents, independent of eight-barrier trace quotas");
        require(runtime.nativeTransaction.omitted(other),"discarding one image never erases another omitted image");
        require(!runtime.nativeTransaction.stopped(),"a discard observation itself never stops a healthy transaction");
        if(discarded)require(!runtime.nativeTransaction.use(watched),"later consumers cannot see a stale omitted version after a real discard");
    }
}
void replacementUndefinedLayoutWrappers(){
    for(unsigned api=0;api<8;++api)for(bool ignoredLayouts:{false,true}){
        Fixture f;std::unique_ptr<ro::Context,decltype(&ro::destroy)> context(ro::create(device,nullptr),ro::destroy);
        require(context!=nullptr,"discard wrapper recording context created");f.state.renderPass=context.get();
        const auto pool=handle<VkCommandPool>(0xCFA0);VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        ro::poolCreated(context.get(),pool,&pci,VK_SUCCESS);VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool=pool;ai.commandBufferCount=1;ai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ro::allocate(context.get(),&ai,&command,VK_SUCCESS);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        ro::begin(context.get(),command,&bi,VK_SUCCESS);const auto facts=ro::recordingFacts(context.get(),command);
        f.state.deviceIncarnation=facts.contextIncarnation;f.state.srQueueFamily=facts.queueFamily;
        GameSrRuntime runtime(&f.state,true);f.state.srRuntime=&runtime;
        struct Unlink {DeviceState& state;~Unlink(){state.srRuntime=nullptr;state.renderPass=nullptr;}} unlink{f.state};
        const auto image=handle<VkImage>(0xCFB0);f.inventory->created(bits(image),{VK_FORMAT_R16_SFLOAT,64,32,1,1,1,1,0,0,0,0,0});
        const auto record=f.inventory->lookup(bits(image)).record;const dm::Key watched{record.image,record.generation};
        runtime.nativeTransaction.produce(watched,true,7);
        std::array<VkImageMemoryBarrier,12> old{};auto& b=old.back();b={VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.image=image;
        b.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;b.newLayout=ignoredLayouts?VK_IMAGE_LAYOUT_UNDEFINED:VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        std::array<VkImageMemoryBarrier2,12> modern{};auto& b2=modern.back();b2={VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b2.image=image;b2.oldLayout=b.oldLayout;b2.newLayout=b.newLayout;b2.srcQueueFamilyIndex=b2.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        b2.subresourceRange=b.subresourceRange;
        std::array<VkDependencyInfo,2> infos{};for(auto& info:infos)info={VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        infos[1].imageMemoryBarrierCount=12;infos[1].pImageMemoryBarriers=modern.data();
        f.state.cmdPipelineBarrier=+[](VkCommandBuffer c,VkPipelineStageFlags,VkPipelineStageFlags,VkDependencyFlags,
            unsigned,const VkMemoryBarrier*,unsigned,const VkBufferMemoryBarrier*,unsigned n,const VkImageMemoryBarrier* p){
            require(n==12&&p==expectedInfo,"legacy barrier forwards its complete original array");called(c);};
        f.state.cmdWaitEvents=+[](VkCommandBuffer c,unsigned,const VkEvent*,VkPipelineStageFlags,VkPipelineStageFlags,
            unsigned,const VkMemoryBarrier*,unsigned,const VkBufferMemoryBarrier*,unsigned n,const VkImageMemoryBarrier* p){
            require(n==12&&p==expectedInfo,"legacy wait forwards its complete original array");called(c);};
        f.state.cmdPipelineBarrier2=f.state.cmdPipelineBarrier2Khr=+[](VkCommandBuffer c,const VkDependencyInfo* p){
            require(p==expectedInfo,"modern barrier preserves original dependency pointer");called(c);};
        f.state.cmdWaitEvents2=f.state.cmdWaitEvents2Khr=+[](VkCommandBuffer c,unsigned n,const VkEvent*,const VkDependencyInfo* p){
            require(n==2&&p==expectedInfo,"modern wait scans all dependencies without changing the native call");called(c);};
        f.state.cmdSetEvent2=f.state.cmdSetEvent2Khr=+[](VkCommandBuffer c,VkEvent,const VkDependencyInfo* p){
            require(p==expectedInfo,"event signal preserves original dependency pointer");called(c);};
        calls=0;std::array<VkEvent,2> events{};expectedInfo=api==0||api==3?static_cast<const void*>(old.data()):
            (api==4||api==5?static_cast<const void*>(infos.data()):static_cast<const void*>(&infos[1]));
        switch(api){
        case 0:vkCmdPipelineBarrier(command,1,1,0,0,nullptr,0,nullptr,12,old.data());break;
        case 1:vkCmdPipelineBarrier2(command,&infos[1]);break;case 2:vkCmdPipelineBarrier2KHR(command,&infos[1]);break;
        case 3:vkCmdWaitEvents(command,2,events.data(),1,1,0,nullptr,0,nullptr,12,old.data());break;
        case 4:vkCmdWaitEvents2(command,2,events.data(),infos.data());break;
        case 5:vkCmdWaitEvents2KHR(command,2,events.data(),infos.data());break;
        case 6:vkCmdSetEvent2(command,events[0],&infos[1]);break;case 7:vkCmdSetEvent2KHR(command,events[0],&infos[1]);break;
        }
        const bool discard=api<6&&!ignoredLayouts;
        require(calls==1&&runtime.nativeTransaction.watches(watched)!=discard,
            "all barrier/wait aliases retire full discards after one native call; equal layouts and signal halves do not");
        require(runtime.replacementUndefinedLayoutDiscards==unsigned(discard),"late array entries are not limited by diagnostic capture capacity");
    }
}
void replacementPhotoOverwrite(){
    for(bool auxiliary:{false,true})for(unsigned variant=0;variant<36;++variant){
        Fixture f;GameSrRuntime runtime(&f.state,true);
        xess_vk::CommandRecordingFacts facts;facts.command=bits(command);facts.rpRecordingGeneration=1;facts.commonRecordOrder=2;
        dm::DispatchEvidence evidence;evidence.pipeline={0xCB00,1};evidence.metadataAccepted=evidence.guardImagesKnown=true;
        evidence.count=evidence.guardImageCount=3;evidence.groups={141,80,1};
        auto& output=evidence.bindings[0];output.setIndex=output.binding=0;output.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        output.image={0xCB10,1};output.view={0xCB11,1};output.viewType=VK_IMAGE_VIEW_TYPE_2D;output.viewFormat=VK_FORMAT_R16_SFLOAT;
        output.declaredLayout=VK_IMAGE_LAYOUT_GENERAL;output.range={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        output.imageDescription={VK_FORMAT_R16_SFLOAT,1128,634,1,1,1,VK_SAMPLE_COUNT_1_BIT,VK_IMAGE_USAGE_STORAGE_BIT};
        for(unsigned n=1;n<3;++n){auto& b=evidence.bindings[n];b.binding=n;b.descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;b.image={0xCB20+n,1};}
        runtime.replacementPipelines[0]={evidence.pipeline,auxiliary?xess_vk::sr_native_shaders::auxiliaryWriterProfile:xess_vk::sr_native_shaders::photoWriterProfile};
        switch(variant){
        case 1:output.range.levelCount=VK_REMAINING_MIP_LEVELS;output.range.layerCount=VK_REMAINING_ARRAY_LAYERS;break;
        case 2:output.imageDescription.width=output.imageDescription.height=1;evidence.groups={1,1,1};break;
        case 3:--evidence.groups[0];break;case 4:--evidence.groups[1];break;
        case 5:evidence.groups[2]=0;break;case 6:evidence.groups[2]=2;break;
        case 7:output.imageDescription.width=0;break;case 8:output.imageDescription.depth=2;break;
        case 9:output.imageDescription.mipLevels=2;break;case 10:output.imageDescription.arrayLayers=2;break;
        case 11:output.imageDescription.format=VK_FORMAT_R32_SFLOAT;break;case 12:output.viewFormat=VK_FORMAT_R32_SFLOAT;break;
        case 13:output.viewType=VK_IMAGE_VIEW_TYPE_2D_ARRAY;break;case 14:output.range.baseMipLevel=1;break;
        case 15:output.range.baseArrayLayer=1;break;case 16:output.range.levelCount=0;break;case 17:output.range.layerCount=0;break;
        case 18:output.range.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT;break;case 19:output.imageDescription.usage=0;break;
        case 20:output.imageDescription.samples=VK_SAMPLE_COUNT_4_BIT;break;case 21:output.imageDescription.flags=VK_IMAGE_CREATE_SPARSE_BINDING_BIT;break;
        case 22:output.components.r=VK_COMPONENT_SWIZZLE_R;break;case 23:output.declaredLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;break;
        case 24:output.arrayElement=1;break;case 25:evidence.metadataAccepted=false;break;
        case 26:runtime.replacementPipelines[0].profile=-1;break;case 27:++runtime.replacementPipelines[0].key.generation;break;
        case 29:evidence.bindings[1].image=output.image;break;case 30:evidence.bindings[2].image=output.image;break;
        case 31:++evidence.groups[0];break;case 32:evidence.issues=dm::bit(dm::Issue::UnsupportedDispatch);break;
        case 33:output.view={};break;case 34:evidence.guardImagesKnown=false;break;
        case 35:output.viewFormat=VK_FORMAT_R16G16B16A16_SFLOAT;output.imageDescription.format=VK_FORMAT_R16G16B16A16_SFLOAT;break;
        }
        const dm::Key otherMissing{0xCB40,1};runtime.nativeTransaction.produce(output.image,true,2);
        runtime.nativeTransaction.produce(otherMissing,true,7);sr::DispatchTicket ticket;
        const bool suppressed=runtime.intercept(facts,evidence,ticket,variant!=28);require(suppressed==runtime.nativeTransaction.stopped(),"full writers forward; an aliased reader is suppressed while synchronization remains intact");
        require(runtime.nativeTransaction.watches(output.image),"intercept alone never claims that a writer has been recorded");
        runtime.replacementFullWriteReturned(facts,!suppressed);
        require(!runtime.fullWriteRetiredWhileSuppressed,"a suppressed writer never retires missing content");
        if(variant==29||variant==30)require((auxiliary?runtime.replacementAuxFullWritesSkipped:runtime.replacementPhotoFullWritesSkipped)==1&&
            !(auxiliary?runtime.replacementAuxFullWrites:runtime.replacementPhotoFullWrites),"aliased full writer is counted as skipped, never applied");
        const bool full=variant<3||(auxiliary&&variant==35);
        require(runtime.nativeTransaction.watches(output.image)!=full&&runtime.nativeTransaction.counts().discards==unsigned(full),
            "both verified full writers retire their overwritten destination, including reused luma textures");
        require(runtime.nativeTransaction.omitted(otherMissing),"photo overwrite never clears another missing intermediate");
        const bool alreadyStopped=runtime.nativeTransaction.stopped();
        require(runtime.nativeTransaction.use(output.image)==(!full&&!alreadyStopped)&&runtime.nativeTransaction.stopped()==!full,
            "later sampled read is allowed only after proven complete overwrite");
    }
}
void replacementWriterReceipts(){
    Fixture f;GameSrRuntime runtime(&f.state,true);
    xess_vk::CommandRecordingFacts facts;facts.command=bits(command);facts.rpRecordingGeneration=4;facts.commonRecordOrder=17;
    const dm::Key image{0xCB80,3};runtime.nativeTransaction.produce(image,true,7);
    runtime.pendingFullWrites.push_back({facts,image,{9,2},false,true,true,1128,634,{141,80,1}});
    auto foreign=facts;++foreign.rpRecordingGeneration;runtime.replacementFullWriteReturned(foreign,true);
    foreign=facts;++foreign.commonRecordOrder;runtime.replacementFullWriteReturned(foreign,true);
    require(runtime.nativeTransaction.omitted(image)&&!runtime.replacementPhotoFullWrites,"foreign receipt cannot consume a full writer's intention");
    runtime.replacementFullWriteReturned(facts,false);
    require(runtime.nativeTransaction.omitted(image)&&runtime.replacementPhotoFullWritesSkipped==1&&!runtime.fullWriteRetiredWhileSuppressed,"suppression never changes content provenance");
    runtime.pendingFullWrites.push_back({facts,image,{9,2},false,true,true,1128,634,{141,80,1}});
    runtime.replacementFullWriteReturned(facts,true);runtime.replacementFullWriteReturned(facts,true);
    require(!runtime.nativeTransaction.omitted(image)&&runtime.replacementPhotoFullWrites==1,"a matching forwarded receipt applies once, even when repeated");
}
void replacementRebuildRecovery(){
    namespace nc=xess_vk::sr_native_resolution;
    const auto reason=nc::control.reason.load();const auto recoveries=nc::control.recoveries.load();const auto applied=nc::control.applied.load();
    Fixture f;GameSrRuntime runtime(&f.state,true);auto& tr=runtime.nativeTransaction;
    xess_vk::CommandRecordingFacts facts;facts.command=bits(command);facts.rpRecordingGeneration=4;facts.pool=91;
    tr.begin(44,facts.command,4);tr.core(2,44,facts.command,4);const dm::Key missing{0xCB90,1};tr.produce(missing,true,2);
    runtime.guard(facts)->coreOmitted=true;runtime.replacementPlanFrame.engineFrame=44;
    runtime.replacementStop(xess_vk::sr_replacement::Fault::Consumer,"fixture",facts);runtime.replacementFatalIfNeeded();
    const auto firstDegrade=runtime.degradedQpc;
    require(runtime.holdingOutput()&&runtime.fallback==GameSrRuntime::Fallback::Waiting,"a stop holds output while the engine-native request is pending");
    runtime.boundary();require(tr.stopped()&&nc::control.requested()&&std::string_view(runtime.recoveryBlocked)=="recording_owners_alive","live owners block recovery and expose the reason");
    runtime.rebuildRequests=1;runtime.boundary();
    require(tr.stopped()&&nc::control.requested(),"a new swapchain alone cannot retire live omission-owning command buffers");
    runtime.retire(facts,sro::Discard::SuccessfulReset);runtime.boundary();
    require(tr.stopped()&&tr.omitted(missing),"legal CB retirement does not erase still-live missing image contents");
    tr.retire(missing);runtime.pendingFullWrites.push_back({facts,missing});runtime.boundary();
    require(tr.stopped(),"an outstanding write receipt prevents recovery");runtime.pendingFullWrites.clear();
    runtime.boundary();
    require(!tr.stopped()&&!nc::control.requested()&&runtime.recoveredRebuild==1&&nc::control.recoveries==recoveries+1,"rebuild plus retired owners and contents permits recovery only at boundary");
    require(runtime.holdingOutput()&&runtime.degradedQpc==firstDegrade&&runtime.fallback==GameSrRuntime::Fallback::None,
        "recovery cannot erase degradation or claim a freshly rendered image");
    runtime.replacementStop(xess_vk::sr_replacement::Fault::Consumer,"graphics_bindings_unknown");runtime.replacementFatalIfNeeded();
    nc::control.request(nc::Reason::ConsumerUnproven);
    runtime.boundary();
    require(!tr.stopped()&&!nc::control.requested()&&nc::control.recoveries==recoveries+2&&runtime.recoveredRebuild==1,
        "retired owners and contents recover on the same swapchain without an extra rebuild condition");
    runtime.executed=1;runtime.resumed(55);
    require(std::string_view(runtime.activeReason)=="none"&&runtime.successfulRecoveries==1&&runtime.lastRecoveryFrame==55&&runtime.degradedQpc==firstDegrade,
        "a fresh SR success clears only the active fault and preserves historical degradation");
    require(std::string_view(xess_vk::sr_status::category("graphics_bindings_unknown"))=="consumer"&&
        std::string_view(xess_vk::sr_status::category("sdk_execute"))=="sdk"&&
        std::string_view(xess_vk::sr_status::category("submission_failed"))=="submission","runtime reason categories remain distinct");
    runtime.stopped=true;runtime.issue(56,"sdk_execute");runtime.reject("profile_or_frame_source",runtime.sourceSkips);
    require(std::string_view(runtime.activeReason)=="sdk_execute"&&std::string_view(runtime.reasonCategory())=="sdk",
        "blocked follow-up frames preserve the SDK root cause rather than relabeling it as missing input");runtime.stopped=false;
    runtime.replacementStop(xess_vk::sr_replacement::Fault::Consumer,"later");runtime.rebuildRequests=2;runtime.recoveryTrackingLost=true;runtime.boundary();
    require(tr.stopped()&&runtime.recoveredRebuild==1,"lost owner tracking never becomes a retirement proof");
    runtime.recoveryTrackingLost=false;runtime.ownerThread=GetCurrentThreadId()+1;runtime.boundary();
    require(tr.stopped()&&std::string_view(runtime.recoveryBlocked)=="sdk_owner_thread","recovery never restarts the SDK on a foreign thread");
    runtime.ownerThread=0;runtime.sdkRestartFailed=true;runtime.restartAttemptRebuild=runtime.rebuildRequests;runtime.boundary();
    require(tr.stopped()&&std::string_view(runtime.recoveryBlocked)=="sdk_restart_failed","failed SDK restart remains bounded rather than retrying every boundary");
    nc::control.reason=reason;nc::control.recoveries=recoveries;nc::control.applied=applied;
}
void replacementNativeFallback(){
    namespace nc=xess_vk::sr_native_resolution;using F=GameSrRuntime::Fallback;
    const auto reason=nc::control.reason.load();const auto recoveries=nc::control.recoveries.load();const auto applied=nc::control.applied.load();
    nc::control.reason=nc::Reason::None;
    Fixture f;GameSrRuntime runtime(&f.state,true);auto& tr=runtime.nativeTransaction;
    xess_vk::CommandRecordingFacts facts;facts.command=bits(command);facts.rpRecordingGeneration=4;facts.pool=91;
    // A completed replacement frame left a Prepare intermediate omitted; a game post-process samples it.
    tr.begin(44,facts.command,4);for(unsigned p=2;p<=8;++p)tr.core(p,44,facts.command,4);
    const dm::Key depth{0xD100,1};tr.produce(depth,true,2);tr.complete(true);runtime.replacementPlanFrame.engineFrame=44;
    const auto evidence=[](dm::Key pipeline){dm::DispatchEvidence e;e.pipeline=pipeline;e.metadataAccepted=e.guardImagesKnown=true;return e;};
    auto consumer=evidence({0xD110,3});consumer.count=consumer.guardImageCount=1;
    auto& read=consumer.bindings[0];read.image=depth;read.binding=4;read.descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    const auto prepare=evidence({0xD120,1}),accumulate=evidence({0xD130,1}),nativeAa=evidence({0xD140,1});
    runtime.replacementPipelines[0]={consumer.pipeline,-1};runtime.replacementPipelines[1]={prepare.pipeline,2};
    runtime.replacementPipelines[2]={accumulate.pipeline,8};runtime.replacementPipelines[3]={nativeAa.pipeline,-1};
    sr::DispatchTicket none;sr::DispatchTicket native;native.containsAccumulate=native.extentsKnown=true;native.frame={45,{7,8,false},0,true};
    native.pipeline=nativeAa.pipeline.handle;native.pipelineGeneration=nativeAa.pipeline.generation;native.renderExtent=native.outputExtent={1920,1080};
    require(runtime.intercept(facts,consumer,none,true)&&tr.stopped()&&runtime.holdOutput&&runtime.fallback==F::Waiting&&
        runtime.fallbackConsumerCount==1&&runtime.fallbackConsumers[0]==consumer.pipeline&&nc::control.requested(),
        "an omitted read stops SR, holds output, names its consumer and asks for the engine's native frames");
    require(runtime.holdingOutput()&&runtime.fallback==F::Waiting,"no native frame has been observed yet");
    nc::control.applied=runtime.fallbackAppliedBase+1;
    require(runtime.holdingOutput()&&runtime.fallback==F::Passthrough,"engine-field readback alone never releases held GPU output");
    require(!runtime.intercept(facts,consumer,none,true)&&runtime.fallbackConsumerAt==runtime.presents,"the stopped consumer is forwarded and observed");
    require(!runtime.intercept(facts,prepare,none,true)&&!runtime.intercept(facts,nativeAa,native,true)&&
        tr.counts().replacementFrames==1&&runtime.nativeAaUnqualified==0,"engine-native passes are forwarded without a replacement frame or a new refusal");
    require(runtime.holdingOutput(),"the first engine-native present still settles");
    require(!runtime.holdingOutput()&&runtime.fallbackReleases==1,"the engine's own frame is presented after settling");
    require(!runtime.intercept(facts,accumulate,none,true)&&runtime.holdingOutput(),"an upscaling Accumulate is never shown as an engine-native frame");
    require(!runtime.holdingOutput()&&runtime.fallbackReleases==2,"native frames after it are shown again");
    std::uint64_t lastHeld{};
    for(unsigned n=0;n<70;++n){runtime.intercept(facts,accumulate,none,true);if(runtime.holdingOutput())lastHeld=runtime.presents;}
    require(!runtime.holdOutput&&lastHeld==runtime.fallbackSince+GameSrRuntime::fallbackUpscaleTolerancePresents-1&&runtime.fallbackUpscaleShown>0,
        "an engine that keeps one upscaling camera is shown after a bounded hold, never frozen");
    for(unsigned n=0;n<300;++n){runtime.intercept(facts,consumer,none,true);runtime.holdingOutput();}
    require(runtime.fallback==F::Passthrough&&!runtime.holdOutput,"an active consumer keeps the engine-native route");
    runtime.boundary();require(tr.stopped()&&nc::control.requested(),"a mid-frame boundary never ends the engine-native route");
    const auto quietFrom=runtime.fallbackConsumerAt;
    while(runtime.fallback==F::Passthrough&&runtime.presents<quietFrom+1000)runtime.holdingOutput();
    require(runtime.fallback==F::Returning&&runtime.presents==quietFrom+runtime.fallbackQuiet()&&!runtime.holdOutput&&
        !tr.missingContents()&&runtime.fallbackAbandoned==1,"a quiet consumer starts the return and abandons only stopped omitted content");
    runtime.recoveryTrackingLost=true;runtime.boundary();
    for(unsigned n=0;n<GameSrRuntime::fallbackReturnPresents;++n)require(!runtime.holdingOutput(),"a pending return keeps showing native frames");
    require(runtime.fallback==F::Passthrough&&tr.stopped()&&nc::control.requested()&&runtime.fallbackLevel==1&&runtime.fallbackReturnTimeouts==1,
        "a blocked return stays engine-native and backs off");
    runtime.recoveryTrackingLost=false;const auto blockedAt=runtime.presents;
    while(runtime.fallback==F::Passthrough&&runtime.presents<blockedAt+1000)runtime.holdingOutput();
    require(runtime.fallback==F::Returning&&runtime.presents==blockedAt+2*GameSrRuntime::fallbackQuietPresents,"the next return waits for the doubled quiet period");
    runtime.boundary();
    require(!tr.stopped()&&!nc::control.requested()&&runtime.fallbackRecovered&&runtime.fallback==F::Returning&&runtime.holdOutput&&
        nc::control.recoveries==recoveries+1,"the return recovers at a boundary, withdraws the engine-native request and holds the switch");
    require(!runtime.intercept(facts,nativeAa,native,true)&&!tr.stopped()&&runtime.holdingOutput()&&runtime.fallback==F::Returning,
        "frames still rendered natively during the return are untouched and held");
    require(!runtime.intercept(facts,accumulate,none,true)&&runtime.holdingOutput()&&runtime.fallback==F::None&&runtime.fallbackReturns==1,
        "an upscaling frame ends the return; output stays held until XeSS succeeds");
    sr::DispatchTicket frame;frame.frame={46,{9,10,false},0,true};
    require(runtime.intercept(facts,prepare,frame,true)&&tr.stopped()&&runtime.fallback==F::Waiting&&runtime.fallbackLevel==2&&
        runtime.fallbackQuiet()==4*GameSrRuntime::fallbackQuietPresents&&!runtime.fallbackConsumerCount,
        "after the return Prepare is replaced again; a prompt re-fault backs off the next return");
    nc::control.reason=reason;nc::control.recoveries=recoveries;nc::control.applied=applied;
}
void nativeAaUnqualified(){
    const auto saved=xess_vk::sr_native_resolution::control.reason.load();
    for(bool previousCore:{false,true}){
    // The second variant also models a Native-AA pass that exists only because native rendering was requested.
    xess_vk::sr_native_resolution::control.reason=previousCore?xess_vk::sr_native_resolution::Reason::InputUnavailable:xess_vk::sr_native_resolution::Reason::None;
    Fixture fixture;GameSrRuntime runtime(&fixture.state,true);
    xess_vk::CommandRecordingFacts f;f.device=5;f.contextIncarnation=2;f.command=10;f.rpRecordingGeneration=8;f.commonRecordOrder=19;
    dm::DispatchEvidence e;e.pipeline={13,17};e.metadataAccepted=e.guardImagesKnown=true;
    sr::DispatchTicket t;t.containsAccumulate=true;t.extentsKnown=true;t.frame={33,{7,8,false},0,true};
    t.pipeline=e.pipeline.handle;t.pipelineGeneration=e.pipeline.generation;t.renderExtent=t.outputExtent={1920,1080};
    t.source.valid=true;t.source.accumulate=f;t.source.accumulatePipeline=e.pipeline;t.source.frame=t.frame;
    t.source.constants.renderSize=t.source.constants.upscaleSize={1920,1080};
    if(previousCore){runtime.nativeTransaction.begin(33,10,8);for(unsigned p=2;p<8;++p)runtime.nativeTransaction.core(p,33,10,8);}
    runtime.replacementPlanFrame=t.frame;
    require(runtime.intercept(f,e,t,true)&&runtime.nativeTransaction.stopped()&&!runtime.nativeTransaction.counts().skipped[6]&&!runtime.currentReplacement,
        "even complete 1:1 callback association cannot promote an unknown shader to a recognised terminal");
    require(runtime.nativeAaUnqualified==1&&runtime.holdOutput,"unsupported Native AA is explicit, never counted as successful XeSS AA");
    require(runtime.fallback==GameSrRuntime::Fallback::Waiting&&runtime.fallbackNativeSeen&&runtime.fallbackConsumerCount==unsigned(!previousCore),
        "a game-selected Native AA pass blocks the return; one caused by our own native request does not");
    require(std::string_view(runtime.reasonCategory())=="configuration","unqualified Native AA stays a configuration cause rather than a generic sequence fault");
    xess_vk::sr_native_resolution::control.reason=saved;
    }
}

void replacementOrderedRecovery(){
    using xess_vk::sr_replacement::Fault;
    const auto saved=xess_vk::sr_native_resolution::control.reason.load();
    const auto recoveries=xess_vk::sr_native_resolution::control.recoveries.load();
    for(const char* origin:{"compute_bindings_unknown","graphics_bindings_unknown","execute_secondary_commands","compute_dispatch"}){
        Fixture f;GameSrRuntime runtime(&f.state,true);auto& tr=runtime.nativeTransaction;
        tr.begin(44,bits(command),7);tr.core(2,44,bits(command),7);runtime.replacementPlanFrame.engineFrame=44;
        runtime.replacementStop(Fault::Consumer,origin);runtime.replacementFatalIfNeeded();
        require(runtime.holdOutput&&runtime.degradedFrame==44&&runtime.degradedQpc&&runtime.degradedNs&&
            xess_vk::sr_native_resolution::control.requested(),"all historical unknown origins hold output and request engine recovery without terminating the fixture");
        const auto first=runtime.degradedQpc;runtime.degrade(45,"color_transition");
        require(runtime.degradedQpc==first&&runtime.degradedFrame==44,"later recovery/transition never erases the first degradation evidence");
    }
    xess_vk::sr_native_resolution::control.reason=saved;xess_vk::sr_native_resolution::control.recoveries=recoveries;
    xess_vk::sr_replacement::Transaction early;early.produce({0xABCD,7},true,7);
    require(early.use({0xABCD,7})&&early.stopped(),"read before full overwrite remains a consumer fault");
    early.use({0xABCD,7},true);require(early.stopped()&&!early.omitted({0xABCD,7}),"a later full write cannot retroactively authorize an earlier read");
    xess_vk::sr_replacement::Transaction ordered;ordered.produce({0xABCD,7},true,7);ordered.use({0xABCD,7},true);
    require(!ordered.use({0xABCD,7})&&!ordered.stopped(),"full overwrite before read authorizes only the new valid contents");
}
void replacementNativeContentRecovery(){
    for(bool omitted:{false,true}){
        Fixture f;GameSrRuntime runtime(&f.state,true);auto& tr=runtime.nativeTransaction;const dm::Key image{0xCE00,7};tr.produce(image,omitted,2);
        xess_vk::CommandRecordingFacts facts;facts.command=bits(command);facts.rpRecordingGeneration=1;
        dm::DispatchEvidence e;e.pipeline={0xCF00,3};e.metadataAccepted=true;e.count=e.guardImageCount=1;e.guardImagesKnown=true;
        auto& b=e.bindings[0];b.image=image;b.set={0xCF01,4};b.binding=1;b.arrayElement=2;b.descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        runtime.replacementPipelines[0]={e.pipeline,-1};sr::DispatchTicket ticket;
        require(runtime.intercept(facts,e,ticket,true)==omitted&&tr.stopped()==omitted,"only actual missing content rejects and suppresses a read");
        if(omitted)require(runtime.lastReplacementFault.contentOmitted&&runtime.lastReplacementFault.producerProfile==2,"fault preserves producer provenance");
        else require(!tr.counts().guards&&tr.watches(image),"valid read does not change SR route");
    }
    Fixture f;GameSrRuntime runtime(&f.state,true);auto& tr=runtime.nativeTransaction;tr.begin(1,bits(command),1);tr.core(2,1,bits(command),1);
    xess_vk::CommandRecordingFacts facts;facts.command=bits(command);facts.rpRecordingGeneration=1;
    dm::DispatchEvidence e;e.pipeline={0xCF10,1};e.metadataAccepted=true;e.count=1;e.guardImagesKnown=true;
    auto& b=e.bindings[0];b.image={0xCF20,1};b.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    runtime.replacementPipelines[0]={e.pipeline,3};sr::DispatchTicket ticket;ticket.frame={1,{1,1,false},0,true};
    require(runtime.intercept(facts,e,ticket,true)&&tr.omitted(b.image)&&tr.producer(b.image)==3,"production core outputs carry suppressed provenance from first frame");
}
void replacementCharacterMenuWriter(){
    // Observed order in one command: e5a58764 writes the Prepare image (254), ce1c samples it (256).
    for(bool full:{true,false}){
        Fixture f;GameSrRuntime runtime(&f.state,true);auto& tr=runtime.nativeTransaction;
        xess_vk::CommandRecordingFacts facts;facts.command=bits(command);facts.rpRecordingGeneration=1;facts.commonRecordOrder=254;
        dm::DispatchEvidence writer;writer.pipeline={0xD300,1};writer.metadataAccepted=writer.guardImagesKnown=true;
        writer.count=writer.guardImageCount=1;writer.groups={141,full?80u:79u,1};
        auto& output=writer.bindings[0];output.setIndex=output.binding=0;output.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        output.image={0xD310,1};output.view={0xD311,1};output.viewType=VK_IMAGE_VIEW_TYPE_2D;output.viewFormat=VK_FORMAT_R16_SFLOAT;
        output.declaredLayout=VK_IMAGE_LAYOUT_GENERAL;output.range={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        output.imageDescription={VK_FORMAT_R16_SFLOAT,1128,634,1,1,1,VK_SAMPLE_COUNT_1_BIT,VK_IMAGE_USAGE_STORAGE_BIT};
        const auto depth=output.image;tr.produce(depth,true,2);
        runtime.replacementPipelines[0]={writer.pipeline,xess_vk::sr_native_shaders::characterMenuWriterProfile,{},true};
        sr::DispatchTicket ticket;
        require(!runtime.intercept(facts,writer,ticket,true)&&!tr.stopped(),"the character-menu writer is a kept writer, never suppressed");
        runtime.replacementFullWriteReturned(facts,true);
        require(tr.watches(depth)!=full&&runtime.replacementPhotoFullWrites==unsigned(full)&&runtime.replacementPhotoCoverageRefusals==unsigned(!full),
            "only its verified full-grid write retires the omitted Prepare image");
        dm::DispatchEvidence reader;reader.pipeline={0xD320,3};reader.metadataAccepted=reader.guardImagesKnown=true;reader.count=reader.guardImageCount=1;
        auto& read=reader.bindings[0];read.image=depth;read.set={0xD321,4};read.binding=4;read.descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        runtime.replacementPipelines[1]={reader.pipeline,-1,{},true};auto later=facts;later.commonRecordOrder=256;
        require(runtime.intercept(later,reader,ticket,true)!=full&&tr.stopped()!=full,
            "the menu post-process reads fresh content after a full write; a partial write still stops SR");
    }
}
void replacementDistinctFaultArchives(){
    namespace co=xess_vk::compute_observer;Fixture f;
    f.state.srShaders=co::createShaderArchive(device,sink,true);
    struct Cleanup {DeviceState& state;~Cleanup(){measurementSummaryTestSink=nullptr;co::destroyShaderArchive(state.srShaders);state.srShaders=nullptr;}} cleanup{f.state};
    const std::uint32_t code[]{0x07230203,0x00010000,0,1024,0,(5u<<16)|15,5,3,0x6e69616d,0,
        (4u<<16)|71,10,34,0,(4u<<16)|71,10,33,0};
    VkShaderModuleCreateInfo si{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};si.codeSize=sizeof(code);si.pCode=code;
    const auto shader=handle<VkShaderModule>(0xCF30);co::archiveShader(f.state.srShaders,shader,&si);
    VkComputePipelineCreateInfo pi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pi.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_COMPUTE_BIT,shader,"main",nullptr};
    static unsigned selections;selections=0;measurementSummaryTestSink=+[](const char* data,DWORD size){
        if(std::string_view(data,size).find("\"event\":\"sr_shader_archive_begin\"")!=std::string_view::npos)++selections;};
    GameSrRuntime runtime(&f.state,true);unsigned frame{},guards{};
    for(const auto n:{1u,1u,2u,3u,4u,5u,6u}){
        const dm::Key pipeline{0xCF40+n,n};
        co::archivePipeline(f.state.srShaders,handle<VkPipeline>(pipeline.handle),pipeline.generation,&pi,VK_SUCCESS);
        auto& tr=runtime.nativeTransaction;tr={};++frame;tr.produce({0xCF50,1},true,2);
        runtime.replacementUse({0xCF50,1},false,"compute_dispatch",{},pipeline);
        guards+=unsigned(tr.counts().guards);
    }
    require(selections==4&&runtime.replacementFaultPipelineCount==4,
        "repeated same-pipeline shadow faults cannot consume the four distinct consumer archive slots");
    require(guards==7,
        "archive identity and output bounds do not suppress later consumer checks");
}
void replacementComputeBindings(){
    namespace stc=xess_vk::sr_tail_commands;
    const auto savedTiming=trackingCpuTimingOverride;const auto savedAudit=trackingCpuAudit;
    for(int enabled=0;enabled<=1;++enabled){trackingCpuTimingOverride=enabled;TrackingCpuAudit audit;trackingCpuAudit=&audit;
    Fixture f;const auto r=setup(f);GameSrRuntime runtime(&f.state,true);audit={};
    xess_vk::CommandRecordingFacts facts;facts.rpRecordingGeneration=1;
    const auto compute=stc::bindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE,r.pipelineLayout,0,1,&r.set,0,nullptr);
    VkBindDescriptorSetsInfo info{VK_STRUCTURE_TYPE_BIND_DESCRIPTOR_SETS_INFO};info.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT;
    info.layout=r.pipelineLayout;info.descriptorSetCount=1;info.pDescriptorSets=&r.set;
    const auto api2=stc::bindDescriptorSets2(&info);
    for(unsigned n=0;n<129;++n){facts.command=0xD000+n;
        runtime.replacementCommand(facts,compute);runtime.replacementCommand(facts,api2);}
    require(!runtime.replacementCommandCpu.count&&!runtime.replacementCommandCalls&&!audit.clocks&&!runtime.nativeTransaction.stopped(),
        "compute-only core/API2 bindings skip the unrelated graphics guard and cannot exhaust its command slots");
    require(std::none_of(runtime.replacementGraphics.begin(),runtime.replacementGraphics.end(),[](const auto& g){return g.command!=0;}),
        "compute tracking does not allocate graphics binding records");
    info.stageFlags=VK_SHADER_STAGE_ALL_GRAPHICS|VK_SHADER_STAGE_COMPUTE_BIT;
    runtime.replacementCommand(facts,stc::bindDescriptorSets2(&info));
    require(runtime.replacementCommandCalls==1&&runtime.replacementCommandCpu.count==unsigned(enabled)&&audit.clocks==unsigned(enabled)*2&&
        runtime.replacementGraphics[0].sets[0].handle==bits(r.set),
        "mixed graphics/compute API2 binding still reaches the image guard; clocks follow the existing timing switch");
    const auto image=di::imageReferences(f.state.descriptors,r.set).referencedImages[0];runtime.nativeTransaction.produce(image,false,2);
    runtime.replacementCommand(facts,stc::simple(stc::Kind::Draw));
    require(!runtime.nativeTransaction.stopped()&&runtime.nativeTransaction.watches(image),"valid graphics reads neither stop SR nor restart native FSR");
    runtime.nativeTransaction.produce(image,true,2);
    runtime.replacementCommand(facts,stc::simple(stc::Kind::Draw));
    require(runtime.nativeTransaction.stopped()&&runtime.replacementCommandCalls==3&&runtime.replacementCommandCpu.count==unsigned(enabled)*3&&
        audit.clocks==unsigned(enabled)*6&&!audit.counterLocks,
        "timing OFF keeps omitted-image consumer rejection, without clocks or a new aggregation lock");
    }
    trackingCpuAudit=savedAudit;trackingCpuTimingOverride=savedTiming;
}
void replacementGraphicsCache(){
    namespace stc=xess_vk::sr_tail_commands;
    for(unsigned variant=0;variant<9;++variant){
        Fixture f;const auto r=setup(f);GameSrRuntime runtime(&f.state,true);
        xess_vk::CommandRecordingFacts facts;facts.command=bits(command);facts.rpRecordingGeneration=1;
        const auto bind=stc::bindDescriptorSets(VK_PIPELINE_BIND_POINT_GRAPHICS,r.pipelineLayout,0,1,&r.set,0,nullptr);
        const auto draw=stc::simple(stc::Kind::Draw);
        runtime.nativeTransaction.produce({0xF0F0,9},true,2);
        const auto image=di::imageReferences(f.state.descriptors,r.set).referencedImages[0];
        if(variant==8)runtime.nativeTransaction.produce(image,false,2);
        const auto other=handle<VkDescriptorSet>(0xCA80);VkDescriptorSetAllocateInfo otherInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        otherInfo.descriptorPool=r.pool;otherInfo.descriptorSetCount=1;otherInfo.pSetLayouts=&r.layout;di::setsAllocated(f.state.descriptors,&otherInfo,&other,VK_SUCCESS);
        VkDescriptorImageInfo otherImage{VK_NULL_HANDLE,r.view,VK_IMAGE_LAYOUT_GENERAL};VkWriteDescriptorSet otherWrite{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        otherWrite.dstSet=other;otherWrite.descriptorCount=1;otherWrite.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;otherWrite.pImageInfo=&otherImage;
        runtime.replacementCommand(facts,bind);runtime.replacementCommand(facts,draw);
        for(unsigned n=0;n<1000;++n){di::updateDescriptors(f.state.descriptors,1,&otherWrite,0,nullptr);runtime.replacementCommand(facts,bind);runtime.replacementCommand(facts,draw);}
        require(!runtime.nativeTransaction.stopped()&&runtime.replacementDescriptorBatches==0&&runtime.replacementIdentityBatches==1&&runtime.replacementSafeCacheHits==1001&&runtime.replacementMaskRefreshes==1,
            "1001 draws with unrelated descriptor churn require no full reference queries and only the initial mask refresh");
        if(variant==0||variant==8){runtime.nativeTransaction.produce(image,true,2);}
        if(variant==1){
            di::viewDestroying(f.state.descriptors,r.view);
        }
        if(variant==2){
            VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};ai.descriptorPool=r.pool;ai.descriptorSetCount=1;ai.pSetLayouts=&r.layout;
            di::setsFreed(f.state.descriptors,r.pool,1,&r.set,VK_SUCCESS);di::setsAllocated(f.state.descriptors,&ai,&r.set,VK_SUCCESS);
        }
        if(variant==3){
            VkDescriptorImageInfo info{VK_NULL_HANDLE,handle<VkImageView>(0xBAD0),VK_IMAGE_LAYOUT_GENERAL};
            VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=r.set;write.descriptorCount=1;write.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;write.pImageInfo=&info;
            di::updateDescriptors(f.state.descriptors,1,&write,0,nullptr);
        }
        if(variant==4){
            runtime.nativeTransaction.produce(image,true,2);runtime.nativeTransaction.use(image,true);runtime.nativeTransaction.produce(image,false,2);
        }
        if(variant==5){auto unsupported=bind;unsupported.extensionsOmitted=true;runtime.replacementCommand(facts,unsupported);}
        if(variant==6){
            const auto templ=handle<VkDescriptorUpdateTemplate>(0xCA90);
            VkDescriptorUpdateTemplateEntry entry{0,0,1,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,0,sizeof(VkDescriptorImageInfo)};
            VkDescriptorUpdateTemplateCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO};info.descriptorSetLayout=r.layout;
            info.templateType=VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET;info.descriptorUpdateEntryCount=1;info.pDescriptorUpdateEntries=&entry;
            di::templateCreated(f.state.descriptors,templ,&info,VK_SUCCESS);runtime.replacementCommand(facts,draw);
            require(!runtime.nativeTransaction.stopped(),"template creation alone cannot poison unrelated live bindings");
            const VkDescriptorImageInfo value{VK_NULL_HANDLE,handle<VkImageView>(0xBAD0),VK_IMAGE_LAYOUT_GENERAL};
            di::updateWithTemplate(f.state.descriptors,r.set,templ,&value,dm::TemplateUpdateKind::Core);
        }
        if(variant==7){
            auto ii=imageInfo();VkImage copiedImage{};returned=0xCA92;require(vkCreateImage(device,&ii,nullptr,&copiedImage)==VK_SUCCESS,"copy source image registered");
            VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=copiedImage;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=ii.format;vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            VkImageView copiedView{};returned=0xCA93;require(vkCreateImageView(device,&vi,nullptr,&copiedView)==VK_SUCCESS,"copy source view registered");
            const auto record=f.inventory->lookup(bits(copiedImage)).record;
            runtime.nativeTransaction.produce({record.image,record.generation},true,2);
            const auto source=handle<VkDescriptorSet>(0xCA91);
            VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};ai.descriptorPool=r.pool;ai.descriptorSetCount=1;ai.pSetLayouts=&r.layout;
            di::setsAllocated(f.state.descriptors,&ai,&source,VK_SUCCESS);
            const VkDescriptorImageInfo value{VK_NULL_HANDLE,copiedView,VK_IMAGE_LAYOUT_GENERAL};
            VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=source;write.descriptorCount=1;write.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;write.pImageInfo=&value;
            di::updateDescriptors(f.state.descriptors,1,&write,0,nullptr);runtime.replacementCommand(facts,draw);
            require(!runtime.nativeTransaction.stopped(),"writing another descriptor set cannot falsely stop this draw");
            VkCopyDescriptorSet copy{VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET};copy.srcSet=source;copy.dstSet=r.set;copy.descriptorCount=1;
            di::updateDescriptors(f.state.descriptors,0,nullptr,1,&copy);
        }
        runtime.replacementCommand(facts,draw);
        require(runtime.nativeTransaction.stopped()==(variant!=4),"content changes, view retirement, descriptor rewrites and set reuse invalidate cached safety; complete repair remains legal");
        require(runtime.replacementDescriptorBatches==unsigned(variant!=4),"only unsafe or unknown masks fall back to the full reference query; repaired contents stay on the bitmap path");
    }
}
void replacementCrossRecordingCache(){
    namespace stc=xess_vk::sr_tail_commands;Fixture f;const auto r=setup(f);GameSrRuntime runtime(&f.state,true);
    const auto bind=stc::bindDescriptorSets(VK_PIPELINE_BIND_POINT_GRAPHICS,r.pipelineLayout,0,1,&r.set,0,nullptr);
    const auto draw=stc::simple(stc::Kind::Draw);runtime.nativeTransaction.produce({0xF0F0,9},true,2);
    xess_vk::CommandRecordingFacts facts;facts.command=bits(command);facts.rpRecordingGeneration=1;
    runtime.replacementCommand(facts,bind);runtime.replacementCommand(facts,draw);
    const auto initial=runtime.replacementIdentityBatches;
    for(unsigned n=0;n<100;++n){facts.command+=0x100;facts.rpRecordingGeneration++;
        runtime.replacementCommand(facts,bind);runtime.replacementCommand(facts,draw);}
    require(runtime.replacementIdentityCacheHits==100&&runtime.replacementIdentityBatches==initial&&!runtime.nativeTransaction.stopped(),
        "coherent tokens avoid identity queries across command buffers and recordings");
    const VkDescriptorImageInfo value{VK_NULL_HANDLE,r.view,VK_IMAGE_LAYOUT_GENERAL};VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet=r.set;write.descriptorCount=1;write.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;write.pImageInfo=&value;
    di::updateDescriptors(f.state.descriptors,1,&write,0,nullptr);facts.rpRecordingGeneration++;
    runtime.replacementCommand(facts,bind);runtime.replacementCommand(facts,draw);
    require(runtime.replacementIdentityBatches==initial+1&&!runtime.nativeTransaction.stopped(),"dirty token misses and rebuilds before draw");
    di::viewDestroying(f.state.descriptors,r.view);facts.rpRecordingGeneration++;
    runtime.replacementCommand(facts,bind);runtime.replacementCommand(facts,draw);
    require(runtime.nativeTransaction.stopped(),"token cache never hides a real image lifetime error");
}
void replacementLazyRefresh(){
    namespace stc=xess_vk::sr_tail_commands;
    for(unsigned mutation=0;mutation<4;++mutation){Fixture f;const auto r=setup(f);GameSrRuntime runtime(&f.state,true);
        xess_vk::CommandRecordingFacts facts;facts.command=bits(command);facts.rpRecordingGeneration=1;
        const auto bind=stc::bindDescriptorSets(VK_PIPELINE_BIND_POINT_GRAPHICS,r.pipelineLayout,0,1,&r.set,0,nullptr);
        const auto draw=stc::simple(stc::Kind::Draw);runtime.nativeTransaction.produce({0xF0F0,9},true,2);
        runtime.replacementCommand(facts,bind);runtime.replacementCommand(facts,draw);
        const auto before=di::stats(f.state.descriptors).metadata;const auto* token=runtime.replacementGraphics.front().masks[0].published;
        const VkDescriptorImageInfo value{VK_NULL_HANDLE,r.view,VK_IMAGE_LAYOUT_GENERAL};
        if(mutation==1||mutation==2){
            const auto templ=handle<VkDescriptorUpdateTemplate>(0xCAB0);const VkDescriptorUpdateTemplateEntry entry{0,0,1,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,0,sizeof(value)};
            VkDescriptorUpdateTemplateCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO};info.descriptorSetLayout=r.layout;
            info.templateType=VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET;info.descriptorUpdateEntryCount=1;info.pDescriptorUpdateEntries=&entry;
            di::templateCreated(f.state.descriptors,templ,&info,VK_SUCCESS);
            di::updateWithTemplate(f.state.descriptors,r.set,templ,&value,mutation==1?dm::TemplateUpdateKind::Core:dm::TemplateUpdateKind::Khr);
        }else{
            auto destination=r.set;
            if(mutation==3){destination=handle<VkDescriptorSet>(0xCAB1);VkDescriptorSetAllocateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
                info.descriptorPool=r.pool;info.descriptorSetCount=1;info.pSetLayouts=&r.layout;di::setsAllocated(f.state.descriptors,&info,&destination,VK_SUCCESS);}
            VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=destination;write.descriptorCount=1;write.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;write.pImageInfo=&value;
            di::updateDescriptors(f.state.descriptors,1,&write,0,nullptr);
            if(mutation==3){VkCopyDescriptorSet copy{VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET};copy.srcSet=destination;copy.dstSet=r.set;copy.descriptorCount=1;
                di::updateDescriptors(f.state.descriptors,0,nullptr,1,&copy);}
        }
        require(token&&!token->read().known&&di::stats(f.state.descriptors).metadata.referenceMaskBuilds==before.referenceMaskBuilds,
            "ordinary/template/copy updates revoke the old draw mask without eagerly building a new one");
        runtime.replacementCommand(facts,draw);
        require(token->read().known&&!runtime.nativeTransaction.stopped()&&!runtime.replacementMaskFallbacks&&!runtime.replacementDescriptorBatches&&
            runtime.replacementMaskRefreshes==2&&di::stats(f.state.descriptors).metadata.referenceMaskBuilds==before.referenceMaskBuilds+1,
            "a draw with the same bound handle rebuilds the dirty publication once before considering a full fallback");
    }
}
void replacementStreamingLifetimes(){
    namespace stc=xess_vk::sr_tail_commands;Fixture f;const auto r=setup(f);GameSrRuntime runtime(&f.state,true);
    xess_vk::CommandRecordingFacts facts;facts.command=bits(command);facts.rpRecordingGeneration=1;
    const auto bind=stc::bindDescriptorSets(VK_PIPELINE_BIND_POINT_GRAPHICS,r.pipelineLayout,0,1,&r.set,0,nullptr);
    const auto draw=stc::simple(stc::Kind::Draw);runtime.nativeTransaction.produce({0xF0F0,9},true,2);
    runtime.replacementCommand(facts,bind);runtime.replacementCommand(facts,draw);
    const auto epoch=di::referenceMaskEpoch(f.state.descriptors);const auto before=di::stats(f.state.descriptors).metadata;
    for(unsigned n=0;n<1000;++n){auto info=imageInfo();VkImage image{};returned=0xCD00;
        require(vkCreateImage(device,&info,nullptr,&image)==VK_SUCCESS,"streaming image wrapper registered");
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=image;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=info.format;vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        VkImageView view{};returned=0xCD01;require(vkCreateImageView(device,&vi,nullptr,&view)==VK_SUCCESS,"streaming view wrapper registered");
        vkDestroyImageView(device,view,nullptr);vkDestroyImage(device,image,nullptr);
        runtime.replacementCommand(facts,bind);runtime.replacementCommand(facts,draw);
    }
    const auto after=di::stats(f.state.descriptors).metadata;
    require(di::referenceMaskEpoch(f.state.descriptors)==epoch&&after.referenceMaskBuilds==before.referenceMaskBuilds&&
        after.referenceLifetimeScans==before.referenceLifetimeScans&&after.referenceLifetimeInvalidations==before.referenceLifetimeInvalidations,
        "1000 unrelated streaming image/view lifetimes cause no epoch change, bitmap build or dependent scan");
    require(!runtime.nativeTransaction.stopped()&&runtime.replacementMaskDraws==1001&&runtime.replacementMaskFallbacks==0&&runtime.replacementMaskRefreshes==1&&runtime.replacementDescriptorBatches==0,
        "1001 graphics draws remain on the bitmap path through actual streaming create/destroy wrappers");
    di::viewDestroying(f.state.descriptors,r.view);runtime.replacementCommand(facts,draw);
    require(runtime.nativeTransaction.stopped()&&runtime.replacementMaskFallbacks==1&&runtime.replacementDescriptorBatches==1,
        "a bound un-watched view retirement still invalidates that set and reaches the full consumer guard");
}
// The binding-record hint must name exactly the record a full scan would,
// through hint collisions (more commands than hint slots), retirement and reuse.
void replacementBindingHints(){
    namespace stc=xess_vk::sr_tail_commands;Fixture f;const auto r=setup(f);GameSrRuntime runtime(&f.state,true);
    runtime.nativeTransaction.produce({0xF0F0,9},true,2);
    const auto bind=stc::bindDescriptorSets(VK_PIPELINE_BIND_POINT_GRAPHICS,r.pipelineLayout,0,1,&r.set,0,nullptr);
    const auto draw=stc::simple(stc::Kind::Draw);
    constexpr unsigned commands=300; // more than the 256 hint slots
    const auto commandAt=[](unsigned n){return 0xC00000ull+0x40ull*n;};
    const auto record=[&](std::uint64_t command,std::uint64_t recording){
        xess_vk::CommandRecordingFacts facts;facts.command=command;facts.rpRecordingGeneration=recording;
        runtime.replacementCommand(facts,bind);runtime.replacementCommand(facts,draw);return facts;};
    const auto owned=[&](std::uint64_t command){const GameSrRuntime::GraphicsBindings* found=nullptr;unsigned matches{};
        for(const auto& g:runtime.replacementGraphics)if(g.command==command){found=&g;++matches;}
        require(matches<=1,"a command never owns two binding records");return found;};
    std::vector<std::uint64_t> recordings(commands);
    for(unsigned n=0;n<commands;++n){recordings[n]=n+1;(void)record(commandAt(n),recordings[n]);}
    for(unsigned pass=0;pass<2;++pass)for(unsigned n=0;n<commands;++n)(void)record(commandAt(n),recordings[n]);
    const auto verify=[&](const char* step){for(unsigned n=0;n<commands;++n){const auto* g=owned(commandAt(n));
        require(g&&g->recording==recordings[n]&&g->sets[0].handle==bits(r.set),step);}};
    verify("colliding hints still resolve every command to its own record");
    require(runtime.replacementGraphics.size()==commands&&!runtime.nativeTransaction.stopped()&&!runtime.guardSuppressedCommands,"no duplicate records and no false stop");
    for(unsigned n=0;n<commands;n+=3){xess_vk::CommandRecordingFacts facts;facts.command=commandAt(n);facts.rpRecordingGeneration=recordings[n];
        runtime.retire(facts,sro::Discard::FreeObserved);require(!owned(commandAt(n)),"retired record is cleared");}
    for(unsigned n=0;n<commands;n+=3)(void)record(commandAt(n)+0x20,5000+n); // reuse the freed records
    for(unsigned n=0;n<commands;n+=3){recordings[n]=9000+n;(void)record(commandAt(n),recordings[n]);} // stale hints must not revive
    verify("re-recorded commands after record reuse resolve to their own records");
    for(unsigned n=0;n<commands;n+=3){const auto* g=owned(commandAt(n)+0x20);require(g&&g->recording==5000+n,"reusing commands keep their records");}
    require(runtime.replacementGraphics.size()==commands+commands/3,"freed records are reused before growing");
    std::array<bool,256> used{};unsigned distinct{};
    for(unsigned n=0;n<48;++n){auto& slot=used[GameSrRuntime::graphicsHintIndex(commandAt(n))];distinct+=!slot;slot=true;}
    for(unsigned n=0;n<48;++n)(void)record(commandAt(n),recordings[n]); // later commands may have taken these hints
    const auto scans=runtime.replacementGraphicsScans;for(unsigned n=0;n<48;++n)(void)record(commandAt(n),recordings[n]);
    require(distinct<48||runtime.replacementGraphicsScans==scans,"warm commands with distinct hints need no record scan");
    verify("hint warm-up leaves every record unchanged");
}
void replacementWideMaskFallback(){
    Fixture f;const auto r=setup(f);GameSrRuntime runtime(&f.state,true);
    xess_vk::CommandRecordingFacts facts;facts.command=bits(command);facts.rpRecordingGeneration=1;
    for(unsigned n=0;n<65;++n)runtime.nativeTransaction.produce({0xDA000+n,1},true,2);
    runtime.replacementCommand(facts,xess_vk::sr_tail_commands::bindDescriptorSets(VK_PIPELINE_BIND_POINT_GRAPHICS,r.pipelineLayout,0,1,&r.set,0,nullptr));
    runtime.replacementCommand(facts,xess_vk::sr_tail_commands::simple(xess_vk::sr_tail_commands::Kind::Draw));
    require(!runtime.nativeTransaction.stopped()&&!runtime.nativeTransaction.counts().capacity&&runtime.replacementMaskFallbacks==1&&runtime.replacementDescriptorBatches==1,
        "more than 64 watched images use full queries without stopping SR or shrinking tracking capacity");
    runtime.nativeTransaction.produce(di::imageReferences(f.state.descriptors,r.set).referencedImages[0],true,2);
    runtime.replacementCommand(facts,xess_vk::sr_tail_commands::simple(xess_vk::sr_tail_commands::Kind::Draw));
    require(runtime.nativeTransaction.stopped(),"wide-mask fallback still detects a real missing consumer");
}
void productTailSeparation(){
    Fixture f;const auto r=setup(f);f.state.srObservationRequested=true;
    std::unique_ptr<ro::Context,decltype(&ro::destroy)> context(ro::create(device,nullptr),ro::destroy);f.state.renderPass=context.get();
    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};ro::poolCreated(context.get(),r.commandPool,&pi,VK_SUCCESS);
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ai.commandPool=r.commandPool;ai.commandBufferCount=1;ro::allocate(context.get(),&ai,&command,VK_SUCCESS);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};ro::begin(context.get(),command,&bi,VK_SUCCESS);
    GameSrRuntime runtime(&f.state,true);f.state.srRuntime=&runtime;srTailWindowOverride=1;
    struct Cleanup{DeviceState& state;xess_vk::sr_native_resolution::Reason reason=xess_vk::sr_native_resolution::control.reason.load();~Cleanup(){state.srRuntime=nullptr;state.srTailCommands=nullptr;state.renderPass=nullptr;srTailWindowOverride=-1;xess_vk::sr_native_resolution::control.reason=reason;}}cleanup{f.state};
    unsigned factories{};require(!srTailCommand(&f.state,command,[&]{++factories;return xess_vk::sr_tail_commands::simple(xess_vk::sr_tail_commands::Kind::Draw);})&&!factories,
        "default product route never constructs a diagnostic command copy");
    const auto image=di::imageReferences(f.state.descriptors,r.set).referencedImages[0];runtime.nativeTransaction.produce(image,true,2);
    vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,r.pipelineLayout,0,1,&r.set,0,nullptr);
    static unsigned draws;draws=0;f.state.cmdDraw=+[](VkCommandBuffer,unsigned,unsigned,unsigned,unsigned){++draws;};
    vkCmdDraw(command,3,1,0,0);
    require(!draws&&runtime.nativeTransaction.stopped()&&runtime.lastReplacementFault.image==image,
        "real graphics bind/draw wrappers still suppress a missing-image read with tail diagnostics disabled");
    runtime.nativeTransaction={};runtime.nativeTransaction.produce(image,false,2);vkCmdDraw(command,3,1,0,0);
    require(draws==1&&!runtime.nativeTransaction.stopped(),"safe product draw forwards exactly once without diagnostic capture");
    f.state.srTailDiagnostics=true;const auto facts=ro::recordingFacts(context.get(),command);
    xess_vk::sr_tail_commands::Observer observer(7);xess_vk::sr_tail_commands::Tail tail;tail.recording=xess_vk::sr_tail_commands::key(facts);tail.frame={1,{1,1,false},0,true};tail.pass=xess_vk::sr_tail_commands::Pass::Accumulate;tail.dispatchOrdinal=1;
    require(observer.tail(tail),"explicit tail fixture armed");f.state.srTailCommands=&observer;
    srTailCommand(&f.state,command,[&]{++factories;return xess_vk::sr_tail_commands::simple(xess_vk::sr_tail_commands::Kind::Draw);});
    require(factories==1&&observer.stats().commands==1,"explicit diagnostics still capture the command exactly once");
}
void replacementLiveRecordingBudgets(){
    namespace stc=xess_vk::sr_tail_commands;
    Fixture f;const auto r=setup(f);GameSrRuntime runtime(&f.state,true);
    xess_vk::CommandRecordingFacts facts;facts.rpRecordingGeneration=1;facts.pool=0xEE00;
    const auto bind=stc::bindDescriptorSets(VK_PIPELINE_BIND_POINT_GRAPHICS,r.pipelineLayout,0,1,&r.set,0,nullptr);
    for(unsigned n=0;n<257;++n){facts.command=0xD000+n;
        runtime.push(facts,VK_SHADER_STAGE_COMPUTE_BIT);runtime.replacementCommand(facts,bind);}
    require(!runtime.stopped&&!runtime.nativeTransaction.stopped()&&runtime.guards.size()==257&&runtime.replacementGraphics.size()==257,
        "live recordings exceeding the old 16/128 diagnostic budgets retain all identities without stopping SR");
    for(auto& g:runtime.guards)require(g.pushSeen&&g.recording.rpRecordingGeneration==1,"push state survives record growth");
    for(auto& g:runtime.replacementGraphics)require(g.sets[0].handle==bits(r.set),"descriptor state survives record growth");
    // Follow the actual command-pool retirement path, then prove slots are reused.
    runtime.pool(facts.pool,sro::Discard::SuccessfulPoolReset);
    const auto guardSize=runtime.guards.size(),graphicsSize=runtime.replacementGraphics.size();
    facts.command=0xF000;facts.rpRecordingGeneration=2;
    runtime.push(facts,VK_SHADER_STAGE_COMPUTE_BIT);runtime.replacementCommand(facts,bind);
    require(runtime.guards.size()==guardSize&&runtime.replacementGraphics.size()==graphicsSize,
        "retired recording slots are reused without unbounded per-frame growth");
}
void replacementGraphicsBindings(){
    namespace stc=xess_vk::sr_tail_commands;
    for(unsigned variant=0;variant<13;++variant){
        Fixture f;const auto r=setup(f);GameSrRuntime runtime(&f.state,true);
        xess_vk::CommandRecordingFacts facts;facts.command=bits(command);facts.rpRecordingGeneration=1;
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};ai.descriptorPool=r.pool;
        ai.descriptorSetCount=1;ai.pSetLayouts=&r.layout;const auto empty=handle<VkDescriptorSet>(0xB100);
        di::setsAllocated(f.state.descriptors,&ai,&empty,VK_SUCCESS);
        auto bind=stc::bindDescriptorSets(VK_PIPELINE_BIND_POINT_GRAPHICS,r.pipelineLayout,0,1,&empty,0,nullptr);
        const auto draw=stc::simple(stc::Kind::Draw);
        runtime.replacementCommand(facts,bind);runtime.replacementCommand(facts,draw);
        require(!runtime.nativeTransaction.stopped(),"unknown graphics input before any core content does not stop the experiment");
        const auto image=di::diagnosticSetImages(f.state.descriptors,r.set).bindings[0].image;
        if(variant==1)di::setUnsupported(f.state.descriptors,empty,dm::Issue::Capacity);
        runtime.nativeTransaction.produce(variant==4||variant==10?image:dm::Key{0xB200,1},variant!=1,2);
        if(variant!=1&&variant!=9){
            std::array<std::uint32_t,20> offsets{};
            bind=stc::bindDescriptorSets(VK_PIPELINE_BIND_POINT_GRAPHICS,r.pipelineLayout,0,1,&r.set,
                variant==3?unsigned(offsets.size()):0,offsets.data());
            if(variant==2){bind.descriptors.setCount=9;bind.truncated=true;}
            if(variant==3)require(bind.truncated&&bind.descriptors.storedSets==1,"dynamic-offset prefix can truncate with all image-bearing set identities present");
            if(variant==4||variant==7||variant==8){
                VkBindDescriptorSetsInfo info{VK_STRUCTURE_TYPE_BIND_DESCRIPTOR_SETS_INFO};info.stageFlags=VK_SHADER_STAGE_ALL_GRAPHICS;
                if(variant==7)info.stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT;
                if(variant==8)info.stageFlags=VK_SHADER_STAGE_MESH_BIT_EXT;
                info.layout=r.pipelineLayout;info.descriptorSetCount=1;info.pDescriptorSets=&r.set;
                bind=stc::bindDescriptorSets2(&info);
            }
            if(variant==5)bind.descriptors.sets[0]=0xB300;
            runtime.replacementCommand(facts,bind);
            if(variant>=10){
                VkDescriptorImageInfo imageInfo{VK_NULL_HANDLE,variant==12?handle<VkImageView>(0xB400):r.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};w.dstSet=r.set;w.descriptorCount=1;
                w.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;w.pImageInfo=&imageInfo;
                di::updateDescriptors(f.state.descriptors,1,&w,0,nullptr);
            }
            if(variant==6){
                di::setsFreed(f.state.descriptors,r.pool,1,&r.set,VK_SUCCESS);
                di::setsAllocated(f.state.descriptors,&ai,&r.set,VK_SUCCESS);
            }
        }
        const auto callsBefore=driverCalls;runtime.replacementCommand(facts,draw);
        const bool safe=variant==0||variant==3||variant==9||variant==11;
        require(runtime.nativeTransaction.stopped()!=safe,"graphics guard recovers after full rebind, but refuses missing identities, stale generations and actual reads");
        require(driverCalls==callsBefore,"graphics guard only checks copied CPU metadata");
        if(variant==1||variant==5||variant==6){
            require(runtime.replacementFault.descriptorSet.handle&&runtime.replacementFault.issues,
                "unknown graphics fault retains the failing set identity and nonzero issue bits");
        }
        if(variant==1)require(runtime.replacementFault.setIssues&dm::bit(dm::Issue::Capacity),"fault preserves structural set capacity rather than attributing it to unused slots");
        if(variant==1){
            runtime.replacementFault={};
            require(runtime.nativeTransaction.stopped()&&runtime.lastReplacementFaultCode==xess_vk::sr_replacement::Fault::Consumer&&
                (runtime.lastReplacementFault.setIssues&dm::bit(dm::Issue::Capacity)),
                "last fault remains visible without any hidden native FSR retry");
        }
        if(variant==4||variant==10)require(std::string_view(runtime.replacementFault.origin)=="graphics_draw"&&runtime.replacementFault.image==image,
            "API2 full-graphics bindings retain real core-image consumer detection");
    }
}
void replacementThirtyThirdImage(){
    for(unsigned imageCount : {33u,34u,64u}){
    const unsigned bindingCount=imageCount==64?64:imageCount+22;
    Fixture f;const auto r=setup(f);
    auto info=imageInfo();VkImage lastImage{};returned=0xBC001;require(vkCreateImage(device,&info,nullptr,&lastImage)==VK_SUCCESS,"boundary image created");
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=lastImage;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;
    vi.format=info.format;vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};VkImageView lastView{};returned=0xBC002;
    require(vkCreateImageView(device,&vi,nullptr,&lastView)==VK_SUCCESS,"boundary view created");
    std::array<VkDescriptorSetLayoutBinding,64> bindings{};std::array<VkDescriptorUpdateTemplateEntry,64> entries{};
    std::array<unsigned char,64*32> data{};
    for(unsigned n=0;n<bindingCount;++n){
        const auto type=n<imageCount?VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[n]={n,type,1,VK_SHADER_STAGE_ALL_GRAPHICS,nullptr};entries[n]={n,0,1,type,n*32,32};
        if(n<imageCount){const VkDescriptorImageInfo value{VK_NULL_HANDLE,n==imageCount-1?lastView:r.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            std::memcpy(data.data()+n*32,&value,sizeof(value));}
        else {const VkDescriptorBufferInfo value{handle<VkBuffer>(99),0,64};std::memcpy(data.data()+n*32,&value,sizeof(value));}
    }
    const auto layout=handle<VkDescriptorSetLayout>(0xBC003);VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount=bindingCount;li.pBindings=bindings.data();require(di::setLayoutCreated(f.state.descriptors,layout,&li,VK_SUCCESS).accepted(),"33-image graphics layout admitted");
    const auto set=handle<VkDescriptorSet>(0xBC004);VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool=r.pool;ai.descriptorSetCount=1;ai.pSetLayouts=&layout;di::setsAllocated(f.state.descriptors,&ai,&set,VK_SUCCESS);
    const auto temp=handle<VkDescriptorUpdateTemplate>(0xBC005);VkDescriptorUpdateTemplateCreateInfo ti{VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO};
    ti.descriptorSetLayout=layout;ti.descriptorUpdateEntryCount=bindingCount;ti.pDescriptorUpdateEntries=entries.data();
    di::templateCreated(f.state.descriptors,temp,&ti,VK_SUCCESS);di::updateWithTemplate(f.state.descriptors,set,temp,data.data());
    const auto snapshot=di::imageReferences(f.state.descriptors,set);
    require(snapshot.imageReferencesKnown&&!snapshot.issues&&snapshot.referencedCount==imageCount,"production adapter retains the whole 33-image membership set");
    xess_vk::CommandRecordingFacts facts;facts.command=bits(command);facts.rpRecordingGeneration=1;
    const auto bind=xess_vk::sr_tail_commands::bindDescriptorSets(VK_PIPELINE_BIND_POINT_GRAPHICS,r.pipelineLayout,0,1,&set,0,nullptr);
    const auto draw=xess_vk::sr_tail_commands::simple(xess_vk::sr_tail_commands::Kind::Draw);
    const auto record=f.inventory->lookup(bits(lastImage)).record;const dm::Key core{record.image,record.generation};
    GameSrRuntime consumer(&f.state,true);consumer.nativeTransaction.produce(core,true,2);consumer.replacementCommand(facts,bind);consumer.replacementCommand(facts,draw);
    require(consumer.nativeTransaction.stopped()&&consumer.replacementFault.image==core&&
        std::string_view(consumer.replacementFault.origin)=="graphics_draw","a core image referenced only by descriptor 33 still triggers the real consumer guard");
    GameSrRuntime unrelated(&f.state,true);unrelated.nativeTransaction.produce({0xBC006,1});unrelated.replacementCommand(facts,bind);unrelated.replacementCommand(facts,draw);
    require(!unrelated.nativeTransaction.stopped(),"complete 33-image graphics metadata does not poison unrelated core content");
    char benchmark[2]{};
    if(imageCount==33&&GetEnvironmentVariableA("XESS_SR_GUARD_BENCHMARK",benchmark,sizeof(benchmark))){
        constexpr unsigned iterations=30000;
        for(unsigned run=0;run<3;++run){
            const auto start=std::chrono::steady_clock::now();
            for(unsigned n=0;n<iterations;++n){unrelated.replacementCommand(facts,bind);unrelated.replacementCommand(facts,draw);}
            const auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();
            std::cout<<"{\"guard_benchmark\":true,\"run\":"<<run<<",\"bind_draw_pairs\":"<<iterations<<",\"total_ns\":"<<ns<<"}\n";
        }
        require(!unrelated.nativeTransaction.stopped(),"guard benchmark preserves the unrelated-content transaction");
    }
    }
}
void replacementPreExecuteFallback(){
    namespace mo=xess_vk::resource_memory_observer;namespace rm=xess_vk::resource_memory_facts;
    Fixture f;const auto existing=setup(f);
    ro::InventoryReader reader{+[](void* p,std::uint64_t h) noexcept {return static_cast<xess_vk::image_inventory::Inventory<>*>(p)->lookup(h);},f.inventory.get()};
    std::unique_ptr<ro::Context,decltype(&ro::destroy)> rp(ro::create(device,nullptr,reader),ro::destroy);f.state.renderPass=rp.get();
    std::array<VkImage,2> images{};std::array<VkImageView,2> views{};std::array<VkImageCreateInfo,2> infos{};
    for(unsigned n=0;n<2;++n){auto& i=infos[n];i={VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};i.imageType=VK_IMAGE_TYPE_2D;
        i.format=n?VK_FORMAT_R16G16B16A16_SFLOAT:VK_FORMAT_B10G11R11_UFLOAT_PACK32;i.extent=n?VkExtent3D{128,64,1}:VkExtent3D{64,32,1};
        i.mipLevels=i.arrayLayers=1;i.samples=VK_SAMPLE_COUNT_1_BIT;i.tiling=VK_IMAGE_TILING_OPTIMAL;i.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        i.usage=n?VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT:VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        i.usage|=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        returned=0xD100+n;require(vkCreateImage(device,&i,nullptr,&images[n])==VK_SUCCESS,"fallback image registered");
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=images[n];vi.format=i.format;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        returned=0xD200+n;require(vkCreateImageView(device,&vi,nullptr,&views[n])==VK_SUCCESS,"fallback view registered");
    }
    VkPhysicalDeviceMemoryProperties properties{};properties.memoryTypeCount=properties.memoryHeapCount=1;
    properties.memoryTypes[0]={VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,0};properties.memoryHeaps[0]={1ull<<30,VK_MEMORY_HEAP_DEVICE_LOCAL_BIT};
    rm::Config config;config.deviceScope=rm::DeviceScope::SinglePhysicalDevice;config.bindingCoverage=rm::BindingCoverage::CompleteOrdinaryCalls;config.bufferImageGranularity=64;
    std::unique_ptr<mo::Context,decltype(&mo::destroy)> memory(mo::create(device,properties,config,nullptr),mo::destroy);f.state.resourceMemory=memory.get();
    for(unsigned n=0;n<2;++n){const auto record=f.inventory->lookup(bits(images[n])).record;VkMemoryRequirements req{32768,64,1};
        mo::imageCreated(memory.get(),record,&infos[n],VK_SUCCESS,&req);const auto allocation=handle<VkDeviceMemory>(0xD300+n);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=65536;mo::memoryAllocated(memory.get(),allocation,&ai,VK_SUCCESS);
        VkBindImageMemoryInfo bind{VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO};bind.image=images[n];bind.memory=allocation;
        mo::afterBind(memory.get(),mo::beforeImageBind(memory.get(),rm::BindApi::ImageCore2,1,&bind),VK_SUCCESS);
    }
    const std::array<VkDescriptorSetLayoutBinding,2> bindings{{{1,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
        {3,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}}};
    const auto layout=handle<VkDescriptorSetLayout>(0xD400);VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};li.bindingCount=2;li.pBindings=bindings.data();
    require(di::setLayoutCreated(f.state.descriptors,layout,&li,VK_SUCCESS).accepted(),"fallback color layout");
    const auto set=handle<VkDescriptorSet>(0xD401);VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};ai.descriptorPool=existing.pool;ai.descriptorSetCount=1;ai.pSetLayouts=&layout;
    di::setsAllocated(f.state.descriptors,&ai,&set,VK_SUCCESS);
    const std::array<VkDescriptorImageInfo,2> iv{{{VK_NULL_HANDLE,views[1],VK_IMAGE_LAYOUT_GENERAL},{VK_NULL_HANDLE,views[0],VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}}};
    std::array<VkWriteDescriptorSet,2> writes{};for(unsigned n=0;n<2;++n){auto& w=writes[n];w={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};w.dstSet=set;w.dstBinding=bindings[n].binding;w.descriptorCount=1;w.descriptorType=bindings[n].descriptorType;w.pImageInfo=&iv[n];}
    di::updateDescriptors(f.state.descriptors,2,writes.data(),0,nullptr);const auto snapshot=di::diagnosticSetImages(f.state.descriptors,set);
    require(snapshot.count==2&&!snapshot.issues,"both fallback image identities are fully typed and current");
    const auto pool=handle<VkCommandPool>(0xD500);VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};ro::poolCreated(rp.get(),pool,&pi,VK_SUCCESS);
    VkCommandBufferAllocateInfo ci{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ci.commandPool=pool;ci.commandBufferCount=1;ro::allocate(rp.get(),&ci,&command,VK_SUCCESS);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;ro::begin(rp.get(),command,&begin,VK_SUCCESS);
    std::array<VkImageMemoryBarrier,2> barriers{};for(unsigned n=0;n<2;++n){auto& b=barriers[n];b={VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.image=images[n];b.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout=n?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;}
    ro::explicitImageBarriers(rp.get(),command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,2,barriers.data());
    const auto facts=ro::dispatchPoint(rp.get(),command);GameSrRuntime runtime(&f.state,true);
    runtime.features[0]=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_BLIT_SRC_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    runtime.features[3]=VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT|VK_FORMAT_FEATURE_BLIT_DST_BIT;
    runtime.nativeTransaction.begin(603,facts.command,facts.rpRecordingGeneration);
    for(unsigned p=2;p<=8;++p)require(runtime.nativeTransaction.core(p,603,facts.command,facts.rpRecordingGeneration),"fallback reproduces all seven native commands already omitted");
    dm::DispatchEvidence evidence;evidence.metadataAccepted=true;evidence.count=2;evidence.bindings[0]=snapshot.bindings[0];evidence.bindings[1]=snapshot.bindings[1];
    const xess_vk::FrameTimingAssociation frame{603,{603,1,false},0,true};runtime.replacementPlanFrame=frame;

    runtime.replacementTerminalReason="menu_temporal_refusal";runtime.replacementTerminalResult={sro::Status::SdkFailureCommandsUncertain};
    require(!runtime.replacementColorFallback(facts,evidence,frame),"a possibly written SDK failure never enters the pre-Execute fallback");
    runtime.replacementTerminalResult={sro::Status::FrameUnverified};auto wrong=frame;++wrong.engineFrame;
    require(!runtime.replacementColorFallback(facts,evidence,wrong),"fallback cannot borrow another frame's color plan");
    auto changed=evidence;changed.bindings[1].declaredLayout=VK_IMAGE_LAYOUT_GENERAL;
    require(!runtime.replacementColorFallback(facts,changed,frame),"fallback rejects a color layout that differs from its current recorded prefix");
    runtime.holdOutput=true;runtime.nativeTransaction.produce(evidence.bindings[1].image,true,7);
    require(!runtime.replacementColorFallback(facts,evidence,frame)&&runtime.holdOutput&&std::string_view(runtime.replacementFallbackRefusal)=="color_source_omitted",
        "a missing current color source never unlocks hold or records a transfer");
    runtime.nativeTransaction.retire(evidence.bindings[1].image);
    static unsigned blits{},barrierCalls{};blits=barrierCalls=0;
    f.state.cmdPipelineBarrier=+[](VkCommandBuffer b,VkPipelineStageFlags,VkPipelineStageFlags,VkDependencyFlags,unsigned,const VkMemoryBarrier*,unsigned,const VkBufferMemoryBarrier*,unsigned n,const VkImageMemoryBarrier*){
        require(b==command&&n==2&&srSdkDepth&&srTailInjectedDepth,"fallback barriers use the same game CB with injection excluded");++barrierCalls;};
    f.state.cmdBlitImage=+[](VkCommandBuffer b,VkImage source,VkImageLayout sl,VkImage destination,VkImageLayout dl,unsigned n,const VkImageBlit* r,VkFilter filter){
        require(b==command&&bits(source)==0xD100&&bits(destination)==0xD101&&n==1&&filter==VK_FILTER_LINEAR&&
            sl==VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL&&dl==VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL&&r->srcOffsets[1].x==64&&r->dstOffsets[1].x==128,
            "production fallback fills the same plan's complete final output");++blits;};
    ro::explicitBarrier(rp.get(),command);const auto fallbackFacts=ro::dispatchPoint(rp.get(),command);
    const auto fallback=runtime.replacementColorFallback(fallbackFacts,evidence,frame);
    if(!fallback){std::cerr<<"fallback refusal: "<<runtime.replacementFallbackRefusal<<"\n";
        const auto q=ro::sourceLayoutQuery(rp.get());for(const auto& b:snapshot.bindings)if(b.image.handle){
            const auto l=q.resolve(q.user,facts,{b.image.handle,b.image.generation},b.range,b.declaredLayout);
            const auto live=f.inventory->lookup(b.image.handle);const auto view=ro::diagnosticViewSnapshot(rp.get(),reinterpret_cast<VkImageView>(b.view.handle));
            std::cerr<<"layout "<<b.binding<<" image="<<b.image.handle<<" gen="<<b.image.generation<<" live="<<int(live.status)<<" live_gen="<<live.record.generation<<" view_known="<<view.known<<" view_issues="<<view.issues<<" queried="<<l.queried<<" resolved="<<l.recordedLayoutResolved<<" issues="<<l.issues<<" recorded="<<l.recordedLayout<<" declared="<<b.declaredLayout<<"\n";}}
    require(fallback&&!runtime.holdOutput&&blits==1&&barrierCalls==2,"menu pre-Execute refusal uses the pinned terminal color contract despite incomplete attachment-prefix history");
    require(!srSdkDepth&&!srTailInjectedDepth&&runtime.replacementBlits==1&&runtime.replacementExecutedFrame==603,
        "fallback balances scopes and requests retirement only at the next frame boundary");
    runtime.nativeTransaction.complete(true);require(!runtime.nativeTransaction.stopped(),"valid color fallback completes this transaction without a fatal stop");
    runtime.nativeTransaction.begin(604,facts.command,facts.rpRecordingGeneration);
    require(runtime.nativeTransaction.core(2,604,facts.command,facts.rpRecordingGeneration),"transition blit does not permit FSR on the next frame");
    f.state.resourceMemory=nullptr;f.state.renderPass=nullptr;
}
void run(){
    Fixture f(false);auto* context=ro::create(device,nullptr);require(context!=nullptr,"interval context created");
    std::unique_ptr<ro::Context,decltype(&ro::destroy)> owned(context,ro::destroy);f.state.renderPass=context;
    f.state.srRuntimeRequested=true;srTailWindowOverride=0;
    VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};image.imageType=VK_IMAGE_TYPE_2D;image.format=VK_FORMAT_D32_SFLOAT_S8_UINT;
    image.extent={64,32,1};image.mipLevels=image.arrayLayers=1;image.samples=VK_SAMPLE_COUNT_1_BIT;
    image.usage=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;image.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    xess_vk::image_inventory::Record record;record.image=bits(depth);record.generation=1;
    record.description={std::uint32_t(image.format),64,32,1,1,1,std::uint32_t(image.samples),image.usage,
        image.flags,std::uint32_t(image.tiling),std::uint32_t(image.sharingMode),std::uint32_t(image.initialLayout)};
    ro::imageCreated(context,&image,depth,VK_SUCCESS,&record);
    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pool.queueFamilyIndex=0;
    const auto poolHandle=handle<VkCommandPool>(0x9030);ro::poolCreated(context,poolHandle,&pool,VK_SUCCESS);
    VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};allocation.commandPool=poolHandle;allocation.commandBufferCount=1;
    ro::allocate(context,&allocation,&command,VK_SUCCESS);
    f.state.cmdCopyImage=copy;f.state.cmdClearDepthStencilImage=clear;f.state.cmdClearColorImage=color;f.state.cmdResolveImage=resolve;
    f.state.cmdResolveImage2=modern<VkResolveImageInfo2>;f.state.cmdResolveImage2Khr=modern<VkResolveImageInfo2>;
    f.state.cmdCopyBufferToImage=bufferToImage;f.state.cmdCopyImageToBuffer=imageToBuffer;
    f.state.cmdCopyBufferToImage2=modern<VkCopyBufferToImageInfo2>;f.state.cmdCopyBufferToImage2Khr=modern<VkCopyBufferToImageInfo2>;
    f.state.cmdCopyImageToBuffer2=modern<VkCopyImageToBufferInfo2>;f.state.cmdCopyImageToBuffer2Khr=modern<VkCopyImageToBufferInfo2>;
    for(unsigned mode=0;mode<28;++mode){
        const auto variant=mode%14;
        ro::reset(context,command,VK_SUCCESS);VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        ro::begin(context,command,&begin,VK_SUCCESS);const auto read=ro::dispatchPoint(context,command);calls=0;
        f.state.srRuntimeRequested=mode>=14&&variant!=12;
        f.state.srObservationRequested=mode<14&&variant!=12;
        VkClearDepthStencilValue value{};VkClearColorValue colorValue{};
        VkResolveImageInfo2 ri{VK_STRUCTURE_TYPE_RESOLVE_IMAGE_INFO_2};ri.srcImage=other;ri.dstImage=depth;
        VkCopyBufferToImageInfo2 bi{VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2};bi.dstImage=depth;
        VkCopyImageToBufferInfo2 ib{VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2};ib.srcImage=depth;
        switch(variant){
        case 0:vkCmdCopyImage(command,depth,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,other,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,nullptr);break;
        case 1:expectedInfo=&value;vkCmdClearDepthStencilImage(command,depth,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&value,0,nullptr);break;
        case 2:vkCmdResolveImage(command,other,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,depth,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,nullptr);break;
        case 3:expectedInfo=&ri;vkCmdResolveImage2(command,&ri);break;
        case 4:expectedInfo=&ri;vkCmdResolveImage2KHR(command,&ri);break;
        case 5:vkCmdCopyBufferToImage(command,handle<VkBuffer>(1),depth,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,nullptr);break;
        case 6:vkCmdCopyImageToBuffer(command,depth,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,handle<VkBuffer>(1),0,nullptr);break;
        case 7:expectedInfo=&bi;vkCmdCopyBufferToImage2(command,&bi);break;
        case 8:expectedInfo=&bi;vkCmdCopyBufferToImage2KHR(command,&bi);break;
        case 9:expectedInfo=&ib;vkCmdCopyImageToBuffer2(command,&ib);break;
        case 10:expectedInfo=&ib;vkCmdCopyImageToBuffer2KHR(command,&ib);break;
        case 11:expectedInfo=&colorValue;vkCmdClearColorImage(command,other,VK_IMAGE_LAYOUT_GENERAL,&colorValue,0,nullptr);break;
        case 12:vkCmdCopyImage(command,depth,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,other,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,nullptr);break;
        case 13:{SrTailInjectedScope injected;vkCmdCopyImage(command,depth,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,other,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,nullptr);break;}
        }
        require(calls==1&&GetLastError()==43210,"exactly one downstream call and original LastError preserved");
        const auto use=ro::dispatchPoint(context,command);const auto interval=ro::imageInterval(context,read,use,{bits(depth),1});
        require(interval.queried&&!interval.issues&&interval.untouched==(variant>=11),"production wrappers record transfer/clear while excluding unrelated, OFF and injected commands");
        auto foreign=read;++foreign.device;require(!ro::imageInterval(context,foreign,use,{bits(depth),1}).untouched,"foreign device cannot query the same numeric command handle");
    }
    ro::reset(context,command,VK_SUCCESS);VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    ro::begin(context,command,&begin,VK_SUCCESS);const auto read=ro::dispatchPoint(context,command);calls=0;
    VkClearDepthStencilValue clearValue{};expectedInfo=&clearValue;f.state.srRuntimeRequested=true;
    {SrSdkScope sdk;vkCmdClearDepthStencilImage(command,depth,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&clearValue,0,nullptr);}
    const auto use=ro::dispatchPoint(context,command);const auto interval=ro::imageInterval(context,read,use,{bits(depth),1});
    require(calls==1&&interval.queried&&interval.untouched&&!interval.issues,"SDK-internal commands reach native Vulkan exactly once without contaminating the game's source interval");
    require(!srSdkDepth&&!srTailInjectedDepth,"SDK exclusion leaves both thread-local scopes balanced");
    f.state.renderPass=nullptr;srTailWindowOverride=-1;
}
}

namespace {
__declspec(noinline) int noRuntimeDiagnosticsFixture(){
    using namespace descriptor_layer_fixture;
    SetEnvironmentVariableW(L"XESS_DIAGNOSTIC_MODE",L"0");
    require(!diagnosticLoggingEnabled(),"runtime diagnostics explicitly disabled");
    Fixture f(false);initializeMeasurements(&f.state);
    require(!f.state.measurements&&!f.state.measurementSnapshot,"real device initialization allocates no telemetry window or snapshot");
    static unsigned presents;presents=0;
    f.state.present=+[](VkQueue,const VkPresentInfoKHR*){++presents;SetLastError(54321);return VK_SUBOPTIMAL_KHR;};
    Dispatch queueDispatch{&marker};auto queue=reinterpret_cast<VkQueue>(&queueDispatch);
    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    require(vkQueuePresentKHR(queue,&present)==VK_SUBOPTIMAL_KHR&&presents==1,"zero diagnostics preserves the real present wrapper's driver result and call count");
    require(!f.state.presents&&!f.state.presentationAttempts,"default present returns without accumulating diagnostic sampling counters");
    xess_vk::CpuWallTime cpu;SetLastError(12345);
    {xess_vk::CpuWallScope scope(cpu);}
    require(!cpu.count&&!cpu.totalNs&&GetLastError()==12345,"disabled CPU timing records nothing and preserves caller error");
    GameSrRuntime runtime(&f.state,true);runtime.classify(1,0);runtime.degrade(1,"color_transition");
    require(!runtime.outputXess&&!runtime.outputTransition&&!runtime.outputUnavailable&&!runtime.degradedQpc,"SR telemetry counters and diagnostic timestamps remain unused");
    xess_vk::presentation::FrameStatus frame{};
    recordMeasurements(&f.state,frame,false,0,1,{});
    require(!asyncLogWriter.load()&&logFile==INVALID_HANDLE_VALUE,"disabled measurement path starts no writer and opens no log");
    std::cout<<"{\"passed\":true,\"runtimeDiagnostics\":false,\"measurementAllocated\":false,\"cpuTimingSamples\":0}\n";return 0;
}
}

int main(int argc,char** argv){try{if(argc==2&&std::string_view(argv[1])=="--no-runtime-diagnostics")return noRuntimeDiagnosticsFixture();sr_interval_fixture::run();sr_interval_fixture::replacementR32Attachments();sr_interval_fixture::replacementKeptMotionWriter();sr_interval_fixture::replacementCompactConsumers();sr_interval_fixture::replacementShaderAccess();sr_interval_fixture::replacementLateShaderAccess();sr_interval_fixture::replacementMaskOverwrite();sr_interval_fixture::replacementUndefinedLayoutDiscards();sr_interval_fixture::replacementUndefinedLayoutWrappers();sr_interval_fixture::replacementPhotoOverwrite();sr_interval_fixture::replacementOrderedRecovery();sr_interval_fixture::replacementWriterReceipts();sr_interval_fixture::replacementRebuildRecovery();sr_interval_fixture::replacementNativeFallback();sr_interval_fixture::nativeAaUnqualified();sr_interval_fixture::replacementNativeContentRecovery();sr_interval_fixture::replacementCharacterMenuWriter();sr_interval_fixture::replacementDistinctFaultArchives();sr_interval_fixture::replacementComputeBindings();sr_interval_fixture::replacementGraphicsCache();sr_interval_fixture::replacementCrossRecordingCache();sr_interval_fixture::replacementLazyRefresh();sr_interval_fixture::replacementStreamingLifetimes();sr_interval_fixture::replacementBindingHints();sr_interval_fixture::replacementWideMaskFallback();sr_interval_fixture::productTailSeparation();sr_interval_fixture::replacementGraphicsBindings();sr_interval_fixture::replacementLiveRecordingBudgets();sr_interval_fixture::replacementThirtyThirdImage();sr_interval_fixture::replacementPreExecuteFallback();sr_interval_fixture::replacementWrapperOrder();descriptor_layer_fixture::driverCalls=0;sr_interval_fixture::consumerArchive();std::cout<<"{\"passed\":true,\"checks\":"<<descriptor_layer_fixture::checks<<",\"gpuTouched\":false}\n";return 0;}
catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
