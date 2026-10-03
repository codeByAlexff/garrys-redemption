// gmod/module/src/guest.h against a real host Link: the anchor and its serial, key events
// and mouse deltas from RDR2's input block, and what a torn read leaves behind.

#include <memory>
#include <string>

#include "gr_test.h"
#include "guest.h"

namespace {

std::wstring UniqueName() {
    static int counter = 0;
    return L"Local\\GRGuestTest_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(++counter);
}

// A host Link and a Guest on one mapping, ticked together on a fake clock.
struct Pair {
    gr::Link host;
    gr::Guest guest;
    uint64_t now = 1000;
    std::unique_ptr<GrHostFrame> frame = std::make_unique<GrHostFrame>();
    std::unique_ptr<GrGuestFrame> seen = std::make_unique<GrGuestFrame>();

    explicit Pair(const std::wstring& name) {
        CHECK(host.Open(gr::Role::Host, now, name.c_str()));
        CHECK(guest.Open(now, name.c_str()));
        frame->ped_pos = {1500.0f, -2200.0f, 60.0f};
        frame->ped_rot = {0.0f, 0.0f, 30.0f};
        frame->anchor_serial = 7;
        frame->input.flags = GR_INPUT_CAPTURED;
    }

    // One frame: the host publishes `frame`, the guest reads it and publishes its own.
    void Step(int frames = 1) {
        for (int i = 0; i < frames; ++i) {
            now += 16;
            host.Tick(now);
            ++frame->frame;
            host.Publish(*frame);
            guest.Tick(now);
            guest.Publish(static_cast<uint32_t>(now * 1000));
        }
    }

    void SetKey(int vk, bool down) {
        const uint8_t mask = static_cast<uint8_t>(1u << (vk % 8));
        if (down) {
            frame->input.keys[vk / 8] |= mask;
        } else {
            frame->input.keys[vk / 8] &= static_cast<uint8_t>(~mask);
        }
    }
};

}  // namespace

GR_TEST(guest_anchors_the_player_to_the_ped_feet) {
    Pair p(UniqueName());
    gr::SrcAngle angles{};
    CHECK(!p.guest.Anchor({0, 0, 0}, angles));  // nothing from the host yet

    p.Step(3);
    CHECK(p.guest.can_anchor());
    // The GMod player stands somewhere on the flat map. After anchoring, that spot is the
    // ped's feet, and nothing in GMod moved.
    const gr::SrcVec player{512.0f, -256.0f, -12800.0f};
    CHECK(p.guest.Anchor(player, angles));
    CHECK(p.guest.anchored());
    CHECK_NEAR(angles.yaw, 120.0f, 1e-4);  // RDR2 yaw 30 is Source yaw 120
    CHECK_NEAR(angles.pitch, 0.0f, 1e-4);

    p.guest.SetPlayer(player, {0, 0, 0}, {player.x, player.y, player.z + 64.0f}, angles, 90.0f, GR_GUESTF_ON_GROUND);
    p.Step();
    CHECK(p.host.Read(*p.seen));
    CHECK((p.seen->flags & GR_GUESTF_PLAYER_VALID) != 0);
    CHECK((p.seen->flags & GR_GUESTF_ON_GROUND) != 0);
    CHECK(p.seen->anchor_serial == 7);
    CHECK_NEAR(p.seen->pos.x, 1500.0f, 1e-3);
    CHECK_NEAR(p.seen->pos.y, -2200.0f, 1e-3);
    CHECK_NEAR(p.seen->pos.z, 60.0f, 1e-3);
    CHECK_NEAR(p.seen->eye_pos.z, 60.0f + 64.0f * 0.01905f, 1e-3);
    CHECK_NEAR(p.seen->eye_rot.z, 30.0f, 1e-3);
    CHECK_NEAR(p.seen->fov, 73.7397953f, 1e-3);  // Source 90 on 4:3 is 73.74 vertical

    // Walking 100 units along +X in GMod is 1.905 m along +X in RDR2.
    p.guest.SetPlayer({player.x + 100.0f, player.y, player.z}, {0, 0, 0}, player, angles, 90.0f, 0);
    p.Step();
    CHECK(p.host.Read(*p.seen));
    CHECK_NEAR(p.seen->pos.x, 1501.905f, 1e-3);
}

