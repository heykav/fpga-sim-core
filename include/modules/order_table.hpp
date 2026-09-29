#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace fpga_sim {

// Fixed-capacity order-reference table (linear search, no heap, no hashing, so
// behaviour and iteration order are identical on every platform). ITCH
// Executed/Cancel messages carry only an order reference; this table recovers
// the side and price of the resting order so the book can be updated.
class OrderTable {
public:
    static constexpr std::size_t capacity = 128;

    struct Entry {
        std::uint64_t reference = 0;
        std::int32_t price = 0;
        std::int32_t remaining = 0;
        bool bid = false;
        bool live = false;
    };

    void reset() noexcept { entries_.fill({}); }

    // False if the table is full or the reference is already live.
    bool insert(std::uint64_t reference, bool bid, std::int32_t price, std::int32_t shares) noexcept {
        Entry* free_slot = nullptr;
        for (auto& e : entries_) {
            if (e.live && e.reference == reference) return false;
            if (!e.live && free_slot == nullptr) free_slot = &e;
        }
        if (free_slot == nullptr) return false;
        *free_slot = {reference, price, shares, bid, true};
        return true;
    }

    // Removes up to `shares` from the order. On success `side_bid`, `price` and
    // `applied` (shares actually removed, clamped to what remained) are set.
    bool reduce(std::uint64_t reference, std::int32_t shares, bool& side_bid,
                std::int32_t& price, std::int32_t& applied) noexcept {
        for (auto& e : entries_) {
            if (e.live && e.reference == reference) {
                side_bid = e.bid;
                price = e.price;
                applied = shares < e.remaining ? shares : e.remaining;
                e.remaining -= applied;
                if (e.remaining <= 0) e = {};
                return true;
            }
        }
        return false;
    }

private:
    std::array<Entry, capacity> entries_{};
};

} // namespace fpga_sim
