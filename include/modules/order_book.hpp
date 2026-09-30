#pragma once

#include "modules/bram_memory.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace fpga_sim {

struct BookLevel {
    std::int32_t price = 0;
    std::int32_t quantity = 0;
    bool valid = false;
};

// Aggregated price levels, at most `levels` distinct prices per side. This is
// a fixed window, not a full-depth book: an add at a new price when the side
// already holds `levels` prices is dropped and counted in overflow_count()
// (nothing is evicted, even if the new price is better). After any overflow the
// best levels are no longer guaranteed to equal those of a full-depth book;
// the demo treats overflow_count() != 0 as a failed run.
//
// Invariants maintained by every call (checked by test-book-invariants):
//   * a valid level has quantity > 0; an invalid level is all zero;
//   * valid prices on one side are distinct;
//   * quantities never overflow int32 (an add that would is rejected).
class FiveLevelOrderBook {
public:
    static constexpr std::size_t levels = 5;

    void reset() noexcept {
        bids_.fill({});
        asks_.fill({});
        collision_ = false;
        overflow_count_ = 0;
        rejected_count_ = 0;
    }

    // True if the quantity was added to the book. False, with the book
    // unchanged, if quantity <= 0 or the level total would exceed INT32_MAX
    // (counted in rejected_count()), or if the side is full at other prices
    // (counted in overflow_count()).
    bool add(bool bid, std::int32_t price, std::int32_t quantity) noexcept {
        if (quantity <= 0) { ++rejected_count_; return false; }
        auto& side = bid ? bids_ : asks_;
        for (auto& level : side) {
            if (level.valid && level.price == price) {
                if (level.quantity > std::numeric_limits<std::int32_t>::max() - quantity) {
                    ++rejected_count_;
                    return false;
                }
                level.quantity += quantity;
                return true;
            }
        }
        for (auto& level : side) {
            if (!level.valid) {
                level = {price, quantity, true};
                return true;
            }
        }
        ++overflow_count_;  // all levels on this side are occupied: the add is dropped
        return false;
    }

    // Removes up to `quantity` from the level at `price`; the level is cleared
    // when it reaches zero. False (book unchanged) if there is no such level or
    // quantity <= 0. Removing more than the level holds clears it.
    bool execute(bool bid, std::int32_t price, std::int32_t quantity) noexcept {
        if (quantity <= 0) return false;
        auto& side = bid ? bids_ : asks_;
        for (auto& level : side) {
            if (level.valid && level.price == price) {
                if (quantity >= level.quantity) level = {};
                else level.quantity -= quantity;
                return true;
            }
        }
        return false;
    }

    // Highest-priced valid bid / lowest-priced valid ask; the returned level has
    // valid == false when that side is empty. Levels are stored unsorted.
    [[nodiscard]] BookLevel best_bid() const noexcept {
        BookLevel best{};
        for (const auto& level : bids_)
            if (level.valid && (!best.valid || level.price > best.price)) best = level;
        return best;
    }
    [[nodiscard]] BookLevel best_ask() const noexcept {
        BookLevel best{};
        for (const auto& level : asks_)
            if (level.valid && (!best.valid || level.price < best.price)) best = level;
        return best;
    }

    [[nodiscard]] const BookLevel& bid(std::size_t level) const noexcept { return bids_[level]; }
    [[nodiscard]] const BookLevel& ask(std::size_t level) const noexcept { return asks_[level]; }
    [[nodiscard]] bool collision() const noexcept { return collision_; }
    // Number of add() calls dropped because the side already held `levels` distinct prices.
    [[nodiscard]] std::uint64_t overflow_count() const noexcept { return overflow_count_; }
    // Number of add() calls rejected for a non-positive quantity or int32 overflow of the level.
    [[nodiscard]] std::uint64_t rejected_count() const noexcept { return rejected_count_; }

private:
    std::array<BookLevel, levels> bids_{};
    std::array<BookLevel, levels> asks_{};
    bool collision_ = false;
    std::uint64_t overflow_count_ = 0;
    std::uint64_t rejected_count_ = 0;
};

} // namespace fpga_sim
