// The light at the player, worked out for GMod: GMod draws the viewmodel itself and would
// otherwise light it with gm_flatgrass's daylight (seen: a gun glowing as at noon in RDR2's
// night). RDR2 has no native for the sun's direction, so it comes from the clock; a ray
// towards the sun (or moon) says whether the player stands in shade, and being in an
// interior dims the rest. Real shadows (a fence's shadow across the gun) are not possible
// this way: only RDR2's renderer knows where they fall.
//
// Script thread only (natives). The ray runs every few frames; colours are smoothed so
// stepping into shade fades instead of popping.

#pragma once

#include <cmath>

#include "gr_protocol.h"
#include "natives.h"

namespace gr {

class LightSync {
public:
    void Reset() noexcept {
        frame_ = 0;
        have_ = false;
    }

    // `eye` is where the GMod player's eyes are in RDR2. Fills the light fields of `host`.
    void Tick(native::Entity ped, const GrVec3& eye, float dt, GrHostFrame& host) noexcept {
        if (ped == 0) {
            host.light_flags = 0;
            return;
        }
        const int h = native::GetClockHours();
        const int m = native::GetClockMinutes();
        const int s = native::GetClockSeconds();
        const float hours = static_cast<float>(h) + static_cast<float>(m) / 60.0f + static_cast<float>(s) / 3600.0f;

        // The sun rises in the east (+X) at 6, crosses the southern sky (-Y) and sets in the
        // west at 18; the moon takes the opposite arc. An approximation, not RDR2's own sky:
        // good enough for which side of the gun is lit.
        const float theta = (hours - 6.0f) / 12.0f * kPi;
        const float sun_up = std::sin(theta);
        const bool day = sun_up > -0.05f;
        const float arc = day ? theta : theta + kPi;
        GrVec3 dir{std::cos(arc), -0.45f, std::sin(arc) * 1.1f};
        if (dir.z < 0.15f) dir.z = 0.15f;  // a light below the horizon would light from under
        Normalise(dir);

        // Day factor: 0 at night, 1 from a little after sunrise; twilight in between.
        const float daylight = Clamp01((sun_up + 0.1f) / 0.35f);
        const float low = 1.0f - Clamp01(sun_up / 0.5f);  // 1 at the horizon: warmer light

        if (frame_++ % kRayEvery == 0 || !have_) {
            native::RayHit hit{};
            const GrVec3 to{eye.x + dir.x * kRayLength, eye.y + dir.y * kRayLength, eye.z + dir.z * kRayLength};
            shaded_ = native::Raycast(eye, to,
                                      native::kRayMap | native::kRayVehicles | native::kRayObjects | native::kRayFoliage,
                                      ped, hit);
            interior_ = native::GetInteriorFromEntity(ped) != 0;
        }

        // Linear colours. The direct light is the sun, or the moon at night, and nothing in
        // shade or indoors; the ambient is the sky, dimmer in shade and indoors.
        // Not yet seen in daylight (only at night): the dawn, noon and sunset values are a
        // first guess, to be checked against RDR2's own look. Noon: near-white sun. Low sun
        // (dawn, sunset): orange and weaker, as the light crosses more air.
        GrVec3 sun{1.0f, 0.93f - 0.35f * low, 0.82f - 0.52f * low};
        Scale(sun, 1.7f * daylight * (0.55f + 0.45f * Clamp01(sun_up / 0.6f)));
        GrVec3 moon{0.10f, 0.13f, 0.22f};
        Scale(moon, 1.0f - daylight);
        GrVec3 direct{sun.x + moon.x, sun.y + moon.y, sun.z + moon.z};
        if (shaded_ || interior_) Scale(direct, 0.0f);

        GrVec3 ambient{Lerp(0.025f, 0.34f, daylight), Lerp(0.03f, 0.36f, daylight), Lerp(0.055f, 0.42f, daylight)};
        // A low sun turns the sky's light warm too (guessed, as above).
        const float warm = low * daylight * 0.35f;
        ambient = {Lerp(ambient.x, 0.32f, warm), Lerp(ambient.y, 0.22f, warm), Lerp(ambient.z, 0.18f, warm)};
        if (shaded_) Scale(ambient, 0.75f);
        if (interior_) Scale(ambient, 0.6f);

        // About a third of a second to settle.
        const float k = have_ ? Clamp01(dt * 3.0f) : 1.0f;
        Approach(direct_, direct, k);
        Approach(ambient_, ambient, k);
        Approach(dir_, dir, k);
        Normalise(dir_);
        have_ = true;

        host.light_dir = dir_;
        host.light_color = direct_;
        host.ambient_color = ambient_;
        host.light_flags = GR_LIGHTF_VALID | (shaded_ ? GR_LIGHTF_SHADED : 0) | (interior_ ? GR_LIGHTF_INTERIOR : 0) |
                           (day ? 0 : GR_LIGHTF_NIGHT);
    }

private:
    static constexpr float kPi = 3.14159265f;
    static constexpr float kRayLength = 200.0f;
    static constexpr uint32_t kRayEvery = 6;

    static float Clamp01(float v) noexcept { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }
    static float Lerp(float a, float b, float t) noexcept { return a + (b - a) * t; }
    static void Scale(GrVec3& v, float s) noexcept {
        v.x *= s;
        v.y *= s;
        v.z *= s;
    }
    static void Normalise(GrVec3& v) noexcept {
        const float n = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
        if (n > 1e-6f) Scale(v, 1.0f / n);
    }
    static void Approach(GrVec3& v, const GrVec3& to, float k) noexcept {
        v.x += (to.x - v.x) * k;
        v.y += (to.y - v.y) * k;
        v.z += (to.z - v.z) * k;
    }

    uint32_t frame_ = 0;
    bool have_ = false;
    bool shaded_ = false;
    bool interior_ = false;
    GrVec3 dir_{0.0f, 0.0f, 1.0f};
    GrVec3 direct_{};
    GrVec3 ambient_{};
};

}  // namespace gr