GR_TEST(guest_drops_the_anchor_when_rdr2_takes_the_player_or_the_serial_changes) {
    Pair p(UniqueName());
    gr::SrcAngle angles{};
    p.Step(3);
    CHECK(p.guest.Anchor({0, 0, 0}, angles));

    // RDR2 owns the player (a scene, a horse, F9): the anchor goes and so does PLAYER_VALID.
    p.frame->flags = GR_HOSTF_RDR2_OWNS_PLAYER;
    p.Step();
    CHECK(!p.guest.anchored());
    CHECK(!p.guest.can_anchor());
    CHECK(p.host.Read(*p.seen));
    CHECK((p.seen->flags & GR_GUESTF_PLAYER_VALID) == 0);
    CHECK(p.seen->anchor_serial == 0);

    // RDR2 hands it back under a new serial: anchoring again takes the new one.
    p.frame->flags = 0;
    p.frame->anchor_serial = 8;
    p.Step();
    CHECK(p.guest.Anchor({0, 0, 0}, angles));
    p.Step();
    CHECK(p.host.Read(*p.seen));
    CHECK(p.seen->anchor_serial == 8);

    // A serial change without the flag in between (a frame was missed) drops it too.
    p.frame->anchor_serial = 9;
    p.Step();
    CHECK(!p.guest.anchored());

    // A host that never names a serial cannot be anchored to.
    p.frame->anchor_serial = 0;
    p.Step();
    CHECK(!p.guest.can_anchor());
}

GR_TEST(guest_reports_key_changes_once_and_releases_everything_when_capture_ends) {
    Pair p(UniqueName());
    gr::SrcAngle angles{};
    int vk = 0;
    bool down = false;

    p.SetKey(0x57, true);  // W held before anchoring: not ours yet
    p.Step(3);
    CHECK(!p.guest.captured());
    CHECK(!p.guest.NextKey(vk, down));

    CHECK(p.guest.Anchor({0, 0, 0}, angles));
    p.Step();
    CHECK(p.guest.captured());
    CHECK(p.guest.NextKey(vk, down));
    CHECK(vk == 0x57 && down);
    CHECK(!p.guest.NextKey(vk, down));  // told once

    p.SetKey(0x20, true);
    p.SetKey(0x01, true);
    p.Step();
    CHECK(p.guest.NextKey(vk, down));
    CHECK(vk == 0x01 && down);
    CHECK(p.guest.NextKey(vk, down));
    CHECK(vk == 0x20 && down);
    CHECK(!p.guest.NextKey(vk, down));

    // RDR2 stops capturing (its window lost focus): every held key comes up.
    p.frame->input.flags = 0;
    p.Step();
    CHECK(!p.guest.captured());
    int released = 0;
    while (p.guest.NextKey(vk, down)) {
        CHECK(!down);
        ++released;
    }
    CHECK(released == 3);
}

GR_TEST(guest_mouse_deltas_start_at_capture_and_survive_the_totals_wrapping) {
    Pair p(UniqueName());
    gr::SrcAngle angles{};
    int dx = 0, dy = 0, wheel = 0;

    // The totals ran a long way before GMod had the player: none of that is a delta.
    p.frame->input.mouse_dx = 2147483000;
    p.frame->input.mouse_dy = -500;
    p.frame->input.wheel = 360;
    p.Step(3);
    CHECK(p.guest.Anchor({0, 0, 0}, angles));
    p.Step();
    p.guest.TakeMouse(dx, dy, wheel);
    CHECK(dx == 0 && dy == 0 && wheel == 0);

    // Past INT32_MAX: the total wraps negative, the delta is still +1000.
    p.frame->input.mouse_dx = static_cast<int32_t>(2147483000u + 1000u);
    p.frame->input.mouse_dy = -520;
    p.frame->input.wheel = 240;
    p.Step();
    p.guest.TakeMouse(dx, dy, wheel);
    CHECK(dx == 1000);
    CHECK(dy == -20);
    CHECK(wheel == -120);
    p.guest.TakeMouse(dx, dy, wheel);
    CHECK(dx == 0 && dy == 0 && wheel == 0);  // taken once

    // Not captured: no deltas, and movement meanwhile is not saved up for later.
    p.frame->input.flags = 0;
    p.frame->input.mouse_dx += 5000;
    p.Step();
    p.guest.TakeMouse(dx, dy, wheel);
    CHECK(dx == 0);
    p.frame->input.flags = GR_INPUT_CAPTURED;
    p.Step();
    p.guest.TakeMouse(dx, dy, wheel);
    CHECK(dx == 0);
}

