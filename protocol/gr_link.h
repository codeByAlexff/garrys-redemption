// The bridge link: maps the shared memory, runs the handshake, and moves frames.
//
// Both sides use the same class with a different Role. Either side may start first and
// either may restart at any time.
//
// Handshake. There is no shared "header written by whoever got there first", because
// that would need a lock. Instead each side owns one GrPeer block and only ever writes
// its own. A side is connected when the other block:
//   - carries the magic and is not marked GONE,
//   - has a heartbeat this side has watched move within GR_PEER_TIMEOUT_MS
//     (a block left behind by a dead process never moves, so it never counts),
//   - reports the same GR_PROTOCOL_VERSION and GR_SHM_SIZE.
// On a version mismatch both sides stay mapped, publish GR_PEER_REFUSED and exchange
// nothing, so each log and tools/memdump.py can say exactly what met what.
//
// Tick() is safe on RDR2's script thread: no waits, no file I/O. The only system calls
// are the remap attempts, which run at most once a second and only while not connected.

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>

#include "gr_protocol.h"
#include "gr_seqlock.h"

namespace gr {

enum class Role : uint32_t { Host, Guest };

enum class LinkState : uint32_t {
    Closed,           // not mapped
    Waiting,          // mapped, the other side is absent or silent
    Connected,
    VersionMismatch,  // the other side is alive but built against a different protocol
    PeerRefused,      // the other side is alive and compatible but refuses to run
    Refusing,         // this side called Refuse()
};

inline const char* LinkStateName(LinkState s) noexcept {
    switch (s) {
        case LinkState::Closed: return "closed";
        case LinkState::Waiting: return "waiting";
        case LinkState::Connected: return "connected";
        case LinkState::VersionMismatch: return "version-mismatch";
        case LinkState::PeerRefused: return "peer-refused";
        case LinkState::Refusing: return "refusing";
    }
    return "?";
}

inline const char* ReasonName(uint32_t reason) noexcept {
    switch (reason) {
        case GR_REASON_NONE: return "none";
        case GR_REASON_VERSION: return "protocol version mismatch";
        case GR_REASON_MULTIPLAYER: return "multiplayer session";
    }
    return "?";
}

inline uint64_t NowMs() noexcept { return GetTickCount64(); }

// Microseconds on the performance counter, truncated to 32 bits (wraps every 71 minutes).
// The counter is system-wide, so both games read the same clock and can tell how old the
// other's frame is. Only ever compare two of these with AgeUs.
inline uint32_t NowUs() noexcept {
    static const int64_t frequency = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    LARGE_INTEGER c{};
    QueryPerformanceCounter(&c);
    // Split so the multiplication cannot overflow: at 10 MHz, ticks * 1000000 would after
    // eleven days of uptime.
    const int64_t whole = c.QuadPart / frequency;
    const int64_t part = c.QuadPart % frequency;
    return static_cast<uint32_t>(whole * 1000000 + part * 1000000 / frequency);
}

// How long ago `then_us` was, as of `now_us`. Negative if it is in the future. Correct
// across the wrap as long as the two are within 35 minutes of each other.
inline int32_t AgeUs(uint32_t now_us, uint32_t then_us) noexcept {
    return static_cast<int32_t>(now_us - then_us);
}

class Link {
public:
    Link() = default;
    ~Link() { Close(); }
    Link(const Link&) = delete;
    Link& operator=(const Link&) = delete;

    // Maps the shared memory, creating it if this side is first. Returns false if the
    // mapping could not be created at all; Tick() keeps retrying.
    bool Open(Role role, uint64_t now_ms, const wchar_t* name = GR_SHM_NAME) noexcept {
        Close();
        role_ = role;
        want_open_ = true;
        size_t i = 0;
        for (; name[i] && i + 1 < kNameChars; ++i) name_[i] = name[i];
        name_[i] = L'\0';
        return Map(now_ms);
    }

