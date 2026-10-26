#include "fsr_pe_guard.hpp"
#include <algorithm>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {
namespace pe=xess_vk::fsr_pe_guard;
constexpr uint32_t ImageSize=34119680;
constexpr uintptr_t Base=0x180000000;
unsigned checks{}, oracleCases{}, exactNormalizationExceptions{};
void require(bool okay,const char* message) { ++checks; if(!okay) throw std::runtime_error(message); }
struct Memory {
    std::vector<uint8_t> data;
    uint64_t failOffset=UINT64_MAX;
    uint32_t error=ERROR_ACCESS_DENIED, partial{};
    bool shortSuccess{};
    unsigned calls{};
    static pe::ReadResult read(void* context,uint64_t offset,void* target,uint32_t size) noexcept {
        auto& m=*static_cast<Memory*>(context); ++m.calls;
        if(offset>m.data.size()||size>m.data.size()-offset) return {false,ERROR_PARTIAL_COPY,0,"fixture_bounds",false};
        if(offset==m.failOffset) {
            const uint32_t bytes=std::min(m.partial,size);
            std::memcpy(target,m.data.data()+offset,bytes);
            return {m.shortSuccess,m.shortSuccess?0:m.error,bytes,"fixture_read",true};
        }
        std::memcpy(target,m.data.data()+offset,size);
        return {true,0,size,"fixture_read",true};
    }
    pe::Reader reader() { return {this,read}; }
    template<class T> void put(size_t at,const T& value) { require(at<=data.size()&&sizeof(T)<=data.size()-at,"fixture put bounds"); std::memcpy(data.data()+at,&value,sizeof(T)); }
    template<class T,class F> void edit(size_t at,F&& operation) {
        T value{}; require(at<=data.size()&&sizeof(T)<=data.size()-at,"fixture edit bounds");
        std::memcpy(&value,data.data()+at,sizeof(T)); operation(value); put(at,value);
    }
};
Memory fixture(uint32_t ntOffset=128,uint16_t sections=2) {
    Memory m; m.data.resize(static_cast<size_t>(ntOffset)+8192);
    IMAGE_DOS_HEADER dos{}; dos.e_magic=IMAGE_DOS_SIGNATURE; dos.e_lfanew=static_cast<LONG>(ntOffset); m.put(0,dos);
    IMAGE_NT_HEADERS64 nt{}; nt.Signature=IMAGE_NT_SIGNATURE;
    nt.FileHeader.Machine=IMAGE_FILE_MACHINE_AMD64; nt.FileHeader.SizeOfOptionalHeader=sizeof(IMAGE_OPTIONAL_HEADER64); nt.FileHeader.NumberOfSections=sections;
    nt.OptionalHeader.Magic=IMAGE_NT_OPTIONAL_HDR64_MAGIC; nt.OptionalHeader.SizeOfImage=ImageSize;
    nt.OptionalHeader.SizeOfHeaders=ntOffset+8192; nt.OptionalHeader.ImageBase=Base;
    m.put(ntOffset,nt);
    for(uint32_t i=0;i<sections;++i) {
        IMAGE_SECTION_HEADER section{}; std::memcpy(section.Name,i?".rdata":".text",i?6:5);
        section.Misc.VirtualSize=4096; section.VirtualAddress=(i+1)*4096;
        section.SizeOfRawData=4096; section.PointerToRawData=(i+1)*4096;
        section.Characteristics=IMAGE_SCN_MEM_READ|(i?0:IMAGE_SCN_MEM_EXECUTE);
        m.put(ntOffset+sizeof(nt)+i*sizeof(section),section);
    }
    return m;
}

