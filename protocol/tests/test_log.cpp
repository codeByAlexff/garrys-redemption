#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "gr_log.h"
#include "gr_test.h"

namespace {

std::wstring TempLogPath(const wchar_t* leaf) {
    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    return std::wstring(dir) + L"gr_test_" + std::to_wstring(GetCurrentProcessId()) + L"_" + leaf;
}

std::string ReadAll(const std::wstring& path) {
    std::string out;
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return out;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    std::fclose(f);
    return out;
}

int Count(const std::string& text, const char* needle) {
    int count = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) ++count;
    return count;
}

}  // namespace

GR_TEST(log_lines_reach_the_file_with_frame_numbers) {
    const std::wstring path = TempLogPath(L"basic.log");
    gr::Log& log = gr::Log::Get();
    CHECK(log.Start(path.c_str()));
    log.SetFrame(740);
    GR_LOG("link: %s -> %s", "waiting", "connected");
    log.SetVerbose(false);
    GR_VERBOSE("hidden %d", 1);
    log.SetVerbose(true);
    GR_VERBOSE("shown %d", 2);
    log.SetVerbose(false);
    log.Stop();

    const std::string text = ReadAll(path);
    CHECK(text.find("[f    740] link: waiting -> connected\n") != std::string::npos);
    CHECK(text.find("hidden") == std::string::npos);
    CHECK(text.find("shown 2") != std::string::npos);
    DeleteFileW(path.c_str());
}

GR_TEST(log_overlong_line_is_cut_not_overrun) {
    const std::wstring path = TempLogPath(L"long.log");
    gr::Log& log = gr::Log::Get();
    CHECK(log.Start(path.c_str()));
    const std::string big(2000, 'x');
    GR_LOG("%s", big.c_str());
    GR_LOG("after");
    log.Stop();

    const std::string text = ReadAll(path);
    CHECK(Count(text, "\n") == 2);
    CHECK(text.size() < 600);
    CHECK(text.find("after\n") != std::string::npos);
    DeleteFileW(path.c_str());
}

GR_TEST(log_many_threads_lose_nothing_silently) {
    const std::wstring path = TempLogPath(L"threads.log");
    gr::Log& log = gr::Log::Get();
    CHECK(log.Start(path.c_str()));

    constexpr int kThreads = 4;
    constexpr int kLines = 2000;  // 8000 lines into a 1024 slot ring: some must drop
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([t] {
            for (int i = 0; i < kLines; ++i) GR_LOG("worker %d line %d", t, i);
        });
    }
    for (auto& t : threads) t.join();
    log.Stop();

    const std::string text = ReadAll(path);
    const int written = Count(text, "worker ");
    int dropped = 0;
    for (size_t at = text.find("[log] "); at != std::string::npos; at = text.find("[log] ", at + 1)) {
        dropped += std::atoi(text.c_str() + at + 6);
    }
    std::printf("    written %d, reported dropped %d\n", written, dropped);
    // Every line is either in the file or accounted for in a "lines dropped" notice.
    CHECK(written + dropped == kThreads * kLines);
    CHECK(written >= 1024);
    DeleteFileW(path.c_str());
}