    // Tells the other side this one is gone and unmaps. Safe to call twice.
    void Close() noexcept {
        want_open_ = false;
        Unmap();
        state_ = LinkState::Closed;
    }

    // Stop exchanging frames and tell the other side why. Latched for the life of this
    // object, across Close() and Open(): there is no way to un-refuse.
    // The RDR2 plugin calls this the moment it sees a multiplayer session.
    void Refuse(uint32_t reason) noexcept {
        refuse_reason_ = reason;
        if (view_) PublishOwnState();
    }

    // Call once per frame. Beats this side's heart and re-evaluates the other side.
    LinkState Tick(uint64_t now_ms) noexcept {
        if (!view_) {
            if (want_open_ && now_ms - last_map_try_ms_ >= kRemapIntervalMs) Map(now_ms);
            if (!view_) return state_ = LinkState::Closed;
        }

        std::atomic_ref<uint32_t>(Mine()->heartbeat).fetch_add(1);

        peer_ = SnapshotPeer();
        const bool alive = WatchPeer(peer_, now_ms);

        // A side built with a smaller layout created the mapping, so only the header
        // could be mapped. Once that side is gone, drop the mapping so Windows destroys
        // it and it can be recreated at this build's size.
        if (!full_ && !alive && now_ms - last_map_try_ms_ >= kRemapIntervalMs) {
            Unmap();
            Map(now_ms);
            if (!view_) return state_ = LinkState::Closed;
        }

        const bool mismatch = alive && (!full_ || peer_.protocol_version != GR_PROTOCOL_VERSION ||
                                        peer_.shm_size != GR_SHM_SIZE);
        if (mismatch != mismatch_) {
            mismatch_ = mismatch;
            PublishOwnState();
        }

        if (refuse_reason_ != GR_REASON_NONE) return state_ = LinkState::Refusing;
        if (!alive) return state_ = LinkState::Waiting;
        if (mismatch) return state_ = LinkState::VersionMismatch;
        if (peer_.state == GR_PEER_REFUSED) return state_ = LinkState::PeerRefused;
        return state_ = LinkState::Connected;
    }

    LinkState state() const noexcept { return state_; }
    bool connected() const noexcept { return state_ == LinkState::Connected; }
    Role role() const noexcept { return role_; }

    // The other side's block as of the last Tick().
    const GrPeer& peer() const noexcept { return peer_; }
    // Counts how many times the other side has (re)opened the link. A change means
    // anything remembered about the other side's previous run is stale.
    uint32_t peer_generation() const noexcept { return peer_generation_; }
    // Reads that found the writer mid-write on every try. Should stay near zero.
    uint32_t torn_reads() const noexcept { return torn_reads_; }
    // True if this side created the mapping rather than finding it.
    bool created() const noexcept { return created_; }
    DWORD last_error() const noexcept { return last_error_; }

    // Frame output. Published whenever mapped and not refusing, connected or not, so
    // the other side finds a current frame the moment it connects.
    void Publish(const GrHostFrame& frame) noexcept {
        if (CanPublish(Role::Host)) SeqWrite(&Shared()->host, frame);
    }
    void Publish(const GrGuestFrame& frame) noexcept {
        if (CanPublish(Role::Guest)) SeqWrite(&Shared()->guest, frame);
    }

    // Frame input. Returns false when not connected or when the copy was torn; `out` is
    // then left exactly as it was, so the caller keeps using its previous frame.
    [[nodiscard]] bool Read(GrGuestFrame& out) noexcept {
        return ReadFrame(Role::Host, &GrShared::guest, out);
    }
    [[nodiscard]] bool Read(GrHostFrame& out) noexcept {
        return ReadFrame(Role::Guest, &GrShared::host, out);
    }

