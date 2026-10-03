// File logger shared by the RDR2 plugin and the GMod module.
//
// Logging from a per-frame path must not touch the disk: RDR2's script thread is never
// allowed to block. So Write() only formats into a slot of a fixed ring (no allocation,
// no system call) and a background thread does the file I/O. If the ring is full the
// line is dropped and counted, and the count is logged once the writer catches up.
//
// Each line is stamped with seconds since Start() and the frame number last given to
// SetFrame():   [   12.345] [f    740] link: waiting -> connected

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace gr {

class Log {
public:
    static Log& Get() noexcept {
        static Log instance;
        return instance;
    }

    // Opens (and truncates) the file and starts the writer thread. Not for per-frame use.
    bool Start(const wchar_t* path) noexcept {
        if (running_.load()) return true;
        file_ = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file_ == INVALID_HANDLE_VALUE) {
            file_ = nullptr;
            return false;
        }
        for (uint32_t i = 0; i < kSlots; ++i) slots_[i].seq.store(i);
        enqueue_pos_.store(0);
        dequeue_pos_.store(0);
        dropped_.store(0);
        stop_.store(false);
        park_.store(false);
        start_ms_ = GetTickCount64();
        wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        parked_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        thread_ = CreateThread(nullptr, 0, &Log::ThreadMain, this, 0, nullptr);
        if (!wake_ || !parked_ || !thread_) {
            CloseAll();
            return false;
        }
        running_.store(true);
        return true;
    }

    // Normal shutdown: flushes and joins the writer. Not callable from DllMain.
    void Stop() noexcept {
        if (!running_.exchange(false)) return;
        stop_.store(true);
        SetEvent(wake_);
        WaitForSingleObject(thread_, INFINITE);
        Drain();
        CloseAll();
    }

    // Shutdown from DllMain(DLL_PROCESS_DETACH). Works around the loader lock: a thread
    // cannot finish exiting while DllMain runs, so joining the writer here would
    // deadlock, and letting it run on would leave it executing code that is about to be
    // unmapped. Instead the writer is asked to flush and park inside Sleep(), where it
    // holds nothing, and is terminated there. `process_terminating` is DllMain's
    // lpReserved != nullptr: Windows has already killed every other thread, so there is
    // no writer left to stop and this thread flushes what remains.
    void StopFromDllMain(bool process_terminating) noexcept {
        if (!running_.exchange(false)) return;
        if (!process_terminating) {
            park_.store(true);
            stop_.store(true);
            SetEvent(wake_);
            WaitForSingleObject(parked_, 2000);
            TerminateThread(thread_, 0);
        }
        Drain();
        CloseAll();
    }

    void SetFrame(uint32_t frame) noexcept { frame_.store(frame, std::memory_order_relaxed); }
    void SetVerbose(bool on) noexcept { verbose_.store(on, std::memory_order_relaxed); }
    bool verbose() const noexcept { return verbose_.load(std::memory_order_relaxed); }

    // printf-style. Never blocks, never allocates. Lines longer than a slot are cut.
    void Write(const char* fmt, ...) noexcept {
        if (!running_.load(std::memory_order_relaxed)) return;

        // Bounded multi-producer queue (Dmitry Vyukov's): claim a slot by advancing
        // enqueue_pos_, fill it, then publish it by bumping the slot's sequence.
        Slot* slot = nullptr;
        uint32_t pos = enqueue_pos_.load(std::memory_order_relaxed);
        for (;;) {
            slot = &slots_[pos & kMask];
            const uint32_t seq = slot->seq.load(std::memory_order_acquire);
            const int32_t diff = static_cast<int32_t>(seq - pos);
            if (diff == 0) {
                if (enqueue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) break;
            } else if (diff < 0) {
                dropped_.fetch_add(1, std::memory_order_relaxed);
                return;
            } else {
                pos = enqueue_pos_.load(std::memory_order_relaxed);
            }
        }

        const double seconds = static_cast<double>(GetTickCount64() - start_ms_) / 1000.0;
        int n = std::snprintf(slot->text, kTextBytes, "[%9.3f] [f %6u] ", seconds,
                              frame_.load(std::memory_order_relaxed));
        if (n < 0) n = 0;
        va_list args;
        va_start(args, fmt);
        const int m = std::vsnprintf(slot->text + n, kTextBytes - static_cast<size_t>(n) - 1, fmt, args);
        va_end(args);
        if (m > 0) n += m;
        if (n > static_cast<int>(kTextBytes) - 2) n = static_cast<int>(kTextBytes) - 2;
        slot->text[n++] = '\n';
        slot->len = static_cast<uint16_t>(n);
        slot->seq.store(pos + 1, std::memory_order_release);
    }

private:
    static constexpr uint32_t kSlots = 1024;  // power of two
    static constexpr uint32_t kMask = kSlots - 1;
    static constexpr size_t kTextBytes = 248;

    struct Slot {
        std::atomic<uint32_t> seq;
        uint16_t len;
        char text[kTextBytes];
    };

    Log() = default;

    static DWORD WINAPI ThreadMain(void* self) noexcept {
        static_cast<Log*>(self)->Run();
        return 0;
    }

    void Run() noexcept {
        while (!stop_.load()) {
            Drain();
            WaitForSingleObject(wake_, 50);
        }
        Drain();
        if (park_.load()) {
            SetEvent(parked_);
            Sleep(INFINITE);
        }
    }

    // Single consumer at a time: the writer thread, or the stopping thread once the
    // writer is finished.
    void Drain() noexcept {
        char batch[16 * 1024];
        size_t used = 0;
        for (;;) {
            const uint32_t pos = dequeue_pos_.load(std::memory_order_relaxed);
            Slot& slot = slots_[pos & kMask];
            const uint32_t seq = slot.seq.load(std::memory_order_acquire);
            if (static_cast<int32_t>(seq - (pos + 1)) != 0) break;
            if (used + slot.len > sizeof(batch)) {
                Flush(batch, used);
                used = 0;
            }
            std::memcpy(batch + used, slot.text, slot.len);
            used += slot.len;
            dequeue_pos_.store(pos + 1, std::memory_order_relaxed);
            slot.seq.store(pos + kSlots, std::memory_order_release);
        }
        const uint32_t dropped = dropped_.exchange(0);
        if (dropped) {
            char line[96];
            const int n = std::snprintf(line, sizeof(line),
                                        "[log] %u lines dropped: the log ring was full\n", dropped);
            if (used + static_cast<size_t>(n) > sizeof(batch)) {
                Flush(batch, used);
                used = 0;
            }
            std::memcpy(batch + used, line, static_cast<size_t>(n));
            used += static_cast<size_t>(n);
        }
        Flush(batch, used);
    }

    void Flush(const char* data, size_t size) noexcept {
        if (!size || !file_) return;
        DWORD written = 0;
        WriteFile(file_, data, static_cast<DWORD>(size), &written, nullptr);
    }

    void CloseAll() noexcept {
        if (thread_) CloseHandle(thread_);
        if (wake_) CloseHandle(wake_);
        if (parked_) CloseHandle(parked_);
        if (file_) CloseHandle(file_);
        thread_ = wake_ = parked_ = file_ = nullptr;
    }

    Slot slots_[kSlots];
    std::atomic<uint32_t> enqueue_pos_{0};
    std::atomic<uint32_t> dequeue_pos_{0};
    std::atomic<uint32_t> dropped_{0};
    std::atomic<uint32_t> frame_{0};
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_{false};
    std::atomic<bool> park_{false};
    std::atomic<bool> verbose_{false};
    uint64_t start_ms_ = 0;
    HANDLE file_ = nullptr;
    HANDLE thread_ = nullptr;
    HANDLE wake_ = nullptr;
    HANDLE parked_ = nullptr;
};

}  // namespace gr

#define GR_LOG(...) ::gr::Log::Get().Write(__VA_ARGS__)
#define GR_VERBOSE(...)                                              \
    do {                                                             \
        if (::gr::Log::Get().verbose()) ::gr::Log::Get().Write(__VA_ARGS__); \
    } while (0)
