// The white noise source. Plain C++, no plugin code. Our own: a small, fast pseudo-random generator (PCG32, the
// published permuted-congruential method), one new value every sample, evenly spread over -1 .. +1 (every value
// equally likely; the manual does not say how the module's analog noise is spread).
// Seeded, so the tests are repeatable; the plugin seeds each copy differently.

#pragma once

#include <cstdint>

namespace particlehold
{
    class Noise
    {
    public:
        explicit Noise (std::uint64_t seed = 0x853c49e6748fea9bULL) { setSeed (seed); }

        void setSeed (std::uint64_t seed)
        {
            state = 0;
            nextBits();
            state += seed;
            nextBits();
        }

        // -1 .. +1 (24 bits of detail, far finer than the 14-bit MIDI output).
        double next()
        {
            const std::uint32_t r = nextBits() >> 8;
            return (double) r * (2.0 / 16777216.0) - 1.0;
        }

    private:
        std::uint32_t nextBits()
        {
            const std::uint64_t old = state;
            state = old * 6364136223846793005ULL + 1442695040888963407ULL;
            const std::uint32_t xorshifted = (std::uint32_t) (((old >> 18u) ^ old) >> 27u);
            const std::uint32_t rot = (std::uint32_t) (old >> 59u);
            return (xorshifted >> rot) | (xorshifted << ((32u - rot) & 31u));
        }

        std::uint64_t state = 0;
    };
}