// Frozen acceptance predicate copied from the pre-diagnostic installer. Keep
// this independent of the new parser to catch accidental guard relaxation.
struct LegacyHeaders { IMAGE_NT_HEADERS64 nt{}; std::vector<IMAGE_SECTION_HEADER> sections; };
template<class Reader> bool legacyHeaders(Reader read, LegacyHeaders& result) {
    IMAGE_DOS_HEADER dos{};
    if (!read(0, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 64 || dos.e_lfanew > 1024 * 1024) return false;
    auto& nt = result.nt;
    const auto offset = static_cast<std::uint64_t>(dos.e_lfanew);
    if (!read(offset, &nt, sizeof(nt)) || nt.Signature != IMAGE_NT_SIGNATURE || nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt.FileHeader.SizeOfOptionalHeader != sizeof(IMAGE_OPTIONAL_HEADER64) || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt.OptionalHeader.SizeOfImage != ImageSize || nt.OptionalHeader.SizeOfHeaders > ImageSize ||
        !nt.FileHeader.NumberOfSections || nt.FileHeader.NumberOfSections > 96) return false;
    const std::uint64_t sectionOffset = offset + sizeof(nt);
    const std::uint64_t sectionBytes = nt.FileHeader.NumberOfSections * sizeof(IMAGE_SECTION_HEADER);
    if (sectionOffset > nt.OptionalHeader.SizeOfHeaders || sectionBytes > nt.OptionalHeader.SizeOfHeaders - sectionOffset) return false;
    result.sections.resize(nt.FileHeader.NumberOfSections);
    if (!read(sectionOffset, result.sections.data(), static_cast<DWORD>(sectionBytes))) return false;
    for (const auto& s : result.sections)
        if (s.VirtualAddress > ImageSize || s.Misc.VirtualSize > ImageSize - s.VirtualAddress) return false;
    return true;
}
bool legacy(Memory& disk,Memory& live,uintptr_t base) {
    LegacyHeaders d,l;
    const auto read=[](Memory& m,uint64_t offset,void* target,DWORD size) {
        const auto r=Memory::read(&m,offset,target,size); return r.success&&r.bytes==size;
    };
    return base<=std::numeric_limits<uintptr_t>::max()-ImageSize &&
        legacyHeaders([&](uint64_t o,void* t,DWORD n) { return read(disk,o,t,n); },d) &&
        legacyHeaders([&](uint64_t o,void* t,DWORD n) { return read(live,o,t,n); },l) &&
        !std::memcmp(&d.nt,&l.nt,sizeof(d.nt)) && d.sections.size()==l.sections.size() &&
        !std::memcmp(d.sections.data(),l.sections.data(),d.sections.size()*sizeof(IMAGE_SECTION_HEADER));
}
// Separate oracle: retain legacy validation, then normalize a private header
// copy only after proving its live ImageBase equals the caller's actual base.
// This is deliberately not implemented using the production difference count.
bool exactNormalizationException(Memory& disk,Memory& live,uintptr_t base) {
    if(!base||base>std::numeric_limits<uintptr_t>::max()-ImageSize) return false;
    LegacyHeaders d,l;
    const auto read=[](Memory& m,uint64_t offset,void* target,DWORD size) {
        const auto r=Memory::read(&m,offset,target,size); return r.success&&r.bytes==size;
    };
    if(!legacyHeaders([&](uint64_t o,void* t,DWORD n) { return read(disk,o,t,n); },d) ||
       !legacyHeaders([&](uint64_t o,void* t,DWORD n) { return read(live,o,t,n); },l)) return false;
    if(l.nt.OptionalHeader.ImageBase!=base||l.nt.OptionalHeader.ImageBase==d.nt.OptionalHeader.ImageBase) return false;
    l.nt.OptionalHeader.ImageBase=d.nt.OptionalHeader.ImageBase;
    return !std::memcmp(&d.nt,&l.nt,sizeof(d.nt)) && d.sections.size()==l.sections.size() &&
        !std::memcmp(d.sections.data(),l.sections.data(),d.sections.size()*sizeof(IMAGE_SECTION_HEADER));
}
struct Result { bool passed{}; pe::Diagnostic diagnostic; pe::Headers disk,live; };
Result run(Memory& disk,Memory& live,uintptr_t base=Base) {
    Result r; r.passed=pe::validate(disk.reader(),live.reader(),base,ImageSize,r.disk,r.live,r.diagnostic);
    const bool original=legacy(disk,live,base);
    const bool exception=exactNormalizationException(disk,live,base);
    require(r.passed==(original||exception),"guard changed beyond the exact actual-module-base exception"); ++oracleCases;
    require(r.diagnostic.imageBaseNormalizationApplied==exception,"normalization diagnostic disagrees with independent oracle");
    if(r.passed!=original) { require(exception,"unexpected legacy predicate exception"); ++exactNormalizationExceptions; }
    require(r.diagnostic.ntDifferencesReported<=16,"unbounded byte difference report");
    require(r.diagnostic.diskReads.calls<=3&&r.diagnostic.liveReads.calls<=3,"unexpected additional reads");
    require(r.diagnostic.diskReads.requested<=4168&&r.diagnostic.liveReads.requested<=4168,"read byte budget grew beyond original guard");
    return r;
}
std::string json(const Result& r) { std::ostringstream s; pe::writeJson(s,r.diagnostic,r.disk,r.live); return s.str(); }
void reason(const Result& r,const char* source,const char* stage,const char* name) {
    require(!r.passed,"malformed fixture accepted");
    require(std::string(r.diagnostic.source)==source&&std::string(r.diagnostic.stage)==stage&&std::string(r.diagnostic.reason)==name,"wrong refusal diagnostic");
    require(!r.diagnostic.comparisonPerformed,"invalid or unread headers were compared");
}
}

