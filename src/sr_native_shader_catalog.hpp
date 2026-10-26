#pragma once
#include "upscale_tail_probe.hpp"
#include <array>
#include <string_view>

namespace xess_vk::sr_native_shaders {
enum class Policy { KeepUnknown,KeepMotionInput,RequiresConsumerProof };
struct Profile {
    std::string_view sha256;
    upscale_tail_probe::Pass callback;
    bool prepare{};
    Policy policy{Policy::KeepUnknown};
};
// Fixed Unity profile only. A fingerprint labels observed code; no entry grants
// suppression. Legacy callback names do not identify FSR-only GPU work.
inline constexpr std::array<Profile,9> profiles{{
    {"d8fcfe8f717806f6fb8272ec85c9f0a81019ec548d8c81304d589f652f068cff",upscale_tail_probe::Pass::Reactive,false,Policy::KeepMotionInput},
    {"94859c2d07f5fd5212f3ebab5d8dd99f4c7395ccf911fd9c0b6e99074bf5890e",upscale_tail_probe::Pass::Transparency,false,Policy::RequiresConsumerProof},
    {upscale_tail_probe::prepareShaderSha256,upscale_tail_probe::Pass::Unknown,true,Policy::RequiresConsumerProof},
    {"96e27f7175b2913e2de6dfe756caaa5b0c4751a7f031c9f3527df3d1f78e483e",upscale_tail_probe::Pass::Pyramid,false,Policy::RequiresConsumerProof},
    {"3106086905f8c8fdc2c3507f5c3b823d9b79b92e4209a7bef45f73cee68081fd",upscale_tail_probe::Pass::Pyramid,false,Policy::RequiresConsumerProof},
    {"8220b59e5859515a1ecba6ad074903cf88cfd0a9646b286e4f29bf8337a7f9c1",upscale_tail_probe::Pass::ShadingChange,false,Policy::RequiresConsumerProof},
    {"75b8efbc63a9301d5a29d4ebb2aeb226a9b4954d65edc01f8eac39a5591b7608",upscale_tail_probe::Pass::PrepareReactivity,false,Policy::RequiresConsumerProof},
    {"cee0f2283f8d4932961588d8647e2dc418654b59400b17589a3cdd42823d62d1",upscale_tail_probe::Pass::LumaInstability,false,Policy::RequiresConsumerProof},
    {"c5ae0cdc2ed88ed0f09cde26bf6390a124aae059e6687281bde21cb55318d09c",upscale_tail_probe::Pass::Accumulate,false,Policy::RequiresConsumerProof}
}};
// Pinned auxiliary compute module: set 0/b0 is used only by OpImageWrite.
// This is a kept writer, not an additional suppressible FSR core pass.
inline constexpr std::string_view auxiliaryWriterSha256="2298ec27217de98a4a4217f9498caa55e3965de5b4ddc5c5ee6b453f7070d519";
inline constexpr int auxiliaryWriterProfile=int(profiles.size());
// Captured on entering the photo interaction: b0 is only OpImageWrite;
// sampled inputs b1/b2 remain consumers. This module is never suppressed.
inline constexpr std::string_view photoWriterSha256="28894814159cd01f0d3e34f305d9e4c47696c90ef563491c939cf34d81a23d01";
inline constexpr int photoWriterProfile=auxiliaryWriterProfile+1;
// Captured 2026-10-26 in the character menu, recorded right before the menu
// post-process samples the same R16 image: b0 is a storage destination the
// module never reads, on the full 8x8 grid like the photo writer. Same kept
// full-destination rule; never suppressed.
inline constexpr std::string_view characterMenuWriterSha256="e5a587642c5a773639e0221cb5fdee0d2a618ae2972c455ca00c0d2247993d3b";
inline constexpr int characterMenuWriterProfile=photoWriterProfile+1;
// The character-menu post-process that samples that image (8x8, writes b0).
// Only a menu signal for FG gating; it grants no content exception.
inline constexpr std::string_view characterMenuReaderSha256="ce1c476892a965633994ff336b12ac4021ba731e67226249785c21443c15eccd";
constexpr const Profile* find(std::string_view sha256) noexcept {
    for(const auto& profile:profiles)if(profile.sha256==sha256)return &profile;
    return nullptr;
}
constexpr Policy policy(std::string_view sha256) noexcept {
    const auto* profile=find(sha256);return profile?profile->policy:Policy::KeepUnknown;
}
constexpr bool needsTypedEvidence(std::string_view sha256) noexcept {
    return sha256.size()!=64||sha256.front()=='\0'||find(sha256)||
        sha256==auxiliaryWriterSha256||sha256==photoWriterSha256||sha256==characterMenuWriterSha256;
}
constexpr const char* policyName(Policy p) noexcept {
    switch(p){case Policy::KeepMotionInput:return "keep_motion_input_producer";
    case Policy::RequiresConsumerProof:return "requires_consumer_proof";default:return "keep_unknown";}
}
}
