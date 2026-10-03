// The only place that converts between RDR2 and Source conventions.
//
//                 RDR2                            Source
//   length        metres                          units, 1 unit = 0.01905 m
//   world axes    +X east, +Y north, +Z up        +X, +Y, +Z up. Mapped 1:1 onto RDR2's.
//   model axes    +Y forward, +X right, +Z up     +X forward, +Y left, +Z up
//   heading 0     faces +Y                        yaw 0 faces +X
//   pitch +       nose up                         nose down
//   rotation      Rz(yaw) * Rx(pitch) * Ry(roll)  Rz(yaw) * Ry(pitch) * Rx(roll)
//
// World axes map straight across, so positions only need the origin shift and the scale.
// Model axes differ by a quarter turn about Z, and that quarter turn is the whole of the
// angle conversion: yaw + 90, pitch negated, roll unchanged. tests/test_units.cpp proves
// that against rotation matrices built separately for each convention.
//
// The RDR2 column is the GTA V "rotation order 2" convention. Verified in RDR2 on
// 2026-10-01: the plugin's start-up check found the ped's forward vector where
// RdrForward predicts (dot 1.0000), and mouse look turned and tilted the view the way
// GMod's sensitivity predicts (docs/NATIVES.md).
//
// The floating origin is the RDR2 world position that sits at Source (0,0,0). It is held
// in doubles: it is kilometres from the RDR2 world origin and is subtracted before
// anything is narrowed to float.

#pragma once

#include <cmath>

#include "gr_protocol.h"

namespace gr {

inline constexpr double kMetresPerUnit = 0.01905;
inline constexpr double kUnitsPerMetre = 1.0 / kMetresPerUnit;

struct Origin {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

struct SrcVec {
    float x;
    float y;
    float z;
};

// Field order matches Source's Angle(pitch, yaw, roll).
struct SrcAngle {
    float pitch;
    float yaw;
    float roll;
};

// Wraps degrees into (-180, 180].
inline float NormalizeDeg(float deg) noexcept {
    float r = std::fmod(deg, 360.0f);
    if (r > 180.0f) r -= 360.0f;
    if (r <= -180.0f) r += 360.0f;
    return r;
}

// World positions.
inline SrcVec RdrPosToSource(const GrVec3& p, const Origin& o) noexcept {
    return {static_cast<float>((static_cast<double>(p.x) - o.x) * kUnitsPerMetre),
            static_cast<float>((static_cast<double>(p.y) - o.y) * kUnitsPerMetre),
            static_cast<float>((static_cast<double>(p.z) - o.z) * kUnitsPerMetre)};
}

inline GrVec3 SourcePosToRdr(const SrcVec& p, const Origin& o) noexcept {
    return {static_cast<float>(static_cast<double>(p.x) * kMetresPerUnit + o.x),
            static_cast<float>(static_cast<double>(p.y) * kMetresPerUnit + o.y),
            static_cast<float>(static_cast<double>(p.z) * kMetresPerUnit + o.z)};
}

// World-space directions with a length: velocities, offsets, forces. Scale only.
inline SrcVec RdrVecToSource(const GrVec3& v) noexcept {
    return {static_cast<float>(v.x * kUnitsPerMetre), static_cast<float>(v.y * kUnitsPerMetre),
            static_cast<float>(v.z * kUnitsPerMetre)};
}

inline GrVec3 SourceVecToRdr(const SrcVec& v) noexcept {
    return {static_cast<float>(v.x * kMetresPerUnit), static_cast<float>(v.y * kMetresPerUnit),
            static_cast<float>(v.z * kMetresPerUnit)};
}

// Rotations. GrVec3 rot is x = pitch, y = roll, z = yaw, as on the wire.
inline SrcAngle RdrRotToSource(const GrVec3& rot) noexcept {
    return {NormalizeDeg(-rot.x), NormalizeDeg(rot.z + 90.0f), NormalizeDeg(rot.y)};
}

inline GrVec3 SourceAngToRdr(const SrcAngle& a) noexcept {
    return {NormalizeDeg(-a.pitch), NormalizeDeg(a.roll), NormalizeDeg(a.yaw - 90.0f)};
}

// The world direction an RDR2 rotation points its model's forward axis (+Y) along. This is
// the RDR2 column of the table above written out, so it can be held against what the game
// reports (the plugin logs both at start-up).
inline GrVec3 RdrForward(const GrVec3& rot) noexcept {
    constexpr double kDegToRad = 3.14159265358979323846 / 180.0;
    const double pitch = rot.x * kDegToRad;
    const double yaw = rot.z * kDegToRad;
    return {static_cast<float>(-std::sin(yaw) * std::cos(pitch)), static_cast<float>(std::cos(yaw) * std::cos(pitch)),
            static_cast<float>(std::sin(pitch))};
}

// Model-space points, such as bounding box corners: quarter turn about Z, then scale.
inline SrcVec RdrLocalToSource(const GrVec3& v) noexcept {
    return {static_cast<float>(v.y * kUnitsPerMetre), static_cast<float>(-v.x * kUnitsPerMetre),
            static_cast<float>(v.z * kUnitsPerMetre)};
}

inline GrVec3 SourceLocalToRdr(const SrcVec& v) noexcept {
    return {static_cast<float>(-v.y * kMetresPerUnit), static_cast<float>(v.x * kMetresPerUnit),
            static_cast<float>(v.z * kMetresPerUnit)};
}

// Field of view. Source's number (fov_desired, Player:GetFOV) is the horizontal angle of
// a 4:3 picture; on a wider screen the picture grows sideways and keeps its height. RDR2's
// camera natives take the vertical angle. So the vertical angle of the 4:3 picture is the
// one number that means the same on both sides, whatever the aspect ratio.
inline float SourceFovToVertical(float fov_4x3_horizontal) noexcept {
    constexpr double kHalfDegToRad = 3.14159265358979323846 / 360.0;
    return static_cast<float>(std::atan(std::tan(fov_4x3_horizontal * kHalfDegToRad) * 0.75) / kHalfDegToRad);
}

inline float VerticalFovToSource(float fov_vertical) noexcept {
    constexpr double kHalfDegToRad = 3.14159265358979323846 / 360.0;
    return static_cast<float>(std::atan(std::tan(fov_vertical * kHalfDegToRad) / 0.75) / kHalfDegToRad);
}

// A model-space box. The quarter turn negates one axis, so min and max swap on it.
inline void RdrBoundsToSource(const GrVec3& rmin, const GrVec3& rmax, SrcVec& smin,
                              SrcVec& smax) noexcept {
    const SrcVec a = RdrLocalToSource(rmin);
    const SrcVec b = RdrLocalToSource(rmax);
    smin = {a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y, a.z < b.z ? a.z : b.z};
    smax = {a.x < b.x ? b.x : a.x, a.y < b.y ? b.y : a.y, a.z < b.z ? b.z : a.z};
}

}  // namespace gr
