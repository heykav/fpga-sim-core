#ifdef NDEBUG
#undef NDEBUG  // these tests rely on assert(); never compile them out
#endif
#include <cassert>
#include <cstdio>

#include "modules/order_book.hpp"

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
} // namespace

int main() {
    overflow_is_counted_and_otherwise_unchanged();
    std::puts("test_order_book: all assertions passed");
    return 0;
}
