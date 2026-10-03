// Jenkins one-at-a-time, the hash RAGE names its models by: a model's hash is the joaat of
// its name in lower case. Both sides can turn a model name into the hash the game uses.

#pragma once

#include <cstdint>

namespace gr {

constexpr uint32_t Joaat(const char* text) noexcept {
    uint32_t h = 0;
    for (; *text; ++text) {
        char c = *text;
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        h += static_cast<uint8_t>(c);
        h += h << 10;
        h ^= h >> 6;
    }
    h += h << 3;
    h ^= h >> 11;
    h += h << 15;
    return h;
}

}  // namespace gr
