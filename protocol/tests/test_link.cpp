#include <memory>
#include <string>

#include "gr_link.h"
#include "gr_test.h"

namespace {

// Every case gets its own mapping name so the tests never touch a real running bridge
// or each other.
std::wstring UniqueName() {
    static int counter = 0;
    return L"Local\\GRTest_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(++counter);
}

// A hand-driven peer for the things a real Link refuses to do: claim another protocol
// version, or create the mapping at another size.
class RawPeer {
public:
    RawPeer(const std::wstring& name, gr::Role role, uint32_t mapping_size) : role_(role) {
        mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, mapping_size,
                                      name.c_str());
        view_ = static_cast<GrHeader*>(MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, GR_HEADER_SIZE));
    }
    ~RawPeer() { Release(); }

    void Announce(uint32_t version, uint32_t shm_size) {
        GrPeer* me = Mine();
        me->protocol_version = version;
        me->shm_size = shm_size;
        me->pid = GetCurrentProcessId();
        me->epoch = 0x1234;
        me->state = GR_PEER_ALIVE;
        me->reason = GR_REASON_NONE;
        me->magic = GR_MAGIC;
    }
    void Beat() { ++Mine()->heartbeat; }
    void Release() {
        if (view_) UnmapViewOfFile(view_);
        if (mapping_) CloseHandle(mapping_);
        view_ = nullptr;
        mapping_ = nullptr;
    }
    bool ok() const { return view_ != nullptr; }
    GrPeer* Mine() { return role_ == gr::Role::Host ? &view_->host : &view_->guest; }
    const GrPeer& Theirs() const { return role_ == gr::Role::Host ? view_->guest : view_->host; }

private:
    gr::Role role_;
    HANDLE mapping_ = nullptr;
    GrHeader* view_ = nullptr;
};

// Ticks both sides for `frames` frames of 16 ms on a fake clock.
void Run(gr::Link& a, gr::Link& b, uint64_t& now, int frames) {
    for (int i = 0; i < frames; ++i) {
        now += 16;
        a.Tick(now);
        b.Tick(now);
    }
}

}  // namespace

GR_TEST(link_host_alone_waits) {
    const std::wstring name = UniqueName();
    gr::Link host;
    uint64_t now = 1000;
    CHECK(host.state() == gr::LinkState::Closed);
    CHECK(host.Open(gr::Role::Host, now, name.c_str()));
    CHECK(host.created());
    for (int i = 0; i < 10; ++i) CHECK(host.Tick(now += 16) == gr::LinkState::Waiting);
}

GR_TEST(link_connects_in_either_start_order) {
    for (int order = 0; order < 2; ++order) {
        const std::wstring name = UniqueName();
        gr::Link host, guest;
        uint64_t now = 1000;
        if (order == 0) {
            CHECK(host.Open(gr::Role::Host, now, name.c_str()));
            Run(host, host, now, 5);
            CHECK(guest.Open(gr::Role::Guest, now, name.c_str()));
            CHECK(!guest.created());
        } else {
            CHECK(guest.Open(gr::Role::Guest, now, name.c_str()));
            Run(guest, guest, now, 5);
            CHECK(host.Open(gr::Role::Host, now, name.c_str()));
            CHECK(!host.created());
        }
        Run(host, guest, now, 3);
        CHECK(host.state() == gr::LinkState::Connected);
        CHECK(guest.state() == gr::LinkState::Connected);
        CHECK(host.peer().protocol_version == GR_PROTOCOL_VERSION);
        CHECK(host.peer().pid == GetCurrentProcessId());
    }
}

GR_TEST(link_silent_peer_times_out_and_comes_back) {
    const std::wstring name = UniqueName();
    gr::Link host, guest;
    uint64_t now = 1000;
    CHECK(host.Open(gr::Role::Host, now, name.c_str()));
    CHECK(guest.Open(gr::Role::Guest, now, name.c_str()));
    Run(host, guest, now, 3);
    CHECK(host.connected());

    // The guest hangs: its block is still there, its heartbeat is not moving. The host
    // first takes in the guest's last beat, and the timeout runs from there.
    CHECK(host.Tick(now) == gr::LinkState::Connected);
    now += GR_PEER_TIMEOUT_MS - 100;
    CHECK(host.Tick(now) == gr::LinkState::Connected);
    now += 200;
    CHECK(host.Tick(now) == gr::LinkState::Waiting);

    Run(host, guest, now, 2);
    CHECK(host.connected());
    CHECK(guest.connected());
}

