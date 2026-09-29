#ifdef NDEBUG
#undef NDEBUG  // these tests rely on assert(); never compile them out
#endif
#include <cassert>
#include <cstdio>

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
} // namespace

int main() {
    overflow_is_counted_and_otherwise_unchanged();
    best_levels_are_price_ordered_not_insertion_ordered();
    order_table_recovers_side_and_price();
    std::puts("test_order_book: all assertions passed");
    return 0;
}
