#pragma once

#include "modules/bram_memory.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace fpga_sim {

struct BookLevel {
    std::int32_t price = 0;
    std::int32_t quantity = 0;
    bool valid = false;
};

class FiveLevelOrderBook {
public:
    static constexpr std::size_t levels = 5;

    void reset() noexcept {
        bids_.fill({});
        asks_.fill({});
        collision_ = false;
        overflow_count_ = 0;
    }

    void add(bool bid, std::int32_t price, std::int32_t quantity) noexcept {
        auto& side = bid ? bids_ : asks_;
        for (auto& level : side) {
            if (level.valid && level.price == price) {
                level.quantity += quantity;
                return;
            }
        }
        for (auto& level : side) {
            if (!level.valid) {
                level = {price, quantity, true};
                return;
            }
        }
        ++overflow_count_;  // all levels on this side are occupied: the add is dropped
    }

    void execute(bool bid, std::int32_t price, std::int32_t quantity) noexcept {
        auto& side = bid ? bids_ : asks_;
        for (auto& level : side) {
            if (level.valid && level.price == price) {
                level.quantity -= quantity;
                if (level.quantity <= 0) level = {};
                return;
            }
        }
    }

    [[nodiscard]] const BookLevel& bid(std::size_t level) const noexcept { return bids_[level]; }
    [[nodiscard]] const BookLevel& ask(std::size_t level) const noexcept { return asks_[level]; }
    [[nodiscard]] bool collision() const noexcept { return collision_; }
    // Number of add() calls dropped because the side already held `levels` distinct prices.
    [[nodiscard]] std::uint64_t overflow_count() const noexcept { return overflow_count_; }

private:
    std::array<BookLevel, levels> bids_{};
    std::array<BookLevel, levels> asks_{};
    bool collision_ = false;
    std::uint64_t overflow_count_ = 0;
};

} // namespace fpga_sim