    // Terrain slots (GrChunk). The host writes them, connected or not, like its frame.
    void PublishChunk(uint32_t slot, const GrChunk& chunk) noexcept {
        if (CanPublish(Role::Host) && slot < GR_MAX_CHUNKS) SeqWrite(&Shared()->chunks[slot], chunk);
    }
    // The slot's serial as it stands, without copying the slot: a cheap way to find the
    // ones that changed. 0 when not connected.
    uint32_t ChunkSerial(uint32_t slot) const noexcept {
        if (state_ != LinkState::Connected || role_ != Role::Guest || slot >= GR_MAX_CHUNKS) return 0;
        return std::atomic_ref<uint32_t>(Shared()->chunks[slot].serial).load();
    }
    // Like Read: false when not connected or torn, and `out` is then unspecified.
    [[nodiscard]] bool ReadChunk(uint32_t slot, GrChunk& out) noexcept {
        if (state_ != LinkState::Connected || role_ != Role::Guest || slot >= GR_MAX_CHUNKS) return false;
        if (SeqRead(&Shared()->chunks[slot], out)) return true;
        ++torn_reads_;
        return false;
    }

private:
    static constexpr size_t kNameChars = 96;
    static constexpr uint64_t kRemapIntervalMs = 1000;

    GrShared* Shared() const noexcept { return static_cast<GrShared*>(view_); }
    GrPeer* Mine() const noexcept {
        return role_ == Role::Host ? &Shared()->header.host : &Shared()->header.guest;
    }
    const GrPeer* Theirs() const noexcept {
        return role_ == Role::Host ? &Shared()->header.guest : &Shared()->header.host;
    }

    bool CanPublish(Role needed) const noexcept {
        return view_ && full_ && role_ == needed && refuse_reason_ == GR_REASON_NONE;
    }

    template <class T>
    bool ReadFrame(Role needed, T GrShared::* member, T& out) noexcept {
        if (state_ != LinkState::Connected || role_ != needed) return false;
        // Through a scratch copy: a torn read leaves half of each of two frames in its
        // destination, and that must never be what the caller goes on using.
        T* scratch = reinterpret_cast<T*>(scratch_);
        if (SeqRead(&(Shared()->*member), *scratch)) {
            std::memcpy(&out, scratch, sizeof(T));
            return true;
        }
        ++torn_reads_;
        return false;
    }

    bool Map(uint64_t now_ms) noexcept {
        last_map_try_ms_ = now_ms;
        mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                      GR_SHM_SIZE, name_);
        last_error_ = GetLastError();
        if (!mapping_) return false;
        created_ = last_error_ != ERROR_ALREADY_EXISTS;

