#pragma once

// Deterministic PRNG for the randomized tests (SplitMix64). Integer-only, no
// std::random distributions (whose output is implementation-defined), so a
// given seed produces the same sequence on every compiler and platform.

#include <cstdint>

namespace fpga_sim_test {

class TestRng {
public:
    explicit TestRng(std::uint64_t seed) noexcept : state_(seed) {}
    std::uint64_t next() noexcept {
        std::uint64_t z = (state_ += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31U);
    }
    // Uniform in [0, n) for n > 0 (modulo bias is irrelevant for these tests).
    std::uint64_t below(std::uint64_t n) noexcept { return next() % n; }
    std::uint32_t u32() noexcept { return static_cast<std::uint32_t>(next() >> 32U); }
    bool chance(std::uint64_t percent) noexcept { return below(100) < percent; }

private:
    std::uint64_t state_;
};

} // namespace fpga_sim_test
