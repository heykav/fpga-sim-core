#ifdef NDEBUG
#undef NDEBUG  // these tests rely on assert(); never compile them out
#endif
#include <cassert>
#include <cstdio>
#include <limits>

#include "modules/order_book.hpp"
#include "modules/order_table.hpp"

using fpga_sim::FiveLevelOrderBook;

namespace {
void overflow_is_counted_and_otherwise_unchanged() {
    FiveLevelOrderBook book;
    for (int i = 0; i < 5; ++i) book.add(true, 100 + i, 10);
    assert(book.overflow_count() == 0);
    book.add(true, 100, 5);   // aggregates into an existing level: not an overflow
    assert(book.bid(0).quantity == 15);
    assert(book.overflow_count() == 0);
    book.add(true, 200, 7);   // sixth distinct price: dropped, counted
    assert(book.overflow_count() == 1);
    for (std::size_t i = 0; i < FiveLevelOrderBook::levels; ++i) assert(book.bid(i).price != 200);
    book.add(false, 300, 1);  // asks side is independent
    assert(book.overflow_count() == 1);
    book.reset();
    assert(book.overflow_count() == 0);
}
void best_levels_are_price_ordered_not_insertion_ordered() {
    FiveLevelOrderBook book;
    assert(!book.best_bid().valid && !book.best_ask().valid);
    book.add(true, 100, 5); book.add(true, 103, 7); book.add(true, 101, 9);
    book.add(false, 210, 4); book.add(false, 205, 6); book.add(false, 207, 8);
    assert(book.best_bid().price == 103 && book.best_bid().quantity == 7);
    assert(book.best_ask().price == 205 && book.best_ask().quantity == 6);
    book.execute(true, 103, 7);    // best bid fully consumed -> next best takes over
    assert(book.best_bid().price == 101 && book.best_bid().quantity == 9);
    book.execute(false, 205, 2);   // partial execution keeps the level
    assert(book.best_ask().price == 205 && book.best_ask().quantity == 4);
}

void order_table_recovers_side_and_price() {
    fpga_sim::OrderTable t;
    bool bid = false; std::int32_t price = 0, applied = 0;
    assert(t.insert(7, true, 1000, 300));
    assert(!t.insert(7, true, 1000, 1));                       // duplicate live reference rejected
    assert(t.reduce(7, 100, bid, price, applied) && bid && price == 1000 && applied == 100);
    assert(t.reduce(7, 500, bid, price, applied) && applied == 200);   // clamped to remaining
    assert(!t.reduce(7, 1, bid, price, applied));              // fully consumed -> gone
    assert(!t.reduce(99, 1, bid, price, applied));             // unknown reference
}
void add_and_execute_report_whether_the_book_changed() {
    FiveLevelOrderBook book;
    assert(book.add(true, 100, 10));
    assert(book.add(true, 100, 5));                 // aggregate
    assert(!book.execute(true, 101, 1));            // no level at that price: unchanged
    assert(!book.execute(false, 100, 1));           // wrong side: unchanged
    assert(book.bid(0).quantity == 15);
    assert(book.execute(true, 100, 20));            // over-execute clears the level
    assert(!book.bid(0).valid && book.bid(0).price == 0 && book.bid(0).quantity == 0);
}

void non_positive_quantities_are_rejected_and_leave_the_book_unchanged() {
    // Regression: add() with quantity <= 0 used to create a valid level with a
    // zero or negative quantity; execute() with a negative quantity used to
    // grow the level.
    FiveLevelOrderBook book;
    assert(!book.add(true, 100, 0));
    assert(!book.add(false, 200, -5));
    assert(!book.best_bid().valid && !book.best_ask().valid);
    assert(book.rejected_count() == 2 && book.overflow_count() == 0);
    assert(book.add(true, 100, 10));
    assert(!book.execute(true, 100, 0));
    assert(!book.execute(true, 100, -7));
    assert(book.best_bid().quantity == 10);
}

void aggregated_quantity_cannot_overflow_int32() {
    // Regression: two adds at one price summing past INT32_MAX were signed
    // overflow (undefined behaviour; UBSan reports it).
    constexpr std::int32_t max = std::numeric_limits<std::int32_t>::max();
    FiveLevelOrderBook book;
    assert(book.add(false, 300, max - 1));
    assert(book.add(false, 300, 1));                // exactly INT32_MAX: allowed
    assert(book.best_ask().quantity == max);
    assert(!book.add(false, 300, 1));               // would overflow: rejected
    assert(book.best_ask().quantity == max);
    assert(book.rejected_count() == 1);
    assert(book.execute(false, 300, max));
    assert(!book.best_ask().valid);
}

void order_table_rejects_non_positive_shares() {
    // Regression: reduce() with negative shares used to *increase* the
    // remaining quantity; insert() accepted zero-share orders.
    fpga_sim::OrderTable t;
    bool bid = false; std::int32_t price = 0, applied = 0;
    assert(!t.insert(1, true, 100, 0));
    assert(!t.insert(1, true, 100, -1));
    assert(t.live_count() == 0);
    assert(t.insert(1, true, 100, 10));
    assert(!t.reduce(1, -5, bid, price, applied));
    assert(!t.reduce(1, 0, bid, price, applied));
    assert(t.lookup(1) != nullptr && t.lookup(1)->remaining == 10);
    const auto r = t.take(1, 10);
    assert(r && r->applied == 10 && r->emptied && t.live_count() == 0 && t.lookup(1) == nullptr);
}

void order_table_full_is_reported() {
    fpga_sim::OrderTable t;
    for (std::uint64_t r = 0; r < fpga_sim::OrderTable::capacity; ++r) assert(t.insert(r, true, 1, 1));
    assert(t.live_count() == fpga_sim::OrderTable::capacity);
    assert(!t.insert(999, true, 1, 1));
    assert(t.take(5, 1));                           // frees one slot
    assert(t.insert(999, true, 1, 1));
}
} // namespace

int main() {
    overflow_is_counted_and_otherwise_unchanged();
    best_levels_are_price_ordered_not_insertion_ordered();
    order_table_recovers_side_and_price();
    add_and_execute_report_whether_the_book_changed();
    non_positive_quantities_are_rejected_and_leave_the_book_unchanged();
    aggregated_quantity_cannot_overflow_int32();
    order_table_rejects_non_positive_shares();
    order_table_full_is_reported();
    std::puts("test_order_book: all assertions passed");
    return 0;
}
