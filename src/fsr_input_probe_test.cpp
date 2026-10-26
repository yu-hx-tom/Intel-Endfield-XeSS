#include "fsr_input_probe.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <vector>

namespace {
using namespace xess_vk::fsr_probe;
unsigned cases = 0, checks = 0;
void require(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
struct Read { Address address; std::size_t size, remaining; };
struct Memory {
    std::map<Address, std::vector<unsigned char>> regions;
    std::vector<Read> calls;
    Address denied = 0, partial = 0, throwing = 0, overReported = 0, changingBounds = 0;
    Address changingDescriptor = 0;
    unsigned boundsReads = 0, boundsChangeOnRead = 2, descriptorReads = 0, descriptorChangeOffset = 16;
    unsigned denyOnRead = 0, partialOnRead = 0;
    std::map<Address,unsigned> addressReadCounts;
    void region(Address address, std::size_t size) { regions.emplace(address, std::vector<unsigned char>(size)); }
    unsigned char* at(Address address, std::size_t size) {
        auto found = regions.upper_bound(address);
        if (found == regions.begin()) return nullptr;
        --found;
        const auto offset = address - found->first;
        if (offset > found->second.size() || size > found->second.size() - offset) return nullptr;
        return found->second.data() + static_cast<std::size_t>(offset);
    }
    void word(Address address, std::uint32_t value) {
        auto* data = at(address, 4);
        if (!data) throw std::runtime_error("Fixture word is outside owned fake memory");
        for (unsigned i = 0; i < 4; ++i) data[i] = static_cast<unsigned char>(value >> (i * 8));
    }
    void pointer(Address address, Address value) {
        auto* data = at(address, 8);
        if (!data) throw std::runtime_error("Fixture pointer is outside owned fake memory");
        for (unsigned i = 0; i < 8; ++i) data[i] = static_cast<unsigned char>(value >> (i * 8));
    }
    static std::size_t read(void* context, Address address, void* destination, std::size_t size, std::size_t remaining) {
        auto& self = *static_cast<Memory*>(context);
        require(size <= remaining, "Reader was invoked beyond supplied remaining byte budget");
        self.calls.push_back({address, size, remaining});
        const auto occurrence=++self.addressReadCounts[address];
        if (address == self.throwing) throw std::runtime_error("Synthetic read failure");
        if (address == self.overReported) return size + 1;
        if (address == self.denied && (!self.denyOnRead || occurrence==self.denyOnRead)) return 0;
        const auto* source = self.at(address, size);
        if (!source) return 0;
        const auto copied = address == self.partial && (!self.partialOnRead || occurrence==self.partialOnRead) ? size - 1 : size;
        std::memcpy(destination, source, copied);
        if (address == self.changingBounds && ++self.boundsReads == self.boundsChangeOnRead && size == 16)
            static_cast<unsigned char*>(destination)[8] ^= 0x20;
        if (address == self.changingDescriptor && ++self.descriptorReads==2 && size==24)
            static_cast<unsigned char*>(destination)[self.descriptorChangeOffset]^=1;
        return copied;
    }
    bool touched(Address address) const {
        return std::any_of(calls.begin(), calls.end(), [address](const Read& read) {
            return address >= read.address && address - read.address < read.size;
        });
    }
};
struct Fixture {
    Memory memory;
    Request request;
    static constexpr Address payload = 0x10000, context = 0x20000, graph = 0x30000, resources = 0x40000;
    static constexpr Address table = 0x100000, objects = 0x3000000;
    static constexpr Address constantsData = 0x50000;
    static constexpr std::array<std::uint32_t, 5> ids{1, 0x3FF, 0x400, 0x10001, 0xFFFFF};
    static constexpr std::array<unsigned, 5> order{2, 4, 0, 3, 1};
    static constexpr std::array<Address, 5> properties{0x1D10784, 0x1D10788, 0x1D107E4, 0x1D107E8, 0x1D107EC};
    static Address page(std::uint32_t id) { return 0x200000 + Address(id >> 10) * 0x4000; }
    Fixture() {
        request.layout = InputLayout::UnityPlayerBee7be52PrepareInput;
        request.moduleBase = 0x700000000; // Deliberately relocated from the preferred image base.
        request.prepareInputPayload = payload; request.graphContext = context;
        memory.region(payload, 0xD0); memory.region(context, 0x20); memory.region(graph, 5 * 0x20);
        memory.region(resources, 0x100); memory.region(table, 0x2000);
        memory.pointer(context + 0x10, graph); memory.pointer(context + 0x18, graph + 5 * 0x20);
        for (unsigned i = 0; i < 5; ++i) {
            memory.word(graph + i * 0x20, (i + 1) * 10);
            memory.word(graph + i * 0x20 + 8, i % 2 ? 3 : 2);
            memory.pointer(graph + i * 0x20 + 0x10, resources + i * 0x10);
            memory.word(resources + i * 0x10, ids[i]);
            memory.pointer(payload + 0x10 + i * 0x20, 0x123400000000ull | (order[i] + 1) * 10);
            memory.pointer(payload + 0x20 + i * 0x20, request.moduleBase + properties[i]);
            memory.region(page(ids[i]), 0x2000);
            memory.pointer(table + Address(ids[i] >> 10) * 8, page(ids[i]));
            memory.pointer(page(ids[i]) + Address(ids[i] & 0x3FF) * 8, objects + i * 0x100);
            memory.region(objects + i * 0x100, 0x40);
            memory.word(objects + i * 0x100 + 0x14, 127 + i);
            memory.word(objects + i * 0x100 + 0x18, 73 + i);
            memory.pointer(objects + i * 0x100 + 0x38, 0xF123000000000000ull + i);
        }
    }
    void native() {
        request.nativeTableBase = table;
        request.nativeLayout = NativeTableLayout::DirectArrayOf1024PagePointers_1024ObjectPointers;
    }
    void constants() {
        request.captureConstantsFI=true;
        memory.region(constantsData,constantsFIBytes);
        memory.word(payload+0xB0,17); memory.word(payload+0xB4,128);
        memory.word(payload+0xB8,constantsFIBytes); memory.word(payload+0xBC,0xDEADBEEF);
        memory.pointer(payload+0xC0,constantsData);
        auto* bytes=memory.at(constantsData,constantsFIBytes);
        for(unsigned i=0;i<constantsFIBytes;++i) bytes[i]=static_cast<unsigned char>(i*37+19);
    }
    Result run() {
        memory.calls.clear(); memory.boundsReads = 0; memory.descriptorReads=0; memory.addressReadCounts.clear();
        auto result = readPrepareInput(request, {Memory::read, &memory});
        require(!result.snapshotAtomic && !result.pixelsRead && !result.gameFunctionsCalled
            && !result.gameFrameInputsVerified && !result.xeFGIntegrated, "Probe upgraded metadata to verification");
        require(result.bytesRequested <= request.limits.maxBytes && result.readCalls <= request.limits.maxReadCalls,
            "Parser exceeded configured read limits");
        require(result.bytesCopied <= result.bytesRequested && result.readCalls == memory.calls.size(), "Read accounting incorrect");
        std::size_t total = 0;
        for (const auto& call : memory.calls) total += call.size;
        require(total == result.bytesRequested, "Failed read requests were not charged");
        for (const auto& binding : result.bindings)
            require(!binding.native.idDomainVerified && !binding.native.vulkanImageVerified, "Native candidate incorrectly verified");
        require(!result.constantsFI.snapshotAtomic&&!result.constantsFI.lifetimePinned&&!result.constantsFI.shaderSemanticsVerified,
            "Raw constant bytes were promoted to shader/lifetime verification");
        if(!result.constantsFI.captured)
            require(std::all_of(result.constantsFI.bytes.begin(),result.constantsFI.bytes.end(),[](auto b){return b==0;}),
                "Failed or unrequested constant snapshot exposed bytes");
        return result;
    }
};

void normalAndNative() {
    ++cases;
    Fixture f;
    auto result = f.run();
    require(result.status == Status::SnapshotParsed && result.graphEntryCount == 5 && result.graphTextureIdsRead == 5,
        "Normal graph snapshot did not resolve five entries");
    require(result.contextBoundsUnchanged && result.allFivePreparePropertiesPresent && result.nativeCandidatesRead == 0,
        "Prepare properties or disabled native lookup incorrect");
    require(!f.memory.touched(Fixture::table), "Optional native table was read without a supplied base");
    for (unsigned i = 0; i < 5; ++i) {
        require(result.bindings[i].textureId == Fixture::ids[Fixture::order[i]], "Key matching used entry order or graph handle high bits");
        require(result.bindings[i].graphHandle > 0xFFFFFFFF && result.bindings[i].graphStatus == Status::GraphTextureIdRead,
            "Full graph handle was not retained separately");
        require(result.bindings[i].property != Property::Unknown && !f.memory.touched(result.bindings[i].propertyAddress),
            "Known property label was not derived from RVA or was unnecessarily dereferenced");
    }
    ++cases;
    f.native(); result = f.run();
    require(result.status == Status::SnapshotParsed && result.nativeCandidatesRead == 5, "Native metadata candidates did not resolve");
    for (unsigned i = 0; i < 5; ++i) {
        const auto index = Fixture::order[i];
        const auto& candidate = result.bindings[i].native;
        require(candidate.status == Status::NativeCandidateReadUnverified && candidate.object == Fixture::objects + index * 0x100,
            "Direct two-level lookup used wrong pointer indirection");
        require(candidate.widthCandidate == 127 + index && candidate.heightCandidate == 73 + index,
            "Candidate dimension offsets are incorrect");
        require(candidate.imageCandidate == 0xF123000000000000ull + index && !f.memory.touched(candidate.imageCandidate),
            "Image candidate was misread or dereferenced as CPU memory");
    }
}

void graphFailureCases() {
    {
        ++cases; Fixture f;
        f.memory.pointer(Fixture::payload + 0x10, 25);
        auto r = f.run(); require(r.bindings[0].graphStatus == Status::GraphKeyMissing, "Lower-bound next key falsely accepted");
        require(r.graphTextureIdsRead == 4, "Missing key was counted as resolved");
    }
    {
        ++cases; Fixture f; f.memory.pointer(Fixture::payload + 0x10, 999);
        require(f.run().bindings[0].graphStatus == Status::GraphKeyMissing, "Past-end search not rejected");
    }
    {
        ++cases; Fixture f; f.memory.word(Fixture::graph + 2 * 0x20 + 8, 1);
        auto r=f.run(); require(r.bindings[0].graphStatus == Status::UnsupportedGraphEntryType, "Nontexture graph type was interpreted as texture");
        require(!f.memory.touched(Fixture::resources + 2 * 0x10), "Unsupported entry payload was followed");
    }
    {
        ++cases; Fixture f; f.memory.pointer(Fixture::graph + 2 * 0x20 + 0x10, 0);
        require(f.run().bindings[0].graphStatus == Status::NullResourcePayload, "Null payload was followed");
    }
    {
        ++cases; Fixture f; f.memory.word(Fixture::resources + 2 * 0x10, 0);
        require(f.run().bindings[0].graphStatus == Status::NullTextureId, "Zero ID marked usable");
    }
    {
        ++cases; Fixture f; f.memory.pointer(Fixture::context + 0x18, Fixture::graph - 1);
        require(f.run().status == Status::InvalidGraphRange, "Reversed table range not rejected");
    }
    {
        ++cases; Fixture f; f.memory.pointer(Fixture::context + 0x18, Fixture::graph + 31);
        require(f.run().status == Status::InvalidGraphRange, "Non-stride-aligned range not rejected");
    }
    {
        ++cases; Fixture f; f.memory.pointer(Fixture::context + 0x10, 0); f.memory.pointer(Fixture::context + 0x18, 0);
        auto r=f.run(); require(r.status == Status::SnapshotParsed && r.graphTextureIdsRead == 0, "Empty graph table not handled");
        require(r.bindings[0].graphStatus == Status::GraphKeyMissing, "Empty graph search read end");
    }
    {
        ++cases; Fixture f; f.memory.word(Fixture::graph + 0x20, 10);
        require(f.run().status == Status::DuplicateGraphKey, "Duplicate keys were not rejected");
    }
    {
        ++cases; Fixture f; f.memory.word(Fixture::graph, 999);
        require(f.run().status == Status::GraphKeysUnsorted, "Unsorted table used for binary search");
    }
    {
        ++cases; Fixture f; f.memory.pointer(Fixture::graph + 2 * 0x20 + 0x10, Fixture::graph + 2 * 0x20);
        require(f.run().bindings[0].graphStatus == Status::PointerCycleOrOverlap, "Self-referential resource payload not rejected");
    }
    {
        ++cases; Fixture f; f.memory.changingBounds=Fixture::context + 0x10;
        auto r=f.run(); require(r.status == Status::ContextBoundsChanged && !r.contextBoundsUnchanged, "Changed graph bounds were not reported");
    }
    {
        ++cases; Fixture f; f.memory.pointer(Fixture::payload + 0x20, f.request.moduleBase + 0xDEAD);
        auto r=f.run(); require(r.bindings[0].property == Property::Unknown && !r.allFivePreparePropertiesPresent,
            "Unknown module-relative property was given an invented label");
    }
}

void readerAndLimitCases() {
    {
        ++cases; Fixture f; f.request.layout=InputLayout::Unknown;
        require(f.run().status == Status::UnknownInputLayout && f.memory.calls.empty(), "Unknown layout read memory");
    }
    {
        ++cases; Fixture f;
        require(readPrepareInput(f.request, {}).status == Status::MissingReader, "Missing callback accepted");
    }
    {
        ++cases; Fixture f; f.memory.denied=Fixture::payload + 0x10;
        require(f.run().status == Status::Unreadable, "No-access initial payload not reported");
    }
    {
        ++cases; Fixture f; f.memory.partial=Fixture::graph;
        require(f.run().status == Status::PartialRead, "Partial table copy was parsed");
    }
    {
        ++cases; Fixture f; f.memory.throwing=Fixture::context + 0x10;
        require(f.run().status == Status::CallbackException, "Callback exception escaped or was ignored");
    }
    {
        ++cases; Fixture f; f.memory.overReported=Fixture::payload + 0x10;
        require(f.run().status == Status::CallbackContractViolation, "Over-reporting reader accepted");
    }
    {
        ++cases; Fixture f; f.request.limits.maxBytes=159;
        require(f.run().status == Status::ByteBudgetExceeded && f.memory.calls.empty(), "Oversize first read reached callback");
    }
    {
        ++cases; Fixture f; f.request.limits.maxBytes=200;
        require(f.run().status == Status::ByteBudgetExceeded && !f.memory.touched(Fixture::graph), "Graph snapshot exceeded byte budget");
    }
    {
        ++cases; Fixture f; f.request.limits.maxReadCalls=1;
        require(f.run().status == Status::ReadCallBudgetExceeded && f.memory.calls.size() == 1, "Read-call budget exceeded");
    }
    {
        ++cases; Fixture f; f.request.limits.maxGraphEntries=4;
        require(f.run().status == Status::GraphEntryLimitExceeded && !f.memory.touched(Fixture::graph), "Oversize table was read");
    }
    {
        ++cases; Fixture f; f.request.limits.maxGraphEntries=hardMaxGraphEntries + 1;
        require(f.run().status == Status::InvalidLimits && f.memory.calls.empty(), "Hard graph limit was bypassed");
    }
    {
        ++cases; Fixture f; f.request.prepareInputPayload=std::numeric_limits<Address>::max() - 15;
        require(f.run().status == Status::AddressOverflow && f.memory.calls.empty(), "Payload offset wrapped");
    }
    {
        ++cases; Fixture f; f.memory.pointer(Fixture::graph + 2 * 0x20 + 0x10, 0xABCDEF00);
        require(f.run().bindings[0].graphStatus == Status::Unreadable, "Unmapped resource pointer was not rejected");
    }
    {
        ++cases; Fixture f; f.memory.pointer(Fixture::graph + 2 * 0x20 + 0x10, std::numeric_limits<Address>::max() - 1);
        require(f.run().bindings[0].graphStatus == Status::AddressOverflow, "Resource ID read wrapped address space");
    }
}

void nativeFailureCases() {
    {
        ++cases; Fixture f; f.native(); f.request.nativeLayout=NativeTableLayout::Unknown;
        auto r=f.run(); require(r.bindings[0].native.status == Status::UnknownNativeTableLayout
            && !f.memory.touched(Fixture::table), "Unknown native layout was guessed");
    }
    {
        ++cases; Fixture f; f.native(); f.memory.word(Fixture::resources + 2 * 0x10, 0x100400);
        auto r=f.run(); require(r.bindings[0].textureId == 0x100400 && r.bindings[0].native.status == Status::NativeIdOutOfRange,
            "High ID bits were masked to manufacture a native match");
    }
    {
        ++cases; Fixture f; f.native(); f.memory.pointer(Fixture::table + 8, 0);
        require(f.run().bindings[0].native.status == Status::NullNativePage, "Null native page followed");
    }
    {
        ++cases; Fixture f; f.native(); f.memory.pointer(Fixture::page(0x400), 0);
        require(f.run().bindings[0].native.status == Status::NullNativeObject, "Null native object followed");
    }
    {
        ++cases; Fixture f; f.native(); f.memory.pointer(Fixture::table + 8, Fixture::table);
        require(f.run().bindings[0].native.status == Status::PointerCycleOrOverlap, "Native page self-cycle not rejected");
    }
    {
        ++cases; Fixture f; f.native(); f.memory.pointer(Fixture::page(0x400), Fixture::page(0x400));
        require(f.run().bindings[0].native.status == Status::PointerCycleOrOverlap, "Native object cycle not rejected");
    }
    {
        ++cases; Fixture f; f.native(); f.memory.denied=Fixture::objects + 2 * 0x100 + 0x14;
        require(f.run().bindings[0].native.status == Status::Unreadable, "Native dimensions no-access ignored");
    }
    {
        ++cases; Fixture f; f.native(); f.request.nativeTableBase=std::numeric_limits<Address>::max() - 4;
        require(f.run().bindings[0].native.status == Status::AddressOverflow, "Native table address overflow ignored");
    }
}

void constantsCases() {
    for(bool native : {false,true}) {
        ++cases; Fixture f; if(native) f.native();
        f.constants(); f.request.captureConstantsFI=false;
        const auto original=f.run();
        require(original.constantsFI.status==Status::NotRequested&&!f.memory.touched(Fixture::payload+0xB0)
            &&!f.memory.touched(Fixture::constantsData),"Default parser read optional constants");
        f.request.captureConstantsFI=true;
        const auto r=f.run(); const auto& c=r.constantsFI;
        require(r.status==Status::SnapshotParsed&&c.status==Status::ConstantsSnapshotReadUnverified&&c.captured,
            "Exact raw constant slice was not captured");
        require(c.descriptorRead&&c.descriptorUnchanged&&c.graphBoundsUnchanged,"Constant consistency rechecks missing");
        require(c.bufferId==17&&c.byteOffset==128&&c.byteSize==224&&c.unknownBC==0xDEADBEEF
            &&c.cpuPointer==Fixture::constantsData,"Constant descriptor offsets or opaque BC changed");
        require(!std::memcmp(c.bytes.data(),f.memory.at(Fixture::constantsData,constantsFIBytes),constantsFIBytes),
            "Raw constant bytes were decoded or truncated");
        require(r.bytesRequested==original.bytesRequested+288&&r.readCalls==original.readCalls+4,
            "Constant snapshot did not share exact bounded reader accounting");
        require(r.graphTextureIdsRead==original.graphTextureIdsRead&&r.nativeCandidatesRead==original.nativeCandidatesRead,
            "Constants changed normal texture candidate counts");
        for(unsigned i=0;i<bindingCount;++i)
            require(r.bindings[i].graphStatus==original.bindings[i].graphStatus&&r.bindings[i].native.status==original.bindings[i].native.status,
                "Constants promoted original binding status");
    }
    for(unsigned failure=0;failure<4;++failure) {
        ++cases; Fixture f; f.constants();
        if(failure==0) f.memory.pointer(Fixture::payload+0x20,f.request.moduleBase+0xABCD);
        if(failure==1) f.memory.pointer(Fixture::payload+0x20,f.request.moduleBase+Fixture::properties[1]);
        if(failure==2) f.memory.pointer(Fixture::payload+0x10,999);
        if(failure==3) { f.memory.changingBounds=Fixture::context+0x10; }
        const auto r=f.run();
        require(r.constantsFI.status==Status::ConstantsPrerequisitesMissing&&!r.constantsFI.captured
            &&!f.memory.touched(Fixture::payload+0xB0),"Constants read without valid original properties/graph");
    }
    for(uint32_t size : {0u,223u,225u,UINT32_MAX}) {
        ++cases; Fixture f; f.constants(); f.memory.word(Fixture::payload+0xB8,size);
        const auto r=f.run();
        require(r.constantsFI.status==Status::ConstantsSizeMismatch&&!f.memory.touched(Fixture::constantsData),
            "Unexpected constant size reached CPU pointer");
        require(r.status==Status::SnapshotParsed&&r.graphTextureIdsRead==5,"Independent constant failure damaged texture status");
    }
    for(Address pointer : {Address(0),UINT64_MAX-constantsFIBytes+1,UINT64_MAX}) {
        ++cases; Fixture f; f.constants(); f.memory.pointer(Fixture::payload+0xC0,pointer);
        const auto r=f.run();
        require(r.constantsFI.status==(pointer?Status::AddressOverflow:Status::InvalidAddress)&&!r.constantsFI.captured,
            "Null/overflowing CPU pointer accepted");
    }
    for(Address pointer : {Fixture::payload,Fixture::payload+0xC8,Fixture::payload-223,Fixture::context,Fixture::graph,
            Fixture::resources+2*0x10,Fixture::table+0x100,Fixture::page(0x400)+0x100,Fixture::objects+2*0x100+0x38}) {
        ++cases; Fixture f; f.constants(); f.native(); f.memory.pointer(Fixture::payload+0xC0,pointer);
        const auto r=f.run();
        require(r.constantsFI.status==Status::PointerCycleOrOverlap&&!r.constantsFI.captured,
            "Constant slice overlapped known input/metadata region");
        require(std::none_of(f.memory.calls.begin(),f.memory.calls.end(),[&](const Read& read){return read.address==pointer&&read.size==224;}),
            "Overlapping constant slice was read");
    }
    for(unsigned fault=0;fault<4;++fault) for(Address where : {Fixture::payload+0xB0,Fixture::constantsData}) {
        ++cases; Fixture f; f.constants();
        if(fault==0) f.memory.denied=where;
        if(fault==1) f.memory.partial=where;
        if(fault==2) f.memory.throwing=where;
        if(fault==3) f.memory.overReported=where;
        const std::array<Status,4> statuses{Status::Unreadable,Status::PartialRead,Status::CallbackException,Status::CallbackContractViolation};
        const auto r=f.run();
        require(r.constantsFI.status==statuses[fault]&&!r.constantsFI.captured,"Constant read failure was ignored");
    }
    for(unsigned changedByte : {0u,4u,8u,12u,16u,23u}) {
        ++cases; Fixture f; f.constants(); f.memory.changingDescriptor=Fixture::payload+0xB0;
        f.memory.descriptorChangeOffset=changedByte;
        const auto r=f.run();
        require(r.constantsFI.status==Status::ConstantsDescriptorChanged&&!r.constantsFI.descriptorUnchanged,
            "Changed descriptor published a constant snapshot");
    }
    for(bool partial : {false,true}) {
        ++cases; Fixture f; f.constants();
        if(partial) { f.memory.partial=Fixture::payload+0xB0; f.memory.partialOnRead=2; }
        else { f.memory.denied=Fixture::payload+0xB0; f.memory.denyOnRead=2; }
        const auto r=f.run();
        require(r.constantsFI.status==(partial?Status::PartialRead:Status::Unreadable)&&!r.constantsFI.captured,
            "Failed descriptor recheck published raw data");
    }
    for(unsigned fault=0;fault<3;++fault) {
        ++cases; Fixture f; f.constants();
        if(fault==0) { f.memory.changingBounds=Fixture::context+0x10; f.memory.boundsChangeOnRead=3; }
        if(fault==1) { f.memory.denied=Fixture::context+0x10; f.memory.denyOnRead=3; }
        if(fault==2) { f.memory.partial=Fixture::context+0x10; f.memory.partialOnRead=3; }
        const std::array<Status,3> statuses{Status::ContextBoundsChanged,Status::Unreadable,Status::PartialRead};
        const auto r=f.run();
        require(r.constantsFI.status==statuses[fault]&&r.status==statuses[fault]&&!r.contextBoundsUnchanged&&!r.constantsFI.captured,
            "Failed final graph recheck left candidate snapshot status valid");
    }
    // Every budget boundary is exercised; no callback may observe a request
    // exceeding its remaining bytes/calls, and no partial result is published.
    {
        Fixture baseline; const auto original=baseline.run();
        for(size_t extra : {size_t(0),size_t(23),size_t(24),size_t(247),size_t(248),size_t(271),size_t(272),size_t(287),size_t(288)}) {
            ++cases; Fixture f; f.constants(); f.request.limits.maxBytes=original.bytesRequested+extra;
            const auto r=f.run();
            require(r.constantsFI.captured==(extra==288),"Constant byte-budget boundary incorrect");
            if(extra!=288) require(r.constantsFI.status==Status::ByteBudgetExceeded,"Byte budget error not retained");
        }
        for(size_t extra=0;extra<=4;++extra) {
            ++cases; Fixture f; f.constants(); f.request.limits.maxReadCalls=original.readCalls+extra;
            const auto r=f.run();
            require(r.constantsFI.captured==(extra==4),"Constant read-call budget boundary incorrect");
            if(extra!=4) require(r.constantsFI.status==Status::ReadCallBudgetExceeded,"Read-call budget error not retained");
        }
    }
    {
        ++cases; Fixture f; f.constants(); f.request.prepareInputPayload=UINT64_MAX-0xC8;
        require(f.run().status==Status::AddressOverflow&&f.memory.calls.empty(),"Full D0 payload extent overflow ignored");
    }
}
void sharedPassCases() {
    constexpr std::array<std::array<Address,5>,3> sets{{Fixture::properties,
        {0x1D1073C,0x1D10740,0x1D10744,0x1D10748,0x1D107A8},
        {0x1D10738,0x1D1076C,0x1D1077C,0x1D10744,0x1D107E0}}};
    for(unsigned pass=0;pass<3;++pass) {
        Fixture f;f.constants();f.native();f.request.layout=InputLayout::UnityPlayerBee7be52SharedFivePass;
        f.request.expectedPass=static_cast<PassKind>(pass+1);
        std::array<unsigned,5> order{0,1,2,3,4};
        do {
            ++cases;
            for(unsigned i=0;i<5;++i) f.memory.pointer(Fixture::payload+0x20+i*0x20,f.request.moduleBase+sets[pass][order[i]]);
            const auto r=f.run();
            require(r.status==Status::SnapshotParsed&&r.pass==f.request.expectedPass&&r.allPassPropertiesPresent,
                "Complete unique shared pass set was not classified");
            require(r.payloadExtent==(pass?0xC8:0xD0)&&r.constantsFI.captured&&r.nativeCandidatesRead==5,
                "Shared-pass extent, constants or native lookup incorrect");
            require(r.allFivePreparePropertiesPresent==(pass==0),"Other pass was mislabeled as Prepare");
        } while(std::next_permutation(order.begin(),order.end()));
        ++cases;
        f.memory.pointer(Fixture::payload+0x20,f.request.moduleBase+sets[pass][1]);
        f.request.expectedPass=PassKind::Unknown;
        auto r=f.run();require(r.status==Status::UnknownPassPropertySet&&f.memory.calls.size()==1&&!r.constantsFI.captured,
            "Duplicate pass properties were resolved or read constants");
        for(unsigned i=0;i<5;++i) f.memory.pointer(Fixture::payload+0x20+i*0x20,f.request.moduleBase+sets[pass][i]);
        ++cases;
        f.request.expectedPass=static_cast<PassKind>((pass+1)%3+1);
        r=f.run();require(r.status==Status::PassPropertySetChanged&&f.memory.calls.size()==1&&!r.constantsFI.captured,
            "Prefilter/parser property-set change was attributed to expected pass");
        ++cases;
        f.request.expectedPass=static_cast<PassKind>(pass+1);
        const std::size_t extent=pass?0xC8:0xD0;
        f.memory.regions[Fixture::payload].resize(extent+constantsFIBytes);
        f.memory.pointer(Fixture::payload+0xC0,Fixture::payload+extent);
        std::fill_n(f.memory.at(Fixture::payload+extent,constantsFIBytes),constantsFIBytes,0xA5);
        r=f.run();require(r.constantsFI.captured&&r.constantsFI.bytes.front()==0xA5&&r.constantsFI.bytes.back()==0xA5,
            "Pass-specific exact payload end incorrectly overlapped adjacent constant bytes");
    }
    ++cases;Fixture f;f.constants();f.request.layout=InputLayout::UnityPlayerBee7be52SharedFivePass;
    f.memory.pointer(Fixture::payload+0x20,f.request.moduleBase+sets[1][0]);
    require(f.run().status==Status::UnknownPassPropertySet,"Mixed pass property set was guessed");
}
} // namespace

int main() {
    try {
        normalAndNative(); graphFailureCases(); readerAndLimitCases(); nativeFailureCases(); constantsCases();sharedPassCases();
        std::cout << "{\"passed\":true,\"cases\":" << cases << ",\"checks\":" << checks
            << ",\"liveProcessRead\":false,\"pixelsRead\":false,\"gameFunctionsCalled\":false,"
               "\"nativeIdDomainVerified\":false,\"gameFrameInputsVerified\":false,\"xeFGIntegrated\":false}\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FSR metadata probe test failed: " << error.what() << '\n';
        return 1;
    }
}
