#include "camera_input_probe.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using namespace xess_vk::camera_probe;
std::atomic<unsigned> checks{};
unsigned cases = 0;
void require(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
struct Call { Address address; std::size_t size, remaining; };
enum class Fault { None, Unreadable, Partial, Throws, OverReport, Changes };
struct Memory {
    Address base = 0x10003; // Deliberately unaligned; parser does byte copies.
    std::array<unsigned char, 0x4C0> bytes{};
    std::vector<Call> calls;
    Fault fault = Fault::None;
    unsigned faultCall = 0;
    std::size_t changeByte = 0;
    Request* alterRequest = nullptr;
    bool reenter = false;
    void word(std::size_t offset, std::uint32_t value) {
        require(offset <= bytes.size() - 4, "fixture word outside memory");
        for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = static_cast<unsigned char>(value >> (8 * i));
    }
    Memory() {
        for (std::size_t i = 0; i < prefixBytes / 4; ++i) word(i * 4, 0x3F000001u + static_cast<std::uint32_t>(i));
        for (std::size_t i = 0; i < clipToPrevBytes / 4; ++i) word(clipToPrevSourceOffset + i * 4, 0xBF000001u + static_cast<std::uint32_t>(i));
    }
    static std::size_t read(void* user, Address address, void* destination, std::size_t size, std::size_t remaining) {
        auto& m = *static_cast<Memory*>(user);
        require(size <= remaining, "reader exceeded remaining budget");
        m.calls.push_back({address, size, remaining});
        if (m.alterRequest) { *m.alterRequest = {}; m.alterRequest = nullptr; }
        if (m.reenter) {
            m.reenter = false;
            Memory inner; Request request;
            request.layout = InputLayout::UnityPlayerBee7be52CameraRaw; request.camera = inner.base;
            require(readCamera(request, {Memory::read, &inner}).snapshot.captured, "reentrant read failed");
        }
        const bool faulty = m.calls.size() == m.faultCall;
        if (faulty && m.fault == Fault::Throws) throw std::runtime_error("synthetic reader exception");
        if (faulty && m.fault == Fault::OverReport) return size + 1;
        if (faulty && m.fault == Fault::Unreadable) return 0;
        require(address >= m.base, "out of bounds read below fixture");
        const auto offset = address - m.base;
        require(offset <= m.bytes.size() && size <= m.bytes.size() - offset, "out of bounds read above fixture");
        const auto copied = faulty && m.fault == Fault::Partial ? size - 1 : size;
        std::memcpy(destination, m.bytes.data() + offset, copied);
        if (faulty && m.fault == Fault::Changes) {
            require(m.changeByte < copied, "change byte outside requested read");
            static_cast<unsigned char*>(destination)[m.changeByte] ^= 1;
        }
        return copied;
    }
};
Request requestFor(const Memory& m, bool temporal = false) {
    Request r; r.layout = InputLayout::UnityPlayerBee7be52CameraRaw; r.camera = m.base; r.captureClipToPrev = temporal; return r;
}
void conservative(const Result& r) {
    require(!r.snapshotAtomic && !r.lifetimePinned && !r.sourceIdentityVerified && !r.matrixSemanticsVerified &&
        !r.sameFrameVerified && !r.pixelsRead && !r.gameFunctionsCalled && !r.xeFGConstantsReady,
        "raw data promoted into semantic or lifetime proof");
}
void noPublication(const Result& r) {
    require(!r.snapshot.captured && !r.snapshot.clipToPrevCaptured, "failed snapshot marked captured");
    require(std::all_of(r.snapshot.prefixFloatBits.begin(), r.snapshot.prefixFloatBits.end(), [](auto v) { return v == 0; }), "partial prefix published");
    require(std::all_of(r.snapshot.clipToPrevFloatBits.begin(), r.snapshot.clipToPrevFloatBits.end(), [](auto v) { return v == 0; }), "partial temporal published");
    conservative(r);
}
void exactBits(const Result& r, bool temporal) {
    for (std::size_t i = 0; i < prefixBytes / 4; ++i)
        require(r.snapshot.prefixFloatBits[i] == 0x3F000001u + i, "prefix transposed, transformed or corrupted");
    for (std::size_t i = 0; i < clipToPrevBytes / 4; ++i)
        require(r.snapshot.clipToPrevFloatBits[i] == (temporal ? 0xBF000001u + i : 0), "optional bits transformed or unexpectedly read");
}
void successCases() {
    for (const bool temporal : {false, true}) {
        ++cases; Memory m; auto request = requestFor(m, temporal);
        request.limits = {temporal ? 640u : 512u, temporal ? 4u : 2u};
        const auto r = readCamera(request, {Memory::read, &m});
        require(r.status == Status::SnapshotReadUnverified && r.phase == Phase::Complete, "valid snapshot failed");
        require(r.snapshot.captured && r.snapshot.clipToPrevCaptured == temporal, "capture flags wrong");
        require(r.prefixReadingsEqual && r.clipToPrevReadingsEqual == temporal && r.requestedReadingsEqual &&
            r.allRetainedFloatCandidatesFinite, "read consistency wrong");
        require(r.bytesRequested == request.limits.maxBytes && r.bytesCopied == r.bytesRequested &&
            r.readCalls == request.limits.maxReadCalls, "budget accounting wrong");
        std::size_t remaining = request.limits.maxBytes;
        for (std::size_t i = 0; i < m.calls.size(); ++i) {
            const bool prefix = i == 0 || i + 1 == m.calls.size();
            require(m.calls[i].address == m.base + (prefix ? 0 : clipToPrevSourceOffset), "read address order wrong");
            require(m.calls[i].size == (prefix ? prefixBytes : clipToPrevBytes), "read extent wrong");
            require(m.calls[i].remaining == remaining, "pre-charge remaining budget wrong");
            remaining -= m.calls[i].size;
        }
        exactBits(r, temporal); conservative(r);
    }
    ++cases; Memory m; auto request = requestFor(m, true); m.alterRequest = &request;
    const auto changed = readCamera(request, {Memory::read, &m});
    require(changed.snapshot.captured && changed.readCalls == 4, "callback mutated the frozen read plan"); exactBits(changed, true);
    ++cases; Memory nested; nested.reenter = true;
    require(readCamera(requestFor(nested, true), {Memory::read, &nested}).snapshot.captured, "outer read lost reentrant state");
}
void currentLayoutCases() {
    ++cases; Memory m;
    auto request = requestFor(m);
    request.layout = InputLayout::UnityPlayer9aa9397aCameraRaw;
    const auto result = readCamera(request, {Memory::read, &m});
    require(result.status == Status::SnapshotReadUnverified && result.snapshot.captured &&
        !result.snapshot.clipToPrevCaptured && result.readCalls == 2 && result.bytesRequested == 512,
        "current camera layout captures the bounded prefix only");
    exactBits(result, false); conservative(result);

    ++cases; m.calls.clear(); request.captureClipToPrev = true;
    const auto temporal = readCamera(request, {Memory::read, &m});
    require(temporal.status == Status::UnverifiedClipToPrevLayout && temporal.phase == Phase::Preflight &&
        m.calls.empty() && !temporal.readCalls && !temporal.bytesRequested,
        "current layout refuses unverified temporal offset before reading");
    noPublication(temporal);

    ++cases; request.captureClipToPrev = false; m.word(projectionSourceOffset, 0x7FC00000u);
    const auto nonfinite = readCamera(request, {Memory::read, &m});
    require(nonfinite.status == Status::NonFinitePrefix &&
        nonfinite.firstNonFiniteByteOffset == projectionSourceOffset,
        "current C0 projection retains finite-value refusal");
    noPublication(nonfinite);
}
void failures() {
    const std::array faults{Fault::Unreadable, Fault::Partial, Fault::Throws, Fault::OverReport};
    const std::array statuses{Status::Unreadable, Status::PartialRead, Status::CallbackException, Status::CallbackContractViolation};
    const std::array phases{Phase::PrefixBefore, Phase::ClipToPrevBefore, Phase::ClipToPrevAfter, Phase::PrefixAfter};
    for (std::size_t f = 0; f < faults.size(); ++f) for (unsigned at = 1; at <= 4; ++at) {
        ++cases; Memory m; m.fault = faults[f]; m.faultCall = at;
        auto r = readCamera(requestFor(m, true), {Memory::read, &m});
        require(r.status == statuses[f] && r.phase == phases[at - 1], "failed read status/phase wrong");
        require(r.readCalls == at && m.calls.size() == at, "reads continued after failure");
        require(r.failedReadAddress == m.calls.back().address && r.failedReadBytes == m.calls.back().size, "failed read diagnostics wrong");
        std::size_t requested = 0, copied = 0;
        for (std::size_t i = 0; i < m.calls.size(); ++i) {
            requested += m.calls[i].size;
            if (i + 1 < m.calls.size()) copied += m.calls[i].size;
        }
        if (faults[f] == Fault::Partial) copied += m.calls.back().size - 1;
        require(r.bytesRequested == requested && r.bytesCopied == copied, "failure budget accounting wrong"); noPublication(r);
    }
    for (std::size_t byte = 0; byte < prefixBytes; ++byte) {
        ++cases; Memory m; m.fault = Fault::Changes; m.faultCall = 4; m.changeByte = byte;
        auto r = readCamera(requestFor(m, true), {Memory::read, &m});
        require(r.status == Status::PrefixChanged && !r.requestedReadingsEqual, "changed prefix accepted"); noPublication(r);
    }
    for (std::size_t byte = 0; byte < clipToPrevBytes; ++byte) {
        ++cases; Memory m; m.fault = Fault::Changes; m.faultCall = 3; m.changeByte = byte;
        auto r = readCamera(requestFor(m, true), {Memory::read, &m});
        require(r.status == Status::ClipToPrevChanged && !r.requestedReadingsEqual, "changed temporal accepted"); noPublication(r);
    }
}
void finiteCases() {
    for (auto bits : {0x7F800000u, 0xFF800000u, 0x7FC00000u, 0x7F800001u, 0xFFFFFFFFu}) {
        for (std::size_t offset = 0; offset < prefixBytes; offset += 4) {
            ++cases; Memory m; m.word(offset, bits);
            const auto r = readCamera(requestFor(m, true), {Memory::read, &m});
            require(r.status == Status::NonFinitePrefix && r.nonFiniteOffsetAvailable && r.firstNonFiniteByteOffset == offset,
                "non-finite prefix escaped or wrong location");
            require(r.requestedReadingsEqual && !r.allRetainedFloatCandidatesFinite, "equality was confused with finite values"); noPublication(r);
        }
        for (std::size_t offset = 0; offset < clipToPrevBytes; offset += 4) {
            ++cases; Memory m; m.word(clipToPrevSourceOffset + offset, bits);
            const auto r = readCamera(requestFor(m, true), {Memory::read, &m});
            require(r.status == Status::NonFiniteClipToPrev && r.firstNonFiniteByteOffset == clipToPrevSourceOffset + offset,
                "non-finite temporal escaped or wrong location"); noPublication(r);
        }
    }
    for (auto bits : {0u, 0x80000000u, 1u, 0x80000001u, 0x007FFFFFu, 0x7F7FFFFFu, 0xFF7FFFFFu}) {
        ++cases; Memory m;
        for (std::size_t offset = 0; offset < prefixBytes; offset += 4) m.word(offset, bits);
        for (std::size_t offset = 0; offset < clipToPrevBytes; offset += 4) m.word(clipToPrevSourceOffset + offset, bits);
        const auto r = readCamera(requestFor(m, true), {Memory::read, &m});
        require(r.snapshot.captured && r.allRetainedFloatCandidatesFinite, "finite raw candidate rejected");
        require(std::all_of(r.snapshot.prefixFloatBits.begin(), r.snapshot.prefixFloatBits.end(), [bits](auto v) { return v == bits; }), "finite bits changed");
        conservative(r);
    }
    ++cases; Memory m; m.word(clipToPrevSourceOffset, 0x7F800000u);
    auto r = readCamera(requestFor(m), {Memory::read, &m});
    require(r.snapshot.captured && r.readCalls == 2 && !r.snapshot.clipToPrevCaptured, "unrequested temporal was read/validated");
}
void preflight() {
    for (const bool temporal : {false, true}) {
        for (std::size_t bytes = 0; bytes <= hardMaxBytes + 1; ++bytes) for (std::size_t calls = 0; calls <= hardMaxReadCalls + 1; ++calls) {
            ++cases; Memory m; auto q = requestFor(m, temporal); q.limits = {bytes, calls};
            auto r = readCamera(q, {Memory::read, &m});
            const bool limitsValid = bytes && bytes <= hardMaxBytes && calls && calls <= hardMaxReadCalls;
            const auto expected = !limitsValid ? Status::InvalidLimits : bytes < (temporal ? 640u : 512u) ? Status::ByteBudgetExceeded :
                calls < (temporal ? 4u : 2u) ? Status::ReadCallBudgetExceeded : Status::SnapshotReadUnverified;
            require(r.status == expected, "preflight budget table mismatch");
            if (expected != Status::SnapshotReadUnverified) {
                require(m.calls.empty() && r.readCalls == 0 && r.bytesRequested == 0, "invalid plan performed read"); noPublication(r);
            }
        }
    }
    for (const auto layout : {InputLayout::Unknown, static_cast<InputLayout>(999)}) {
        ++cases; Memory m; auto q = requestFor(m); q.layout = layout;
        auto r = readCamera(q, {Memory::read, &m}); require(r.status == Status::UnknownInputLayout && m.calls.empty(), "unknown layout read"); noPublication(r);
    }
    ++cases; Memory m;
    auto missing = readCamera(requestFor(m), {}); require(missing.status == Status::MissingReader, "missing reader not refused"); noPublication(missing);
    for (const bool temporal : {false, true}) {
        const auto max = std::numeric_limits<Address>::max();
        const auto extent = temporal ? clipToPrevSourceOffset + clipToPrevBytes : prefixBytes;
        for (const Address address : {Address(0), max, max - extent + 1, max - extent}) {
            ++cases; Memory memory; memory.base = address;
            auto result = readCamera(requestFor(memory, temporal), {Memory::read, &memory});
            if (address == max - extent) require(result.snapshot.captured, "representable exclusive end rejected");
            else {
                require(result.status == (address ? Status::AddressOverflow : Status::InvalidAddress), "bad address status");
                require(memory.calls.empty(), "bad address invoked reader"); noPublication(result);
            }
        }
    }
}
void independentThreads() {
    ++cases; std::atomic<bool> passed{true}; std::vector<std::thread> threads;
    for (unsigned t = 0; t < 8; ++t) threads.emplace_back([t, &passed] {
        try {
            for (unsigned n = 0; n < 500; ++n) {
                Memory m; m.base += Address(t) * 0x100000;
                const auto value = 0x3E800000u + t * 500 + n; m.word(0, value);
                auto r = readCamera(requestFor(m, n % 2), {Memory::read, &m});
                require(r.snapshot.captured && r.snapshot.prefixFloatBits[0] == value, "thread mixed snapshots"); conservative(r);
            }
        } catch (...) { passed = false; }
    });
    for (auto& thread : threads) thread.join(); require(passed, "independent threaded readers failed");
}
} // namespace
int main() {
    try {
        successCases(); currentLayoutCases(); failures(); finiteCases(); preflight(); independentThreads();
        require(std::strcmp(statusName(Status::SnapshotReadUnverified), "SnapshotReadUnverified") == 0, "status text wrong");
        require(std::strcmp(phaseName(Phase::ClipToPrevBefore), "ClipToPrevBefore") == 0, "phase text wrong");
        require(std::strcmp(statusName(static_cast<Status>(999)), "UnknownStatus") == 0, "unknown status label wrong");
        require(std::strcmp(phaseName(static_cast<Phase>(999)), "UnknownPhase") == 0, "unknown phase label wrong");
        std::cout << "{\"passed\":true,\"cases\":" << cases << ",\"checks\":" << checks.load()
                  << ",\"gameAccessed\":false,\"gpuCalls\":0,\"xeFGConstantsReady\":false}\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
