#include "gr_test.h"
#include "gr_units.h"

namespace {

constexpr double kPi = 3.14159265358979323846;
double Rad(double deg) { return deg * kPi / 180.0; }

struct V {
    double x, y, z;
};

// RDR2 side, built from the documented convention and nothing else: a model whose
// forward is +Y, right is +X and up is +Z, rotated by Rz(yaw) * Rx(pitch) * Ry(roll).
V RdrRotate(const GrVec3& rot, V v) {
    const double p = Rad(rot.x), r = Rad(rot.y), y = Rad(rot.z);
    V a{v.x * std::cos(r) + v.z * std::sin(r), v.y, -v.x * std::sin(r) + v.z * std::cos(r)};  // Ry
    V b{a.x, a.y * std::cos(p) - a.z * std::sin(p), a.y * std::sin(p) + a.z * std::cos(p)};   // Rx
    return {b.x * std::cos(y) - b.y * std::sin(y), b.x * std::sin(y) + b.y * std::cos(y), b.z};  // Rz
}

// Source side: AngleVectors from the Source SDK's mathlib, written out as it is there.
void SourceAngleVectors(const gr::SrcAngle& a, V& forward, V& right, V& up) {
    const double sp = std::sin(Rad(a.pitch)), cp = std::cos(Rad(a.pitch));
    const double sy = std::sin(Rad(a.yaw)), cy = std::cos(Rad(a.yaw));
    const double sr = std::sin(Rad(a.roll)), cr = std::cos(Rad(a.roll));
    forward = {cp * cy, cp * sy, -sp};
    right = {-1 * sr * sp * cy + -1 * cr * -sy, -1 * sr * sp * sy + -1 * cr * cy, -1 * sr * cp};
    up = {cr * sp * cy + -sr * -sy, cr * sp * sy + -sr * cy, cr * cp};
}

void CheckSameDirection(const V& a, const V& b) {
    CHECK_NEAR(a.x, b.x, 1e-5);
    CHECK_NEAR(a.y, b.y, 1e-5);
    CHECK_NEAR(a.z, b.z, 1e-5);
}

}  // namespace

GR_TEST(units_scale_is_exact) {
    CHECK_NEAR(gr::kMetresPerUnit, 0.01905, 0.0);
    // 1 m is 52.4934 units; a 72 unit tall GMod player is 1.3716 m.
    CHECK_NEAR(gr::RdrVecToSource({1.0f, 0.0f, 0.0f}).x, 52.4934, 1e-3);
    CHECK_NEAR(gr::SourceVecToRdr({0.0f, 0.0f, 72.0f}).z, 1.3716, 1e-6);
}

GR_TEST(units_position_uses_origin) {
    const gr::Origin origin{-1234.5, 2100.25, 45.0};
    const GrVec3 at_origin{-1234.5f, 2100.25f, 45.0f};
    const gr::SrcVec s0 = gr::RdrPosToSource(at_origin, origin);
    CHECK_NEAR(s0.x, 0.0, 1e-3);
    CHECK_NEAR(s0.y, 0.0, 1e-3);
    CHECK_NEAR(s0.z, 0.0, 1e-3);

    // 10 m east, 20 m north, 1 m up of the origin.
    const gr::SrcVec s1 = gr::RdrPosToSource({-1224.5f, 2120.25f, 46.0f}, origin);
    CHECK_NEAR(s1.x, 524.934, 0.02);
    CHECK_NEAR(s1.y, 1049.869, 0.02);
    CHECK_NEAR(s1.z, 52.493, 0.02);
}

GR_TEST(units_position_round_trip) {
    const gr::Origin origin{2500.0, -1300.0, 80.0};
    const gr::SrcVec cases[] = {{0, 0, 0}, {8000, -8000, 512}, {-15000.5f, 123.25f, -64.0f}};
    for (const gr::SrcVec& s : cases) {
        const gr::SrcVec back = gr::RdrPosToSource(gr::SourcePosToRdr(s, origin), origin);
        // RDR2 positions are float32. A few km out that is about a quarter of a
        // millimetre, which is a hundredth of a Source unit.
        CHECK_NEAR(back.x, s.x, 0.05);
        CHECK_NEAR(back.y, s.y, 0.05);
        CHECK_NEAR(back.z, s.z, 0.05);
    }
}

GR_TEST(units_normalize_deg) {
    CHECK_NEAR(gr::NormalizeDeg(0.0f), 0.0, 0.0);
    CHECK_NEAR(gr::NormalizeDeg(180.0f), 180.0, 0.0);
    CHECK_NEAR(gr::NormalizeDeg(-180.0f), 180.0, 0.0);
    CHECK_NEAR(gr::NormalizeDeg(270.0f), -90.0, 0.0);
    CHECK_NEAR(gr::NormalizeDeg(-270.0f), 90.0, 0.0);
    CHECK_NEAR(gr::NormalizeDeg(725.0f), 5.0, 1e-4);
}

GR_TEST(units_heading_north_is_source_yaw_90) {
    // RDR2 heading 0 faces north (+Y). In Source that is yaw 90.
    const gr::SrcAngle a = gr::RdrRotToSource({0.0f, 0.0f, 0.0f});
    CHECK_NEAR(a.pitch, 0.0, 0.0);
    CHECK_NEAR(a.yaw, 90.0, 0.0);
    CHECK_NEAR(a.roll, 0.0, 0.0);

    // Looking up 30 degrees in RDR2 is pitch -30 in Source.
    CHECK_NEAR(gr::RdrRotToSource({30.0f, 0.0f, 0.0f}).pitch, -30.0, 0.0);
}

