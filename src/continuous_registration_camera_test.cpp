// Include the actual registration implementation to exercise its private
// before/close path without adding a production testing API. Owned CPU memory
// only: no game image, hooks, graphics driver, SDK, or GPU participates.
#include "continuous_registration.cpp"
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <vector>

namespace cr = xess_vk::continuous_registration;
namespace rb = xess_vk::registration_bridge;
namespace {
constexpr std::uintptr_t base = 0x10000000;
std::uint32_t frameReads{}, seenThread{};
bool advanceFrameOnRead{};
xess_vk::FrameTimingAssociation currentFrame{101, {201, 55, true}, 0, true};
xess_vk::FrameTimingAssociation frame(void*, std::uint32_t thread) {
    ++frameReads; seenThread = thread;
    const auto result = currentFrame;
    if (advanceFrameOnRead) { ++currentFrame.engineFrame; advanceFrameOnRead = false; }
    return result;
}
std::uint64_t unusedOriginal(void*, std::uint32_t, const void*) { return 0; }
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class T> void put(void* memory, std::size_t offset, T value) {
    std::memcpy(static_cast<unsigned char*>(memory) + offset, &value, sizeof(value));
}
std::uintptr_t pointer(const void* value) { return reinterpret_cast<std::uintptr_t>(value); }
struct Memory {
    // Both F+B1E8 and R+B1E8 are readable. Only the renderer slot is valid.
    std::vector<unsigned char> producer = std::vector<unsigned char>(0xB1F0);
    std::vector<unsigned char> renderer = std::vector<unsigned char>(0xB1F0);
    std::array<unsigned char, 0x30> context{};
    std::array<unsigned char, xess_vk::camera_probe::prefixBytes> camera{};
    std::array<unsigned char, 0x140> copied{};
    rb::EntrySnapshot call{};
    Memory() {
        put(producer.data(), 0x7F0, pointer(renderer.data()));
        put(renderer.data(), 0xB1E8, pointer(context.data()));
        put(context.data(), 0x28, pointer(camera.data()));
        const float position = 42.375f;
        put(camera.data(), 0x30, position);
        for (std::size_t i = 0; i < copied.size(); ++i) producer[0x220 + i] = static_cast<unsigned char>(i ^ 0xA5);
        std::memcpy(copied.data(), producer.data() + 0x220, copied.size());
        call.rbp = pointer(producer.data()); call.r8 = call.rbp + 0x220;
        call.rcx = pointer(context.data());
        call.returnAddress = base + 0x18352CD; call.threadId = GetCurrentThreadId();
    }
};
rb::BeforeResult begin(Memory& m) {
    rb::BeforeResult result;
    require(cr::before(nullptr, m.call, result) && result.published && result.associationAvailable,
            "production before follows saved renderer RBP");
    return result;
}
rb::CloseEvent normal(Memory& m, const rb::BeforeResult& result) {
    rb::CloseEvent event;
    event.entry = m.call; event.token = result.token;
    event.exit = rb::Exit::Normal; event.copiedPayload = pointer(m.copied.data());
    return event;
}
void rejected(Memory& m, const char* message) {
    rb::BeforeResult result;
    const auto reads = frameReads;
    require(!cr::before(nullptr, m.call, result) && !result.published && !result.associationAvailable, message);
    require(frameReads == reads, "bad camera chain never requests a frame association");
}
void outputRegions() {
    const xess_vk::FrameTimingAssociation older{101, {201, 55, true}, 0, true};
    const xess_vk::FrameTimingAssociation newer{102, {202, 55, true}, 0, true};
    const std::array<std::int32_t, 4> rectangle{0, 90, 2880, 1620};
    std::array<unsigned char, 0x50> compose{};
    put(compose.data(), 0x40, rectangle);
    const auto attach = [&](Memory& m, void* input = nullptr, bool candidate = true) {
        return cr::observeCompose(input ? input : compose.data(), reinterpret_cast<void*>(m.call.rcx), GetCurrentThreadId(), candidate);
    };
    const auto publish = [&](Memory& m) {
        const auto registration = begin(m);
        require(cr::close(nullptr, normal(m, registration)), "publish exact registration for Compose region");
    };
    Memory a, b;
    b.call.rcx = a.call.rcx;
    currentFrame = older; publish(a);
    currentFrame = newer; publish(b); require(attach(b), "this first exact graph/frame/ticket Compose certifies itself");
    currentFrame = older; require(attach(a), "CPU-ahead graph retains a distinct exact older ticket");
    const auto first = cr::consume(pointer(a.copied.data()));
    const auto second = cr::consume(pointer(b.copied.data()));
    require(first.outputRegion.valid && second.outputRegion.valid && first.outputRegion.rect == rectangle &&
            second.outputRegion.rect == rectangle && first.frame.ticket == older.ticket && second.frame.ticket == newer.ticket,
            "CPU-ahead entries on one graph retain their own complete frame/ticket and actual output rect");
    require(first.outputRegion.localFiOffVerified && second.outputRegion.localFiOffVerified,
            "each exact ticket retains its own suppression proof while another frame is built ahead");
    currentFrame = older;
    publish(a); require(attach(a, nullptr, false), "legacy Compose still attaches a valid ROI");
    const auto legacy = cr::consume(pointer(a.copied.data()));
    require(legacy.outputRegion.valid && !legacy.outputRegion.localFiOffVerified,
            "ROI validity alone cannot manufacture a local suppression proof");
    publish(a);
    require(!cr::observeCompose(compose.data(), reinterpret_cast<void*>(a.call.rcx + 1), GetCurrentThreadId()),
            "unknown graph returns no same-call suppression proof");
    auto sample = cr::consume(pointer(a.copied.data()));
    require(sample.matched && !sample.outputRegion.observed, "another graph never supplies this registration's region");
    publish(a); ++currentFrame.ticket.contextGeneration;
    require(!attach(a), "another ticket cannot borrow the previous graph match");
    sample = cr::consume(pointer(a.copied.data()));
    require(sample.matched && !sample.outputRegion.valid && !sample.outputRegion.localFiOffVerified,
            "another context cannot authenticate the same engine frame or suppression proof");
    currentFrame = older; publish(a); advanceFrameOnRead = true;
    require(!attach(a), "changing frame during this call returns false");
    require(!cr::consume(pointer(a.copied.data())).outputRegion.valid, "frame change during the Compose read cannot certify a region");
    currentFrame = older; publish(a); require(attach(a), "fresh exact call succeeds");
    require(!attach(a), "duplicate Compose returns false even after a successful match");
    require(!cr::consume(pointer(a.copied.data())).outputRegion.valid, "a repeated Compose is ambiguous even with identical rect bytes");
    publish(a); attach(a); require(!attach(a, reinterpret_cast<void*>(1)), "failed ROI read returns false this time");
    require(!cr::consume(pointer(a.copied.data())).outputRegion.valid, "failed Compose read revokes a previously attached region");
    publish(a); attach(a); currentFrame = {}; require(!attach(a), "unknown frame returns false this time");
    require(!cr::consume(pointer(a.copied.data())).outputRegion.valid, "unknown source frame revokes only this graph's pending region");
    currentFrame = older; publish(a); publish(b); require(!attach(a), "ambiguous same-ticket registrations return false");
    require(!cr::consume(pointer(a.copied.data())).outputRegion.valid &&
            !cr::consume(pointer(b.copied.data())).outputRegion.valid,
            "two registrations with the same graph and complete ticket reject a guessed selection");
    publish(a); attach(a); publish(b);
    require(!cr::consume(pointer(a.copied.data())).outputRegion.valid &&
            !cr::consume(pointer(b.copied.data())).outputRegion.valid,
            "a later duplicate registration revokes the already attached same-frame region");
    publish(a); attach(a); publish(a);
    sample = cr::consume(pointer(a.copied.data()));
    require(sample.matched && !sample.outputRegion.observed, "recycled copied payload never inherits an earlier rect");
    auto invalid = rectangle; invalid[1] = -1; put(compose.data(), 0x40, invalid);
    publish(a); require(!attach(a), "invalid ROI returns no suppression proof");
    require(!cr::consume(pointer(a.copied.data())).outputRegion.valid, "negative destination offset cannot certify a mapping");
    currentFrame = older;
}
}