GR_TEST(link_torn_read_leaves_the_callers_frame_alone) {
    const std::wstring name = UniqueName();
    gr::Link host, guest;
    uint64_t now = 1000;
    CHECK(host.Open(gr::Role::Host, now, name.c_str()));
    CHECK(guest.Open(gr::Role::Guest, now, name.c_str()));
    for (int i = 0; i < 3; ++i) {
        now += 16;
        host.Tick(now);
        guest.Tick(now);
    }
    auto frame = std::make_unique<GrHostFrame>();
    auto seen = std::make_unique<GrHostFrame>();
    frame->frame = 5;
    frame->ped_pos = {1.0f, 2.0f, 3.0f};
    host.Publish(*frame);
    CHECK(guest.Read(*seen));
    CHECK(seen->frame == 5);

    // A writer stuck halfway: odd sequence, half-written fields. Every retry fails.
    HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name.c_str());
    CHECK(mapping != nullptr);
    auto* shared = static_cast<GrShared*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(GrShared)));
    CHECK(shared != nullptr);
    shared->host.seq |= 1u;
    shared->host.frame = 6;
    shared->host.ped_pos.x = 99.0f;
    CHECK(!guest.Read(*seen));
    CHECK(guest.torn_reads() == 1);
    CHECK(seen->frame == 5);
    CHECK(seen->ped_pos.x == 1.0f);
    UnmapViewOfFile(shared);
    CloseHandle(mapping);
}

GR_TEST(guest_rebase_keeps_rdr2_positions_and_fixes_a_stale_player) {
    Pair p(UniqueName());
    p.Step(3);
    gr::SrcAngle angles{};
    CHECK(p.guest.Anchor({0, 0, 0}, angles));
    const uint32_t serial = p.guest.origin_serial();

    // The player walked 8100 units east and 300 down; the server rebases by that much.
    const gr::SrcVec before{8100.0f, 0.0f, -300.0f};
    p.guest.SetPlayer(before, {0, 0, 0}, {before.x, before.y, before.z + 64.0f}, angles, 90.0f, 0);
    p.Step();
    CHECK(p.host.Read(*p.seen));
    const GrVec3 rdr_before = p.seen->pos;

    p.guest.Rebase(before, before);
    CHECK(p.guest.origin_serial() != serial);

    // The client still reports the old position for a frame or two: same RDR2 position.
    p.guest.SetPlayer(before, {0, 0, 0}, {before.x, before.y, before.z + 64.0f}, angles, 90.0f, 0);
    p.Step();
    CHECK(p.host.Read(*p.seen));
    CHECK_NEAR(p.seen->pos.x, rdr_before.x, 1e-3);
    CHECK_NEAR(p.seen->pos.z, rdr_before.z, 1e-3);

    // Then the moved one, a little further on: also right.
    p.guest.SetPlayer({10.0f, 0.0f, 0.0f}, {0, 0, 0}, {10.0f, 0.0f, 64.0f}, angles, 90.0f, 0);
    p.Step();
    CHECK(p.host.Read(*p.seen));
    CHECK_NEAR(p.seen->pos.x, rdr_before.x + 10.0f * 0.01905f, 1e-3);
    CHECK_NEAR(p.seen->pos.z, rdr_before.z, 1e-3);
}