        // An existing mapping keeps the size its creator gave it. If that is smaller
        // than this build's layout the full view fails, and the header alone is mapped
        // so the two sides can still report the mismatch to each other.
        view_ = MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, GR_SHM_SIZE);
        full_ = view_ != nullptr;
        if (!view_) view_ = MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, GR_HEADER_SIZE);
        if (!view_) {
            last_error_ = GetLastError();
            CloseHandle(mapping_);
            mapping_ = nullptr;
            return false;
        }

        // Start from a zeroed frame so the other side never consumes what a previous
        // run of this side left behind.
        if (full_) {
            if (role_ == Role::Host) {
                SeqWrite(&Shared()->host, GrHostFrame{});
                for (GrChunk& chunk : Shared()->chunks) SeqWrite(&chunk, GrChunk{});
            } else {
                SeqWrite(&Shared()->guest, GrGuestFrame{});
            }
        }

        GrPeer* me = Mine();
        std::atomic_ref<uint32_t> magic(me->magic);
        magic.store(0);
        LARGE_INTEGER qpc{};
        QueryPerformanceCounter(&qpc);
        uint32_t epoch = static_cast<uint32_t>(qpc.QuadPart) ^ (GetCurrentProcessId() << 16);
        if (epoch == 0 || epoch == me->epoch) epoch += 1;
        me->protocol_version = GR_PROTOCOL_VERSION;
        me->shm_size = GR_SHM_SIZE;
        me->pid = GetCurrentProcessId();
        me->epoch = epoch;
        mismatch_ = false;
        PublishOwnState();
        magic.store(GR_MAGIC);

        peer_ = SnapshotPeer();
        watched_epoch_ = peer_.epoch;
        watched_heartbeat_ = peer_.heartbeat;
        watched_change_ms_ = now_ms;
        seen_change_ = false;
        state_ = LinkState::Waiting;
        return true;
    }

    void Unmap() noexcept {
        if (view_) {
            std::atomic_ref<uint32_t>(Mine()->state).store(GR_PEER_GONE);
            UnmapViewOfFile(view_);
            view_ = nullptr;
        }
        if (mapping_) {
            CloseHandle(mapping_);
            mapping_ = nullptr;
        }
        full_ = false;
    }

    void PublishOwnState() noexcept {
        GrPeer* me = Mine();
        uint32_t state = GR_PEER_ALIVE;
        uint32_t reason = GR_REASON_NONE;
        if (refuse_reason_ != GR_REASON_NONE) {
            state = GR_PEER_REFUSED;
            reason = refuse_reason_;
        } else if (mismatch_) {
            state = GR_PEER_REFUSED;
            reason = GR_REASON_VERSION;
        }
        std::atomic_ref<uint32_t>(me->reason).store(reason);
        std::atomic_ref<uint32_t>(me->state).store(state);
    }

    // The other side rewrites its block field by field when it reopens. Take two copies
    // and accept only when the fields that identify it agree, so a half-rewritten block
    // is never judged.
    GrPeer SnapshotPeer() const noexcept {
        GrPeer a{};
        GrPeer b{};
        for (int i = 0; i < 4; ++i) {
            std::memcpy(&a, Theirs(), sizeof(GrPeer));
            std::memcpy(&b, Theirs(), sizeof(GrPeer));
            if (a.magic == b.magic && a.epoch == b.epoch &&
                a.protocol_version == b.protocol_version && a.shm_size == b.shm_size) {
                return b;
            }
        }
        // Still changing after four looks: treat as absent this frame.
        return GrPeer{};
    }

    bool WatchPeer(const GrPeer& p, uint64_t now_ms) noexcept {
        if (p.magic != GR_MAGIC || p.state == GR_PEER_ABSENT || p.state == GR_PEER_GONE) {
            seen_change_ = false;
            return false;
        }
        if (p.epoch != watched_epoch_) {
            ++peer_generation_;
            watched_epoch_ = p.epoch;
            watched_heartbeat_ = p.heartbeat;
            watched_change_ms_ = now_ms;
            seen_change_ = true;
        } else if (p.heartbeat != watched_heartbeat_) {
            watched_heartbeat_ = p.heartbeat;
            watched_change_ms_ = now_ms;
            seen_change_ = true;
        }
        if (now_ms - watched_change_ms_ > GR_PEER_TIMEOUT_MS) seen_change_ = false;
        return seen_change_;
    }

    Role role_ = Role::Host;
    LinkState state_ = LinkState::Closed;
    HANDLE mapping_ = nullptr;
    void* view_ = nullptr;
    bool full_ = false;
    bool created_ = false;
    bool want_open_ = false;
    bool mismatch_ = false;
    bool seen_change_ = false;
    uint32_t refuse_reason_ = GR_REASON_NONE;
    uint32_t watched_epoch_ = 0;
    uint32_t watched_heartbeat_ = 0;
    uint32_t peer_generation_ = 0;
    uint32_t torn_reads_ = 0;
    uint64_t watched_change_ms_ = 0;
    uint64_t last_map_try_ms_ = 0;
    DWORD last_error_ = 0;
    GrPeer peer_{};
    wchar_t name_[kNameChars] = {};
    // Big enough for either frame. A member rather than a local: 20 KB does not belong on
    // the stack of RDR2's script fiber.
    alignas(GrHostFrame) unsigned char scratch_[sizeof(GrHostFrame) > sizeof(GrGuestFrame)
                                                    ? sizeof(GrHostFrame)
                                                    : sizeof(GrGuestFrame)] = {};
};

}  // namespace gr