GR_TEST(link_block_left_by_a_dead_peer_never_counts) {
    const std::wstring name = UniqueName();
    uint64_t now = 1000;

    // Something else keeps the mapping alive (memdump, say) while a guest crashes
    // without marking itself GONE. A host that starts afterwards finds the corpse.
    RawPeer keeper(name, gr::Role::Host, GR_SHM_SIZE);
    {
        RawPeer dead(name, gr::Role::Guest, GR_SHM_SIZE);
        CHECK(dead.ok());
        dead.Announce(GR_PROTOCOL_VERSION, GR_SHM_SIZE);
        dead.Beat();
    }

    gr::Link host;
    CHECK(host.Open(gr::Role::Host, now, name.c_str()));
    CHECK(!host.created());
    for (int i = 0; i < 300; ++i) CHECK(host.Tick(now += 16) == gr::LinkState::Waiting);
}

GR_TEST(link_clean_close_is_seen_at_once) {
    const std::wstring name = UniqueName();
    gr::Link host, guest;
    uint64_t now = 1000;
    CHECK(host.Open(gr::Role::Host, now, name.c_str()));
    CHECK(guest.Open(gr::Role::Guest, now, name.c_str()));
    Run(host, guest, now, 3);
    CHECK(host.connected());

    guest.Close();
    CHECK(guest.state() == gr::LinkState::Closed);
    CHECK(host.Tick(now += 16) == gr::LinkState::Waiting);
    CHECK(host.peer().state == GR_PEER_GONE);
}

GR_TEST(link_peer_restart_is_a_new_generation) {
    const std::wstring name = UniqueName();
    gr::Link host;
    auto guest = std::make_unique<gr::Link>();
    uint64_t now = 1000;
    CHECK(host.Open(gr::Role::Host, now, name.c_str()));
    CHECK(guest->Open(gr::Role::Guest, now, name.c_str()));
    Run(host, *guest, now, 3);
    CHECK(host.connected());
    const uint32_t generation = host.peer_generation();

    auto frame = std::make_unique<GrGuestFrame>();
    frame->frame = 42;
    frame->driven_count = 3;
    guest->Publish(*frame);

    guest.reset();
    host.Tick(now += 16);
    guest = std::make_unique<gr::Link>();
    CHECK(guest->Open(gr::Role::Guest, now, name.c_str()));
    Run(host, *guest, now, 3);
    CHECK(host.connected());
    CHECK(host.peer_generation() == generation + 1);

    // What the old guest published must not survive into the new session.
    auto seen = std::make_unique<GrGuestFrame>();
    CHECK(host.Read(*seen));
    CHECK(seen->frame == 0);
    CHECK(seen->driven_count == 0);
}

GR_TEST(link_version_mismatch_is_refused_by_both) {
    const std::wstring name = UniqueName();
    gr::Link host;
    uint64_t now = 1000;
    CHECK(host.Open(gr::Role::Host, now, name.c_str()));

    RawPeer old_guest(name, gr::Role::Guest, GR_SHM_SIZE);
    CHECK(old_guest.ok());
    old_guest.Announce(GR_PROTOCOL_VERSION + 1, GR_SHM_SIZE);
    for (int i = 0; i < 5; ++i) {
        old_guest.Beat();
        host.Tick(now += 16);
    }
    CHECK(host.state() == gr::LinkState::VersionMismatch);
    CHECK(host.peer().protocol_version == GR_PROTOCOL_VERSION + 1);
    // The host says so in its own block, where the peer and memdump can read it.
    CHECK(old_guest.Theirs().state == GR_PEER_REFUSED);
    CHECK(old_guest.Theirs().reason == GR_REASON_VERSION);

    auto frame = std::make_unique<GrGuestFrame>();
    CHECK(!host.Read(*frame));

    // The mismatched guest goes away: the host goes back to waiting, not refusing.
    old_guest.Mine()->state = GR_PEER_GONE;
    CHECK(host.Tick(now += 16) == gr::LinkState::Waiting);
    CHECK(old_guest.Theirs().state == GR_PEER_ALIVE);
    CHECK(old_guest.Theirs().reason == GR_REASON_NONE);
}

GR_TEST(link_same_version_different_size_is_a_mismatch) {
    const std::wstring name = UniqueName();
    gr::Link host;
    uint64_t now = 1000;
    CHECK(host.Open(gr::Role::Host, now, name.c_str()));
    RawPeer guest(name, gr::Role::Guest, GR_SHM_SIZE);
    guest.Announce(GR_PROTOCOL_VERSION, GR_SHM_SIZE + 4);  // forgot to bump the version
    for (int i = 0; i < 5; ++i) {
        guest.Beat();
        host.Tick(now += 16);
    }
    CHECK(host.state() == gr::LinkState::VersionMismatch);
}