int main() {
    try {
        auto disk=fixture(),live=disk;
        const auto untouched=disk.data;
        auto r=run(disk,live); require(r.passed&&r.diagnostic.diskReads.requested==408&&r.diagnostic.liveReads.requested==408,"baseline headers did not pass");
        require(disk.data==untouched&&live.data==untouched,"guard changed input bytes");
        std::string errorSample,ntSample,sectionSample,normalizedSample;
        for(bool failLive : {false,true}) for(uint64_t offset : {uint64_t(0),uint64_t(128),uint64_t(392)}) for(bool shortRead : {false,true}) {
            disk=fixture(); live=disk;
            auto& failed=failLive?live:disk; failed.failOffset=offset; failed.shortSuccess=shortRead; failed.partial=shortRead?7:3;
            failed.error=ERROR_PARTIAL_COPY;
            r=run(disk,live);
            reason(r,failLive?"live":"disk",offset==0?"dos":offset==128?"nt":"sections",shortRead?"short_read":"read_failed");
            require(r.diagnostic.failedOffset==offset&&r.diagnostic.readFailure.bytes==failed.partial,"lost failing read offset or partial count");
            require(r.diagnostic.readFailure.error==(shortRead?0:ERROR_PARTIAL_COPY),"wrong captured Win32 error");
            require(!r.diagnostic.ntDifferencesReported&&!r.diagnostic.firstSectionDifferenceAvailable,"partial read leaked difference values");
            errorSample=json(r);
        }
        const auto mutateDos=[&](auto change,const char* expected) {
            disk=fixture(); live=disk; live.edit<IMAGE_DOS_HEADER>(0,change); r=run(disk,live); reason(r,"live","dos",expected);
        };
        mutateDos([](auto& h){h.e_magic=0;},"signature");
        mutateDos([](auto& h){h.e_lfanew=63;},"nt_offset_below_64");
        mutateDos([](auto& h){h.e_lfanew=1024*1024+1;},"nt_offset_above_1mib");
        const auto mutateNt=[&](auto change,const char* stage,const char* expected) {
            disk=fixture(); live=disk; live.edit<IMAGE_NT_HEADERS64>(128,change); r=run(disk,live); reason(r,"live",stage,expected);
        };
        mutateNt([](auto& h){h.Signature=0;},"nt","signature");
        mutateNt([](auto& h){h.FileHeader.Machine=0x14c;},"nt","machine");
        mutateNt([](auto& h){h.FileHeader.SizeOfOptionalHeader=0;},"nt","optional_header_size");
        mutateNt([](auto& h){h.OptionalHeader.Magic=0x10b;},"nt","optional_magic");
        mutateNt([](auto& h){h.OptionalHeader.SizeOfImage=ImageSize+1;},"nt","image_size");
        mutateNt([](auto& h){h.OptionalHeader.SizeOfHeaders=ImageSize+1;},"nt","headers_size_above_image");
        mutateNt([](auto& h){h.FileHeader.NumberOfSections=0;},"nt","section_count");
        mutateNt([](auto& h){h.FileHeader.NumberOfSections=97;},"nt","section_count");
        mutateNt([](auto& h){h.OptionalHeader.SizeOfHeaders=391;},"sections","table_offset_above_headers");
        mutateNt([](auto& h){h.OptionalHeader.SizeOfHeaders=392;},"sections","table_size_above_headers");
        for(bool virtualAddress : {false,true}) {
            disk=fixture(); live=disk;
            live.edit<IMAGE_SECTION_HEADER>(392,[&](auto& h){ if(virtualAddress) h.VirtualAddress=ImageSize+1; else h.Misc.VirtualSize=ImageSize; });
            r=run(disk,live); reason(r,"live","sections","virtual_range_above_image"); require(r.diagnostic.sectionIndex==0,"wrong invalid section index");
        }
        disk=fixture(); live=disk; r=run(disk,live,UINTPTR_MAX-ImageSize+1);
        reason(r,"address","base","address_overflow"); require(r.diagnostic.diskReads.calls==0&&r.diagnostic.liveReads.calls==0,"overflow address triggered a read");
        disk=fixture(1024*1024,96); live=disk; r=run(disk,live);
        require(r.passed&&r.diagnostic.diskReads.requested==4168,"original max NT offset / 96-section boundary was tightened");
        disk=fixture(); live=disk;
        live.edit<IMAGE_NT_HEADERS64>(128,[](auto& h){h.OptionalHeader.ImageBase+=0x10000;});
        r=run(disk,live); require(!r.passed&&std::string(r.diagnostic.state)=="refused_pe_nt_mismatch","NT mismatch not refused");
        require(std::string(pe::ntField(r.diagnostic.ntDifferences[0].offset).name)=="OptionalHeader.ImageBase","NT difference field attribution wrong"); ntSample=json(r);
        // A normal loaded image can retain the preferred ImageBase in its PE
        // header; identical headers must still pass even when mapped elsewhere.
        constexpr uintptr_t actualBase=140707188965376ull;
        disk=fixture(); live=disk;
        r=run(disk,live,actualBase);
        require(r.passed&&!r.diagnostic.imageBaseNormalizationApplied&&!r.diagnostic.ntDifferenceCount,"unchanged preferred base must retain strict success");
        require(r.diagnostic.actualModuleBase==actualBase&&!r.diagnostic.liveImageBaseMatchesActualModuleBase,"actual base diagnostics missing");
        // The single additional accepted case: the live header records the
        // actual loaded base. Retain every original differing byte in diagnostics.
        live.edit<IMAGE_NT_HEADERS64>(128,[](auto& h){h.OptionalHeader.ImageBase=actualBase;});
        const auto preferredBytes=disk.data,relocatedBytes=live.data;
        r=run(disk,live,actualBase);
        require(r.passed&&r.diagnostic.imageBaseNormalizationApplied&&r.diagnostic.imageBaseNormalizationEligible,"exact live module base was not accepted");
        require(r.diagnostic.ntDifferenceCount>0&&r.diagnostic.ntDifferencesOutsideImageBase==0&&r.diagnostic.ntDifferencesReported==r.diagnostic.ntDifferenceCount,"normalization hid original byte differences");
        require(disk.data==preferredBytes&&live.data==relocatedBytes,"normalization changed source bytes");
        normalizedSample=json(r);
        constexpr size_t imageBaseOffset=offsetof(IMAGE_NT_HEADERS64,OptionalHeader)+offsetof(IMAGE_OPTIONAL_HEADER64,ImageBase);
        for(size_t i=0;i<sizeof(IMAGE_NT_HEADERS64);++i) {
            if(i>=imageBaseOffset&&i<imageBaseOffset+sizeof(ULONGLONG)) continue;
            for(unsigned bit=0;bit<8;++bit) {
                live.data=relocatedBytes; live.data[128+i]^=static_cast<uint8_t>(1u<<bit);
                r=run(disk,live,actualBase);
                require(!r.passed&&!r.diagnostic.imageBaseNormalizationApplied,"a non-ImageBase NT bit difference was normalized");
            }
        }
        for(size_t i=0;i<sizeof(ULONGLONG);++i) for(unsigned bit=0;bit<8;++bit) {
            live.data=relocatedBytes; live.data[128+imageBaseOffset+i]^=static_cast<uint8_t>(1u<<bit);
            r=run(disk,live,actualBase);
            require(!r.passed&&!r.diagnostic.imageBaseNormalizationEligible,"an arbitrary third ImageBase was normalized");
        }
        for(size_t i=0;i<2*sizeof(IMAGE_SECTION_HEADER);++i) for(unsigned bit=0;bit<8;++bit) {
            live.data=relocatedBytes; live.data[392+i]^=static_cast<uint8_t>(1u<<bit);
            r=run(disk,live,actualBase);
            require(!r.passed&&!r.diagnostic.imageBaseNormalizationApplied,"a section bit difference was normalized");
        }
        for(uintptr_t invalidBase : {uintptr_t(0),UINTPTR_MAX-ImageSize+1,UINTPTR_MAX}) {
            live.data=relocatedBytes;
            live.edit<IMAGE_NT_HEADERS64>(128,[&](auto& h){h.OptionalHeader.ImageBase=invalidBase;});
            r=run(disk,live,invalidBase);
            require(!r.passed&&!r.diagnostic.imageBaseNormalizationApplied,"null or overflowing actual base was accepted for normalization");
            if(invalidBase) require(!r.diagnostic.diskReads.calls&&!r.diagnostic.liveReads.calls,"overflow read headers before rejecting");
        }
        // Preserve the original overflow boundary; it is a pure address-range
        // test, not proof that this synthetic address is mapped by Windows.
        live.data=relocatedBytes;
        constexpr uintptr_t boundaryBase=UINTPTR_MAX-ImageSize;
        live.edit<IMAGE_NT_HEADERS64>(128,[](auto& h){h.OptionalHeader.ImageBase=boundaryBase;});
        r=run(disk,live,boundaryBase);
        require(r.passed&&r.diagnostic.imageBaseNormalizationApplied,"non-overflow address boundary changed");
        disk=fixture(); live=disk;
        live.edit<IMAGE_SECTION_HEADER>(392,[](auto& h){h.Characteristics^=IMAGE_SCN_MEM_WRITE;});
        r=run(disk,live); require(!r.passed&&std::string(r.diagnostic.state)=="refused_pe_section_mismatch","section mismatch not refused");
        require(r.diagnostic.sectionDifferenceCount==1&&r.diagnostic.differentSections==1&&std::string(pe::sectionField(r.diagnostic.firstSectionDifference.offset))=="Characteristics","section difference metadata wrong"); sectionSample=json(r);
        disk=fixture(); live=disk;
        live.edit<IMAGE_NT_HEADERS64>(128,[](auto& h){for(auto& entry:h.OptionalHeader.DataDirectory) {entry.VirtualAddress=0xffffffff;entry.Size=0xffffffff;}});
        for(unsigned i=0;i<2;++i) live.edit<IMAGE_SECTION_HEADER>(392+i*40,[](auto& h){std::memset(h.Name,'X',8);});
        r=run(disk,live);
        require(r.diagnostic.ntDifferenceCount==128&&r.diagnostic.ntDifferencesReported==16,"NT truncation did not retain full count and fixed cap");
        require(r.diagnostic.sectionDifferenceCount==16&&r.diagnostic.differentSections==2,"section counts were not derived from already-read buffers");
        require(json(r).size()<8192,"bounded diagnostic unexpectedly became a dump");
        // Every one-bit mutation in the 1 KiB fixture header area, in live only
        // and in both copies, must preserve the old acceptance semantics. At
        // Base the live-only ImageBase mutations cannot equal the actual base.
        const auto original=fixture();
        for(unsigned offset=0;offset<1024;++offset) for(unsigned bit=0;bit<8;++bit) for(bool both : {false,true}) {
            disk=original; live=original;
            live.data[offset]^=static_cast<uint8_t>(1u<<bit);
            if(both) disk.data[offset]^=static_cast<uint8_t>(1u<<bit);
            r=run(disk,live);
        }
        std::cout<<"{\"passed\":true,\"checks\":"<<checks<<",\"originalPredicateCases\":"<<oracleCases
                 <<",\"exactActualModuleBaseExceptions\":"<<exactNormalizationExceptions
                 <<",\"maxReadBytesPerImage\":4168,\"maxNtDifferenceBytesReported\":16,\"processesOpened\":0,\"gameModuleLoaded\":false,\"hookAttempted\":false"
                 <<",\"partialReadDiagnostic\":"<<errorSample<<",\"ntMismatchDiagnostic\":"<<ntSample<<",\"sectionMismatchDiagnostic\":"<<sectionSample
                 <<",\"actualModuleBaseNormalizationDiagnostic\":"<<normalizedSample<<"}\n";
    } catch(const std::exception& error) { std::cerr<<"FSR_PE_GUARD_FAIL: "<<error.what()<<'\n'; return 1; }
}
