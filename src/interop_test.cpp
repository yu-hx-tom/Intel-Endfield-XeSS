#include "interop.hpp"
#include <chrono>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
std::string quote(const std::string& s) {
    std::string out = "\"";
    for (const unsigned char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
        else if (c < 32) { const char* hex = "0123456789abcdef"; out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15]; }
        else out += static_cast<char>(c);
    }
    return out + '"';
}
uint32_t number(const char* text) {
    std::string s(text);
    if (s.empty() || s.find_first_not_of("0123456789") != std::string::npos) throw std::invalid_argument("Expected unsigned integer argument");
    const auto value = std::stoull(s);
    if (value > std::numeric_limits<uint32_t>::max()) throw std::invalid_argument("Integer argument out of range");
    return static_cast<uint32_t>(value);
}
}
int main(int argc, char** argv) {
    uint32_t frames = 100, completed = 0;
    bool corruptExpected = false;
    xess_vk::InteropConfig config;
    const auto start = std::chrono::steady_clock::now();
    try {
        for (int i = 1; i < argc; ++i) {
            const std::string option = argv[i];
            if (option == "--negative-mismatch") { corruptExpected = true; continue; }
            if (option == "--help") { std::cout << "Usage: xess-vulkan-interop-test [--frames 100] [--width 127] [--height 73] [--input-width 0] [--input-height 0] [--timeout-ms 10000] [--negative-mismatch]\n"; return 0; }
            if (i + 1 >= argc) throw std::invalid_argument("Missing value after " + option);
            const auto value = number(argv[++i]);
            if (option == "--frames") frames = value;
            else if (option == "--width") config.width = value;
            else if (option == "--height") config.height = value;
            else if (option == "--input-width") config.inputWidth = value;
            else if (option == "--input-height") config.inputHeight = value;
            else if (option == "--timeout-ms") config.timeoutMs = value;
            else throw std::invalid_argument("Unknown option: " + option);
        }
        if (!frames || frames > 1000000) throw std::invalid_argument("--frames must be 1..1000000");
        xess_vk::InteropBridge bridge(config);
        for (uint32_t frame = 1; frame <= frames; ++frame) {
            auto expected = xess_vk::InteropBridge::makePattern(config, frame);
            bridge.publishFrame(expected);
            if (corruptExpected && frame == 1) expected[0][0] ^= 0xff;
            bridge.verifyFrame(expected); ++completed;
        }
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        const auto& stats = bridge.stats();
        const auto input = config.extent(xess_vk::Format::DepthR32F);
        std::cout << "{\"stage\":\"P1-shared-texture-transfer\",\"passed\":true,\"adapter\":" << quote(stats.adapter)
                  << ",\"width\":" << config.width << ",\"height\":" << config.height << ",\"framesVerified\":" << stats.framesVerified
                  << ",\"inputWidth\":" << input.width << ",\"inputHeight\":" << input.height
                  << ",\"bytesVerified\":" << stats.bytesVerified << ",\"elapsedMs\":" << ms
                  << ",\"formats\":[\"RGBA8_UNORM\",\"R32_FLOAT\",\"RG16_FLOAT\"],\"sameAdapterLuid\":true"
                  << ",\"producer\":\"Vulkan buffer-to-image GPU copy\",\"consumer\":\"D3D12 shared-texture GPU copy to readback\""
                  << ",\"synchronization\":\"external ownership and two shared D3D12 timeline fences\",\"everyPixelCompared\":true"
                  << ",\"validationLayersEnabled\":false,\"generatedFramesVerified\":false}\n";
        return 0;
    } catch (const std::exception& error) {
        std::cout << "{\"stage\":\"P1-shared-texture-transfer\",\"passed\":false,\"framesVerified\":" << completed
                  << ",\"error\":" << quote(error.what()) << ",\"validationLayersEnabled\":false,\"generatedFramesVerified\":false}\n";
        return 1;
    }
}
