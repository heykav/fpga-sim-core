#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace fpga_sim {

class DiscreteEngine {
public:
    // Single source of truth for the modelled clock: 322.265625 MHz.
    static constexpr std::uint64_t clock_hz = 322265625ULL;
    // Period as an exact fraction: 1e9 / clock_hz ns = 512/165 ns = 3.10303... ns.
    static constexpr std::uint64_t cycle_ns_num = 512;
    static constexpr std::uint64_t cycle_ns_den = 165;
    static_assert(clock_hz * cycle_ns_num == 1000000000ULL * cycle_ns_den,
                  "cycle period fraction must equal 1/clock_hz");
    static constexpr double cycle_ns = 1.0e9 / static_cast<double>(clock_hz);

    // Cycles -> nanoseconds x 10^4 (four decimals), rounded half up, integer-only
    // so reports are bit-identical on every platform.
    [[nodiscard]] static constexpr std::uint64_t cycles_to_ns_e4(std::uint64_t cycles) noexcept {
        return (cycles * cycle_ns_num * 10000ULL + cycle_ns_den / 2) / cycle_ns_den;
    }
    using Callback = void (*)(void*, std::uint64_t) noexcept;

    static constexpr std::size_t max_callbacks = 16;

    // Returns false (and registers nothing) if the callback is null or all
    // slots for that edge are already in use.
    [[nodiscard]] bool on_posedge(Callback callback, void* context) noexcept {
        if (callback == nullptr || count_ >= max_callbacks) return false;
        posedge_[count_] = {callback, context};
        ++count_;
        return true;
    }
    [[nodiscard]] bool on_negedge(Callback callback, void* context) noexcept {
        if (callback == nullptr || negedge_count_ >= max_callbacks) return false;
        negedge_[negedge_count_] = {callback, context};
        ++negedge_count_;
        return true;
    }

    void reset() noexcept { cycle_ = 0; }
    void step() {
        for (std::size_t i = 0; i < count_; ++i) posedge_[i].callback(posedge_[i].context, cycle_);
        for (std::size_t i = 0; i < negedge_count_; ++i) negedge_[i].callback(negedge_[i].context, cycle_);
        ++cycle_;
    }
    void run(std::uint64_t cycles) {
        for (std::uint64_t i = 0; i < cycles; ++i) step();
    }
    [[nodiscard]] std::uint64_t cycle() const noexcept { return cycle_; }

private:
    struct Slot { Callback callback = nullptr; void* context = nullptr; };
    std::array<Slot, max_callbacks> posedge_{};
    std::array<Slot, max_callbacks> negedge_{};
    std::size_t count_ = 0;
    std::size_t negedge_count_ = 0;
    std::uint64_t cycle_ = 0;
};

} // namespace fpga_sim
