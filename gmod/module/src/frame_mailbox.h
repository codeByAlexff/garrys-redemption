// Hands whole overlay frames from one thread to another without a lock: a triple buffer.
// The writer (GMod's render thread, in the Present hook) always has a buffer of its own to
// fill, the reader (the overlay thread) always has one of its own to read, and the third
// is the newest finished frame. Swapping is one atomic exchange on each side, so neither
// side ever waits for the other; a reader that falls behind just skips frames.
//
// Buffers are allocated when the frame size changes (a resolution change), never per frame.

#pragma once

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace gr {

struct OverlayFrame {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t serial = 0;    // counts frames written, so the reader can tell a new one
    uint32_t time_us = 0;   // gr::NowUs() when the capture finished
    uint8_t* pixels = nullptr;  // width * height * 4 bytes, B G R A, rows packed
    size_t capacity = 0;

    bool Reserve(uint32_t w, uint32_t h) noexcept {
        const size_t need = static_cast<size_t>(w) * h * 4;
        if (need > capacity) {
            uint8_t* grown = static_cast<uint8_t*>(std::realloc(pixels, need));
            if (!grown) return false;
            pixels = grown;
            capacity = need;
        }
        width = w;
        height = h;
        return true;
    }
};

class FrameMailbox {
public:
    // Writer side. Fill the returned frame, then Post().
    OverlayFrame& WriteBuffer() noexcept { return frames_[write_]; }
    void Post() noexcept {
        const uint32_t old = ready_.exchange(write_ | kFresh, std::memory_order_acq_rel);
        write_ = old & kIndex;
    }

    // Reader side. Returns the newest posted frame, or nullptr if nothing was posted since
    // the last call. The frame stays the reader's until the next successful Take().
    const OverlayFrame* Take() noexcept {
        if ((ready_.load(std::memory_order_acquire) & kFresh) == 0) return nullptr;
        const uint32_t old = ready_.exchange(read_, std::memory_order_acq_rel);
        read_ = old & kIndex;
        return &frames_[read_];
    }

private:
    static constexpr uint32_t kFresh = 4;
    static constexpr uint32_t kIndex = 3;

    OverlayFrame frames_[3];
    uint32_t write_ = 0;              // touched by the writer only
    uint32_t read_ = 1;               // touched by the reader only
    std::atomic<uint32_t> ready_{2};  // the third buffer, plus kFresh once it holds a new frame
};

}  // namespace gr
