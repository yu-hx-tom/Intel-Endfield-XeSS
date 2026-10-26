#include "frame_camera_input.hpp"
#include <cmath>
#include <cstring>

namespace xess_vk::frame_input {
namespace {
float value(std::uint32_t bits) noexcept {
    float result{};
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}
bool finite(const std::array<float, 16>& matrix) noexcept {
    for (float element : matrix) if (!std::isfinite(element)) return false;
    return true;
}
}
CameraMatricesResult convertFrameCamera(const camera_probe::Snapshot& camera,
                                       const FrameConstantsSample& source,
                                       const ProjectionConvention& convention) noexcept {
    CameraMatricesResult result;
    result.sample = source;
    result.sample.validityMask &= ~(View | Projection);
    result.sample.view = {};
    result.sample.projection = {};
    if (!camera.captured) {
        result.status = CameraStatus::MissingSnapshot;
        return result;
    }
    // Native XYZ groups are camera-to-world columns for column-vector math,
    // equivalently rows for SDK row-vector storage. Invert in column form here,
    // then transpose the resulting view for the SDK. Fourth components remain
    // unclassified; the affine homogeneous row is constructed explicitly.
    double basis[3][3]{};
    double position[3]{};
    for (unsigned row = 0; row < 3; ++row) {
        position[row] = value(camera.prefixFloatBits[12 + row]);
        if (!std::isfinite(position[row])) {
            result.status = CameraStatus::NonFiniteBasis;
            return result;
        }
        for (unsigned column = 0; column < 3; ++column) {
            basis[row][column] = value(camera.prefixFloatBits[column * 4 + row]);
            if (!std::isfinite(basis[row][column])) {
                result.status = CameraStatus::NonFiniteBasis;
                return result;
            }
        }
    }
    const double a = basis[0][0], b = basis[0][1], c = basis[0][2];
    const double d = basis[1][0], e = basis[1][1], f = basis[1][2];
    const double g = basis[2][0], h = basis[2][1], i = basis[2][2];
    const double determinant = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (!std::isfinite(determinant) || determinant == 0) {
        result.status = CameraStatus::SingularBasis;
        return result;
    }
    const double inverse[3][3] = {
        {(e*i-f*h)/determinant, (c*h-b*i)/determinant, (b*f-c*e)/determinant},
        {(f*g-d*i)/determinant, (a*i-c*g)/determinant, (c*d-a*f)/determinant},
        {(d*h-e*g)/determinant, (b*g-a*h)/determinant, (a*e-b*d)/determinant}
    };
    std::array<float, 16> view{};
    for (unsigned row = 0; row < 3; ++row) {
        double translation{};
        for (unsigned column = 0; column < 3; ++column) {
            view[row * 4 + column] = static_cast<float>(inverse[row][column]);
            translation -= inverse[row][column] * position[column];
        }
        view[row * 4 + 3] = static_cast<float>(translation);
    }
    view[15] = 1;
    if (!finite(view)) {
        result.status = CameraStatus::SingularBasis;
        return result;
    }
    if (convention.source == ProjectionSource::Unspecified || convention.clipY == ClipY::Unspecified ||
        convention.depthRange == DepthRange::Unspecified || convention.jitter == ProjectionJitter::Unspecified) {
        result.status = CameraStatus::ProjectionConventionMissing;
        return result;
    }
    const auto offset = convention.source == ProjectionSource::Camera80 ? 0x80u / 4 : 0xC0u / 4;
    std::array<float, 16> projection{};
    // The actual SL path copies C0 without a transpose. Both SDK contracts use
    // row-major arrays and the XeFG example uses DirectX row-vector matrices.
    for (unsigned index = 0; index < 16; ++index)
        projection[index] = value(camera.prefixFloatBits[offset + index]);
    if (!finite(projection)) {
        result.status = CameraStatus::NonFiniteProjection;
        return result;
    }
    if (convention.jitter == ProjectionJitter::CurrentPixelJitter) {
        if ((source.validityMask & (Extents | Jitter)) != (Extents | Jitter) ||
            source.renderSize[0] <= 0 || source.renderSize[1] <= 0 ||
            !std::isfinite(source.jitter[0]) || !std::isfinite(source.jitter[1]) ||
            (convention.pixelJitterSignX != -1 && convention.pixelJitterSignX != 1) ||
            (convention.pixelJitterSignY != -1 && convention.pixelJitterSignY != 1)) {
            result.status = CameraStatus::MissingJitterFields;
            return result;
        }
        const float x = convention.pixelJitterSignX * 2.0f * source.jitter[0] / source.renderSize[0];
        const float y = convention.pixelJitterSignY * 2.0f * source.jitter[1] / source.renderSize[1];
        // Undo a clip translation using the complete W column. This also works for
        // perspective W=-Z and orthographic W=1; no hard-coded matrix element.
        for (unsigned row = 0; row < 4; ++row) {
            projection[row * 4] -= x * projection[row * 4 + 3];
            projection[row * 4 + 1] -= y * projection[row * 4 + 3];
        }
    }
    if (convention.clipY == ClipY::Flip)
        for (unsigned row = 0; row < 4; ++row) projection[row * 4 + 1] = -projection[row * 4 + 1];
    if (convention.depthRange == DepthRange::MinusOneToOne)
        for (unsigned row = 0; row < 4; ++row) projection[row * 4 + 2] = 0.5f * (projection[row * 4 + 2] + projection[row * 4 + 3]);
    if (!finite(projection)) {
        result.status = CameraStatus::NonFiniteProjection;
        return result;
    }
    for (unsigned row = 0; row < 4; ++row)
        for (unsigned column = 0; column < 4; ++column)
            result.sample.view[row * 4 + column] = view[column * 4 + row];
    result.sample.projection = projection;
    result.sample.validityMask |= View | Projection;
    result.status = CameraStatus::Converted;
    return result;
}
ProjectionConvention pinnedUnityProjectionConvention() noexcept {
    return {ProjectionSource::CameraC0, ClipY::Flip, DepthRange::ZeroToOne, ProjectionJitter::AlreadyUnjittered};
}
bool xefgJitterInRange(const FrameConstantsSample& sample) noexcept {
    return (sample.validityMask & Jitter) && std::isfinite(sample.jitter[0]) && std::isfinite(sample.jitter[1]) &&
        std::abs(sample.jitter[0]) <= 0.5f && std::abs(sample.jitter[1]) <= 0.5f;
}
}
