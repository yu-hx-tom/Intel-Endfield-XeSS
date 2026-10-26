#pragma once
// Pure, bounded PE-header validation and diagnostics. Readers are injected;
// this component cannot open processes, load modules, patch code or retry hooks.
#include <windows.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <ostream>
#include <vector>

namespace xess_vk::fsr_pe_guard {
inline constexpr size_t MaximumDifferences = 16;
struct ReadResult {
    bool success{};
    uint32_t error{};
    uint64_t bytes{};
    const char* operation = "reader"; // Only fixed internal labels are accepted.
    bool apiCalled{};
};
using ReadFunction = ReadResult (*)(void*, uint64_t, void*, uint32_t) noexcept;
struct Reader { void* context{}; ReadFunction read{}; };
struct Headers {
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    std::vector<IMAGE_SECTION_HEADER> sections;
    bool dosRead{}, ntRead{}, sectionsRead{};
};
struct ReadStats { uint32_t calls{}; uint64_t requested{}, actual{}; };
struct ByteDifference { uint32_t offset{}; uint8_t disk{}, live{}; };
struct Diagnostic {
    const char* state = "not_attempted";
    const char* source = "none";
    const char* stage = "none";
    const char* reason = "none";
    bool attempted{}, passed{}, failedRead{};
    uint64_t failedOffset{};
    uint32_t failedSize{};
    ReadResult readFailure{};
    uint64_t observed{}, expected{};
    bool scalarAvailable{};
    uint32_t sectionIndex = UINT32_MAX;
    ReadStats diskReads{}, liveReads{};
    uint32_t ntDifferenceCount{}, ntDifferencesReported{};
    uint64_t actualModuleBase{};
    uint32_t ntDifferencesOutsideImageBase{};
    bool liveImageBaseMatchesActualModuleBase{};
    bool imageBaseNormalizationEligible{}, imageBaseNormalizationApplied{};
    std::array<ByteDifference, MaximumDifferences> ntDifferences{};
    bool comparisonPerformed{}, sectionCountsMatch{};
    uint32_t sectionDifferenceCount{}, differentSections{};
    bool firstSectionDifferenceAvailable{};
    uint32_t firstSectionIndex{};
    ByteDifference firstSectionDifference{};
};
inline bool reject(Diagnostic& d, const char* stage, const char* reason) noexcept {
    d.stage = stage; d.reason = reason; return false;
}
inline bool rejectScalar(Diagnostic& d, const char* stage, const char* reason, uint64_t observed, uint64_t expected) noexcept {
    d.scalarAvailable = true; d.observed = observed; d.expected = expected;
    return reject(d, stage, reason);
}
inline bool readExactly(Reader reader, uint64_t offset, void* output, uint32_t size,
                        const char* stage, Diagnostic& d, ReadStats& stats) noexcept {
    ++stats.calls; stats.requested += size;
    const auto result = reader.read ? reader.read(reader.context, offset, output, size)
                                   : ReadResult{false, ERROR_INVALID_PARAMETER, 0, "missing_reader", false};
    stats.actual += result.bytes;
    if (result.success && result.bytes == size) return true;
    d.failedRead = true; d.failedOffset = offset; d.failedSize = size; d.readFailure = result;
    return reject(d, stage, result.success ? "short_read" : "read_failed");
}
inline bool parse(Reader reader, uint32_t imageSize, Headers& h, Diagnostic& d, ReadStats& stats) {
    if (!readExactly(reader, 0, &h.dos, sizeof(h.dos), "dos", d, stats)) return false;
    h.dosRead = true;
    if (h.dos.e_magic != IMAGE_DOS_SIGNATURE) return rejectScalar(d,"dos","signature",h.dos.e_magic,IMAGE_DOS_SIGNATURE);
    if (h.dos.e_lfanew < 64) return reject(d,"dos","nt_offset_below_64");
    if (h.dos.e_lfanew > 1024 * 1024) return reject(d,"dos","nt_offset_above_1mib");
    auto& nt = h.nt;
    const auto offset = static_cast<uint64_t>(h.dos.e_lfanew);
    if (!readExactly(reader,offset,&nt,sizeof(nt),"nt",d,stats)) return false;
    h.ntRead = true;
    if (nt.Signature != IMAGE_NT_SIGNATURE) return rejectScalar(d,"nt","signature",nt.Signature,IMAGE_NT_SIGNATURE);
    if (nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) return rejectScalar(d,"nt","machine",nt.FileHeader.Machine,IMAGE_FILE_MACHINE_AMD64);
    if (nt.FileHeader.SizeOfOptionalHeader != sizeof(IMAGE_OPTIONAL_HEADER64)) return rejectScalar(d,"nt","optional_header_size",nt.FileHeader.SizeOfOptionalHeader,sizeof(IMAGE_OPTIONAL_HEADER64));
    if (nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return rejectScalar(d,"nt","optional_magic",nt.OptionalHeader.Magic,IMAGE_NT_OPTIONAL_HDR64_MAGIC);
    if (nt.OptionalHeader.SizeOfImage != imageSize) return rejectScalar(d,"nt","image_size",nt.OptionalHeader.SizeOfImage,imageSize);
    if (nt.OptionalHeader.SizeOfHeaders > imageSize) return rejectScalar(d,"nt","headers_size_above_image",nt.OptionalHeader.SizeOfHeaders,imageSize);
    if (!nt.FileHeader.NumberOfSections || nt.FileHeader.NumberOfSections > 96) return rejectScalar(d,"nt","section_count",nt.FileHeader.NumberOfSections,96);
    const uint64_t sectionOffset = offset + sizeof(nt);
    const uint64_t sectionBytes = nt.FileHeader.NumberOfSections * sizeof(IMAGE_SECTION_HEADER);
    if (sectionOffset > nt.OptionalHeader.SizeOfHeaders) return reject(d,"sections","table_offset_above_headers");
    if (sectionBytes > nt.OptionalHeader.SizeOfHeaders - sectionOffset) return reject(d,"sections","table_size_above_headers");
    h.sections.resize(nt.FileHeader.NumberOfSections);
    if (!readExactly(reader,sectionOffset,h.sections.data(),static_cast<uint32_t>(sectionBytes),"sections",d,stats)) return false;
    h.sectionsRead = true;
    for (uint32_t i=0; i<h.sections.size(); ++i) {
        const auto& s = h.sections[i];
        if (s.VirtualAddress > imageSize || s.Misc.VirtualSize > imageSize - s.VirtualAddress) {
            d.sectionIndex = i;
            return reject(d,"sections","virtual_range_above_image");
        }
    }
    return true;
}
// The original acceptance predicate remains in force, with one exact exception:
// only OptionalHeader.ImageBase may differ if its live value equals the actual,
// nonzero loaded module base supplied by the caller. Every other NT byte and the
// entire section table must still match. The original buffers are never changed.
// Diagnostics compare only successfully read buffers; they perform no extra reads.
inline bool validate(Reader diskReader, Reader liveReader, uintptr_t liveBase, uint32_t imageSize,
                     Headers& disk, Headers& live, Diagnostic& d) {
    disk = {}; live = {}; d = {}; d.attempted = true; d.actualModuleBase = liveBase;
    if (liveBase > std::numeric_limits<uintptr_t>::max() - imageSize) {
        d.state = "refused_pe_base_range"; d.source = "address";
        return reject(d,"base","address_overflow");
    }
    d.source = "disk"; d.state = "refused_pe_disk_headers";
    if (!parse(diskReader,imageSize,disk,d,d.diskReads)) return false;
    d.source = "live"; d.state = "refused_pe_live_headers";
    if (!parse(liveReader,imageSize,live,d,d.liveReads)) return false;
    d.source = "comparison"; d.stage = "comparison"; d.comparisonPerformed = true;
    const auto* a = reinterpret_cast<const uint8_t*>(&disk.nt);
    const auto* b = reinterpret_cast<const uint8_t*>(&live.nt);
    constexpr size_t imageBaseOffset = offsetof(IMAGE_NT_HEADERS64,OptionalHeader) + offsetof(IMAGE_OPTIONAL_HEADER64,ImageBase);
    constexpr size_t imageBaseEnd = imageBaseOffset + sizeof(live.nt.OptionalHeader.ImageBase);
    for (uint32_t i=0; i<sizeof(disk.nt); ++i) if (a[i]!=b[i]) {
        ++d.ntDifferenceCount;
        if (i<imageBaseOffset || i>=imageBaseEnd) ++d.ntDifferencesOutsideImageBase;
        if (d.ntDifferencesReported<MaximumDifferences) d.ntDifferences[d.ntDifferencesReported++] = {i,a[i],b[i]};
    }
    d.liveImageBaseMatchesActualModuleBase = liveBase != 0 && live.nt.OptionalHeader.ImageBase == liveBase;
    d.imageBaseNormalizationEligible = d.ntDifferenceCount != 0 &&
        d.ntDifferencesOutsideImageBase == 0 && d.liveImageBaseMatchesActualModuleBase;
    d.sectionCountsMatch = disk.sections.size()==live.sections.size();
    if (d.sectionCountsMatch) for (uint32_t s=0; s<disk.sections.size(); ++s) {
        a=reinterpret_cast<const uint8_t*>(&disk.sections[s]); b=reinterpret_cast<const uint8_t*>(&live.sections[s]);
        bool different=false;
        for (uint32_t i=0; i<sizeof(IMAGE_SECTION_HEADER); ++i) if (a[i]!=b[i]) {
            ++d.sectionDifferenceCount; different=true;
            if (!d.firstSectionDifferenceAvailable) {
                d.firstSectionDifferenceAvailable=true; d.firstSectionIndex=s; d.firstSectionDifference={i,a[i],b[i]};
            }
        }
        if (different) ++d.differentSections;
    }
    if (d.ntDifferenceCount && !d.imageBaseNormalizationEligible) { d.state="refused_pe_nt_mismatch"; return reject(d,"comparison","nt_bytes_differ"); }
    if (!d.sectionCountsMatch) { d.state="refused_pe_section_count_mismatch"; return reject(d,"comparison","section_counts_differ"); }
    if (d.sectionDifferenceCount) { d.state="refused_pe_section_mismatch"; return reject(d,"comparison","section_bytes_differ"); }
    d.imageBaseNormalizationApplied=d.imageBaseNormalizationEligible;
    d.state="passed"; d.reason=d.imageBaseNormalizationApplied?"validated_headers_with_actual_module_image_base":"identical_validated_headers"; d.passed=true;
    return true;
}
struct Field { const char* name; int index=-1; };
inline Field ntField(uint32_t offset) noexcept {
#define FIELD(type,member,prefix) if(offset>=offsetof(type,member)&&offset<offsetof(type,member)+sizeof(((type*)nullptr)->member)) return {prefix #member}
    FIELD(IMAGE_NT_HEADERS64,Signature,"");
    if (offset>=offsetof(IMAGE_NT_HEADERS64,FileHeader) && offset<offsetof(IMAGE_NT_HEADERS64,OptionalHeader)) {
        offset-=offsetof(IMAGE_NT_HEADERS64,FileHeader);
        FIELD(IMAGE_FILE_HEADER,Machine,"FileHeader."); FIELD(IMAGE_FILE_HEADER,NumberOfSections,"FileHeader.");
        FIELD(IMAGE_FILE_HEADER,TimeDateStamp,"FileHeader."); FIELD(IMAGE_FILE_HEADER,PointerToSymbolTable,"FileHeader.");
        FIELD(IMAGE_FILE_HEADER,NumberOfSymbols,"FileHeader."); FIELD(IMAGE_FILE_HEADER,SizeOfOptionalHeader,"FileHeader.");
        FIELD(IMAGE_FILE_HEADER,Characteristics,"FileHeader.");
    } else if (offset>=offsetof(IMAGE_NT_HEADERS64,OptionalHeader)) {
        offset-=offsetof(IMAGE_NT_HEADERS64,OptionalHeader);
        FIELD(IMAGE_OPTIONAL_HEADER64,Magic,"OptionalHeader.");
        FIELD(IMAGE_OPTIONAL_HEADER64,MajorLinkerVersion,"OptionalHeader."); FIELD(IMAGE_OPTIONAL_HEADER64,MinorLinkerVersion,"OptionalHeader.");
        FIELD(IMAGE_OPTIONAL_HEADER64,SizeOfCode,"OptionalHeader."); FIELD(IMAGE_OPTIONAL_HEADER64,SizeOfInitializedData,"OptionalHeader.");
        FIELD(IMAGE_OPTIONAL_HEADER64,SizeOfUninitializedData,"OptionalHeader."); FIELD(IMAGE_OPTIONAL_HEADER64,AddressOfEntryPoint,"OptionalHeader.");
        FIELD(IMAGE_OPTIONAL_HEADER64,BaseOfCode,"OptionalHeader."); FIELD(IMAGE_OPTIONAL_HEADER64,ImageBase,"OptionalHeader.");
        FIELD(IMAGE_OPTIONAL_HEADER64,SectionAlignment,"OptionalHeader."); FIELD(IMAGE_OPTIONAL_HEADER64,FileAlignment,"OptionalHeader.");
        FIELD(IMAGE_OPTIONAL_HEADER64,MajorOperatingSystemVersion,"OptionalHeader."); FIELD(IMAGE_OPTIONAL_HEADER64,MinorOperatingSystemVersion,"OptionalHeader.");
        FIELD(IMAGE_OPTIONAL_HEADER64,MajorImageVersion,"OptionalHeader."); FIELD(IMAGE_OPTIONAL_HEADER64,MinorImageVersion,"OptionalHeader.");
        FIELD(IMAGE_OPTIONAL_HEADER64,MajorSubsystemVersion,"OptionalHeader."); FIELD(IMAGE_OPTIONAL_HEADER64,MinorSubsystemVersion,"OptionalHeader.");
        FIELD(IMAGE_OPTIONAL_HEADER64,Win32VersionValue,"OptionalHeader."); FIELD(IMAGE_OPTIONAL_HEADER64,SizeOfImage,"OptionalHeader.");
        FIELD(IMAGE_OPTIONAL_HEADER64,SizeOfHeaders,"OptionalHeader."); FIELD(IMAGE_OPTIONAL_HEADER64,CheckSum,"OptionalHeader.");
        FIELD(IMAGE_OPTIONAL_HEADER64,Subsystem,"OptionalHeader."); FIELD(IMAGE_OPTIONAL_HEADER64,DllCharacteristics,"OptionalHeader.");
        FIELD(IMAGE_OPTIONAL_HEADER64,SizeOfStackReserve,"OptionalHeader."); FIELD(IMAGE_OPTIONAL_HEADER64,SizeOfStackCommit,"OptionalHeader.");
        FIELD(IMAGE_OPTIONAL_HEADER64,SizeOfHeapReserve,"OptionalHeader."); FIELD(IMAGE_OPTIONAL_HEADER64,SizeOfHeapCommit,"OptionalHeader.");
        FIELD(IMAGE_OPTIONAL_HEADER64,LoaderFlags,"OptionalHeader."); FIELD(IMAGE_OPTIONAL_HEADER64,NumberOfRvaAndSizes,"OptionalHeader.");
        if (offset>=offsetof(IMAGE_OPTIONAL_HEADER64,DataDirectory)) {
            const auto within=offset-static_cast<uint32_t>(offsetof(IMAGE_OPTIONAL_HEADER64,DataDirectory));
            return {within%8<4?"OptionalHeader.DataDirectory.VirtualAddress":"OptionalHeader.DataDirectory.Size",static_cast<int>(within/8)};
        }
    }
    return {"unknown_field"};
#undef FIELD
}
inline const char* sectionField(uint32_t offset) noexcept {
    if(offset<8) return "Name";
    if(offset<12) return "VirtualSize";
    if(offset<16) return "VirtualAddress";
    if(offset<20) return "SizeOfRawData";
    if(offset<24) return "PointerToRawData";
    if(offset<28) return "PointerToRelocations";
    if(offset<32) return "PointerToLinenumbers";
    if(offset<34) return "NumberOfRelocations";
    if(offset<36) return "NumberOfLinenumbers";
    return "Characteristics";
}
inline void writeHeaderSnapshot(std::ostream& out,const Headers& h) {
    out<<"{\"dos_read\":"<<(h.dosRead?"true":"false")<<",\"nt_read\":"<<(h.ntRead?"true":"false")<<",\"sections_read\":"<<(h.sectionsRead?"true":"false");
    if(h.dosRead) out<<",\"dos_magic\":"<<h.dos.e_magic<<",\"nt_offset\":"<<h.dos.e_lfanew;
    if(h.ntRead) out<<",\"nt_signature\":"<<h.nt.Signature<<",\"machine\":"<<h.nt.FileHeader.Machine<<",\"section_count\":"<<h.nt.FileHeader.NumberOfSections
        <<",\"optional_header_size\":"<<h.nt.FileHeader.SizeOfOptionalHeader<<",\"optional_magic\":"<<h.nt.OptionalHeader.Magic
        <<",\"image_size\":"<<h.nt.OptionalHeader.SizeOfImage<<",\"headers_size\":"<<h.nt.OptionalHeader.SizeOfHeaders
        <<",\"image_base\":"<<h.nt.OptionalHeader.ImageBase;
    out<<'}';
}
// At most 16 NT bytes, one section byte, scalar snapshots and counts. No dump.
inline void writeJson(std::ostream& out,const Diagnostic& d,const Headers& disk,const Headers& live) {
    out<<"{\"attempted\":"<<(d.attempted?"true":"false")<<",\"passed\":"<<(d.passed?"true":"false")
       <<",\"state\":\""<<d.state<<"\",\"source\":\""<<d.source<<"\",\"stage\":\""<<d.stage<<"\",\"reason\":\""<<d.reason<<'"';
    out<<",\"actual_module_base\":"<<d.actualModuleBase
       <<",\"live_image_base_matches_actual_module_base\":"<<(d.liveImageBaseMatchesActualModuleBase?"true":"false")
       <<",\"nt_differences_outside_image_base\":"<<d.ntDifferencesOutsideImageBase
       <<",\"image_base_normalization_eligible\":"<<(d.imageBaseNormalizationEligible?"true":"false")
       <<",\"image_base_normalization_applied\":"<<(d.imageBaseNormalizationApplied?"true":"false");
    if(d.scalarAvailable) out<<",\"observed\":"<<d.observed<<",\"expected_or_limit\":"<<d.expected;
    if(d.sectionIndex!=UINT32_MAX) out<<",\"invalid_section_index\":"<<d.sectionIndex;
    out<<",\"failed_read\":";
    if(!d.failedRead) out<<"null";
    else out<<"{\"operation\":\""<<d.readFailure.operation<<"\",\"api_called\":"<<(d.readFailure.apiCalled?"true":"false")
        <<",\"api_succeeded\":"<<(d.readFailure.success?"true":"false")<<",\"win32_error\":"<<d.readFailure.error
        <<",\"offset\":"<<d.failedOffset<<",\"requested_bytes\":"<<d.failedSize<<",\"actual_bytes\":"<<d.readFailure.bytes<<'}';
    const auto reads=[&](const char* name,const ReadStats& s) { out<<",\""<<name<<"\":{\"calls\":"<<s.calls<<",\"requested_bytes\":"<<s.requested<<",\"actual_bytes\":"<<s.actual<<'}'; };
    reads("disk_reads",d.diskReads); reads("live_reads",d.liveReads);
    out<<",\"disk\":"; writeHeaderSnapshot(out,disk); out<<",\"live\":"; writeHeaderSnapshot(out,live);
    out<<",\"comparison_performed\":"<<(d.comparisonPerformed?"true":"false")<<",\"nt_different_bytes\":"<<d.ntDifferenceCount<<",\"nt_first_differences\":[";
    for(uint32_t i=0;i<d.ntDifferencesReported;++i) {
        const auto& b=d.ntDifferences[i]; const auto f=ntField(b.offset);
        if(i) out<<',';
        out<<"{\"nt_offset\":"<<b.offset<<",\"field\":\""<<f.name<<'"';
        if(f.index>=0) out<<",\"directory_index\":"<<f.index;
        out<<",\"disk_byte\":"<<static_cast<unsigned>(b.disk)<<",\"live_byte\":"<<static_cast<unsigned>(b.live)<<'}';
    }
    out<<"],\"section_counts_match\":"<<(d.sectionCountsMatch?"true":"false")<<",\"section_different_bytes\":"<<d.sectionDifferenceCount
       <<",\"different_sections\":"<<d.differentSections<<",\"first_section_difference\":";
    if(!d.firstSectionDifferenceAvailable) out<<"null";
    else {
        const auto& b=d.firstSectionDifference;
        out<<"{\"index\":"<<d.firstSectionIndex<<",\"section_offset\":"<<b.offset<<",\"field\":\""<<sectionField(b.offset)
           <<"\",\"disk_byte\":"<<static_cast<unsigned>(b.disk)<<",\"live_byte\":"<<static_cast<unsigned>(b.live)<<'}';
    }
    out<<",\"nt_difference_limit\":16,\"additional_memory_reads_for_diff\":false}";
}
} // namespace xess_vk::fsr_pe_guard
