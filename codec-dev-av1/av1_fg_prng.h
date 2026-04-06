

#pragma once
#include <cstdint>

class Av1FgPrng {
  private:
    void init()
    {
        uint16_t msb = (seed_coded >> 8) & 255;
        uint16_t lsb = seed_coded & 255;

        random_register_cb = (msb << 8) + lsb;
        random_register_cb ^= ((kCbRngStripeIdx * 37 + 178) & 255) << 8;
        random_register_cb ^= ((kCbRngStripeIdx * 173 + 105) & 255);

        random_register_cr = (msb << 8) + lsb;
        random_register_cr ^= ((kCrRngStripeIdx * 37 + 178) & 255) << 8;
        random_register_cr ^= ((kCrRngStripeIdx * 173 + 105) & 255);
    }

    uint16_t random_register_cb{0};
    uint16_t random_register_cr{0};
    uint16_t seed_coded{0};

  public:
    static constexpr int kCbRngStripeIdx = 7 << 5;
    static constexpr int kCrRngStripeIdx = 11 << 5;

    Av1FgPrng(uint16_t seed) : seed_coded(seed) { init(); }

    int getPrngCb(int bits)
    {
        uint16_t bit       = ((random_register_cb >> 0) ^ (random_register_cb >> 1) ^
                              (random_register_cb >> 3) ^ (random_register_cb >> 12)) &
                             1;
        random_register_cb = (random_register_cb >> 1) | (bit << 15);
        return (random_register_cb >> (16 - bits)) & ((1 << bits) - 1);
    }

    int getPrngCr(int bits)
    {
        uint16_t bit       = ((random_register_cr >> 0) ^ (random_register_cr >> 1) ^
                              (random_register_cr >> 3) ^ (random_register_cr >> 12)) &
                             1;
        random_register_cr = (random_register_cr >> 1) | (bit << 15);
        return (random_register_cr >> (16 - bits)) & ((1 << bits) - 1);
    }
};