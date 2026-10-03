#include <atomic>
#include <memory>
#include <thread>

#include "gr_protocol.h"
#include "gr_seqlock.h"
#include "gr_test.h"

namespace {

// Every field of a frame is derived from one number, so a copy that mixes two writes is
// detectable from the copy alone.
void Fill(GrHostFrame& f, uint32_t n) {
    f.frame = n;
    f.game_time_ms = n * 3u;
    f.entity_count = GR_MAX_ENTITIES;
    for (uint32_t i = 0; i < GR_MAX_ENTITIES; ++i) {
        f.entities[i].handle = static_cast<int32_t>(n + i);
        f.entities[i].health = static_cast<float>(n % 1000u);
    }
}

bool Consistent(const GrHostFrame& f) {
    const uint32_t n = f.frame;
    if (f.game_time_ms != n * 3u) return false;
    for (uint32_t i = 0; i < GR_MAX_ENTITIES; ++i) {
        if (f.entities[i].handle != static_cast<int32_t>(n + i)) return false;
        if (f.entities[i].health != static_cast<float>(n % 1000u)) return false;
    }
    return true;
}

}  // namespace

GR_TEST(seqlock_write_then_read) {
    auto shared = std::make_unique<GrHostFrame>();
    auto local = std::make_unique<GrHostFrame>();
    auto out = std::make_unique<GrHostFrame>();

    Fill(*local, 7);
    gr::SeqWrite(shared.get(), *local);
    CHECK(shared->seq == 2);
    CHECK(gr::SeqRead(shared.get(), *out));
    CHECK(out->frame == 7);
    CHECK(out->seq == 2);
    CHECK(Consistent(*out));

    // The writer's local seq must never leak into the shared counter.
    local->seq = 0xDEAD;
    Fill(*local, 8);
    gr::SeqWrite(shared.get(), *local);
    CHECK(shared->seq == 4);
}

GR_TEST(seqlock_reader_gives_up_on_a_write_in_progress) {
    auto shared = std::make_unique<GrHostFrame>();
    auto out = std::make_unique<GrHostFrame>();
    shared->seq = 5;  // odd: a writer is in the middle of a write, or died there
    CHECK(!gr::SeqRead(shared.get(), *out));
}

GR_TEST(seqlock_writer_recovers_from_a_dead_writer) {
    auto shared = std::make_unique<GrHostFrame>();
    auto local = std::make_unique<GrHostFrame>();
    auto out = std::make_unique<GrHostFrame>();
    shared->seq = 5;
    Fill(*local, 11);
    gr::SeqWrite(shared.get(), *local);
    CHECK(shared->seq == 6);
    CHECK(gr::SeqRead(shared.get(), *out));
    CHECK(out->frame == 11);
}

GR_TEST(seqlock_no_torn_frame_is_ever_accepted) {
    auto shared = std::make_unique<GrHostFrame>();
    std::atomic<bool> stop{false};

    // Start from a frame that passes Consistent(): all zeros would not, and the reader
    // can get in before the writer's first write.
    {
        auto first = std::make_unique<GrHostFrame>();
        Fill(*first, 0);
        gr::SeqWrite(shared.get(), *first);
    }

    std::thread writer([&] {
        auto local = std::make_unique<GrHostFrame>();
        uint32_t n = 1;
        while (!stop.load()) {
            Fill(*local, n++);
            gr::SeqWrite(shared.get(), *local);
        }
    });

    auto out = std::make_unique<GrHostFrame>();
    int accepted = 0;
    int torn_accepted = 0;
    int rejected = 0;
    uint32_t last_frame = 0;
    bool went_backwards = false;
    for (int i = 0; i < 200000; ++i) {
        if (gr::SeqRead(shared.get(), *out, 2)) {
            ++accepted;
            if (!Consistent(*out)) ++torn_accepted;
            if (out->frame < last_frame) went_backwards = true;
            last_frame = out->frame;
        } else {
            ++rejected;
        }
    }
    stop.store(true);
    writer.join();

    std::printf("    accepted %d (%d torn), rejected %d\n", accepted, torn_accepted, rejected);
    CHECK(torn_accepted == 0);
    CHECK(!went_backwards);
    CHECK(accepted > 0);
}
