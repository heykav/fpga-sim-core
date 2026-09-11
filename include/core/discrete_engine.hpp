#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>

namespace fpga_sim {

class DiscreteEngine {
public:
    static constexpr double cycle_ns = 3.1032;
    using Callback = std::function<void(std::uint64_t)>;

    void on_posedge(Callback callback) { posedge_[count_++] = std::move(callback); }
    void on_negedge(Callback callback) { negedge_[negedge_count_++] = std::move(callback); }

    void reset() noexcept { cycle_ = 0; }
    void step() {
        for (std::size_t i = 0; i < count_; ++i) posedge_[i](cycle_);
        for (std::size_t i = 0; i < negedge_count_; ++i) negedge_[i](cycle_);
        ++cycle_;
    }
    void run(std::uint64_t cycles) {
        for (std::uint64_t i = 0; i < cycles; ++i) step();
    }
    [[nodiscard]] std::uint64_t cycle() const noexcept { return cycle_; }

private:
    std::array<Callback, 16> posedge_{};
    std::array<Callback, 16> negedge_{};
    std::size_t count_ = 0;
    std::size_t negedge_count_ = 0;
    std::uint64_t cycle_ = 0;
};

} // namespace fpga_sim