GR_TEST(units_rotation_matches_both_conventions) {
    const GrVec3 rots[] = {{0, 0, 0},      {0, 0, 90},      {0, 0, -135},  {30, 0, 0},
                           {-45, 0, 60},   {0, 25, 0},      {10, 20, 30},  {-80, -170, 179},
                           {89, 45, -90},  {15, -30, 270}};
    for (const GrVec3& rot : rots) {
        const gr::SrcAngle a = gr::RdrRotToSource(rot);
        V forward, right, up;
        SourceAngleVectors(a, forward, right, up);
        // The same physical model must point the same way in the world under both.
        CheckSameDirection(forward, RdrRotate(rot, {0, 1, 0}));
        CheckSameDirection(right, RdrRotate(rot, {1, 0, 0}));
        CheckSameDirection(up, RdrRotate(rot, {0, 0, 1}));
    }
}

GR_TEST(units_rotation_round_trip) {
    const gr::SrcAngle angles[] = {{0, 0, 0}, {-89, 179, 0}, {45, -90, 10}, {12.5f, 33.25f, -7.75f}};
    for (const gr::SrcAngle& a : angles) {
        const gr::SrcAngle back = gr::RdrRotToSource(gr::SourceAngToRdr(a));
        CHECK_NEAR(back.pitch, a.pitch, 1e-4);
        CHECK_NEAR(back.yaw, a.yaw, 1e-4);
        CHECK_NEAR(back.roll, a.roll, 1e-4);
    }
}

GR_TEST(units_local_axes) {
    // RDR2 model forward (+Y) is Source model forward (+X).
    const gr::SrcVec f = gr::RdrLocalToSource({0.0f, 1.0f, 0.0f});
    CHECK_NEAR(f.x, gr::kUnitsPerMetre, 1e-3);
    CHECK_NEAR(f.y, 0.0, 1e-6);
    // RDR2 model right (+X) is Source model right (-Y).
    const gr::SrcVec r = gr::RdrLocalToSource({1.0f, 0.0f, 0.0f});
    CHECK_NEAR(r.x, 0.0, 1e-6);
    CHECK_NEAR(r.y, -gr::kUnitsPerMetre, 1e-3);

    const GrVec3 back = gr::SourceLocalToRdr(gr::RdrLocalToSource({0.3f, -1.2f, 0.9f}));
    CHECK_NEAR(back.x, 0.3, 1e-5);
    CHECK_NEAR(back.y, -1.2, 1e-5);
    CHECK_NEAR(back.z, 0.9, 1e-5);
}

GR_TEST(units_bounds_stay_ordered) {
    // A horse-ish box: 0.6 m wide, 2.4 m long, 1.8 m tall, off-centre on every axis.
    gr::SrcVec smin, smax;
    gr::RdrBoundsToSource({-0.25f, -1.0f, -0.9f}, {0.35f, 1.4f, 0.9f}, smin, smax);
    CHECK(smin.x < smax.x);
    CHECK(smin.y < smax.y);
    CHECK(smin.z < smax.z);
    // Length runs along Source X, width along Source Y.
    CHECK_NEAR(smax.x - smin.x, 2.4 * gr::kUnitsPerMetre, 1e-2);
    CHECK_NEAR(smax.y - smin.y, 0.6 * gr::kUnitsPerMetre, 1e-2);
    // RDR2 right (+X, 0.35) is Source -Y.
    CHECK_NEAR(smin.y, -0.35 * gr::kUnitsPerMetre, 1e-2);
}

GR_TEST(units_fov_source_and_vertical_round_trip) {
    // Source's fov is the horizontal angle of a 4:3 picture: 90 is 73.74 degrees vertical.
    CHECK_NEAR(gr::SourceFovToVertical(90.0f), 73.7397953, 1e-4);
    CHECK_NEAR(gr::VerticalFovToSource(73.7397953f), 90.0, 1e-4);
    for (float fov = 20.0f; fov <= 120.0f; fov += 5.0f) {
        CHECK_NEAR(gr::VerticalFovToSource(gr::SourceFovToVertical(fov)), fov, 1e-3);
    }
}

GR_TEST(units_rdr_forward_matches_the_rotation) {
    // Yaw 0 faces +Y (north), yaw 90 faces -X (west), positive pitch faces up. Verified in
    // the real game against GET_ENTITY_FORWARD_VECTOR (dot 1.0000, docs/DESIGN.md).
    GrVec3 f = gr::RdrForward({0.0f, 0.0f, 0.0f});
    CHECK_NEAR(f.x, 0.0, 1e-6);
    CHECK_NEAR(f.y, 1.0, 1e-6);
    f = gr::RdrForward({0.0f, 0.0f, 90.0f});
    CHECK_NEAR(f.x, -1.0, 1e-6);
    CHECK_NEAR(f.y, 0.0, 1e-6);
    f = gr::RdrForward({30.0f, 0.0f, 0.0f});
    CHECK_NEAR(f.y, 0.8660254, 1e-6);
    CHECK_NEAR(f.z, 0.5, 1e-6);
}
