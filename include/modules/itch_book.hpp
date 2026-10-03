#pragma once

// Applies decoded ITCH events to an OrderTable + FiveLevelOrderBook pair. This
// is the book-update logic of the tick-to-trade pipeline, factored out so the
// invariant tests replay long streams through exactly the same code.

#include "modules/ofi_dsp.hpp"
#include "modules/order_book.hpp"
#include "modules/order_table.hpp"
#include "modules/parser_itch50.hpp"

#include <cstdint>
#include <limits>

namespace fpga_sim {

enum class ApplyStatus : std::uint8_t {
    applied,          // table and book updated
    book_overflow,    // Add accepted by the table, dropped by the book (side full); order kept off-book
    book_rejected,    // Add accepted by the table, rejected by the book (level total would exceed INT32_MAX)
    out_of_range,     // shares or price above INT32_MAX, or zero shares: nothing changed
    duplicate_or_full,// Add whose reference is already live, or order table full: nothing changed
    unknown_order,    // Executed/Cancel for a reference that is not live: nothing changed
    bad_event         // event type not A/E/X: nothing changed
};

class ItchBookApplier {
public:
    static constexpr std::int32_t empty_bid_price = 0;
    static constexpr std::int32_t empty_ask_price = std::numeric_limits<std::int32_t>::max();

    void reset() noexcept { orders_.reset(); book_.reset(); }

    ApplyStatus apply(const ItchEvent& e) noexcept {
        constexpr auto int32_max = static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max());
        if (e.type != ItchType::add && e.type != ItchType::executed && e.type != ItchType::cancel)
            return ApplyStatus::bad_event;
        if (e.shares == 0 || e.shares > int32_max) return ApplyStatus::out_of_range;
        const auto shares = static_cast<std::int32_t>(e.shares);
        if (e.type == ItchType::add) {
            if (e.side != 'B' && e.side != 'S') return ApplyStatus::bad_event;
            if (e.price_ticks > int32_max) return ApplyStatus::out_of_range;
            const bool bid = e.side == 'B';
            const auto price = static_cast<std::int32_t>(e.price_ticks);
            if (!orders_.insert(e.order_reference, bid, price, shares)) return ApplyStatus::duplicate_or_full;
            const std::uint64_t overflow_before = book_.overflow_count();
            if (book_.add(bid, price, shares)) return ApplyStatus::applied;
            orders_.set_in_book(e.order_reference, false);
            return book_.overflow_count() != overflow_before ? ApplyStatus::book_overflow : ApplyStatus::book_rejected;
        }
        const auto r = orders_.take(e.order_reference, shares);
        if (!r) return ApplyStatus::unknown_order;
        if (r->in_book) book_.execute(r->bid, r->price, r->applied);
        return ApplyStatus::applied;
    }

    // Best bid/ask after the last event. Empty bid side is priced 0 and an
    // empty ask side INT32_MAX, both with quantity 0.
    [[nodiscard]] BookQuote quote() const noexcept {
        const BookLevel b = book_.best_bid(), a = book_.best_ask();
        return BookQuote{b.valid ? b.price : empty_bid_price, b.valid ? b.quantity : 0,
                         a.valid ? a.price : empty_ask_price, a.valid ? a.quantity : 0};
    }

    [[nodiscard]] const FiveLevelOrderBook& book() const noexcept { return book_; }
    [[nodiscard]] const OrderTable& orders() const noexcept { return orders_; }

private:
    OrderTable orders_{};
    FiveLevelOrderBook book_{};
};

} // namespace fpga_sim
