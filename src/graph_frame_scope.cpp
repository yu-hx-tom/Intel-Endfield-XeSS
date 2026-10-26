#include "graph_frame_scope.hpp"
#include <atomic>
#include <cstring>
#include <limits>

namespace xess_vk::graph_scope {
namespace {
constexpr Address callerRva = 0x20DA70, initializerRva = 0x20DF00;
constexpr Address rendererRva = 0x280C40, executorRva = 0x7EA00;
std::atomic<std::uint64_t> incarnation{1};
bool add(Address a, Address b, Address& out) noexcept {
    if (b > std::numeric_limits<Address>::max() - a) return false;
    out = a + b; return true;
}
bool sub(Address a, Address b, Address& out) noexcept {
    if (a < b) return false;
    out = a - b; return true;
}
bool pointer(Address a) noexcept { return a != 0 && (a & 7) == 0; }
bool stackRange(StackBounds b, Address a, Address n) noexcept {
    Address end = 0;
    return b.low != 0 && b.low < b.high && add(a,n,end) && a >= b.low && end <= b.high;
}
bool identity(const Stamp& s) noexcept {
    Address end = 0;
    if (!s.thread || !pointer(s.moduleBase) || !s.generations.module ||
        !s.generations.load || !s.generations.device || !s.generations.observer ||
        !add(s.moduleBase,0x1D107F4,end) || s.moduleSha256[64] != 0) return false;
    for (std::size_t i=0;i<64;++i) {
        char c=s.moduleSha256[i];
        if (c>='a' && c<='f') c=static_cast<char>(c-'a'+'A');
        if (c != camera_probe::requiredUnitySha256[i]) return false;
    }
    return true;
}
bool provenanceEqual(const Provenance& a, const Provenance& b) noexcept {
    return a.helperRva==b.helperRva && a.helperCallerReturnRva==b.helperCallerReturnRva &&
        a.helperOriginalRsp==b.helperOriginalRsp && a.helperOriginalRbp==b.helperOriginalRbp &&
        a.savedRendererRbp==b.savedRendererRbp && a.savedProducerReturnRva==b.savedProducerReturnRva &&
        a.savedGraph==b.savedGraph && a.sourcePayload==b.sourcePayload &&
        a.rendererOwner==b.rendererOwner && a.viewContext==b.viewContext && a.camera==b.camera;
}
bool payloadEqual(const Payload& a, const Payload& b) noexcept {
    return a.size==b.size && a.size<=maxPayloadBytes &&
        std::memcmp(a.bytes.data(),b.bytes.data(),a.size)==0;
}
bool cameraEqual(const camera_probe::Snapshot& a, const camera_probe::Snapshot& b) noexcept {
    return a.captured==b.captured && a.clipToPrevCaptured==b.clipToPrevCaptured &&
        a.prefixFloatBits==b.prefixFloatBits &&
        a.clipToPrevFloatBits==b.clipToPrevFloatBits;
}
Status cameraValid(const camera_probe::Snapshot& s) noexcept {
    if (!s.captured) return Status::CameraUnavailable;
    for (auto v:s.prefixFloatBits) if ((v & 0x7F800000)==0x7F800000) return Status::CameraNonFinite;
    if (s.clipToPrevCaptured)
        for (auto v:s.clipToPrevFloatBits) if ((v & 0x7F800000)==0x7F800000) return Status::CameraNonFinite;
    return Status::Accepted;
}
Address read64(const unsigned char* p) noexcept {
    Address out=0;
    for (std::size_t i=0;i<8;++i) out |= static_cast<Address>(p[i])<<(8*i);
    return out;
}
struct PassSpec {
    std::size_t extent, descriptorOffset, count;
    Address helper, helperReturn, thunk;
    Producer producer;
    std::array<Address,8> properties;
};
const PassSpec* spec(Pass p) noexcept {
    static constexpr PassSpec specs[]{
        {0xD0,0x10,5,0xE7B4D0,0x4A5608,0xE7A6A0,Producer::Fi,
            {0x1D10784,0x1D10788,0x1D107E4,0x1D107E8,0x1D107EC}},
        {0xC8,0x10,5,0xE7AF20,0x4A5BC6,0xE7A6A0,Producer::Fi,
            {0x1D1073C,0x1D10740,0x1D10744,0x1D10748,0x1D107A8}},
        {0xC8,0x10,5,0xE7AF20,0x4A6E7B,0xE7A6A0,Producer::Fi,
            {0x1D10738,0x1D1076C,0x1D1077C,0x1D10744,0x1D107E0}},
        {0x140,0x28,8,0xE7B480,0x16BD9E3,0xE7A7B0,Producer::Upscaler,
            {0x1D10628,0x1D1062C,0x1D10630,0x1D10684,0x1D10688,0x1D1068C,0x1D10690,0x1D10694}}
    };
    switch (p) {
    case Pass::FiPrepare:return &specs[0];
    case Pass::FiReconstruct:return &specs[1];
    case Pass::FiInpaint:return &specs[2];
    case Pass::UpscalePrepare:return &specs[3];
    default:return nullptr;
    }
}
bool graphSame(const GraphState& a,const GraphState& b) noexcept {
    return a.graph==b.graph && a.arena==b.arena;
}
bool executingGraphSame(const GraphState& a,const GraphState& b) noexcept {
    return graphSame(a,b) && a.table==b.table && a.count==b.count && a.capacity==b.capacity;
}
} // namespace

bool Generations::operator==(const Generations& x) const noexcept {
    return module==x.module && load==x.load && device==x.device && observer==x.observer;
}
bool Stamp::operator==(const Stamp& x) const noexcept {
    return thread==x.thread && moduleBase==x.moduleBase && generations==x.generations && moduleSha256==x.moduleSha256;
}
bool Key::operator==(const Key& x) const noexcept {
    return engine==x.engine && id==x.id && callerId==x.callerId && stamp==x.stamp;
}
const char* passName(Pass p) noexcept {
    switch(p) {
    case Pass::FiPrepare:return "fi_prepare";
    case Pass::FiReconstruct:return "fi_reconstruct";
    case Pass::FiInpaint:return "fi_inpaint";
    case Pass::UpscalePrepare:return "upscale_prepare";
    default:return "unknown";
    }
}
const char* statusName(Status s) noexcept {
    switch(s) {
#define NAME(x) case Status::x:return #x
    NAME(Accepted); NAME(ActiveAssociation); NAME(CompletedAssociation); NAME(UnexecutedRegistrations);
    NAME(MissingScope); NAME(StaleKey); NAME(CrossThread); NAME(GenerationMismatch); NAME(InvalidIdentity);
    NAME(UnknownRoute); NAME(UnsupportedPass); NAME(OutOfOrder); NAME(ReentrantScope); NAME(ScopeInvalidated);
    NAME(InvalidAddress); NAME(AddressOverflow); NAME(InvalidStackBounds); NAME(GraphMismatch);
    NAME(TableMismatch); NAME(EntryMismatch); NAME(SnapshotSizeMismatch); NAME(UnknownPropertySet);
    NAME(PayloadChanged); NAME(CameraUnavailable); NAME(CameraChanged); NAME(CameraNonFinite);
    NAME(CameraProvenanceMismatch); NAME(ContextMismatch); NAME(DuplicateRegistration);
    NAME(AmbiguousMatch); NAME(UnmatchedCallback); NAME(DuplicateCallback); NAME(OddSchedule);
    NAME(CapacityExhausted); NAME(IdExhausted); NAME(AbnormalExit);
#undef NAME
    }
    return "UnknownStatus";
}
Engine::Engine(Limits l) noexcept : limits_(l) {
    if (!l.threads || l.threads>maxThreads || !l.callerDepth || l.callerDepth>maxCallerDepth ||
        !l.registrations || l.registrations>maxRegistrations || !l.producers || l.producers>maxProducers) {
        configuration_=Status::CapacityExhausted; return;
    }
    auto value=incarnation.load(std::memory_order_relaxed);
    while (value != std::numeric_limits<std::uint64_t>::max()) {
        if (incarnation.compare_exchange_weak(value,value+1,std::memory_order_relaxed)) {
            engineId_=value; return;
        }
    }
    configuration_=Status::IdExhausted;
}
Key Engine::allocate(const Stamp& s,std::uint64_t parent) noexcept {
    if (nextId_==std::numeric_limits<std::uint64_t>::max()) return {};
    Key k{engineId_,nextId_++,parent,s};
    if (!parent) k.callerId=k.id;
    return k;
}
Summary Engine::summarize(const CallerSlot& c) const noexcept {
    Summary s; s.invalidated=c.invalidated;
    for (std::size_t i=0;i<c.registrationCount;++i) {
        const auto& r=c.registrations[i];
        s.registered+=r.ready; s.callbacksStarted+=r.started; s.callbacksCompleted+=r.completed;
        s.unexecuted+=r.ready && !r.completed;
    }
    return s;
}
Outcome Engine::accept(const Key& k,const CallerSlot* c) const noexcept {
    return {Status::Accepted,k,c?summarize(*c):Summary{}};
}
Outcome Engine::fail(CallerSlot* c,Status s) noexcept {
    if(c) c->invalidated=true;
    return {s,{},c?summarize(*c):Summary{}};
}
Engine::CallerSlot* Engine::locate(const Stamp& s,const Key& k,Status& status,bool allowInvalid) noexcept {
    status=configuration_;
    if (status!=Status::Accepted) return nullptr;
    if (!identity(s)) { status=Status::InvalidIdentity; return nullptr; }
    if (!k || k.engine!=engineId_) { status=Status::StaleKey; return nullptr; }
    if (s.thread!=k.stamp.thread) { status=Status::CrossThread; return nullptr; }
    if (!(s==k.stamp)) { status=Status::GenerationMismatch; return nullptr; }
    for(auto& t:threads_) if(t.depth && t.stamp.thread==s.thread) {
        if (!(t.stamp==s)) { status=Status::GenerationMismatch; return nullptr; }
        for(std::size_t i=0;i<t.depth;++i) if(t.callers[i].key.id==k.callerId) {
            if (i+1!=t.depth) { status=Status::OutOfOrder; return nullptr; }
            auto& c=t.callers[i];
            if(c.invalidated && !allowInvalid) { status=Status::ScopeInvalidated; return nullptr; }
            return &c;
        }
        status=Status::StaleKey; return nullptr;
    }
    status=Status::MissingScope; return nullptr;
}
Status Engine::graphValid(const GraphState& g,std::uint32_t index) const noexcept {
    if (!pointer(g.graph) || !pointer(g.arena) || !pointer(g.table)) return Status::InvalidAddress;
    if (!g.count || g.count>g.capacity || g.capacity>maxGraphEntries || index>=g.count)
        return Status::EntryMismatch;
    Address end=0;
    if(!add(g.table,static_cast<Address>(g.capacity)*0x38,end)) return Status::AddressOverflow;
    return Status::Accepted;
}
Status Engine::payloadValid(const Stamp& stamp,Pass pass,const Payload& payload,Descriptors& d) const noexcept {
    const auto* p=spec(pass);
    if(!p) return Status::UnsupportedPass;
    if(payload.size!=p->extent) return Status::SnapshotSizeMismatch;
    unsigned seen=0;
    for(std::size_t i=0;i<p->count;++i) {
        const Address property=read64(payload.bytes.data()+p->descriptorOffset+i*0x20+0x10);
        unsigned match=0;
        for(std::size_t j=0;j<p->count;++j) {
            Address expected=0;
            if(!add(stamp.moduleBase,p->properties[j],expected)) return Status::AddressOverflow;
            if(property==expected) match=1u<<j;
        }
        if(!match || (seen&match)) return Status::UnknownPropertySet;
        seen|=match;
    }
    d={}; d.count=p->count; d.size=p->count*0x20;
    std::memcpy(d.bytes.data(),payload.bytes.data()+p->descriptorOffset,d.size);
    return Status::Accepted;
}
Status Engine::provenanceValid(const CallerSlot& c,const ProducerSlot& producer,const RegistrationBefore& r) const noexcept {
    const auto* p=spec(r.pass);
    if(!p || producer.data.kind!=p->producer) return Status::UnsupportedPass;
    const auto& v=r.provenance; const bool fi=p->producer==Producer::Fi;
    Address source=0, savedFrame=0, savedReturn=0, savedGraph=0;
    if(!add(producer.data.rbp,fi?0x240:0x320,source) ||
       !add(producer.data.rbp,fi?0x6F0:0x800,savedFrame) ||
       !add(producer.data.rbp,fi?0x6F8:0x808,savedReturn) ||
       !add(producer.data.rbp,fi?0x718:0x820,savedGraph)) return Status::AddressOverflow;
    if(v.helperRva!=p->helper || v.helperCallerReturnRva!=p->helperReturn) return Status::UnknownRoute;
    if(v.helperOriginalRbp!=producer.data.rbp || v.savedRendererRbp!=c.rendererData.rbp ||
       v.savedProducerReturnRva!=producer.data.callerReturnRva ||
       v.savedGraph!=c.constructedGraph.graph || v.sourcePayload!=source) return Status::GraphMismatch;
    if(!pointer(v.helperOriginalRsp) || v.helperOriginalRsp>=producer.data.originalRsp ||
       !stackRange(c.caller.stack,v.helperOriginalRsp,8) || !stackRange(c.caller.stack,source,p->extent) ||
       !stackRange(c.caller.stack,savedFrame,8) || !stackRange(c.caller.stack,savedReturn,8) ||
       !stackRange(c.caller.stack,savedGraph,8)) return Status::InvalidStackBounds;
    if(v.rendererOwner!=c.rendererData.owner || v.viewContext!=c.rendererData.viewContext ||
       v.camera!=c.rendererData.camera) return Status::CameraProvenanceMismatch;
    return Status::Accepted;
}
Outcome Engine::enterCaller(const Stamp& s,const CallerEntry& e) noexcept {
    if(configuration_!=Status::Accepted) return fail(nullptr,configuration_);
    if(!identity(s)) return fail(nullptr,Status::InvalidIdentity);
    if(e.entryRva!=callerRva) return fail(nullptr,Status::UnknownRoute);
    Address body=0,graph=0;
    if(!sub(e.originalRsp,0x118,body) || !add(body,0x30,graph)) return fail(nullptr,Status::AddressOverflow);
    if(!pointer(e.originalRsp) || !stackRange(e.stack,e.originalRsp,8) || !stackRange(e.stack,graph,0x68))
        return fail(nullptr,Status::InvalidStackBounds);
    ThreadSlot* t=nullptr;
    for(std::size_t i=0;i<limits_.threads;++i) if(threads_[i].depth && threads_[i].stamp.thread==s.thread) {
        t=&threads_[i]; break;
    }
    if(t && !(t->stamp==s)) {
        // A newly observed caller in a changed epoch cannot leave suspended old
        // callbacks eligible. Cleanup still uses their original scope keys.
        for(std::size_t i=0;i<t->depth;++i) t->callers[i].invalidated=true;
        return fail(&t->callers[t->depth-1],Status::GenerationMismatch);
    }
    if(!t) for(std::size_t i=0;i<limits_.threads;++i) if(!threads_[i].depth) { t=&threads_[i]; t->stamp=s; break; }
    if(!t || t->depth==limits_.callerDepth)
        return fail(t && t->depth ? &t->callers[t->depth-1] : nullptr,Status::CapacityExhausted);
    // Distinct scopes require real descending stack frames, not a duplicate entry.
    if(t->depth && e.originalRsp>=t->callers[t->depth-1].caller.originalRsp)
        return fail(&t->callers[t->depth-1],Status::ReentrantScope);
    const Key k=allocate(s,0);
    if(!k) return fail(nullptr,Status::IdExhausted);
    auto& c=t->callers[t->depth++]; c=CallerSlot{}; c.key=k; c.caller=e;
    return accept(k,&c);
}
Outcome Engine::constructGraph(const Stamp& s,const Key& key,const Construction& e) noexcept {
    Status status; auto* c=locate(s,key,status);
    if(!c) return fail(nullptr,status);
    if(!(key==c->key)) return fail(c,Status::StaleKey);
    if(c->construction || c->renderer || c->execution) return fail(c,Status::OutOfOrder);
    if(e.initializerRva!=initializerRva || e.callerReturnRva!=0x20DE00) return fail(c,Status::UnknownRoute);
    Address graph=0;
    if(!sub(c->caller.originalRsp,0xE8,graph)) return fail(c,Status::AddressOverflow);
    if(e.graph.graph!=graph) return fail(c,Status::GraphMismatch);
    status=graphValid(e.graph,0); if(status!=Status::Accepted) return fail(c,status);
    c->construction=allocate(s,c->key.id); if(!c->construction) return fail(c,Status::IdExhausted);
    c->constructedGraph=e.graph;
    return accept(c->construction,c);
}
Outcome Engine::enterRenderer(const Stamp& s,const Key& key,const RendererEntry& e) noexcept {
    Status status; auto* c=locate(s,key,status);
    if(!c) return fail(nullptr,status);
    if(!(key==c->key)) return fail(c,Status::StaleKey);
    if(!c->construction || c->renderer || c->execution) return fail(c,Status::OutOfOrder);
    if(e.entryRva!=rendererRva || e.callerReturnRva!=0x20DE30) return fail(c,Status::UnknownRoute);
    Address rsp=0,rbp=0,slot=0;
    if(!sub(c->caller.originalRsp,0x120,rsp) || !sub(rsp,0x9868,rbp) || !add(rsp,0x30,slot))
        return fail(c,Status::AddressOverflow);
    if(e.originalRsp!=rsp || e.rbp!=rbp || e.viewContextSlot!=slot) return fail(c,Status::GraphMismatch);
    if(!stackRange(c->caller.stack,rbp,0x98A0)) return fail(c,Status::InvalidStackBounds);
    if(!pointer(e.owner) || !pointer(e.viewContext) || !pointer(e.camera)) return fail(c,Status::InvalidAddress);
    Address rangeEnd=0;
    if(!add(e.owner,0x2E8,rangeEnd) || !add(e.viewContext,0x30,rangeEnd) ||
       !add(e.camera,camera_probe::prefixBytes,rangeEnd)) return fail(c,Status::AddressOverflow);
    c->renderer=allocate(s,c->key.id); if(!c->renderer) return fail(c,Status::IdExhausted);
    c->rendererData=e; c->rendererActive=true;
    return accept(c->renderer,c);
}
Outcome Engine::exitRenderer(const Stamp& s,const Key& key,Exit exit) noexcept {
    Status status; auto* c=locate(s,key,status,true);
    if(!c) return fail(nullptr,status);
    if(!(key==c->renderer)) return fail(c,Status::StaleKey);
    if(exit==Exit::Abnormal) { c->rendererActive=false; return fail(c,Status::AbnormalExit); }
    if(c->invalidated) return fail(c,Status::ScopeInvalidated);
    if(!c->rendererActive || c->activeProducer!=maxProducers || c->pendingRegistration!=maxRegistrations)
        return fail(c,Status::OutOfOrder);
    c->rendererActive=false; c->rendererCompleted=true;
    return accept(key,c);
}
Outcome Engine::enterProducer(const Stamp& s,const Key& key,const ProducerEntry& e) noexcept {
    Status status; auto* c=locate(s,key,status);
    if(!c) return fail(nullptr,status);
    if(!(key==c->renderer)) return fail(c,Status::StaleKey);
    if(!c->rendererActive || c->execution) return fail(c,Status::OutOfOrder);
    if(c->activeProducer!=maxProducers) return fail(c,Status::ReentrantScope);
    if(c->producerCount==limits_.producers) return fail(c,Status::CapacityExhausted);
    const bool fi=e.kind==Producer::Fi;
    if(e.kind!=Producer::Fi && e.kind!=Producer::Upscaler) return fail(c,Status::UnsupportedPass);
    if(e.entryRva!=(fi?0x4A4A20:0x16BC610) || e.callerReturnRva!=(fi?0x28EB63:0x28D8F9))
        return fail(c,Status::UnknownRoute);
    Address rbp=0;
    if(!sub(e.originalRsp,fi?0x6F8:0x808,rbp)) return fail(c,Status::AddressOverflow);
    if(e.rbp!=rbp || e.savedRendererRbp!=c->rendererData.rbp) return fail(c,Status::GraphMismatch);
    if(!pointer(e.originalRsp) || e.originalRsp>=c->rendererData.originalRsp ||
       !stackRange(c->caller.stack,rbp,fi?0x720:0x828)) return fail(c,Status::InvalidStackBounds);
    Key k=allocate(s,c->key.id); if(!k) return fail(c,Status::IdExhausted);
    c->activeProducer=c->producerCount++;
    c->producers[c->activeProducer]={k,e,true};
    return accept(k,c);
}
Outcome Engine::exitProducer(const Stamp& s,const Key& key,Exit exit) noexcept {
    Status status; auto* c=locate(s,key,status,true);
    if(!c) return fail(nullptr,status);
    if(c->activeProducer==maxProducers || !(key==c->producers[c->activeProducer].key)) return fail(c,Status::StaleKey);
    if(exit==Exit::Abnormal) {
        c->producers[c->activeProducer].active=false; c->activeProducer=maxProducers;
        return fail(c,Status::AbnormalExit);
    }
    if(c->invalidated) return fail(c,Status::ScopeInvalidated);
    if(c->pendingRegistration!=maxRegistrations) return fail(c,Status::OutOfOrder);
    c->producers[c->activeProducer].active=false; c->activeProducer=maxProducers;
    return accept(key,c);
}
Outcome Engine::beforeRegistration(const Stamp& s,const Key& key,const RegistrationBefore& e) noexcept {
    Status status; auto* c=locate(s,key,status);
    if(!c) return fail(nullptr,status);
    if(c->activeProducer==maxProducers || !(key==c->producers[c->activeProducer].key)) return fail(c,Status::StaleKey);
    if(c->pendingRegistration!=maxRegistrations) return fail(c,Status::ReentrantScope);
    if(c->registrationCount==limits_.registrations) return fail(c,Status::CapacityExhausted);
    if(!graphSame(e.graph,c->constructedGraph)) return fail(c,Status::GraphMismatch);
    status=graphValid(e.graph,e.passIndex); if(status!=Status::Accepted) return fail(c,status);
    if(e.parentPassIndex>=e.graph.count) return fail(c,Status::EntryMismatch);
    status=provenanceValid(*c,c->producers[c->activeProducer],e); if(status!=Status::Accepted) return fail(c,status);
    Descriptors descriptors;
    status=payloadValid(s,e.pass,e.source,descriptors); if(status!=Status::Accepted) return fail(c,status);
    status=cameraValid(e.camera); if(status!=Status::Accepted) return fail(c,status);
    Address cameraEnd=0;
    if(!add(e.provenance.camera,e.camera.clipToPrevCaptured?0x4C0:camera_probe::prefixBytes,cameraEnd))
        return fail(c,Status::AddressOverflow);
    for(std::size_t i=0;i<c->registrationCount;++i)
        if(c->registrations[i].before.passIndex==e.passIndex) return fail(c,Status::DuplicateRegistration);
    Key k=allocate(s,c->key.id); if(!k) return fail(c,Status::IdExhausted);
    c->pendingRegistration=c->registrationCount++;
    auto& r=c->registrations[c->pendingRegistration];
    r.key=k; r.producer=key; r.before=e; r.descriptors=descriptors;
    return accept(k,c);
}
Outcome Engine::returnRegistration(const Stamp& s,const Key& key,const RegistrationReturn& e) noexcept {
    Status status; auto* c=locate(s,key,status);
    if(!c) return fail(nullptr,status);
    if(c->pendingRegistration==maxRegistrations || !(key==c->registrations[c->pendingRegistration].key))
        return fail(c,Status::StaleKey);
    auto& r=c->registrations[c->pendingRegistration];
    if(!provenanceEqual(e.provenance,r.before.provenance)) return fail(c,Status::GraphMismatch);
    if(!graphSame(e.graph,c->constructedGraph)) return fail(c,Status::GraphMismatch);
    status=graphValid(e.graph,e.entry.index); if(status!=Status::Accepted) return fail(c,status);
    Address entry=0,thunk=0;
    if(!add(e.graph.table,static_cast<Address>(e.entry.index)*0x38,entry) ||
       !add(s.moduleBase,spec(r.before.pass)->thunk,thunk)) return fail(c,Status::AddressOverflow);
    if(e.entry.index!=r.before.passIndex || e.entry.address!=entry || e.entry.executor!=thunk ||
       !pointer(e.helperReturnRax) || (e.helperReturnRax&0xF) ||
       e.entry.copiedPayload!=e.helperReturnRax) return fail(c,Status::EntryMismatch);
    if(e.sourceAfter.size!=r.before.source.size || e.copy.size!=r.before.source.size) return fail(c,Status::SnapshotSizeMismatch);
    if(!payloadEqual(e.sourceAfter,r.before.source) || !payloadEqual(e.copy,r.before.source)) return fail(c,Status::PayloadChanged);
    status=cameraValid(e.camera); if(status!=Status::Accepted) return fail(c,status);
    if(!cameraEqual(e.camera,r.before.camera)) return fail(c,Status::CameraChanged);
    Address end=0;
    if(!add(e.helperReturnRax,e.copy.size,end)) return fail(c,Status::AddressOverflow);
    r.entry=e.entry; r.returnedGraph=e.graph; r.ready=true;
    c->pendingRegistration=maxRegistrations;
    return accept(key,c);
}
Outcome Engine::abortRegistration(const Stamp& s,const Key& key) noexcept {
    Status status; auto* c=locate(s,key,status,true);
    if(!c) return fail(nullptr,status);
    if(c->pendingRegistration==maxRegistrations || !(key==c->registrations[c->pendingRegistration].key))
        return fail(c,Status::StaleKey);
    c->pendingRegistration=maxRegistrations;
    return fail(c,Status::AbnormalExit);
}
Outcome Engine::enterExecutor(const Stamp& s,const Key& key,const ExecutorEntry& e) noexcept {
    Status status; auto* c=locate(s,key,status);
    if(!c) return fail(nullptr,status);
    if(!(key==c->key)) return fail(c,Status::StaleKey);
    if(c->executionActive || c->execution) return fail(c,Status::ReentrantScope);
    if(!c->construction || !c->rendererCompleted || c->rendererActive ||
       c->activeProducer!=maxProducers || c->pendingRegistration!=maxRegistrations) return fail(c,Status::OutOfOrder);
    if(e.entryRva!=executorRva || e.callerReturnRva!=0x20DE6F) return fail(c,Status::UnknownRoute);
    Address rsp=0,context=0;
    if(!sub(c->caller.originalRsp,0x120,rsp) || !sub(rsp,0x80,context)) return fail(c,Status::AddressOverflow);
    if(e.originalRsp!=rsp || !graphSame(e.graph,c->constructedGraph)) return fail(c,Status::GraphMismatch);
    status=graphValid(e.graph,0); if(status!=Status::Accepted) return fail(c,status);
    if(!stackRange(c->caller.stack,context,0x38)) return fail(c,Status::InvalidStackBounds);
    if(!pointer(e.graphValue40) || !pointer(e.graphValue58)) return fail(c,Status::InvalidAddress);
    c->execution=allocate(s,c->key.id); if(!c->execution) return fail(c,Status::IdExhausted);
    c->executorData=e; c->executionActive=true;
    return accept(c->execution,c);
}
Outcome Engine::enterCallback(const Stamp& s,const Key& key,const CallbackEntry& e) noexcept {
    Status status; auto* c=locate(s,key,status);
    if(!c) return fail(nullptr,status);
    if(!(key==c->execution)) return fail(c,Status::StaleKey);
    if(!c->executionActive) return fail(c,Status::OutOfOrder);
    if(c->callback) return fail(c,Status::ReentrantScope);
    if(e.callRva!=0x7EE0F || e.callerReturnRva!=0x7EE12) return fail(c,Status::UnknownRoute);
    if(e.encodedSchedulePass&1) return fail(c,Status::OddSchedule);
    if(!executingGraphSame(e.graph,c->executorData.graph)) return fail(c,Status::TableMismatch);
    status=graphValid(e.graph,e.entry.index); if(status!=Status::Accepted) return fail(c,status);
    Address entry=0,context=0,allocator=0;
    if(!add(e.graph.table,static_cast<Address>(e.entry.index)*0x38,entry) ||
       !sub(c->executorData.originalRsp,0x80,context) || !add(s.moduleBase,0x27F170,allocator)) return fail(c,Status::AddressOverflow);
    if(e.entry.index!=(e.encodedSchedulePass>>1) || e.entry.address!=entry ||
       !pointer(e.actualPayload) || e.actualPayload!=e.entry.copiedPayload) return fail(c,Status::EntryMismatch);
    const auto& x=e.context;
    if(x.address!=context || x.address==e.graph.graph || !pointer(x.deviceContext) ||
       x.ownerContext!=c->executorData.graphValue58 || x.graphValue40!=c->executorData.graphValue40 ||
       x.allocator!=allocator || x.arena!=e.graph.arena || !pointer(x.resourceBegin) ||
       x.resourceEnd<=x.resourceBegin || (x.resourceEnd-x.resourceBegin)%0x20 ||
       (x.resourceEnd-x.resourceBegin)/0x20>maxGraphEntries) return fail(c,Status::ContextMismatch);
    std::size_t found=maxRegistrations,matches=0;
    for(std::size_t i=0;i<c->registrationCount;++i) {
        const auto& r=c->registrations[i];
        if(r.ready && r.before.passIndex==e.entry.index && r.entry.executor==e.entry.executor &&
           r.entry.copiedPayload==e.actualPayload) { found=i; ++matches; }
    }
    if(!matches) return fail(c,Status::UnmatchedCallback);
    if(matches!=1) return fail(c,Status::AmbiguousMatch);
    auto& r=c->registrations[found];
    if(r.started) return fail(c,Status::DuplicateCallback);
    if(e.snapshot.size!=r.before.source.size) return fail(c,Status::SnapshotSizeMismatch);
    if(!payloadEqual(e.snapshot,r.before.source)) return fail(c,Status::PayloadChanged);
    c->callback=allocate(s,c->key.id); if(!c->callback) return fail(c,Status::IdExhausted);
    c->callbackRegistration=found; c->callbackData=e; ++c->callbackOrdinal;
    r.started=true;
    return accept(c->callback,c);
}
void Engine::copyAssociation(const CallerSlot& c,bool completed,Association& a) const noexcept {
    a={}; const auto& r=c.registrations[c.callbackRegistration];
    a.caller=c.key; a.construction=c.construction; a.renderer=c.renderer; a.producer=r.producer;
    a.registration=r.key; a.execution=c.execution; a.callback=c.callback;
    a.pass=r.before.pass; a.provenance=r.before.provenance; a.graph=c.callbackData.graph;
    a.entry=c.callbackData.entry; a.context=c.callbackData.context;
    a.parentPassIndex=r.before.parentPassIndex; a.encodedSchedulePass=c.callbackData.encodedSchedulePass;
    a.callbackOrdinal=c.callbackOrdinal; a.payload=r.before.source; a.descriptors=r.descriptors;
    a.camera=r.before.camera; a.cpuBuildExecutionAssociated=true;
    a.executionActive=c.executionActive; a.callbackActive=!completed; a.callbackCompleted=completed;
}
Outcome Engine::activeAssociation(const Stamp& s,const Key& key,Association& a) noexcept {
    a={}; Status status; auto* c=locate(s,key,status);
    if(!c) return fail(nullptr,status);
    // A rejected read-only query never poisons a different live callback.
    if(!(key==c->callback)) return fail(nullptr,Status::StaleKey);
    if(!c->executionActive || c->callbackRegistration==maxRegistrations) return fail(nullptr,Status::OutOfOrder);
    copyAssociation(*c,false,a);
    auto out=accept(key,c); out.status=Status::ActiveAssociation; return out;
}
Outcome Engine::exitCallback(const Stamp& s,const Key& key,Exit exit,Association* a) noexcept {
    if(a) *a={}; Status status; auto* c=locate(s,key,status,true);
    if(!c) return fail(nullptr,status);
    if(!(key==c->callback) || c->callbackRegistration==maxRegistrations) return fail(c,Status::StaleKey);
    if(exit==Exit::Abnormal) {
        c->callback={}; c->callbackRegistration=maxRegistrations;
        return fail(c,Status::AbnormalExit);
    }
    if(c->invalidated) return fail(c,Status::ScopeInvalidated);
    if(!c->executionActive) return fail(c,Status::OutOfOrder);
    if(a) copyAssociation(*c,true,*a);
    c->registrations[c->callbackRegistration].completed=true;
    c->callback={}; c->callbackRegistration=maxRegistrations;
    auto out=accept(key,c); out.status=Status::CompletedAssociation; return out;
}
Outcome Engine::exitExecutor(const Stamp& s,const Key& key,Exit exit) noexcept {
    Status status; auto* c=locate(s,key,status,true);
    if(!c) return fail(nullptr,status);
    if(!(key==c->execution)) return fail(c,Status::StaleKey);
    if(exit==Exit::Abnormal) {
        c->executionActive=false; c->callback={}; c->callbackRegistration=maxRegistrations;
        return fail(c,Status::AbnormalExit);
    }
    if(c->invalidated) { c->executionActive=false; return fail(c,Status::ScopeInvalidated); }
    if(!c->executionActive || c->callback) return fail(c,Status::OutOfOrder);
    c->executionActive=false; c->executionCompleted=true;
    auto out=accept(key,c);
    if(out.summary.unexecuted) out.status=Status::UnexecutedRegistrations;
    return out;
}
Outcome Engine::exitCaller(const Stamp& s,const Key& key,Exit exit) noexcept {
    Status status; auto* c=locate(s,key,status,true);
    if(!c) return fail(nullptr,status);
    if(!(key==c->key)) return fail(c,Status::StaleKey);
    const Summary summary=summarize(*c);
    Status result=Status::Accepted;
    if(exit==Exit::Abnormal) result=Status::AbnormalExit;
    else if(c->invalidated) result=Status::ScopeInvalidated;
    else if(c->rendererActive || c->activeProducer!=maxProducers ||
            c->pendingRegistration!=maxRegistrations || c->executionActive || c->callback ||
            !c->construction || !c->rendererCompleted || !c->executionCompleted) result=Status::OutOfOrder;
    else if(summary.unexecuted) result=Status::UnexecutedRegistrations;
    for(auto& t:threads_) if(t.depth && &t.callers[t.depth-1]==c) {
        // Release logical ownership only; owned output values remain in their owners.
        t.callers[--t.depth]=CallerSlot{};
        if(!t.depth) t.stamp={};
        break;
    }
    return {result,{},summary};
}
} // namespace xess_vk::graph_scope
