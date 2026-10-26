#pragma once
#include "camera_input_probe.hpp"
#include "upscale_frame_constants.hpp"

namespace xess_vk::frame_input {
enum class ProjectionSource { Unspecified, Camera80, CameraC0 };
enum class ClipY { Unspecified, Keep, Flip };
enum class DepthRange { Unspecified, ZeroToOne, MinusOneToOne };
enum class ProjectionJitter { Unspecified, AlreadyUnjittered, CurrentPixelJitter };
struct ProjectionConvention {
    ProjectionSource source{ProjectionSource::Unspecified};
    ClipY clipY{ClipY::Unspecified};
    DepthRange depthRange{DepthRange::Unspecified};
    ProjectionJitter jitter{ProjectionJitter::Unspecified};
    // Used only for CurrentPixelJitter: explicitly calibrated +/-1 native clip
    // shifts per current FSR pixel offset, before optional clip-Y conversion.
    int pixelJitterSignX{}, pixelJitterSignY{};
};
enum class CameraStatus {
    NotAttempted, Converted, MissingSnapshot, NonFiniteBasis, SingularBasis,
    ProjectionConventionMissing, MissingJitterFields, NonFiniteProjection
};
struct CameraMatricesResult {
    CameraStatus status{CameraStatus::NotAttempted};
    FrameConstantsSample sample;
};
// Camera snapshot must be copied at the selected registration and matched to
// this exact Prepare's provenance by the caller. The first four XYZ groups are
// actual right/up/native-Z/position groups. Invert that affine camera-to-world
// transform into SDK row-vector, row-major view storage (translation last row).
// Do not manufacture an identity fallback.
// Projection source/layout/Y/depth/jitter choices are explicit declarations,
// never guesses from plausible floats. No declaration proves game conventions.
CameraMatricesResult convertFrameCamera(const camera_probe::Snapshot&,
                                       const FrameConstantsSample&,
                                       const ProjectionConvention&) noexcept;
// Selected experimental mapping for the pinned Unity source: C0 is copied
// directly into SL's documented row-major, unjittered projection and indices
// 1/5/9/13 are flipped there. XeFG's XMStoreFloat4x4 sample uses the same row-
// vector storage. The combined game session still validates image orientation.
ProjectionConvention pinnedUnityProjectionConvention() noexcept;
// SDK adapter check only. The upstream decoder retains all finite game jitter;
// this helper never clamps or substitutes it.
bool xefgJitterInRange(const FrameConstantsSample&) noexcept;
}
