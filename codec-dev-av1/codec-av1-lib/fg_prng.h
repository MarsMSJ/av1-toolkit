#pragma once

// [MMSJ] §7.18.3.1 — AV1 film grain PRNG

#include <cstdint>

namespace av1::fg {

class Av1FgPrng {
private:
    uint16_t random_register_{0};

public:
    // [MMSJ] §7.18.3.2 — chroma PRNG stripe indices
    static constexpr int kCbStripeIdx = 7 << 5;
    static constexpr int kCrStripeIdx = 11 << 5;

    Av1FgPrng() = default;

    // Direct seed — used for luma: prng.seed(params.random_seed)
    explicit Av1FgPrng(uint16_t seed) : random_register_(seed) {}

    // [MMSJ] §7.18.3.1 init_random_generator
    void init(int luma_line, uint16_t seed) {
        uint16_t msb = (seed >> 8) & 255;
        uint16_t lsb = seed & 255;
        random_register_ = (msb << 8) + lsb;
        int luma_num = luma_line >> 5;
        random_register_ ^= ((luma_num * 37 + 178) & 255) << 8;
        random_register_ ^= ((luma_num * 173 + 105) & 255);
    }

    // [MMSJ] §7.18.3.1 get_random_number
    int get(int bits) {
        uint16_t bit = ((random_register_ >> 0) ^ (random_register_ >> 1) ^
                        (random_register_ >> 3) ^ (random_register_ >> 12)) & 1;
        random_register_ = (random_register_ >> 1) | (bit << 15);
        return (random_register_ >> (16 - bits)) & ((1 << bits) - 1);
    }

    void seed(uint16_t s) { random_register_ = s; }
};

}  // namespace av1::fg
