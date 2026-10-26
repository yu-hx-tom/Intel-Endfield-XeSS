#include "continuous_registration.hpp"
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <windows.h>
extern "C" {
void *continuous_registration_original = nullptr;
}
namespace xess_vk::continuous_registration {
namespace {
constexpr std::size_t payloadBytes = 0x140;
struct Entry {
    std::uint64_t token{};
    std::uintptr_t source{}, copied{}, graph{};
    std::array<unsigned char, payloadBytes> bytes{};
    camera_probe::Snapshot camera;
    FrameTimingAssociation frame;
    frame_input::OutputRegionSample outputRegion;
    bool constructing{}, published{};
};
std::array<Entry, 32> entries;
std::mutex mutex;
std::uint64_t serial{}, moduleBase{};
registration_bridge::Control control;
bool configured{};
FrameReader frameReader{};
void *frameUser{};
bool read(std::uint64_t address, void *output, std::size_t size) noexcept {
    if (!address || address > UINTPTR_MAX - size)
        return false;
    SIZE_T count{};
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void *>(address), output,
                             size, &count) &&
           count == size;
}
std::size_t cameraRead(void *, std::uint64_t address, void *output, std::size_t bytes,
                       std::size_t budget) {
    return bytes <= budget && read(address, output, bytes) ? bytes : 0;
}
std::uint64_t original(void *graph, std::uint32_t index, const void *source) {
    const auto pointer =
        InterlockedCompareExchangePointer(&continuous_registration_original, nullptr, nullptr);
    const auto function = reinterpret_cast<registration_bridge::Original>(pointer);
    return function(graph, index, source);
}
bool before(void *, const registration_bridge::EntrySnapshot &call,
            registration_bridge::BeforeResult &result) {
    if (call.returnAddress != moduleBase + 0x18352CD || !call.rbp ||
        call.rbp > UINTPTR_MAX - 0x7F0 || !call.r8 || !call.rcx)
        return false;
    // Incoming RBP is the upscaler producer's F, not the renderer's R.
    // 1833B08 saves R and sets F=entryRSP-7F8, so [F+7F0]=R.
    std::uint64_t renderer{}, context{}, camera{};
    if (!read(call.rbp + 0x7F0, &renderer, sizeof(renderer)) || !renderer ||
        renderer > UINTPTR_MAX - 0xB1E8 ||
        !read(renderer + 0xB1E8, &context, sizeof(context)) || !context || context > UINTPTR_MAX - 0x28 ||
        !read(context + 0x28, &camera, sizeof(camera)))
        return false;
    camera_probe::Request request;
    request.layout = camera_probe::InputLayout::UnityPlayer9aa9397aCameraRaw;
    request.camera = camera;
    const auto sample = camera_probe::readCamera(request, {cameraRead, nullptr});
    if (sample.status != camera_probe::Status::SnapshotReadUnverified || !sample.snapshot.captured)
        return false;
    const auto frame =
        frameReader ? frameReader(frameUser, call.threadId) : FrameTimingAssociation{};
    std::array<unsigned char, payloadBytes> payload{};
    if (!read(call.r8, payload.data(), payload.size()))
        return false;
    std::lock_guard lock(mutex);
    Entry *available = nullptr;
    for (auto &entry : entries)
        if (!entry.constructing && !entry.published) {
            available = &entry;
            break;
        }
    if (!available || serial == UINT64_MAX)
        return false;
    available->token = ++serial;
    available->source = call.r8;
    available->graph = call.rcx;
    available->bytes = payload;
    available->camera = sample.snapshot;
    available->frame = frame;
    available->constructing = true;
    result.token = {moduleBase, available->token};
    result.associationAvailable = true;
    result.published = true;
    return true;
}
bool close(void *, const registration_bridge::CloseEvent &call) {
    std::lock_guard lock(mutex);
    for (auto &entry : entries)
        if (entry.constructing && entry.token == call.token.scope) {
            entry.constructing = false;
            std::array<unsigned char, payloadBytes> source{}, copy{};
            if (call.exit != registration_bridge::Exit::Normal || !call.copiedPayload ||
                !read(entry.source, source.data(), source.size()) || source != entry.bytes ||
                !read(call.copiedPayload, copy.data(), copy.size()) || copy != source) {
                entry = {};
                return false;
            }
            // A recycled arena address replaces only its previous registration;
            // callbacks still recheck the actual current copied payload bytes.
            for (auto &other : entries)
                if (&other != &entry && other.published && other.copied == call.copiedPayload)
                    other = {};
            for (auto& other : entries)
                if (&other != &entry && other.published && other.graph == entry.graph &&
                    entry.frame.valid && entry.frame.ticket && other.frame.valid &&
                    other.frame.engineFrame == entry.frame.engineFrame && other.frame.ticket == entry.frame.ticket) {
                    other.outputRegion.observed = entry.outputRegion.observed = true;
                    other.outputRegion.valid = entry.outputRegion.valid = false;
                    other.outputRegion.localFiOffVerified = entry.outputRegion.localFiOffVerified = false;
                }
            entry.copied = call.copiedPayload;
            entry.published = true;
            return true;
        }
    return false;
}
} // namespace
bool configure(std::uintptr_t base, std::uint64_t device, FrameReader reader, void *user) noexcept {
    std::lock_guard lock(mutex);
    if (configured || !base || !device || !continuous_registration_original)
        return false;
    moduleBase = base;
    frameReader = reader;
    frameUser = user;
    control.sampling = false;
    configured = true;
    return true;
}
void activate() noexcept {
    if (configured)
        control.sampling = true;
}
void stop() noexcept {
    control.sampling = false;
}
bool observeCompose(void* input, void* graph, std::uint32_t thread, bool localFiOffCandidate) noexcept {
    if (!control.sampling.load() || !graph)
        return false;
    FrameTimingAssociation first{}, last{};
    std::array<std::int32_t, 4> rect{}, recheck{};
    bool captured{};
    try {
        first = frameReader ? frameReader(frameUser, thread) : FrameTimingAssociation{};
        const auto address = reinterpret_cast<std::uintptr_t>(input);
        captured = address && address <= UINTPTR_MAX - 0x40 &&
            read(address + 0x40, rect.data(), sizeof(rect)) &&
            read(address + 0x40, recheck.data(), sizeof(recheck)) && rect == recheck;
        last = frameReader ? frameReader(frameUser, thread) : FrameTimingAssociation{};
    } catch (...) {
        captured = false;
    }
    const bool exactFrame = first.valid && first.ticket && last.valid &&
        first.engineFrame == last.engineFrame && first.ticket == last.ticket;
    std::lock_guard lock(mutex);
    if (!control.sampling.load())
        return false;
    Entry* selected{};
    std::size_t matches{};
    for (auto& entry : entries) {
        if (!entry.published || entry.graph != reinterpret_cast<std::uintptr_t>(graph))
            continue;
        if (!exactFrame) {
            // An unknown frame can only revoke mappings for this graph, never borrow a latest frame.
            entry.outputRegion.observed = true;
            entry.outputRegion.valid = false;
            entry.outputRegion.localFiOffVerified = false;
        } else if (entry.frame.valid && entry.frame.engineFrame == first.engineFrame &&
                   entry.frame.ticket == first.ticket) {
            selected = &entry;
            ++matches;
        }
    }
    if (!exactFrame || !matches)
        return false;
    if (matches != 1) {
        for (auto& entry : entries)
            if (entry.published && entry.graph == reinterpret_cast<std::uintptr_t>(graph) &&
                entry.frame.valid && entry.frame.engineFrame == first.engineFrame && entry.frame.ticket == first.ticket)
                entry.outputRegion = {rect, true, false};
        return false;
    }
    const bool firstCompose = !selected->outputRegion.observed;
    selected->outputRegion = {rect, true, firstCompose && captured &&
        rect[0] >= 0 && rect[1] >= 0 && rect[2] > 0 && rect[3] > 0};
    selected->outputRegion.localFiOffVerified = selected->outputRegion.valid && localFiOffCandidate;
    return selected->outputRegion.valid;
}
CameraSample consume(std::uintptr_t payload) noexcept {
    std::lock_guard lock(mutex);
    CameraSample result;
    if (!control.sampling.load())
        return result;
    std::array<unsigned char, payloadBytes> current{};
    if (!read(payload, current.data(), current.size()))
        return result;
    for (auto &entry : entries)
        if (entry.published && entry.copied == payload) {
            if (entry.bytes == current) {
                result.camera = entry.camera;
                result.registration = entry.token;
                result.matched = true;
                result.frame = entry.frame;
                result.outputRegion = entry.outputRegion;
            }
            entry = {};
            return result;
        }
    return result;
}
} // namespace xess_vk::continuous_registration
extern "C" {
const xess_vk::registration_bridge::Route grb_fi_prepare_route{nullptr, nullptr, nullptr, nullptr,
                                                               nullptr};
const xess_vk::registration_bridge::Route grb_fi_copy_route{nullptr, nullptr, nullptr, nullptr,
                                                            nullptr};
const xess_vk::registration_bridge::Route grb_upscale_prepare_route{
    xess_vk::continuous_registration::original, xess_vk::continuous_registration::before,
    xess_vk::continuous_registration::close, nullptr, &xess_vk::continuous_registration::control};
}
