#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace fpga_sim {

// Fixed-capacity order-reference table (linear search, no heap, no hashing, so
// behaviour and iteration order are identical on every platform). ITCH
// Executed/Cancel messages carry only an order reference; this table recovers
// the side and price of the resting order so the book can be updated.
//
// `in_book` records whether the order's shares were actually added to the
// price-level book. An order the book dropped (side full) stays in the table
// so later Executed/Cancel messages for it are still recognised, but its
// reductions must not be applied to a book level (which may by then hold
// other orders' shares at the same price).
class OrderTable {
public:
    static constexpr std::size_t capacity = 128;

    struct Entry {
        std::uint64_t reference = 0;
        std::int32_t price = 0;
        std::int32_t remaining = 0;
        bool bid = false;
        bool live = false;
        bool in_book = true;
    };

    struct Reduction {
        bool bid = false;
        std::int32_t price = 0;
        std::int32_t applied = 0;   // shares actually removed (clamped to what remained)
        bool in_book = true;
        bool emptied = false;       // the order is gone after this reduction
    };

    void reset() noexcept { entries_.fill({}); live_ = 0; }

    // False if shares <= 0, the table is full or the reference is already live.
    bool insert(std::uint64_t reference, bool bid, std::int32_t price, std::int32_t shares) noexcept {
        if (shares <= 0) return false;
        Entry* free_slot = nullptr;
        for (auto& e : entries_) {
            if (e.live && e.reference == reference) return false;
            if (!e.live && free_slot == nullptr) free_slot = &e;
        }
        if (free_slot == nullptr) return false;
        *free_slot = {reference, price, shares, bid, true, true};
        ++live_;
        return true;
    }

    // Marks a live order as not resting in the book. False if unknown.
    bool set_in_book(std::uint64_t reference, bool in_book) noexcept {
        Entry* e = find(reference);
        if (e == nullptr) return false;
        e->in_book = in_book;
        return true;
    }

    // Removes up to `shares` from the order; nullopt if the reference is not
    // live or shares <= 0 (table unchanged).
    std::optional<Reduction> take(std::uint64_t reference, std::int32_t shares) noexcept {
        if (shares <= 0) return std::nullopt;
        Entry* e = find(reference);
        if (e == nullptr) return std::nullopt;
        Reduction r{e->bid, e->price, shares < e->remaining ? shares : e->remaining, e->in_book, false};
        e->remaining -= r.applied;
        if (e->remaining <= 0) { *e = {}; --live_; r.emptied = true; }
        return r;
    }

    // Removes up to `shares` from the order. On success `side_bid`, `price` and
    // `applied` (shares actually removed, clamped to what remained) are set.
    bool reduce(std::uint64_t reference, std::int32_t shares, bool& side_bid,
                std::int32_t& price, std::int32_t& applied) noexcept {
        const std::optional<Reduction> r = take(reference, shares);
        if (!r) return false;
        side_bid = r->bid;
        price = r->price;
        applied = r->applied;
        return true;
    }

    [[nodiscard]] const Entry* lookup(std::uint64_t reference) const noexcept {
        for (const auto& e : entries_)
            if (e.live && e.reference == reference) return &e;
        return nullptr;
    }
    [[nodiscard]] std::size_t live_count() const noexcept { return live_; }
    [[nodiscard]] const Entry& slot(std::size_t i) const noexcept { return entries_[i]; }

private:
    Entry* find(std::uint64_t reference) noexcept {
        for (auto& e : entries_)
            if (e.live && e.reference == reference) return &e;
        return nullptr;
    }

    std::array<Entry, capacity> entries_{};
    std::size_t live_ = 0;
};

} // namespace fpga_sim