int main() {
    try {
        continuous_registration_original = reinterpret_cast<void*>(&unusedOriginal);
        require(cr::configure(base, 1, frame, nullptr), "configure production route");
        cr::activate();
        Memory m;
        const auto first = begin(m);
        require(frameReads == 1 && seenThread == m.call.threadId, "registration uses the actual source thread");
        require(cr::close(nullptr, normal(m, first)), "actual source and exact 140-byte copy publish");
        auto sample = cr::consume(pointer(m.copied.data()));
        std::uint32_t expectedPosition{};
        std::memcpy(&expectedPosition, m.camera.data() + 0x30, sizeof(expectedPosition));
        require(sample.matched && sample.registration == first.token.scope && sample.camera.captured &&
                sample.camera.prefixFloatBits[12] == expectedPosition && sample.frame.valid &&
                sample.frame.engineFrame == 101 && sample.frame.ticket == xess_vk::FrameTimingTicket{201, 55, true},
                "exact payload retains the actual renderer camera and source frame ticket");
        require(!cr::consume(pointer(m.copied.data())).matched, "registered sample is consumed once");

        auto result = begin(m);
        m.producer[0x220] ^= 1;
        require(!cr::close(nullptr, normal(m, result)) && !cr::consume(pointer(m.copied.data())).matched,
                "source changed during original call refuses publication");
        m.producer[0x220] ^= 1;
        result = begin(m); m.copied[0x13F] ^= 1;
        require(!cr::close(nullptr, normal(m, result)), "copy mismatch at final byte of 140 refuses");
        m.copied[0x13F] ^= 1;
        result = begin(m);
        require(cr::close(nullptr, normal(m, result)), "publish fresh exact payload");
        m.copied[0x13F] ^= 1;
        require(!cr::consume(pointer(m.copied.data())).matched, "execution payload change refuses association");
        m.copied[0x13F] ^= 1;
        result = begin(m);
        auto abnormal = normal(m, result); abnormal.exit = rb::Exit::Abnormal;
        require(!cr::close(nullptr, abnormal), "abnormal original return cannot publish");

        --m.call.returnAddress; rejected(m, "foreign caller return address refuses"); ++m.call.returnAddress;
        m.call.returnAddress = base + 0x16BD9E3;
        rejected(m, "historical caller does not select current camera offsets");
        m.call.returnAddress = base + 0x18352CD;
        const auto originalF = m.call.rbp;
        m.call.rbp = 1; rejected(m, "unreadable producer saved-R slot refuses");
        m.call.rbp = std::numeric_limits<std::uintptr_t>::max() - 0x7EF;
        rejected(m, "producer slot addition overflow refuses"); m.call.rbp = originalF;
        for (const auto value : {std::uintptr_t{0}, std::uintptr_t{1},
             std::numeric_limits<std::uintptr_t>::max() - 0xB1E7,
             std::numeric_limits<std::uintptr_t>::max() - 0xB1E8}) {
            put(m.producer.data(), 0x7F0, value); rejected(m, "invalid saved renderer pointer/range refuses");
        }
        put(m.producer.data(), 0x800, pointer(m.renderer.data()));
        put(m.renderer.data(), 0x9898, pointer(m.context.data()));
        put(m.producer.data(), 0x7F0, std::uintptr_t{0});
        rejected(m, "valid historical saved-R/view slots never provide a fallback");
        put(m.producer.data(), 0x7F0, pointer(m.renderer.data()));
        for (const auto value : {std::uintptr_t{0}, std::uintptr_t{1},
             std::numeric_limits<std::uintptr_t>::max() - 0x27}) {
            put(m.renderer.data(), 0xB1E8, value); rejected(m, "invalid renderer view-context pointer refuses");
        }
        put(m.renderer.data(), 0xB1E8, pointer(m.context.data()));
        put(m.context.data(), 0x28, std::uintptr_t{1}); rejected(m, "unreadable camera pointer refuses");
        put(m.context.data(), 0x28, pointer(m.camera.data()));
        put(m.camera.data(), 0x30, std::uint32_t{0x7FC00000}); rejected(m, "nonfinite actual camera refuses");
        put(m.camera.data(), 0x30, expectedPosition);
        const auto source = m.call.r8; m.call.r8 = 1;
        rb::BeforeResult unreadablePayload;
        require(!cr::before(nullptr, m.call, unreadablePayload) && !unreadablePayload.published,
                "unreadable source payload refuses"); m.call.r8 = source;
        result = begin(m);
        require(cr::close(nullptr, normal(m, result)) && cr::consume(pointer(m.copied.data())).matched,
                "all refusal paths leave a subsequent valid registration usable");
        outputRegions();
        cr::stop();
        require(!cr::observeCompose(m.copied.data(), reinterpret_cast<void*>(m.call.rcx), GetCurrentThreadId()),
                "stopped registration cannot certify this Compose");
        require(!cr::consume(pointer(m.copied.data())).matched, "stopped production consumer refuses");
        std::printf("PASS production continuous_registration camera/Compose region: RA18352CD, exact 140-byte payload/frame ticket, CPU-ahead graph/frame/context isolation, duplicate/read/unknown/reuse refusals; no game/GPU/SDK/hooks\n");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL continuous_registration camera CPU: %s\n", e.what());
        return 1;
    }
}
