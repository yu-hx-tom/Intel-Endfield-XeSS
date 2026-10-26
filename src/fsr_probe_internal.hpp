#pragma once

// Shared implementation only. Public five-binding layout/API remains separate.
// Templates require the named request/result metadata fields; they do not select
// a layout, classify pass properties, certify a snapshot, or perform live reads.
#include "fsr_input_probe.hpp"
#include <array>
#include <limits>
#include <new>
#include <vector>

namespace xess_vk::fsr_probe::detail {
inline constexpr Address contextRangeOffset=0x10, graphStride=0x20;
inline constexpr Address pointerPageBytes=1024*8;
inline constexpr Address maxAddress=std::numeric_limits<Address>::max();
inline bool add(Address base, Address offset, Address& out) noexcept {
    if (offset > maxAddress - base) return false;
    out = base + offset; return true;
}
inline bool validRange(Address base, Address bytes) noexcept {
    return base != 0 && bytes != 0 && bytes <= maxAddress - base;
}
inline bool overlaps(Address a, Address aBytes, Address b, Address bBytes) noexcept {
    // Every supplied region has already passed validRange; empty graph arrays
    // are represented by bBytes == 0 and cannot overlap.
    return aBytes && bBytes && a < b + bBytes && b < a + aBytes;
}
inline std::uint32_t u32(const unsigned char* data) noexcept {
    return std::uint32_t(data[0]) | std::uint32_t(data[1]) << 8 | std::uint32_t(data[2]) << 16 | std::uint32_t(data[3]) << 24;
}
inline std::uint64_t u64(const unsigned char* data) noexcept {
    return std::uint64_t(u32(data)) | std::uint64_t(u32(data + 4)) << 32;
}
template<class ResultT>
class BoundedReader {
public:
    BoundedReader(Reader callback, Limits limits, ResultT& result) : callback_(callback), limits_(limits), result_(result) {}
    Status read(Address address, void* destination, std::size_t bytes) noexcept {
        if (!address || !bytes) return Status::InvalidAddress;
        if (!validRange(address, bytes)) return Status::AddressOverflow;
        if (bytes > limits_.maxBytes - result_.bytesRequested) return Status::ByteBudgetExceeded;
        if (result_.readCalls >= limits_.maxReadCalls) return Status::ReadCallBudgetExceeded;
        const auto remaining = limits_.maxBytes - result_.bytesRequested;
        result_.bytesRequested += bytes;
        ++result_.readCalls;
        try {
            const auto copied = callback_.read(callback_.user, address, destination, bytes, remaining);
            if (copied > bytes) return Status::CallbackContractViolation;
            result_.bytesCopied += copied;
            if (!copied) return Status::Unreadable;
            return copied == bytes ? Status::SnapshotParsed : Status::PartialRead;
        } catch (...) { return Status::CallbackException; }
    }
    Status pointer(Address address, Address& out) noexcept {
        std::array<unsigned char, 8> bytes{};
        const auto status = read(address, bytes.data(), bytes.size());
        if (status == Status::SnapshotParsed) out = u64(bytes.data());
        return status;
    }
    Status word(Address address, std::uint32_t& out) noexcept {
        std::array<unsigned char, 4> bytes{};
        const auto status = read(address, bytes.data(), bytes.size());
        if (status == Status::SnapshotParsed) out = u32(bytes.data());
        return status;
    }
private:
    Reader callback_;
    Limits limits_;
    ResultT& result_;
};

template<class RequestT, class ResultT>
bool overlapsInput(const RequestT& request, const ResultT& result, Address address, Address bytes) noexcept {
    Address contextBytes=contextRangeOffset+16;
    if constexpr (requires { result.graphIndexedEntries; }) {
        if (result.graphIndexedEntries) {
            contextBytes=0x18;
            for (const auto& binding : result.bindings)
                if (binding.graphEntry && overlaps(address,bytes,binding.graphEntry,16)) return true;
        }
    }
    return overlaps(address, bytes, request.prepareInputPayload, result.payloadExtent)
        || overlaps(address, bytes, request.graphContext, contextBytes)
        || overlaps(address, bytes, result.graphBegin, result.graphEntryCount * graphStride);
}

inline Status indexedGraphEntry(Address base, std::uint32_t key, std::size_t ceiling, Address& entry) noexcept {
    if (key >= ceiling) return Status::GraphEntryLimitExceeded;
    constexpr Address stride=16;
    if (Address(key)>maxAddress/stride || !add(base,Address(key)*stride,entry) || !validRange(entry,stride))
        return Status::AddressOverflow;
    return base ? Status::SnapshotParsed : Status::InvalidAddress;
}

template<class RequestT, class ResultT, class BindingT>
void resolveNative(const RequestT& request, const ResultT& result, BindingT& binding, BoundedReader<ResultT>& reader) noexcept {
    auto& candidate = binding.native;
    if (!request.nativeTableBase) return;
    candidate.tableBase = request.nativeTableBase;
    if (request.nativeLayout != NativeTableLayout::DirectArrayOf1024PagePointers_1024ObjectPointers) {
        candidate.status = Status::UnknownNativeTableLayout; return;
    }
    // The graph key uses low32, but the texture ID is a separate full DWORD.
    // It must NOT be masked to force it into this unverified native ID domain.
    if (!binding.textureId || binding.textureId > 0xFFFFF) { candidate.status = Status::NativeIdOutOfRange; return; }
    if (!validRange(request.nativeTableBase, pointerPageBytes)) { candidate.status = Status::AddressOverflow; return; }
    if (overlapsInput(request, result, request.nativeTableBase, pointerPageBytes)
        || overlaps(request.nativeTableBase, pointerPageBytes, binding.resourcePayload, 4)) {
        candidate.status = Status::PointerCycleOrOverlap; return;
    }
    // Saved 0x1801069E2: mov rax,[rcx+8*(id>>10)] -- rcx IS the array base.
    if (!add(request.nativeTableBase, Address(binding.textureId >> 10) * 8, candidate.pageSlot)) {
        candidate.status = Status::AddressOverflow; return;
    }
    auto status = reader.pointer(candidate.pageSlot, candidate.page);
    if (status != Status::SnapshotParsed) { candidate.status = status; return; }
    if (!candidate.page) { candidate.status = Status::NullNativePage; return; }
    if (!validRange(candidate.page, pointerPageBytes)) { candidate.status = Status::AddressOverflow; return; }
    if (overlaps(candidate.page, pointerPageBytes, request.nativeTableBase, pointerPageBytes)
        || overlapsInput(request, result, candidate.page, pointerPageBytes)
        || overlaps(candidate.page, pointerPageBytes, binding.resourcePayload, 4)) {
        candidate.status = Status::PointerCycleOrOverlap; return;
    }
    if (!add(candidate.page, Address(binding.textureId & 0x3FF) * 8, candidate.objectSlot)) {
        candidate.status = Status::AddressOverflow; return;
    }
    status = reader.pointer(candidate.objectSlot, candidate.object);
    if (status != Status::SnapshotParsed) { candidate.status = status; return; }
    if (!candidate.object) { candidate.status = Status::NullNativeObject; return; }
    if (!validRange(candidate.object, 0x40)) { candidate.status = Status::AddressOverflow; return; }
    if (overlaps(candidate.object, 0x40, request.nativeTableBase, pointerPageBytes)
        || overlaps(candidate.object, 0x40, candidate.page, pointerPageBytes)
        || overlapsInput(request, result, candidate.object, 0x40)
        || overlaps(candidate.object, 0x40, binding.resourcePayload, 4)) {
        candidate.status = Status::PointerCycleOrOverlap; return;
    }
    Address dimensions = 0, image = 0;
    if (!add(candidate.object, 0x14, dimensions) || !add(candidate.object, 0x38, image)) {
        candidate.status = Status::AddressOverflow; return;
    }
    std::array<unsigned char, 8> dimensionsBytes{};
    status = reader.read(dimensions, dimensionsBytes.data(), dimensionsBytes.size());
    if (status != Status::SnapshotParsed) { candidate.status = status; return; }
    candidate.widthCandidate = u32(dimensionsBytes.data());
    candidate.heightCandidate = u32(dimensionsBytes.data() + 4);
    status = reader.pointer(image, candidate.imageCandidate);
    candidate.status = status == Status::SnapshotParsed ? Status::NativeCandidateReadUnverified : status;
}

template<class RequestT, class ResultT>
Status readGraphSnapshot(const RequestT& request, ResultT& result, Address contextRange, BoundedReader<ResultT>& reader,
                         std::array<unsigned char,16>& bounds, std::vector<unsigned char>& graph) {
    // The caller supplies its prechecked address. The legacy parser computes it
    // before its first callback; preserve that cached-address behavior exactly.
    auto status = reader.read(contextRange, bounds.data(), bounds.size());
    if (status != Status::SnapshotParsed) return status;
    result.graphBegin = u64(bounds.data()); result.graphEnd = u64(bounds.data() + 8);
    if (result.graphEnd < result.graphBegin || (result.graphEnd - result.graphBegin) % graphStride
        || ((!result.graphBegin || !result.graphEnd) && result.graphBegin != result.graphEnd)) return Status::InvalidGraphRange;
    const auto graphBytes = result.graphEnd - result.graphBegin;
    const auto count = graphBytes / graphStride;
    if (count > request.limits.maxGraphEntries) return Status::GraphEntryLimitExceeded;
    result.graphEntryCount = static_cast<std::size_t>(count);
    if (graphBytes && (!validRange(result.graphBegin, graphBytes)
        || overlaps(result.graphBegin, graphBytes, request.prepareInputPayload, result.payloadExtent)
        || overlaps(result.graphBegin, graphBytes, request.graphContext, contextRangeOffset + 16)))
        return Status::PointerCycleOrOverlap;
    if (graphBytes > request.limits.maxBytes - result.bytesRequested) return Status::ByteBudgetExceeded;
    try { graph.resize(static_cast<std::size_t>(graphBytes)); }
    catch (const std::bad_alloc&) { return Status::AllocationFailed; }
    if (!graph.empty()) {
        status = reader.read(result.graphBegin, graph.data(), graph.size());
        if (status != Status::SnapshotParsed) return status;
    }
    for (std::size_t i = 1; i < result.graphEntryCount; ++i) {
        const auto previous = u32(graph.data() + (i - 1) * graphStride), current = u32(graph.data() + i * graphStride);
        if (current == previous) return Status::DuplicateGraphKey;
        if (current < previous) return Status::GraphKeysUnsorted;
    }
    return Status::SnapshotParsed;
}

template<class RequestT, class ResultT, class BindingT>
void resolveGraphBinding(const RequestT& request, ResultT& result, BindingT& binding,
                         BoundedReader<ResultT>& reader, const std::vector<unsigned char>& graph) noexcept {
    std::size_t first = 0, last = result.graphEntryCount;
    while (first < last) {
        const auto middle = first + (last - first) / 2;
        if (u32(graph.data() + middle * graphStride) < binding.graphKey) first = middle + 1;
        else last = middle;
    }
    // Unlike the original helper, reject end and require exact key equality.
    if (first == result.graphEntryCount || u32(graph.data() + first * graphStride) != binding.graphKey) {
        binding.graphStatus = Status::GraphKeyMissing; return;
    }
    binding.graphEntry = result.graphBegin + first * graphStride;
    const auto* entry = graph.data() + first * graphStride;
    binding.entryType = u32(entry + 8);
    if (binding.entryType != 2 && binding.entryType != 3) { binding.graphStatus = Status::UnsupportedGraphEntryType; return; }
    binding.resourcePayload = u64(entry + 0x10);
    if (!binding.resourcePayload) { binding.graphStatus = Status::NullResourcePayload; return; }
    if (!validRange(binding.resourcePayload, 4)) { binding.graphStatus = Status::AddressOverflow; return; }
    if (overlapsInput(request, result, binding.resourcePayload, 4)) { binding.graphStatus = Status::PointerCycleOrOverlap; return; }
    binding.graphStatus = reader.word(binding.resourcePayload, binding.textureId);
    if (binding.graphStatus != Status::SnapshotParsed) return;
    if (!binding.textureId) { binding.graphStatus = Status::NullTextureId; return; }
    binding.graphStatus = Status::GraphTextureIdRead;
    ++result.graphTextureIdsRead;
    resolveNative(request, result, binding, reader);
    if (binding.native.status == Status::NativeCandidateReadUnverified) ++result.nativeCandidatesRead;
}

} // namespace xess_vk::fsr_probe::detail
