// gr_probe: a console stand-in for either side of the bridge that runs the real C++ link
// code (protocol/gr_link.h). It carries no scene: it beats its heart, publishes a frame
// counter and reports what it sees. The Python tests use it to prove the Python fakes and
// the C++ code agree on the handshake and the layout.
//
//   gr_probe --role host|guest [--seconds N] [--name MAPPING] [--refuse-multiplayer]
//
// Prints one line per state change and a final summary line starting with "summary".

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include "gr_link.h"

int main(int argc, char** argv) {
    gr::Role role = gr::Role::Guest;
    bool role_given = false;
    double seconds = 5.0;
    bool refuse = false;
    std::wstring name = GR_SHM_NAME;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const char* value = i + 1 < argc ? argv[i + 1] : nullptr;
        if (arg == "--role" && value) {
            role = std::strcmp(value, "host") == 0 ? gr::Role::Host : gr::Role::Guest;
            role_given = true;
            ++i;
        } else if (arg == "--seconds" && value) {
            seconds = std::atof(value);
            ++i;
        } else if (arg == "--name" && value) {
            name.assign(value, value + std::strlen(value));
            ++i;
        } else if (arg == "--refuse-multiplayer") {
            refuse = true;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
            return 2;
        }
    }
    if (!role_given) {
        std::fprintf(stderr, "usage: gr_probe --role host|guest [--seconds N] [--name MAPPING] "
                             "[--refuse-multiplayer]\n");
        return 2;
    }

    const bool is_host = role == gr::Role::Host;
    gr::Link link;
    if (refuse) link.Refuse(GR_REASON_MULTIPLAYER);
    if (!link.Open(role, gr::NowMs(), name.c_str())) {
        std::printf("open failed, error %lu\n", link.last_error());
        return 1;
    }
    std::printf("probe role=%s protocol=%u shm_size=%u created=%d\n", is_host ? "host" : "guest",
                GR_PROTOCOL_VERSION, GR_SHM_SIZE, link.created() ? 1 : 0);

    auto host_frame = std::make_unique<GrHostFrame>();
    auto guest_frame = std::make_unique<GrGuestFrame>();
    gr::LinkState last = gr::LinkState::Closed;
    uint32_t frame = 0;
    uint32_t frames_read = 0;
    uint32_t last_peer_frame = 0;
    uint32_t peer_entities = 0;
    uint32_t peer_driven = 0;
    bool ever_connected = false;

    const uint64_t end = gr::NowMs() + static_cast<uint64_t>(seconds * 1000.0);
    while (gr::NowMs() < end) {
        ++frame;
        const gr::LinkState state = link.Tick(gr::NowMs());
        if (state != last) {
            std::printf("state %s -> %s (peer protocol=%u shm_size=%u state=%u reason=%s)\n",
                        gr::LinkStateName(last), gr::LinkStateName(state), link.peer().protocol_version,
                        link.peer().shm_size, link.peer().state, gr::ReasonName(link.peer().reason));
            std::fflush(stdout);
            last = state;
        }
        if (state == gr::LinkState::Connected) ever_connected = true;

        if (is_host) {
            if (link.Read(*guest_frame)) {
                ++frames_read;
                last_peer_frame = guest_frame->frame;
                peer_driven = guest_frame->driven_count;
            }
            host_frame->frame = frame;
            host_frame->time_us = gr::NowUs();
            host_frame->game_time_ms = static_cast<uint32_t>(gr::NowMs());
            host_frame->anchor_serial = 1;  // one stretch of the guest owning the player, never 0
            link.Publish(*host_frame);
        } else {
            if (link.Read(*host_frame)) {
                ++frames_read;
                last_peer_frame = host_frame->frame;
                peer_entities = host_frame->entity_count;
            }
            guest_frame->frame = frame;
            guest_frame->time_us = gr::NowUs();
            guest_frame->host_frame_seen = last_peer_frame;
            link.Publish(*guest_frame);
        }
        Sleep(16);
    }

    std::printf("summary state=%s ever_connected=%d frames_read=%u last_peer_frame=%u "
                "peer_entities=%u peer_driven=%u torn_reads=%u\n",
                gr::LinkStateName(link.state()), ever_connected ? 1 : 0, frames_read, last_peer_frame,
                peer_entities, peer_driven, link.torn_reads());
    link.Close();
    return 0;
}
