#include "spirv_metadata.hpp"

#include <algorithm>
#include <array>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace {
using namespace xess_vk::spirv;
using Words = std::vector<std::uint32_t>;
unsigned checks = 0, cases = 0;
void require(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
Words header(std::uint32_t bound = 64) { return {0x07230203, 0x00010000, 0, bound, 0}; }
void instruction(Words& words, std::uint16_t opcode, std::initializer_list<std::uint32_t> operands) {
    words.push_back((static_cast<std::uint32_t>(operands.size() + 1) << 16) | opcode);
    words.insert(words.end(), operands);
}
void stringInstruction(Words& words, std::uint16_t opcode,
                       std::initializer_list<std::uint32_t> prefix, std::string_view text,
                       std::initializer_list<std::uint32_t> suffix = {}) {
    const auto start = words.size();
    words.push_back(opcode);
    words.insert(words.end(), prefix);
    const auto stringStart = words.size();
    words.resize(words.size() + (text.size() + 4) / 4, 0);
    for (std::size_t i = 0; i < text.size(); ++i)
        words[stringStart + i / 4] |= std::uint32_t(static_cast<unsigned char>(text[i])) << ((i % 4) * 8);
    words.insert(words.end(), suffix);
    const auto count = words.size() - start;
    if (count > 0xffff) throw std::runtime_error("Test instruction too large");
    words[start] |= static_cast<std::uint32_t>(count) << 16;
}
const Binding* findBinding(const Metadata& result, std::uint32_t id) {
    const auto found = std::find_if(result.bindings.begin(), result.bindings.end(),
        [&](const Binding& binding) { return binding.id == id; });
    return found == result.bindings.end() ? nullptr : &*found;
}
void imageAccess(){
    auto words=header();
    stringInstruction(words,15,{5,3},"main");
    instruction(words,71,{10,34,0});instruction(words,71,{10,33,1});
    instruction(words,71,{10,25});instruction(words,59,{7,10,0});
    instruction(words,71,{11,34,2});instruction(words,71,{11,33,1});
    instruction(words,59,{7,11,0});
    const auto parsed=parse(words);const auto reads=imageReads(parsed);
    require(reads.known&&!reads.mayRead(0,1)&&reads.mayRead(2,1)&&!reads.mayRead(0,2),
        "NonReadable and absent shader bindings are not consumers; same binding in another set remains readable");
    auto incomplete=parsed;incomplete.bindings[0].uniformConstantVariable=false;
    require(imageReads(incomplete).mayRead(0,1),"non-variable qualifiers cannot prove an image write-only");
    incomplete=parsed;incomplete.bindingsTruncated=incomplete.truncated=true;
    require(imageReads(incomplete).mayRead(0,1),"truncated metadata keeps consumer checks");
    incomplete=parsed;incomplete.hasUnresolvedDecorationGroups=true;
    require(imageReads(incomplete).mayRead(0,1),"unresolved decorations keep consumer checks");
    instruction(words,332,{10,25,12});
    require(!imageReads(parse(words)).known,"Id-based decorations never acquire direct-access authority");
    words.pop_back();words.pop_back();words.pop_back();words.pop_back();
    instruction(words,71,{10,25,1});
    require(!parse(words).valid,"malformed NonReadable decoration is refused");
}
void directImageWrites(){
    for(unsigned variant=0;variant<8;++variant){
        auto words=header();stringInstruction(words,15,{5,3},"main");
        instruction(words,71,{10,34,0});instruction(words,71,{10,33,0});instruction(words,59,{7,10,0});
        instruction(words,54,{1,3,0,2});instruction(words,248,{4});instruction(words,61,{8,20,10});
        if(variant!=1)instruction(words,99,{20,30,31});
        if(variant==2)instruction(words,98,{9,21,20,30}); // image read
        if(variant==3)instruction(words,83,{8,21,20}); // copied image handle
        if(variant==4)instruction(words,57,{1,22,5,10}); // pointer passed to a function
        if(variant==5)instruction(words,57,{1,22,5,20}); // loaded image passed to a function
        if(variant==6)instruction(words,60,{7,21,20,30,31}); // texel pointer / atomic alias
        instruction(words,253,{});instruction(words,56,{});
        if(variant==7)instruction(words,73,{40});
        const auto metadata=parse(words);const auto reads=imageReads(metadata);
        require(metadata.valid,"direct image access fixture parses");
        require(reads.mayRead(0,0)==(variant!=0),"only direct write-only uses bypass old-content reads; aliases, calls and reads stay checked");
    }
}
void expectFailure(const Words& words, const char* status, Limits limits = {}) {
    ++cases;
    const auto result = parse(words, limits);
    require(!result.valid, "Malformed metadata stream was accepted");
    require(std::string_view(result.parseStatus) == status, result.parseStatus);
    require(result.names.empty() && result.bindings.empty() && result.computeEntryPoints.empty(),
        "Failed parse exposed partial metadata");
    require(result.wordsRead <= words.size(), "Parser read beyond the input span");
}

void minimalComputeAndUnalignedBytes() {
    ++cases;
    // A minimal compute module; the parser does not execute/compile it.
    auto words = header();
    instruction(words, 17, {1}); // OpCapability Shader
    instruction(words, 14, {0, 1}); // OpMemoryModel Logical GLSL450
    stringInstruction(words, 15, {5, 1}, "CS");
    instruction(words, 16, {1, 17, 8, 8, 1}); // LocalSize
    instruction(words, 19, {2}); // void
    instruction(words, 33, {3, 2}); // function type
    instruction(words, 54, {2, 1, 0, 3});
    instruction(words, 248, {4});
    instruction(words, 253, {});
    instruction(words, 56, {});
    const auto result = parse(words);
    require(result.valid && !result.truncated && std::string_view(result.parseStatus) == "ok", "Minimal module did not parse");
    require(result.computeEntryPoints == std::vector<std::string>{"CS"}, "Generic compute entry point was lost");
    require(result.names.empty() && result.bindings.empty(), "Parser invented resource metadata");
    require(result.wordsRead == words.size() && result.wordCount == words.size(), "Instruction traversal was incomplete");
    std::vector<std::byte> bytes(1 + words.size() * 4);
    for (std::size_t i = 0; i < words.size(); ++i) for (unsigned b = 0; b < 4; ++b)
        bytes[1 + i * 4 + b] = static_cast<std::byte>(words[i] >> (b * 8));
    const auto unaligned = parse(std::span<const std::byte>(bytes).subspan(1));
    require(unaligned.valid && unaligned.computeEntryPoints == result.computeEntryPoints,
        "Unaligned little-endian byte parsing differs from word parsing");
}

void namesDecorationsAndOwnership() {
    ++cases;
    // A metadata fixture, not a complete shader: deliberately does not declare
    // descriptor variable types, which are outside this reader's validation scope.
    auto words = header();
    instruction(words, 71, {8, 33, 4});
    stringInstruction(words, 5, {8}, "r_input_motion_vectors");
    instruction(words, 71, {8, 34, 2});
    instruction(words, 71, {8, 33, 4}); // identical duplicate is unambiguous
    stringInstruction(words, 5, {9}, "r_reconstructed_previous_depth");
    instruction(words, 71, {9, 33, 0});
    stringInstruction(words, 6, {12, 3}, "motionVectorScale");
    instruction(words, 71, {12, 34, 0});
    instruction(words, 71, {12, 33, 1});
    stringInstruction(words, 7, {13}, "ffx_fsr3_frameinterpolation");
    stringInstruction(words, 7, {14}, "C:/shaders/fsr3/private_source.hlsl");
    stringInstruction(words, 5, {10}, "temporary_10");
    stringInstruction(words, 15, {5, 1}, "CS", {8, 9});
    const auto result = parse(words);
    require(result.valid && result.names.size() == 4, "Meaningful name filtering or OpString/member parsing failed");
    require(result.bindings.size() == 3, "Decoration target IDs were not coalesced");
    const auto* motion = findBinding(result, 8);
    require(motion && motion->set == 2 && motion->binding == 4 && motion->name == "r_input_motion_vectors",
        "Binding/name association depended on instruction order");
    const auto* depth = findBinding(result, 9);
    require(depth && !depth->set && depth->binding == 0, "Missing descriptor set was fabricated as set zero");
    const auto* member = findBinding(result, 12);
    require(member && member->name.empty(), "A structure member name was used as a resource name");
    require(result.names[2].kind == NameKind::Member && result.names[2].memberIndex == 3
        && result.names[3].kind == NameKind::String, "Name provenance was lost");
    std::fill(words.begin(), words.end(), 0);
    require(motion->name == "r_input_motion_vectors" && result.computeEntryPoints[0] == "CS",
        "Returned metadata aliases the input");
}

void orderAndStage() {
    ++cases;
    auto first = header();
    stringInstruction(first, 5, {8}, "R_Input_Depth");
    instruction(first, 71, {8, 34, 3}); instruction(first, 71, {8, 33, 9});
    auto second = header();
    instruction(second, 71, {8, 33, 9}); instruction(second, 71, {8, 34, 3});
    stringInstruction(second, 5, {8}, "R_Input_Depth");
    stringInstruction(second, 15, {4, 2}, "fragmentDepth");
    stringInstruction(second, 15, {5, 1}, "CS");
    const auto a = parse(first), b = parse(second);
    require(a.valid && b.valid && a.bindings[0].name == b.bindings[0].name
        && a.bindings[0].set == b.bindings[0].set && a.bindings[0].binding == b.bindings[0].binding,
        "Binding-first and name-first results differ");
    require(b.computeEntryPoints == std::vector<std::string>{"CS"}, "Non-compute entry point was retained as compute");
    stringInstruction(second, 5, {8}, "r_input_motion_vectors");
    const auto ambiguous = parse(second);
    require(ambiguous.valid && ambiguous.bindings[0].name.empty(), "Ambiguous retained object names were silently resolved");
}

void headerAndIdFailures() {
    auto words = header();
    words[0] = 0; expectFailure(words, "bad_magic");
    words[0] = 0x03022307; expectFailure(words, "byte_swapped_not_supported");
    words = header(); words.resize(4); expectFailure(words, "header_too_short");
    for (const auto version : {0x00010700u, 0x00010001u, 0x00020000u, 0x01010000u}) {
        words = header(); words[1] = version; expectFailure(words, "unsupported_version");
    }
    words = header(0); expectFailure(words, "invalid_id_bound");
    words = header(); words[4] = 1; expectFailure(words, "nonzero_instruction_schema");
    for (const auto id : {0u, 64u, 0xffffffffu}) {
        words = header(); stringInstruction(words, 5, {id}, "depth"); expectFailure(words, "id_out_of_bound");
        words = header(); instruction(words, 71, {id, 33, 1}); expectFailure(words, "id_out_of_bound");
    }
    words = header(); stringInstruction(words, 15, {5, 0}, "CS"); expectFailure(words, "id_out_of_bound");
    words = header(); stringInstruction(words, 15, {5, 1}, "CS", {64}); expectFailure(words, "id_out_of_bound");
    ++cases;
    // Large sparse ID bounds must not allocate a bound-sized array. The universal
    // limit is not a universal maximum. Metadata operands still obey id < bound.
    words = header(0xffffffffu);
    stringInstruction(words, 5, {0xfffffffeu}, "depth");
    instruction(words, 71, {0xfffffffeu, 33, 0xffffffffu});
    const auto large = parse(words);
    require(large.valid && large.names.size() == 1 && large.bindings.size() == 1,
        "Sparse extreme ID bound caused a false rejection or invalid allocation");
}

void malformedInstructionAndStrings() {
    auto words = header(); words.push_back(0); expectFailure(words, "zero_instruction_word_count");
    words = header(); words.push_back((0xffffu << 16) | 5); expectFailure(words, "instruction_past_end");
    words = header(); instruction(words, 5, {1}); expectFailure(words, "metadata_operand_count");
    words = header(); instruction(words, 6, {1, 0}); expectFailure(words, "metadata_operand_count");
    words = header(); instruction(words, 7, {1, 0x41414141}); expectFailure(words, "missing_string_nul");
    words = header(); instruction(words, 5, {1, 0x41414141}); expectFailure(words, "missing_string_nul");
    words = header(); instruction(words, 5, {1, 0x41007864}); expectFailure(words, "nonzero_string_padding");
    words = header(); instruction(words, 5, {1, 0, 0}); expectFailure(words, "metadata_operand_count");
    words = header(); instruction(words, 15, {5, 1}); expectFailure(words, "entry_point_operand_count");
    words = header(); instruction(words, 71, {1}); expectFailure(words, "decoration_operand_count");
    words = header(); instruction(words, 71, {1, 33}); expectFailure(words, "decoration_operand_count");
    words = header(); instruction(words, 71, {1, 34, 2, 3}); expectFailure(words, "decoration_operand_count");
    words = header(); instruction(words, 71, {1, 33, 2}); instruction(words, 71, {1, 33, 3});
    expectFailure(words, "conflicting_decoration");
    ++cases;
    words = header(); instruction(words, 65535, {0xffffffff, 0, 42}); stringInstruction(words, 5, {1}, "depth");
    const auto unknown = parse(words);
    require(unknown.valid && unknown.names.size() == 1,
        "Unknown instruction could not be skipped within its checked word count");
}

void utf8Validation() {
    ++cases;
    auto words = header(); stringInstruction(words, 5, {1}, "motion_\xe4\xb8\xad");
    auto result = parse(words);
    require(result.valid && result.names.size() == 1 && result.names[0].name == "motion_\xe4\xb8\xad",
        "Valid UTF-8 metadata was changed or rejected");
    for (const std::string_view bad : {"\xc0\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xe2\x41", "\xe2\x82", "\x80"}) {
        words = header(); stringInstruction(words, 5, {1}, std::string("depth_") + std::string(bad));
        expectFailure(words, "invalid_utf8");
    }
}

void boundedOutputStillValidates() {
    ++cases;
    auto words = header();
    stringInstruction(words, 15, {5, 1}, "CS"); stringInstruction(words, 15, {5, 2}, "CS2");
    stringInstruction(words, 5, {8}, "depthA"); stringInstruction(words, 5, {9}, "motionB");
    instruction(words, 71, {8, 33, 0}); instruction(words, 71, {9, 33, 1});
    Limits limits; limits.maxNames = limits.maxBindings = limits.maxComputeEntryPoints = 1;
    const auto result = parse(words, limits);
    require(result.valid && result.truncated && result.namesTruncated && result.bindingsTruncated
        && result.entryPointsTruncated && !result.readLimitExceeded, "Output truncation flags do not describe exceeded budgets");
    require(result.names.size() == 1 && result.bindings.size() == 1 && result.computeEntryPoints.size() == 1,
        "Output exceeded configured limits");
    require(result.wordsRead == words.size(), "Output limit stopped validating the instruction stream");
    words.push_back(0); expectFailure(words, "zero_instruction_word_count", limits);
    words.pop_back(); limits.maxNames = limits.maxBindings = limits.maxComputeEntryPoints = 0;
    const auto zero = parse(words, limits);
    require(zero.valid && zero.truncated && zero.names.empty() && zero.bindings.empty() && zero.computeEntryPoints.empty(),
        "Zero output budgets were not respected");
}

void longNamesAndMalformedTail() {
    ++cases;
    auto words = header();
    stringInstruction(words, 5, {8}, "depthMap"); // exactly 8 bytes plus NUL
    stringInstruction(words, 5, {9}, "motion_vectors");
    stringInstruction(words, 15, {5, 1}, "longComputeEntry");
    Limits limits; limits.maxNameBytes = 8;
    const auto result = parse(words, limits);
    require(result.valid && result.stringTruncated && result.entryPointsTruncated && result.truncated,
        "Long string dropping was not reported");
    require(result.names.size() == 1 && result.names[0].name == "depthMap" && result.computeEntryPoints.empty(),
        "Long names were retained as misleading prefixes or exact-limit string was dropped");
    ++cases;
    words = header(); stringInstruction(words, 5, {8}, std::string("motion_") + std::string(200000, 'x'));
    const auto large = parse(words);
    require(large.valid && large.stringTruncated && large.names.empty() && large.wordsRead == words.size(),
        "Extreme valid string was not bounded and fully scanned");
    words = header(); words.push_back((60003u << 16) | 5); words.push_back(8); words.insert(words.end(), 60001, 0x41414141);
    expectFailure(words, "missing_string_nul");
    words = header(); stringInstruction(words, 5, {8}, "motion_vectors");
    instruction(words, 5, {9, 0x41414141}); expectFailure(words, "missing_string_nul", limits);
}

void inputLimitsAndGroups() {
    ++cases;
    std::vector<std::byte> bytes(maxInputBytes + 1);
    auto result = parse(std::span<const std::byte>(bytes));
    require(!result.valid && result.readLimitExceeded && result.truncated && result.wordsRead == 0,
        "Oversized byte input was read");
    Words words(maxInputBytes / 4 + 1);
    result = parse(words);
    require(!result.valid && result.readLimitExceeded && result.wordsRead == 0, "Oversized word input was read");
    words = header(); Limits limits; limits.maxBytes = 16;
    result = parse(words, limits);
    require(!result.valid && result.readLimitExceeded && result.wordsRead == 0, "Configured input cap was ignored");
    limits = {}; limits.maxBindings = std::numeric_limits<std::size_t>::max();
    expectFailure(words, "invalid_limits", limits);
    bytes.resize(21); result = parse(std::span<const std::byte>(bytes));
    require(!result.valid && std::string_view(result.parseStatus) == "byte_count_not_word_aligned" && result.wordsRead == 0,
        "Partial trailing word was read");
    ++cases;
    words = header(); words.resize(maxInputBytes / 4, 1u << 16); // one-word OpNop instructions
    result = parse(words);
    require(result.valid && !result.truncated && result.wordsRead == words.size(), "Exact hard cap rejected or traversal truncated");
    ++cases;
    words = header(); instruction(words, 73, {12}); instruction(words, 71, {12, 33, 2});
    instruction(words, 74, {12, 8}); instruction(words, 75, {12, 9, 3});
    result = parse(words);
    require(result.valid && result.hasUnresolvedDecorationGroups && !findBinding(result, 8) && !findBinding(result, 9),
        "Unexpanded decoration groups were silently treated as resolved bindings");
    words = header(); instruction(words, 73, {12, 8}); expectFailure(words, "group_operand_count");
    words = header(); instruction(words, 75, {12, 8}); expectFailure(words, "group_operand_count");
}
} // namespace

int main() {
    try {
        imageAccess(); directImageWrites(); minimalComputeAndUnalignedBytes(); namesDecorationsAndOwnership(); orderAndStage();
        headerAndIdFailures(); malformedInstructionAndStrings(); utf8Validation();
        boundedOutputStillValidates(); longNamesAndMalformedTail(); inputLimitsAndGroups();
        std::cout << "{\"passed\":true,\"cases\":" << cases << ",\"checks\":" << checks
                  << ",\"maxInputBytes\":" << maxInputBytes
                  << ",\"shaderExecuted\":false,\"shaderBinaryPersisted\":false,\"gameFrameInputsVerified\":false}\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "SPIR-V metadata test failed: " << error.what() << '\n';
        return 1;
    }
}
