#include "graph_frame_scope.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>

// Allocation counting surrounds only the event-machine calls, never fixture or
// output setup. The engine owns no allocation path; fixtures live on startup heap.
static std::size_t allocations=0;
void* operator new(std::size_t n) {
    ++allocations;
    if(void* p=std::malloc(n)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p,std::size_t) noexcept { std::free(p); }
void operator delete[](void* p,std::size_t) noexcept { std::free(p); }

using namespace xess_vk::graph_scope;
namespace {
std::size_t checks=0;
void require(bool b,const char* text,int line) {
    ++checks;
    if(!b) { std::fprintf(stderr,"line %d: %s\n",line,text); std::exit(1); }
}
#define CHECK(x) require((x),#x,__LINE__)
void status(Outcome x,Status expected,int line) {
    ++checks;
    if(x.status!=expected) {
        std::fprintf(stderr,"line %d: expected %s, got %s\n",line,statusName(expected),statusName(x.status)); std::exit(1);
    }
}
#define EXPECT(x,s) status((x),Status::s,__LINE__)
constexpr Address base=0x180000000;
Stamp stamp(std::uint64_t thread=1) {
    Stamp s; s.thread=thread; s.moduleBase=base; s.generations={1,2,3,4};
    std::memcpy(s.moduleSha256.data(),xess_vk::camera_probe::requiredUnitySha256,65);
    return s;
}
void write64(unsigned char* p,Address v) { for(std::size_t i=0;i<8;++i) p[i]=static_cast<unsigned char>(v>>(i*8)); }
std::array<Address,8> properties(Pass p) {
    switch(p) {
    case Pass::FiPrepare:return {0x1D10784,0x1D10788,0x1D107E4,0x1D107E8,0x1D107EC};
    case Pass::FiReconstruct:return {0x1D1073C,0x1D10740,0x1D10744,0x1D10748,0x1D107A8};
    case Pass::FiInpaint:return {0x1D10738,0x1D1076C,0x1D1077C,0x1D10744,0x1D107E0};
    case Pass::UpscalePrepare:return {0x1D10628,0x1D1062C,0x1D10630,0x1D10684,0x1D10688,0x1D1068C,0x1D10690,0x1D10694};
    default:return {};
    }
}
Payload payload(Pass pass) {
    Payload p; p.size=pass==Pass::FiPrepare?0xD0:pass==Pass::UpscalePrepare?0x140:0xC8;
    for(std::size_t i=0;i<p.size;++i) p.bytes[i]=static_cast<unsigned char>(0x31+i%97);
    const bool up=pass==Pass::UpscalePrepare;
    const auto props=properties(pass);
    for(std::size_t i=0;i<(up?8u:5u);++i) write64(p.bytes.data()+(up?0x28:0x10)+i*0x20+0x10,base+props[i]);
    return p;
}
xess_vk::camera_probe::Snapshot camera() {
    xess_vk::camera_probe::Snapshot c; c.captured=true; c.clipToPrevCaptured=true;
    c.prefixFloatBits.fill(0x3F800000); c.clipToPrevFloatBits.fill(0x3F000000); return c;
}
struct Fixture {
    Engine engine;
    Stamp s=stamp();
    CallerEntry callerData{0x20DA70,0x900008,{0x800000,0xA00000}};
    GraphState graph{0x8FFF20,0x60000000,0x50000000,8,16};
    RendererEntry rendererData;
    ProducerEntry producerData;
    ExecutorEntry executorData;
    Key callerKey,constructionKey,rendererKey,producerKey,executionKey;
    std::array<RegistrationBefore,8> before{};
    std::array<RegistrationReturn,8> after{};
    std::array<Key,8> registrations{};
    std::size_t registered=0;
    explicit Fixture(Limits limits={}) :engine(limits) {
        rendererData={0x280C40,0x20DE30,callerData.originalRsp-0x120,
            callerData.originalRsp-0x120-0x9868,0x81000000,
            callerData.originalRsp-0x120+0x30,0x82000000,0x83000000};
    }
    void open(Producer kind=Producer::Fi) {
        auto result=engine.enterCaller(s,callerData); EXPECT(result,Accepted); callerKey=result.key;
        result=engine.constructGraph(s,callerKey,{0x20DF00,0x20DE00,graph}); EXPECT(result,Accepted); constructionKey=result.key;
        result=engine.enterRenderer(s,callerKey,rendererData); EXPECT(result,Accepted); rendererKey=result.key;
        producer(kind);
    }
    void producer(Producer kind) {
        const bool fi=kind==Producer::Fi;
        const Address rsp=rendererData.rbp-(fi?0x200:0x400)+8;
        producerData={kind,fi?0x4A4A20ull:0x16BC610ull,fi?0x28EB63ull:0x28D8F9ull,
            rsp,rsp-(fi?0x6F8:0x808),rendererData.rbp};
        auto r=engine.enterProducer(s,rendererKey,producerData); EXPECT(r,Accepted); producerKey=r.key;
    }
    RegistrationBefore makeBefore(Pass p,std::uint32_t index) {
        const bool up=p==Pass::UpscalePrepare;
        Provenance v;
        v.helperRva=p==Pass::FiPrepare?0xE7B4D0:up?0xE7B480:0xE7AF20;
        v.helperCallerReturnRva=p==Pass::FiPrepare?0x4A5608:p==Pass::FiReconstruct?0x4A5BC6:p==Pass::FiInpaint?0x4A6E7B:0x16BD9E3;
        v.helperOriginalRsp=producerData.rbp-0x88; v.helperOriginalRbp=producerData.rbp;
        v.savedRendererRbp=rendererData.rbp; v.savedProducerReturnRva=producerData.callerReturnRva;
        v.savedGraph=graph.graph; v.sourcePayload=producerData.rbp+(up?0x320:0x240);
        v.rendererOwner=rendererData.owner; v.viewContext=rendererData.viewContext; v.camera=rendererData.camera;
        return {p,v,graph,index,1,payload(p),camera()};
    }
    RegistrationReturn makeAfter(const RegistrationBefore& b) {
        const Address copied=0x70000000+static_cast<Address>(b.passIndex)*0x200;
        return {copied,b.provenance,graph,
            {b.passIndex,graph.table+static_cast<Address>(b.passIndex)*0x38,
            base+(b.pass==Pass::UpscalePrepare?0xE7A7B0:0xE7A6A0),copied},b.source,b.source,b.camera};
    }
    Key registration(Pass p=Pass::FiPrepare,std::uint32_t index=2) {
        const std::size_t pos=registered;
        before[pos]=makeBefore(p,index);
        auto r=engine.beforeRegistration(s,producerKey,before[pos]); EXPECT(r,Accepted);
        registrations[pos]=r.key; after[pos]=makeAfter(before[pos]);
        EXPECT(engine.returnRegistration(s,r.key,after[pos]),Accepted);
        ++registered; return r.key;
    }
    void finishBuild() {
        EXPECT(engine.exitProducer(s,producerKey,Exit::Normal),Accepted);
        EXPECT(engine.exitRenderer(s,rendererKey,Exit::Normal),Accepted);
    }
    void execute() {
        executorData={0x7EA00,0x20DE6F,callerData.originalRsp-0x120,graph,0x84000000,0x85000000};
        auto r=engine.enterExecutor(s,callerKey,executorData); EXPECT(r,Accepted); executionKey=r.key;
    }
    CallbackEntry callback(std::size_t pos=0) {
        auto entry=after[pos].entry;
        entry.address=graph.table+static_cast<Address>(entry.index)*0x38;
        Context context{executorData.originalRsp-0x80,0x86000000,executorData.graphValue58,
            0x88000000,0x88000200,executorData.graphValue40,base+0x27F170,graph.arena};
        return {0x7EE0F,0x7EE12,entry.index*2,graph,entry,entry.copiedPayload,context,before[pos].source};
    }
    Key callbackStart(std::size_t pos=0) {
        auto r=engine.enterCallback(s,executionKey,callback(pos)); EXPECT(r,Accepted); return r.key;
    }
    void ready() { open(); registration(); finishBuild(); execute(); }
};
void closedFlags(const Association& a) {
    CHECK(!a.sameFrameVerified && !a.matrixSemanticsVerified && !a.arenaLifetimePinned);
    CHECK(!a.gpuComplete && !a.copyPermitted && !a.xeFGConstantsReady);
}
void normalAndActive() {
    auto f=std::make_unique<Fixture>();
    const std::size_t n=allocations;
    f->ready(); const Key cb=f->callbackStart();
    Association active,completed;
    EXPECT(f->engine.activeAssociation(f->s,cb,active),ActiveAssociation);
    CHECK(active.cpuBuildExecutionAssociated && active.executionActive && active.callbackActive && !active.callbackCompleted);
    CHECK(active.payload.bytes==f->before[0].source.bytes && active.descriptors.size==0xA0);
    CHECK(active.context.address!=active.graph.graph && active.graph.graph-active.context.address==0xB8);
    CHECK(active.camera.prefixFloatBits==f->before[0].camera.prefixFloatBits);
    closedFlags(active);
    EXPECT(f->engine.exitCallback(f->s,cb,Exit::Normal,&completed),CompletedAssociation);
    CHECK(completed.callbackCompleted && !completed.callbackActive);
    EXPECT(f->engine.activeAssociation(f->s,cb,active),StaleKey); CHECK(!active.cpuBuildExecutionAssociated);
    EXPECT(f->engine.exitExecutor(f->s,f->executionKey,Exit::Normal),Accepted);
    EXPECT(f->engine.exitCaller(f->s,f->callerKey,Exit::Normal),Accepted);
    CHECK(completed.cpuBuildExecutionAssociated && completed.payload.size==0xD0);
    closedFlags(completed);
    CHECK(allocations==n);
}
void relocationAndFourPasses() {
    auto f=std::make_unique<Fixture>(); f->open();
    auto b=f->makeBefore(Pass::FiPrepare,2);
    auto r=f->engine.beforeRegistration(f->s,f->producerKey,b); EXPECT(r,Accepted);
    f->graph.table+=0x10000;
    auto a=f->makeAfter(b); EXPECT(f->engine.returnRegistration(f->s,r.key,a),Accepted);
    f->before[0]=b; f->after[0]=a; f->registrations[0]=r.key; f->registered=1;
    f->registration(Pass::FiReconstruct,3); f->registration(Pass::FiInpaint,4);
    EXPECT(f->engine.exitProducer(f->s,f->producerKey,Exit::Normal),Accepted);
    f->producer(Producer::Upscaler); f->registration(Pass::UpscalePrepare,5);
    f->finishBuild(); f->graph.table+=0x10000; f->execute();
    Association first,last;
    for(std::size_t i=0;i<4;++i) {
        const Key cb=f->callbackStart(i); Association out;
        EXPECT(f->engine.activeAssociation(f->s,cb,out),ActiveAssociation);
        CHECK(out.entry.address!=f->after[i].entry.address);
        EXPECT(f->engine.exitCallback(f->s,cb,Exit::Normal,&out),CompletedAssociation);
        if(i==0) first=out; if(i==3) last=out;
        CHECK(out.payload.size==f->before[i].source.size); closedFlags(out);
    }
    CHECK(first.caller==last.caller && first.renderer==last.renderer && first.construction==last.construction);
    CHECK(!(first.producer==last.producer) && !(first.registration==last.registration));
    CHECK(first.callback.id<last.callback.id && last.descriptors.count==8 && last.descriptors.size==0x100);
    EXPECT(f->engine.exitExecutor(f->s,f->executionKey,Exit::Normal),Accepted);
    EXPECT(f->engine.exitCaller(f->s,f->callerKey,Exit::Normal),Accepted);
}
void byteFailures() {
    for(int which=0;which<5;++which) {
        auto f=std::make_unique<Fixture>(); f->open(); auto b=f->makeBefore(Pass::FiPrepare,2);
        auto r=f->engine.beforeRegistration(f->s,f->producerKey,b); EXPECT(r,Accepted);
        auto a=f->makeAfter(b);
        if(which==0) a.copy.bytes[0xCF]^=1; // last byte, beyond descriptors
        if(which==1) a.sourceAfter.bytes[0xB9]^=1; // trailing constants slice descriptor
        if(which==2) a.camera.prefixFloatBits[4]^=1;
        if(which==3) a.camera.clipToPrevFloatBits[15]^=1;
        if(which==4) --a.copy.size;
        status(f->engine.returnRegistration(f->s,r.key,a),which<2?Status::PayloadChanged:which<4?Status::CameraChanged:Status::SnapshotSizeMismatch,__LINE__);
        EXPECT(f->engine.exitCaller(f->s,f->callerKey,Exit::Abnormal),AbnormalExit);
    }
    for(int which=0;which<4;++which) {
        auto f=std::make_unique<Fixture>(); f->open(); auto b=f->makeBefore(Pass::FiPrepare,2);
        if(which==0) b.camera.captured=false;
        if(which==1) b.camera.prefixFloatBits[0]=0x7F800000;
        if(which==2) b.camera.clipToPrevFloatBits[7]=0xFFC00001;
        if(which==3) b.source.size=0xC8;
        status(f->engine.beforeRegistration(f->s,f->producerKey,b),which==0?Status::CameraUnavailable:which==3?Status::SnapshotSizeMismatch:Status::CameraNonFinite,__LINE__);
    }
    auto f=std::make_unique<Fixture>(); f->ready(); auto c=f->callback();
    c.snapshot.bytes[0xCF]^=0x40;
    EXPECT(f->engine.enterCallback(f->s,f->executionKey,c),PayloadChanged);
    Association out;
    EXPECT(f->engine.activeAssociation(f->s,{},out),StaleKey); CHECK(!out.cpuBuildExecutionAssociated);
}
void propertiesAndProvenance() {
    for(int which=0;which<6;++which) {
        auto f=std::make_unique<Fixture>(); f->open(); auto b=f->makeBefore(Pass::FiPrepare,2);
        if(which==0) write64(b.source.bytes.data()+0x20,base+0x1D10788); // duplicate exact property
        if(which==1) write64(b.source.bytes.data()+0x20,base+0x1234);
        if(which==2) b.provenance.sourcePayload+=8;
        if(which==3) b.provenance.camera+=8;
        if(which==4) b.provenance.helperCallerReturnRva=0x4A5BC6;
        if(which==5) b.provenance.savedRendererRbp+=0x10;
        status(f->engine.beforeRegistration(f->s,f->producerKey,b),which<2?Status::UnknownPropertySet:which==3?Status::CameraProvenanceMismatch:which==4?Status::UnknownRoute:Status::GraphMismatch,__LINE__);
    }
    // Descriptor order can differ, but the complete unique property set must hold.
    auto f=std::make_unique<Fixture>(); f->open(); auto b=f->makeBefore(Pass::FiPrepare,2);
    unsigned char temp[0x20];
    std::memcpy(temp,b.source.bytes.data()+0x10,0x20);
    std::memcpy(b.source.bytes.data()+0x10,b.source.bytes.data()+0x30,0x20);
    std::memcpy(b.source.bytes.data()+0x30,temp,0x20);
    auto r=f->engine.beforeRegistration(f->s,f->producerKey,b); EXPECT(r,Accepted);
    EXPECT(f->engine.returnRegistration(f->s,r.key,f->makeAfter(b)),Accepted);
}
void callbackFailures() {
    for(int which=0;which<10;++which) {
        auto f=std::make_unique<Fixture>(); f->ready(); auto c=f->callback();
        if(which==0) c.encodedSchedulePass|=1;
        if(which==1) c.entry.index=8;
        if(which==2) c.entry.address+=0x38;
        if(which==3) c.actualPayload+=0x200;
        if(which==4) c.entry.executor=base+0xE7A7B0;
        if(which==5) c.context.address=c.graph.graph;
        if(which==6) c.context.arena+=8;
        if(which==7) c.context.resourceEnd+=1;
        if(which==8) c.graph.table+=0x10000;
        if(which==9) c.callerReturnRva=0x459EFA;
        status(f->engine.enterCallback(f->s,f->executionKey,c),which==0?Status::OddSchedule:which<4?Status::EntryMismatch:which==4?Status::UnmatchedCallback:which<8?Status::ContextMismatch:which==8?Status::TableMismatch:Status::UnknownRoute,__LINE__);
        Association a; EXPECT(f->engine.activeAssociation(f->s,f->executionKey,a),ScopeInvalidated); CHECK(!a.cpuBuildExecutionAssociated);
    }
    auto f=std::make_unique<Fixture>(); f->ready(); Key cb=f->callbackStart();
    EXPECT(f->engine.exitCallback(f->s,cb,Exit::Normal),CompletedAssociation);
    EXPECT(f->engine.enterCallback(f->s,f->executionKey,f->callback()),DuplicateCallback);
}
void wrongRoutesOrderAndIncomplete() {
    auto f=std::make_unique<Fixture>();
    EXPECT(f->engine.enterExecutor(f->s,{},{}),StaleKey);
    f->open(); f->registration();
    ExecutorEntry e{0x7EA00,0x20DE6F,f->callerData.originalRsp-0x120,f->graph,0x84000000,0x85000000};
    EXPECT(f->engine.enterExecutor(f->s,f->callerKey,e),OutOfOrder);
    auto g=std::make_unique<Fixture>(); g->open(); g->registration(); g->finishBuild();
    e.originalRsp=g->callerData.originalRsp-0x120; e.callerReturnRva=0x459EFA;
    EXPECT(g->engine.enterExecutor(g->s,g->callerKey,e),UnknownRoute);
    auto h=std::make_unique<Fixture>(); h->open();
    auto b=h->makeBefore(Pass::FiPrepare,2);
    auto r=h->engine.beforeRegistration(h->s,h->producerKey,b); EXPECT(r,Accepted);
    EXPECT(h->engine.abortRegistration(h->s,r.key),AbnormalExit);
    EXPECT(h->engine.returnRegistration(h->s,r.key,h->makeAfter(b)),ScopeInvalidated);
    auto j=std::make_unique<Fixture>(); j->open(); j->registration();
    EXPECT(j->engine.beforeRegistration(j->s,j->producerKey,j->makeBefore(Pass::FiPrepare,2)),DuplicateRegistration);
    auto k=std::make_unique<Fixture>(); k->open();
    r=k->engine.beforeRegistration(k->s,k->producerKey,k->makeBefore(Pass::FiPrepare,2)); EXPECT(r,Accepted);
    EXPECT(k->engine.beforeRegistration(k->s,k->producerKey,k->makeBefore(Pass::FiReconstruct,3)),ReentrantScope);
}
void generationsAndAba() {
    auto f=std::make_unique<Fixture>(); f->ready(); const Key oldExecution=f->executionKey;
    const Key cb=f->callbackStart(); Association evidence;
    EXPECT(f->engine.exitCallback(f->s,cb,Exit::Normal,&evidence),CompletedAssociation);
    EXPECT(f->engine.exitExecutor(f->s,oldExecution,Exit::Normal),Accepted);
    EXPECT(f->engine.exitCaller(f->s,f->callerKey,Exit::Normal),Accepted);
    const auto oldCaller=evidence.caller,oldRegistration=evidence.registration;
    f->registered=0; f->ready();
    CHECK(f->callerKey.id>oldCaller.id && !(f->callerKey==oldCaller));
    CHECK(f->registrations[0].id>oldRegistration.id && f->after[0].entry.copiedPayload==evidence.entry.copiedPayload);
    EXPECT(f->engine.enterCallback(f->s,oldExecution,f->callback()),StaleKey);
    Key fresh=f->callbackStart(); Association active;
    EXPECT(f->engine.activeAssociation(f->s,fresh,active),ActiveAssociation);
    CHECK(active.payload.bytes==evidence.payload.bytes && !(active.registration==evidence.registration));
    for(int which=0;which<4;++which) {
        Stamp wrong=f->s;
        if(which==0) ++wrong.generations.module;
        if(which==1) ++wrong.generations.load;
        if(which==2) ++wrong.generations.device;
        if(which==3) ++wrong.generations.observer;
        EXPECT(f->engine.activeAssociation(wrong,fresh,active),GenerationMismatch); CHECK(!active.cpuBuildExecutionAssociated);
    }
    Stamp other=f->s; ++other.thread;
    EXPECT(f->engine.activeAssociation(other,fresh,active),CrossThread); CHECK(!active.cpuBuildExecutionAssociated);
    auto another=std::make_unique<Fixture>(); another->ready();
    EXPECT(another->engine.enterCallback(another->s,oldExecution,another->callback()),StaleKey);
}
void abnormalAndUnexecuted() {
    auto f=std::make_unique<Fixture>(); f->ready();
    auto r=f->engine.exitExecutor(f->s,f->executionKey,Exit::Normal); EXPECT(r,UnexecutedRegistrations);
    CHECK(r.summary.unexecuted==1 && r.summary.callbacksCompleted==0);
    EXPECT(f->engine.enterCallback(f->s,f->executionKey,f->callback()),OutOfOrder);
    EXPECT(f->engine.exitCaller(f->s,f->callerKey,Exit::Abnormal),AbnormalExit);
    auto g=std::make_unique<Fixture>(); g->ready(); auto cb=g->callbackStart(); Association a;
    EXPECT(g->engine.activeAssociation(g->s,cb,a),ActiveAssociation);
    EXPECT(g->engine.exitCallback(g->s,cb,Exit::Abnormal,&a),AbnormalExit); CHECK(!a.cpuBuildExecutionAssociated);
    EXPECT(g->engine.activeAssociation(g->s,cb,a),ScopeInvalidated); CHECK(!a.cpuBuildExecutionAssociated);
    EXPECT(g->engine.exitExecutor(g->s,g->executionKey,Exit::Abnormal),AbnormalExit);
    EXPECT(g->engine.exitCaller(g->s,g->callerKey,Exit::Abnormal),AbnormalExit);
    auto h=std::make_unique<Fixture>(); h->ready(); cb=h->callbackStart();
    EXPECT(h->engine.exitExecutor(h->s,h->executionKey,Exit::Abnormal),AbnormalExit);
    EXPECT(h->engine.activeAssociation(h->s,cb,a),ScopeInvalidated);
    auto j=std::make_unique<Fixture>(); j->open();
    EXPECT(j->engine.exitProducer(j->s,j->producerKey,Exit::Abnormal),AbnormalExit);
    EXPECT(j->engine.exitRenderer(j->s,j->rendererKey,Exit::Normal),ScopeInvalidated);
}
void boundedNestingThreadsAndReentry() {
    auto f=std::make_unique<Fixture>(); f->ready(); const Key cb=f->callbackStart(); Association a;
    CallerEntry child=f->callerData; child.originalRsp-=0x20000;
    auto nested=f->engine.enterCaller(f->s,child); EXPECT(nested,Accepted);
    EXPECT(f->engine.activeAssociation(f->s,cb,a),OutOfOrder); CHECK(!a.cpuBuildExecutionAssociated);
    EXPECT(f->engine.exitCaller(f->s,nested.key,Exit::Abnormal),AbnormalExit);
    EXPECT(f->engine.activeAssociation(f->s,cb,a),ActiveAssociation); // child did not poison parent
    Stamp second=stamp(2);
    auto parallel=f->engine.enterCaller(second,f->callerData); EXPECT(parallel,Accepted);
    EXPECT(f->engine.activeAssociation(f->s,cb,a),ActiveAssociation);
    EXPECT(f->engine.exitCaller(second,parallel.key,Exit::Abnormal),AbnormalExit);
    EXPECT(f->engine.enterCallback(f->s,f->executionKey,f->callback()),ReentrantScope);
    EXPECT(f->engine.activeAssociation(f->s,cb,a),ScopeInvalidated);
}
void capacityAndArithmetic() {
    Limits limits; limits.registrations=1;
    auto f=std::make_unique<Fixture>(limits); f->open(); f->registration();
    EXPECT(f->engine.beforeRegistration(f->s,f->producerKey,f->makeBefore(Pass::FiReconstruct,3)),CapacityExhausted);
    EXPECT(f->engine.exitCaller(f->s,f->callerKey,Exit::Normal),ScopeInvalidated);
    limits={}; limits.callerDepth=1;
    auto g=std::make_unique<Fixture>(limits); g->ready(); Key cb=g->callbackStart();
    CallerEntry nested=g->callerData; nested.originalRsp-=0x20000;
    EXPECT(g->engine.enterCaller(g->s,nested),CapacityExhausted);
    Association a; EXPECT(g->engine.activeAssociation(g->s,cb,a),ScopeInvalidated);
    limits={}; limits.threads=1;
    auto h=std::make_unique<Fixture>(limits); h->open();
    EXPECT(h->engine.enterCaller(stamp(2),h->callerData),CapacityExhausted);
    EXPECT(h->engine.exitProducer(h->s,h->producerKey,Exit::Normal),Accepted);
    limits={}; limits.producers=1;
    auto p=std::make_unique<Fixture>(limits); p->open();
    EXPECT(p->engine.exitProducer(p->s,p->producerKey,Exit::Normal),Accepted);
    EXPECT(p->engine.enterProducer(p->s,p->rendererKey,p->producerData),CapacityExhausted);
    auto j=std::make_unique<Fixture>(); CallerEntry bad=j->callerData; bad.originalRsp=0x100;
    EXPECT(j->engine.enterCaller(j->s,bad),AddressOverflow);
    bad=j->callerData; bad.stack.high=bad.originalRsp;
    EXPECT(j->engine.enterCaller(j->s,bad),InvalidStackBounds);
    Stamp invalid=j->s; invalid.moduleBase=0xFFFFFFFFFFFF0000ull;
    EXPECT(j->engine.enterCaller(invalid,j->callerData),InvalidIdentity);
    j->open(); auto b=j->makeBefore(Pass::FiPrepare,2); b.graph.table=0xFFFFFFFFFFFFFFF8ull;
    EXPECT(j->engine.beforeRegistration(j->s,j->producerKey,b),AddressOverflow);
    auto renderer=std::make_unique<Fixture>();
    auto caller=renderer->engine.enterCaller(renderer->s,renderer->callerData); EXPECT(caller,Accepted);
    EXPECT(renderer->engine.constructGraph(renderer->s,caller.key,{0x20DF00,0x20DE00,renderer->graph}),Accepted);
    renderer->rendererData.camera=0xFFFFFFFFFFFFFFF8ull;
    EXPECT(renderer->engine.enterRenderer(renderer->s,caller.key,renderer->rendererData),AddressOverflow);
    auto producer=std::make_unique<Fixture>(); producer->open();
    EXPECT(producer->engine.enterProducer(producer->s,producer->rendererKey,producer->producerData),ReentrantScope);
}
void ownedSnapshotsAndUniqueMatch() {
    auto f=std::make_unique<Fixture>(); f->open();
    f->registration(Pass::FiPrepare,2);
    auto b=f->makeBefore(Pass::FiPrepare,3);
    auto r=f->engine.beforeRegistration(f->s,f->producerKey,b); EXPECT(r,Accepted);
    auto a=f->makeAfter(b);
    // Same pointer and bytes do not merge independent entry/registration IDs.
    a.helperReturnRax=f->after[0].helperReturnRax; a.entry.copiedPayload=a.helperReturnRax;
    EXPECT(f->engine.returnRegistration(f->s,r.key,a),Accepted);
    f->before[1]=b; f->after[1]=a; f->registered=2;
    f->finishBuild(); f->execute();
    auto input=f->callback(0);
    const Payload expected=input.snapshot;
    f->before[0].source.bytes[0]^=1; // mutate caller-owned inputs after capture
    f->before[0].camera.prefixFloatBits[0]^=1;
    auto cb=f->engine.enterCallback(f->s,f->executionKey,input); EXPECT(cb,Accepted);
    Association first;
    EXPECT(f->engine.activeAssociation(f->s,cb.key,first),ActiveAssociation);
    CHECK(first.payload.bytes==expected.bytes && first.camera.prefixFloatBits[0]==0x3F800000);
    first.payload.bytes[0]^=2;
    EXPECT(f->engine.activeAssociation(f->s,cb.key,first),ActiveAssociation);
    CHECK(first.payload.bytes==expected.bytes);
    EXPECT(f->engine.exitCallback(f->s,cb.key,Exit::Normal,&first),CompletedAssociation);
    cb=f->engine.enterCallback(f->s,f->executionKey,f->callback(1)); EXPECT(cb,Accepted);
    Association second;
    EXPECT(f->engine.exitCallback(f->s,cb.key,Exit::Normal,&second),CompletedAssociation);
    CHECK(first.entry.copiedPayload==second.entry.copiedPayload && !(first.registration==second.registration));
    CHECK(first.entry.index==2 && second.entry.index==3);
    EXPECT(f->engine.exitExecutor(f->s,f->executionKey,Exit::Normal),Accepted);
    EXPECT(f->engine.exitCaller(f->s,f->callerKey,Exit::Normal),Accepted);
    CHECK(first.payload.bytes==expected.bytes);
    auto g=std::make_unique<Fixture>(); g->ready(); Key active=g->callbackStart();
    Stamp changed=g->s; ++changed.generations.device;
    CallerEntry nested=g->callerData; nested.originalRsp-=0x20000;
    EXPECT(g->engine.enterCaller(changed,nested),GenerationMismatch);
    EXPECT(g->engine.activeAssociation(g->s,active,first),ScopeInvalidated);
    CHECK(!first.cpuBuildExecutionAssociated);
}
} // namespace
int main() {
    normalAndActive(); relocationAndFourPasses(); byteFailures(); propertiesAndProvenance();
    callbackFailures(); wrongRoutesOrderAndIncomplete(); generationsAndAba();
    abnormalAndUnexecuted(); boundedNestingThreadsAndReentry(); capacityAndArithmetic(); ownedSnapshotsAndUniqueMatch();
    std::printf("graph_frame_scope_test: PASS (%zu checks), sizeof(Engine)=%zu, sizeof(Association)=%zu\n",checks,sizeof(Engine),sizeof(Association));
}
