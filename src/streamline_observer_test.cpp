// Exercises only forwarding wrappers with real ABI-shaped, local test objects.
// Never calls tryInstall and never reads or patches a game module.
#include "streamline_observer.hpp"
#include <windows.h>
#include <atomic>
#include <cstring>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
namespace observer = xess_vk::streamline_observer;
struct Token final : sl::FrameToken { uint32_t number = 77; operator uint32_t() const override { return number; } } token;
std::vector<std::string> lines;
std::mutex linesMutex;
std::atomic<uint64_t> forwardedTags{}, forwardedConstants{}, forwardedTokens{};
std::atomic<bool> argumentsOkay{true};
bool checkErrorPreservation = false;
sl::ViewportHandle viewport(19);
sl::Resource resource;
sl::ResourceTag tag;
sl::Constants constants;
sl::CommandBuffer* command = reinterpret_cast<sl::CommandBuffer*>(0xabcdef00);
constexpr auto tagResult = static_cast<sl::Result>(0x234);
constexpr auto constantsResult = static_cast<sl::Result>(0x235);
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void sink(const char* line) { std::lock_guard lock(linesMutex); lines.emplace_back(line); }
void throwingSink(const char*) { throw std::runtime_error("simulated logging failure"); }
sl::Result tagsOriginal(const sl::FrameToken& f, const sl::ViewportHandle& v, const sl::ResourceTag* t, uint32_t n, sl::CommandBuffer* c) {
    ++forwardedTags;
    if (&f != &token || &v != &viewport || c != command || (n != 0 && (n != 1 || t != &tag))) argumentsOkay = false;
    if (checkErrorPreservation && GetLastError() != 0x4321) argumentsOkay = false;
    SetLastError(0x9876);
    return tagResult;
}
sl::Result constantsOriginal(const sl::Constants& c, const sl::FrameToken& f, const sl::ViewportHandle& v) {
    ++forwardedConstants;
    if (&c != &constants || &f != &token || &v != &viewport) argumentsOkay = false;
    return constantsResult;
}
sl::Result tokenOriginal(sl::FrameToken*& result, const uint32_t* index) {
    ++forwardedTokens;
    if (!index || *index != 77) argumentsOkay = false;
    result = &token; return sl::Result::eOk;
}
bool contains(const char* text) { for (const auto& line : lines) if (line.find(text) != std::string::npos) return true; return false; }
}
int main() {
    try {
        static_assert(sizeof(sl::BaseStructure) == 32);
        static_assert(sizeof(sl::FrameToken) == 40);
        // Derived SL structures are not standard-layout: check actual ABI
        // addresses without applying offsetof to a non-standard-layout type.
        require(reinterpret_cast<const char*>(&tag.resource) - reinterpret_cast<const char*>(&tag) == 32 &&
                reinterpret_cast<const char*>(&tag.extent) - reinterpret_cast<const char*>(&tag) == 48, "ResourceTag ABI mismatch");
        require(reinterpret_cast<const char*>(&resource.native) - reinterpret_cast<const char*>(&resource) == 40 &&
                reinterpret_cast<const char*>(&resource.reserved) - reinterpret_cast<const char*>(&resource) == 104, "Resource ABI mismatch");
        resource.native = reinterpret_cast<void*>(0x12345000); resource.state = 5; resource.nativeFormat = 97;
        resource.width = 640; resource.height = 360; resource.mipLevels = 1; resource.arrayLayers = 1; resource.flags = 0; resource.usage = 17;
        tag.resource = &resource; tag.type = sl::kBufferTypeMotionVectors; tag.lifecycle = sl::eValidUntilPresent; tag.extent = {0, 0, 640, 360};
        std::memset(&constants.cameraViewToClip, 0, sizeof(constants.cameraViewToClip));
        std::memset(&constants.clipToCameraView, 0, sizeof(constants.clipToCameraView));
        std::memset(&constants.clipToPrevClip, 0, sizeof(constants.clipToPrevClip));
        std::memset(&constants.prevClipToClip, 0, sizeof(constants.prevClipToClip));
        constants.cameraFOV = std::numeric_limits<float>::infinity();
        constants.jitterOffset = {0.25f, -0.25f}; constants.mvecScale = {0.5f, 0.25f};
        auto functions = observer::testing::reset(&sink, {&tagsOriginal, &constantsOriginal, &tokenOriginal});
        const auto savedResource = resource; const auto savedTag = tag;
        require(functions.tags(token, viewport, &tag, 1, command) == tagResult, "Tag return code changed");
        require(functions.constants(constants, token, viewport) == constantsResult, "Constants return code changed");
        uint32_t index = 77; sl::FrameToken* result = nullptr;
        require(functions.token(result, &index) == sl::Result::eOk && result == &token, "Token result/output changed");
        require(std::memcmp(&resource, &savedResource, sizeof(resource)) == 0 && std::memcmp(&tag, &savedTag, sizeof(tag)) == 0, "Observer changed resource/tag data");
        require(contains("\"frame\":77") && contains("\"viewport\":19"), "Incorrect frame/viewport ABI");
        require(contains("\"native\":\"0x12345000\"") && contains("\"format\":97") && contains("\"usage\":17"), "Missing resource metadata");
        require(contains("\"fov\":null"), "Non-finite constant would produce invalid JSON");
        require(contains("\"pid\":") && contains("\"uptime_ms\":") && contains("\"thread_id\":"), "Missing correlation metadata");
        // A guarded resource pointer must fail observation without preventing forwarding.
        void* inaccessible = VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
        require(inaccessible != nullptr, "Cannot reserve inaccessible test resource");
        tag.resource = static_cast<sl::Resource*>(inaccessible);
        checkErrorPreservation = true; SetLastError(0x4321);
        require(functions.tags(token, viewport, &tag, 1, command) == tagResult, "Unreadable resource prevented forwarding");
        require(GetLastError() == 0x9876, "Trace changed original's Win32 last-error result"); checkErrorPreservation = false;
        VirtualFree(inaccessible, 0, MEM_RELEASE); tag.resource = &resource;
        require(contains("\"resourceReadable\":false"), "Unreadable resource was not reported");
        std::vector<std::thread> workers;
        for (unsigned i = 0; i < 4; ++i) workers.emplace_back([&] {
            for (unsigned n = 0; n < 40000; ++n) if (functions.tags(token, viewport, nullptr, 0, command) != tagResult) argumentsOkay = false;
        });
        for (auto& worker : workers) worker.join();
        for (unsigned n = 1; n < 20000; ++n) require(functions.constants(constants, token, viewport) == constantsResult, "Late constants forwarding changed");
        observer::flushSummary();
        require(argumentsOkay && forwardedTags == 160002 && forwardedConstants == 20000 && forwardedTokens == 1, "Forwarded argument/count mismatch");
        require(contains("\"tagCalls\":160002") && contains("\"tagsTotal\":2"), "Summary counters incorrect");
        require(contains("\"event\":\"sl_tags\",\"call\":115200,") && !contains("\"event\":\"sl_tags\",\"call\":116400,"), "Sparse sampling did not stop at its bound");
        require(contains("\"event\":\"sl_constants\",\"call\":18000,") && !contains("\"event\":\"sl_constants\",\"call\":18600,"), "Late constants sampling bound incorrect");
        require(lines.size() == 162, "Sampling was not bounded to 128 tags, 32 constants, one token and summary");
        // Simulate Unity overwriting CPU data cells after installation. Summary
        // reads must report it and must never reinstall or mutate a test cell.
        void* tagCell = reinterpret_cast<void*>(functions.tags);
        void* constantsCell = reinterpret_cast<void*>(functions.constants);
        void* tokenCell = reinterpret_cast<void*>(functions.token);
        observer::testing::observeSlots(&tagCell, &constantsCell, &tokenCell, 7);
        observer::flushSummary();
        require(lines.back().find("\"hookSlotsStillActive\":7") != std::string::npos, "Active hook cells not recognized");
        tagCell = reinterpret_cast<void*>(&tagsOriginal);
        constantsCell = reinterpret_cast<void*>(0x12345678);
        observer::flushSummary();
        require(lines.back().find("\"hookSlotsStillActive\":4") != std::string::npos &&
                lines.back().find("\"state\":\"restored_original\"") != std::string::npos &&
                lines.back().find("\"state\":\"replaced\"") != std::string::npos, "Overwritten cells falsely reported active");
        require(tagCell == reinterpret_cast<void*>(&tagsOriginal) && constantsCell == reinterpret_cast<void*>(0x12345678), "Summary rewrote test cells");
        inaccessible = VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
        require(inaccessible != nullptr, "Cannot reserve inaccessible slot test");
        observer::testing::observeSlots(&tagCell, &constantsCell, static_cast<void**>(inaccessible), 7);
        observer::flushSummary();
        require(lines.back().find("\"hookSlotsStillActive\":0") != std::string::npos && lines.back().find("\"state\":\"unreadable\"") != std::string::npos,
                "Unreadable cell falsely reported active");
        observer::testing::observeSlots(nullptr, nullptr, nullptr, 0);
        VirtualFree(inaccessible, 0, MEM_RELEASE);
        functions = observer::testing::reset(&throwingSink, {&tagsOriginal, &constantsOriginal, &tokenOriginal});
        require(functions.tags(token, viewport, &tag, 1, command) == tagResult, "Sink exception changed forwarding");
        require(functions.constants(constants, token, viewport) == constantsResult, "Sink exception changed constants forwarding");
        std::cout << "{\"stage\":\"streamline-observer-forwarding\",\"passed\":true,\"forwardedTagCalls\":160003,\"boundedSamples\":128,\"constantsSamples\":32,\"readOnlySlotChecksPassed\":true,\"gameSlotPatched\":false}\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
