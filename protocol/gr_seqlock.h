// Seqlock for the two frame blocks. One writer per block, any number of readers, no
// mutex shared between the processes.
//
// The writer makes the counter odd, copies the payload in, and makes it even again. A
// reader copies the payload out and keeps the copy only if the counter was even and
// unchanged across the copy.
//
// The payload copy races with the writer by design. The C++ standard calls that
// undefined; in practice it is a memcpy of plain bytes whose result is thrown away
// whenever the counter says it was torn, which is how every seqlock works.

#pragma once

#include <atomic>
#include <concepts>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace gr {

// Frame types start with `uint32_t seq`. The payload is everything after it.
template <class T>
concept SeqFrame = std::is_trivially_copyable_v<T> && std::is_standard_layout_v<T> &&
                   requires(T t) {
                       { t.seq } -> std::same_as<uint32_t&>;
                   };

namespace detail {
template <SeqFrame T>
inline std::atomic_ref<uint32_t> SeqOf(T* shared) noexcept {
    return std::atomic_ref<uint32_t>(shared->seq);
}
inline constexpr size_t kSeqBytes = sizeof(uint32_t);
}  // namespace detail

// Publish `local` into `shared`. local.seq is ignored. Only the single writer of this
// block may call this.
template <SeqFrame T>
inline void SeqWrite(T* shared, const T& local) noexcept {
    auto seq = detail::SeqOf(shared);
    // A writer that died mid-write leaves the counter odd. `| 1` picks up from there
    // rather than flipping it to even while the payload is still being written.
    const uint32_t odd = seq.load() | 1u;
    seq.store(odd);
    std::memcpy(reinterpret_cast<unsigned char*>(shared) + detail::kSeqBytes,
                reinterpret_cast<const unsigned char*>(&local) + detail::kSeqBytes,
                sizeof(T) - detail::kSeqBytes);
    seq.store(odd + 1u);
}

// Copy `shared` into `out`. Returns false, leaving `out` unspecified, if no consistent
// copy was obtained within `max_tries`. Bounded so a caller on RDR2's script thread can
// never spin: on failure it keeps using its previous frame.
template <SeqFrame T>
[[nodiscard]] inline bool SeqRead(const T* shared, T& out, int max_tries = 8) noexcept {
    auto seq = detail::SeqOf(const_cast<T*>(shared));
    for (int i = 0; i < max_tries; ++i) {
        const uint32_t before = seq.load();
        if (before & 1u) continue;
        std::memcpy(reinterpret_cast<unsigned char*>(&out) + detail::kSeqBytes,
                    reinterpret_cast<const unsigned char*>(shared) + detail::kSeqBytes,
                    sizeof(T) - detail::kSeqBytes);
        if (seq.load() == before) {
            out.seq = before;
            return true;
        }
    }
    return false;
}

}  // namespace gr
