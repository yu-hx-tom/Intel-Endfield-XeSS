#undef NDEBUG
#include "render_tail_diagnostics.hpp"
#include <cassert>
#include <iostream>
#include <string>
#include <vector>

using namespace xess_vk;
using namespace xess_vk::render_tail;
static std::vector<std::string> lines;
static void sink(const char* line) { lines.emplace_back(line); }
static CommandRecordingFacts facts(std::uint64_t command, std::uint64_t generation) {
    CommandRecordingFacts f{};f.device=1;f.contextIncarnation=1;f.command=command;f.rpRecordingGeneration=generation;
    f.recording=true;f.stateKnown=true;return f;
}
static Event copy(const CommandRecordingFacts& f, std::uint64_t destination=200, std::uint64_t generation=2) {
    Event e{};e.kind=Kind::Copy;e.recording=f;e.imageCount=2;
    e.regionCount=1;e.regions[0].extent={1920,1080,1};
    e.images[0].record={100,1,{37,1920,1080,1,1,1}};e.images[0].role=TransferSource;
    e.images[1].record={destination,generation,{37,2880,1800,1,1,1}};e.images[1].role=TransferDestination;return e;
}
int main(int argc,char** argv) {
    Store store(1,sink);
    wsi_image_inventory::OwnedImage wsi{};wsi.record={200,2,{37,2880,1800,1,1,1}};wsi.owner={300,5};wsi.index=1;
    store.wsiAdded(wsi);auto f=facts(10,1);store.record(copy(f));assert(!store.stats().observed);
    assert(!store.beforeSubmit(7,1,std::array<Key,1>{{{10,1}}}).serial);
    store.arm();assert(store.active());
    store.begin(f,VK_SUCCESS);store.record(copy(f,999,99));
    assert(!store.beforeSubmit(7,2,std::array<Key,1>{{{10,1}}}).serial); // Native private destination.
    for(unsigned n=0;n<40;++n){Event e{};e.kind=Kind::Dispatch;e.recording=f;store.record(e);}
    store.record(copy(f));assert(store.stats().overwritten>0);
    auto ticket=store.beforeSubmit(7,3,std::array<Key,1>{{{10,1}}});assert(ticket.serial);
    store.reset(10);auto newer=facts(10,2);store.begin(newer,VK_SUCCESS); // Frozen snapshot survives numeric CB reuse.
    store.afterSubmit(ticket,VK_ERROR_DEVICE_LOST);assert(lines.empty()&&store.stats().failedTargets==1);
    store.record(copy(newer));ticket=store.beforeSubmit(7,4,std::array<Key,1>{{{10,2}}});assert(ticket.serial);
    store.afterSubmit(ticket,VK_SUCCESS);assert(store.stats().emittedSamples==1&&!lines.empty());
    assert(lines.front().find("\"frame_association_unknown\":true")!=std::string::npos);
    bool oldGeneration=false;for(const auto& line:lines)if(line.find("\"rp_recording_generation\":2")!=std::string::npos)oldGeneration=true;assert(oldGeneration);
    store.wsiRemoved({200,2});store.record(copy(newer));assert(!store.beforeSubmit(7,5,std::array<Key,1>{{{10,2}}}).serial);
    wsi.record.generation=3;store.wsiAdded(wsi);store.record(copy(newer));assert(!store.beforeSubmit(7,6,std::array<Key,1>{{{10,2}}}).serial);
    store.record(copy(newer,200,3));ticket=store.beforeSubmit(7,7,std::array<Key,1>{{{10,2}}});assert(ticket.serial);
    store.afterSubmit(ticket,VK_SUCCESS);assert(store.stats().emittedSamples==2&&!store.active());
    assert(!store.beforeSubmit(7,8,std::array<Key,1>{{{10,2}}}).serial);
    Store drawn(1,sink);drawn.wsiAdded(wsi);drawn.arm();drawn.begin(f,VK_SUCCESS);
    Event pass{};pass.kind=Kind::Pass;pass.recording=f;pass.imageCount=1;pass.images[0].record=wsi.record;pass.images[0].role=Attachment;
    drawn.record(pass);assert(!drawn.beforeSubmit(7,1,std::array<Key,1>{{{10,1}}}).serial); // load-only pass is not a write anchor.
    for(unsigned n=0;n<1000;++n)drawn.draw(f,{});
    assert(drawn.stats().observed==1);assert(drawn.beforeSubmit(7,2,std::array<Key,1>{{{10,1}}}).serial);
    Store timeout(1,sink);timeout.arm();for(unsigned n=0;n<120;++n)timeout.present();assert(!timeout.active());
    Store workers(1,sink);workers.arm();workers.wsiAdded(wsi);
    for(unsigned n=0;n<16;++n){Event e{};e.kind=Kind::Dispatch;e.recording=facts(1000+n,100+n);workers.record(e);}
    const auto final=facts(9000,900);workers.record(copy(final,200,3));
    assert(workers.stats().evictedCommands==1);
    assert(workers.beforeSubmit(7,99,std::array<Key,1>{{{9000,900}}}).serial);
    // A later private copy must not replace the actual WSI anchor's frame.
    Store framed(1,sink);framed.arm();framed.wsiAdded(wsi);
    Source accepted{};accepted.accepted=true;accepted.frame={123,{77,8,true},0,true};framed.source(accepted);
    auto anchor=copy(f,200,3);anchor.kind=Kind::Blit;anchor.frame=accepted.frame;framed.record(anchor);
    auto privateCopy=copy(f,999,99);privateCopy.frame={124,{78,8,true},0,true};framed.record(privateCopy);
    lines.clear();const auto known=framed.beforeSubmit(7,100,std::array<Key,1>{{{10,1}}});assert(known.serial);
    assert(!framed.beforeSubmit(7,101,std::array<Key,1>{{{10,1}}}).serial); // Pending known ticket is not sampled twice.
    framed.afterSubmit(known,VK_SUCCESS);assert(framed.stats().emittedSamples==1);
    assert(lines.front().find("\"frame_association_unknown\":false")!=std::string::npos);
    assert(lines.front().find("\"engine_frame\":123")!=std::string::npos);
    framed.record(anchor);assert(!framed.beforeSubmit(7,102,std::array<Key,1>{{{10,1}}}).serial);
    // A direct WSI graphics pass can read two inputs with separate writer CBs.
    Store composed(1,sink);composed.arm();composed.wsiAdded(wsi);
    auto writerA=copy(facts(31,31),100,1);composed.record(writerA);
    auto writerB=copy(facts(32,32),101,1);composed.record(writerB);
    composed.submitted(8,80,std::array<Key,1>{{{32,32}}},VK_SUCCESS);
    pass.recording=facts(33,33);pass.images[0].record=wsi.record;composed.record(pass);
    std::array<Image,2> inputs{};inputs[0].record={100,1,{37,1920,1080,1,1,1}};inputs[0].role=Sampled;
    inputs[1]=inputs[0];inputs[1].record.image=101;
    composed.draw(pass.recording,inputs);lines.clear();
    auto combined=composed.beforeSubmit(7,103,std::array<Key,2>{{{31,31},{33,33}}});assert(combined.serial);
    composed.afterSubmit(combined,VK_SUCCESS);assert(composed.stats().emittedSamples==1);
    assert(lines.front().find("\"related_command_count\":2")!=std::string::npos);
    bool sameSubmit=false,recent=false,passAnchor=false;
    for(const auto& row:lines){sameSubmit|=row.find("same_submit_writer_candidate")!=std::string::npos;recent|=row.find("recent_other_cb_not_dependency_proof")!=std::string::npos;passAnchor|=row.find("\"anchor_kind\":\"render_pass\"")!=std::string::npos;}
    assert(sameSubmit&&recent&&passAnchor);
    assert(store.stats().emittedBytes<=byteLimit);
    if(argc==2&&std::string(argv[1])=="--jsonl")for(const auto& row:lines)std::cout<<row<<'\n';
    else std::cout<<"render tail: disabled/private/ABA/failed-submit/ring/WSI-generation/two-sample/draw-aggregation/timeout/capacity/anchor-frame/dedup/multi-input checks passed; fixed bytes="<<Store::storageBytes()<<'\n';
}