GR_TEST(guest_reads_terrain_chunks_and_places_them_through_the_origin) {
    Pair p(UniqueName());
    p.Step(3);
    gr::SrcAngle angles{};
    CHECK(p.guest.Anchor({0, 0, 0}, angles));  // origin = ped feet (1500, -2200, 60)

    auto chunk = std::make_unique<GrChunk>();
    chunk->serial = 42;
    chunk->cx = 93;   // 93 * 16 = 1488 m
    chunk->cy = -138; // -138 * 16 = -2208 m
    chunk->base_z = 58.0f;
    for (float& h : chunk->heights) h = 58.0f;
    CHECK(p.guest.ChunkSerial(5) == 0);
    p.host.PublishChunk(5, *chunk);
    CHECK(p.guest.ChunkSerial(5) == 42);
    CHECK(p.guest.ReadChunk(5));
    CHECK(p.guest.chunk().cx == 93);

    const gr::SrcVec at = p.guest.ChunkPos(93, -138, 58.0f);
    CHECK_NEAR(at.x, (1488.0 - 1500.0) / 0.01905, 0.05);
    CHECK_NEAR(at.y, (-2208.0 + 2200.0) / 0.01905, 0.05);
    CHECK_NEAR(at.z, (58.0 - 60.0) / 0.01905, 0.05);

    int32_t cx = 0, cy = 0;
    p.guest.ChunkOf({0.0f, 0.0f, 0.0f}, cx, cy);  // the ped stands at (1500, -2200)
    CHECK(cx == 93 && cy == -138);
}

GR_TEST(joaat_matches_the_games_model_hashes) {
    // From femga/rdr3_discoveries peds/peds_list.lua and vehicles/vehicles_list.lua.
    CHECK(gr::Joaat("a_c_cow") == 0xFCFA9E1Eu);
    CHECK(gr::Joaat("mp_male") == 0xF5C1611Eu);
    CHECK(gr::Joaat("ArmySupplyWagon") == 0x276DFE5Eu);  // names are hashed in lower case
}

GR_TEST(guest_spawn_and_remove_events_go_through_the_ring_and_results_come_back) {
    Pair p(UniqueName());
    p.Step(3);
    CHECK(!p.guest.AddSpawn(1, 0x1234, {0, 0, 0}, {1, 0, 0}));  // not anchored yet
    gr::SrcAngle angles{};
    CHECK(p.guest.Anchor({0, 0, 0}, angles));
    CHECK(p.guest.AddSpawn(7, 0xFCFA9E1Eu, {100.0f, 0.0f, 0.0f}, {0.0f, -2.0f, 0.0f}));
    CHECK(p.guest.AddRemove(0, 7, 0));
    CHECK(p.guest.AddClearWanted());
    p.Step();
    CHECK(p.host.Read(*p.seen));
    const GrGuestFrame& g = *p.seen;
    CHECK(g.event_count >= 3);
    const GrEvent& spawn = g.events[(g.event_count - 3) % GR_MAX_EVENTS];
    CHECK(spawn.type == GR_EVENT_SPAWN && spawn.id == 7 && spawn.model == 0xFCFA9E1Eu);
    CHECK_NEAR(spawn.pos.x, 1500.0 + 100.0 * 0.01905, 1e-3);
    CHECK_NEAR(spawn.dir.y, -1.0, 1e-6);
    const GrEvent& remove = g.events[(g.event_count - 2) % GR_MAX_EVENTS];
    CHECK(remove.type == GR_EVENT_REMOVE && remove.id == 7 && remove.handle == 0 && remove.flags == 0);
    CHECK(g.events[(g.event_count - 1) % GR_MAX_EVENTS].type == GR_EVENT_CLEAR_WANTED);

    p.frame->spawn_result_count = 1;
    p.frame->spawn_results[0] = {1, 7, 4242, 0xFCFA9E1Eu, GR_SPAWN_OK};
    p.Step();
    GrSpawnResult r{};
    CHECK(p.guest.spawn_result_count() == 1);
    CHECK(p.guest.SpawnResult(1, r) && r.id == 7 && r.handle == 4242 && r.status == GR_SPAWN_OK);
    CHECK(!p.guest.SpawnResult(2, r));
}
