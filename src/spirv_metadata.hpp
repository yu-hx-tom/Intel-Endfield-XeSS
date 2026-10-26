#pragma once

#include <cstddef>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace xess_vk::spirv {

inline constexpr std::size_t maxInputBytes = 1024 * 1024;

struct Limits {
    std::size_t maxBytes = maxInputBytes;
    std::size_t maxNames = 128;
    std::size_t maxBindings = 128;
    std::size_t maxComputeEntryPoints = 16;
    std::size_t maxNameBytes = 256;
};

enum class NameKind { Object, Member, String };
struct Name {
    std::uint32_t id = 0;
    std::string name;
    NameKind kind = NameKind::Object;
    std::optional<std::uint32_t> memberIndex;
};
struct Binding {
    std::uint32_t id = 0;
    std::optional<std::uint32_t> set, binding;
    // Only linked from a retained OpName for exactly this target ID. An
    // OpMemberName/OpString is never treated as the resource's object name.
    std::string name;
    bool nonReadable = false;
    bool uniformConstantVariable = false;
    bool imageWriteOnly = false;
};
// Queries apply only to successfully created Vulkan compute pipelines. Unknown
// or incomplete metadata always retains the original consumer check.
struct ImageReads {
    struct Entry { std::uint32_t set{}, binding{}; };
    std::array<Entry,128> bindings{};
    std::size_t count{};
    bool known{};
    bool mayRead(std::uint32_t set, std::uint32_t binding) const noexcept {
        if (!known) return true;
        for (std::size_t n=0;n<count;++n)
            if (bindings[n].set==set && bindings[n].binding==binding) return true;
        return false;
    }
};
struct Metadata {
    // This is a bounded metadata parse, NOT complete SPIR-V validation or proof
    // of shader execution, resource contents, frame generation, depth, or motion.
    bool valid = false;
    const char* parseStatus = "not_parsed";
    bool truncated = false;
    bool readLimitExceeded = false;
    bool namesTruncated = false;
    bool bindingsTruncated = false;
    bool entryPointsTruncated = false;
    bool stringTruncated = false;
    // Direct Binding/DescriptorSet decorations only; groups are not expanded.
    bool hasUnresolvedDecorationGroups = false;
    std::size_t wordCount = 0;
    std::size_t wordsRead = 0;
    std::size_t errorWordOffset = 0;
    std::uint32_t version = 0;
    std::uint32_t idBound = 0;
    std::vector<std::string> computeEntryPoints;
    // Heuristically relevant names only. Missing/stripped names are not negative
    // evidence. Names and decorations remain unverified shader metadata.
    std::vector<Name> names;
    std::vector<Binding> bindings;
};

// Caller owns readable input for the synchronous call. No driver calls, file
// access, pointers into the input, or binary persistence. Native-word input is
// suitable for VkShaderModuleCreateInfo::pCode; byte input is little-endian and
// may be unaligned. Inputs over either maxBytes or the hard 1 MiB cap are not read.
// Output budget exhaustion drops additional metadata but still checks the full
// bounded instruction stream. Malformed input clears all extracted metadata.
// Limits above 1024 names/bindings/name bytes, 64 entry points, or 1 MiB refuse.
Metadata parse(std::span<const std::uint32_t> words, Limits limits = {}) noexcept;
Metadata parse(std::span<const std::byte> bytes, Limits limits = {}) noexcept;
ImageReads imageReads(const Metadata&) noexcept;

} // namespace xess_vk::spirv