GR_TEST(link_smaller_mapping_from_an_old_build_is_replaced_once_it_is_gone) {
    const std::wstring name = UniqueName();
    uint64_t now = 1000;

    // An old build with a smaller layout got there first and created the mapping.
    auto old_guest = std::make_unique<RawPeer>(name, gr::Role::Guest, 4096);
    CHECK(old_guest->ok());
    old_guest->Announce(GR_PROTOCOL_VERSION - 1, 4096);

    gr::Link host;
    CHECK(host.Open(gr::Role::Host, now, name.c_str()));  // only the header fits
    CHECK(!host.created());
    for (int i = 0; i < 5; ++i) {
        old_guest->Beat();
        host.Tick(now += 16);
    }
    CHECK(host.state() == gr::LinkState::VersionMismatch);
    CHECK(old_guest->Theirs().reason == GR_REASON_VERSION);

    // The old build exits. The host must let go of the undersized mapping so Windows
    // destroys it, then recreate it at the right size.
    old_guest.reset();
    for (int i = 0; i < 400; ++i) host.Tick(now += 16);
    CHECK(host.state() == gr::LinkState::Waiting);
    CHECK(host.created());

    gr::Link guest;
    CHECK(guest.Open(gr::Role::Guest, now, name.c_str()));
    Run(host, guest, now, 3);
    CHECK(host.connected());
    CHECK(guest.connected());
}

GR_TEST(link_refuse_is_seen_by_the_peer_and_stops_frames) {
    const std::wstring name = UniqueName();
    gr::Link host, guest;
    uint64_t now = 1000;
    CHECK(host.Open(gr::Role::Host, now, name.c_str()));
    CHECK(guest.Open(gr::Role::Guest, now, name.c_str()));
    Run(host, guest, now, 3);
    CHECK(guest.connected());

    auto frame = std::make_unique<GrHostFrame>();
    frame->frame = 5;
    host.Publish(*frame);

    host.Refuse(GR_REASON_MULTIPLAYER);
    Run(host, guest, now, 2);
    CHECK(host.state() == gr::LinkState::Refusing);
    CHECK(guest.state() == gr::LinkState::PeerRefused);
    CHECK(guest.peer().reason == GR_REASON_MULTIPLAYER);

    // Nothing more is published or read on either side.
    frame->frame = 6;
    host.Publish(*frame);
    auto seen = std::make_unique<GrHostFrame>();
    CHECK(!guest.Read(*seen));
    auto guest_frame = std::make_unique<GrGuestFrame>();
    CHECK(!host.Read(*guest_frame));

    // Refusing is latched even if the link is reopened.
    host.Close();
    CHECK(host.Open(gr::Role::Host, now, name.c_str()));
    Run(host, guest, now, 3);
    CHECK(host.state() == gr::LinkState::Refusing);
    CHECK(guest.state() == gr::LinkState::PeerRefused);
}

GR_TEST(link_frames_flow_both_ways_only_when_connected) {
    const std::wstring name = UniqueName();
    gr::Link host, guest;
    uint64_t now = 1000;
    auto host_frame = std::make_unique<GrHostFrame>();
    auto guest_frame = std::make_unique<GrGuestFrame>();
    auto host_seen = std::make_unique<GrHostFrame>();
    auto guest_seen = std::make_unique<GrGuestFrame>();

    CHECK(host.Open(gr::Role::Host, now, name.c_str()));
    host.Tick(now += 16);
    CHECK(!host.Read(*guest_seen));  // nobody there

    // Published before the guest exists, so it is waiting for the guest on arrival.
    host_frame->frame = 100;
    host_frame->entity_count = 1;
    host_frame->entities[0].handle = 77;
    host_frame->entities[0].pos = {1.0f, 2.0f, 3.0f};
    host.Publish(*host_frame);

    CHECK(guest.Open(gr::Role::Guest, now, name.c_str()));
    Run(host, guest, now, 3);
    CHECK(guest.Read(*host_seen));
    CHECK(host_seen->frame == 100);
    CHECK(host_seen->entities[0].handle == 77);
    CHECK(host_seen->entities[0].pos.y == 2.0f);

    guest_frame->frame = 9;
    guest_frame->host_frame_seen = host_seen->frame;
    guest_frame->driven_count = 1;
    guest_frame->driven[0].handle = 77;
    guest_frame->driven[0].flags = GR_DRIVE_HELD;
    guest.Publish(*guest_frame);
    CHECK(host.Read(*guest_seen));
    CHECK(guest_seen->frame == 9);
    CHECK(guest_seen->host_frame_seen == 100);
    CHECK(guest_seen->driven[0].handle == 77);

    // A side can only write its own block and read the other's.
    guest.Publish(*host_frame);
    CHECK(!guest.Read(*guest_seen));
    CHECK(host.torn_reads() == 0);
}
