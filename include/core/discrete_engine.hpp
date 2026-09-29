#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace fpga_sim {

class DiscreteEngine {
public:
    static constexpr double cycle_ns = 3.1032;
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
