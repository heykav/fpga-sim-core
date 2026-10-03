// Replays long seeded ITCH streams through Itch50Encoder -> Itch50Parser ->
// ItchBookApplier (the pipeline's book-update code) and checks, after EVERY
// event, the book against an independent reference model and a set of
// invariants:
//   * the ApplyStatus equals the reference model's prediction;
//   * the book's valid levels equal the reference's in-book levels exactly
//     (5-price window, drop-on-overflow semantics reproduced independently);
//   * valid levels have quantity > 0, invalid levels are all zero, prices on
//     one side are distinct;
//   * quantity conservation: each level's quantity equals the sum of the
//     remaining shares of the live, in-book orders in the OrderTable at that
//     side and price, and every such order has a level;
//   * best bid/ask are the extreme valid prices and quote() reports them;
//   * the book is never crossed (the generator only emits uncrossed adds, as
//     a real ITCH feed does);
//   * while no live order is off-book, the book equals the full-depth book.
// Fault events are mixed in: unknown references, duplicate references, zero
// and > INT32_MAX share counts, prices > INT32_MAX, level totals that would
// overflow int32, invalid side bytes, a full order table.
#ifdef NDEBUG
#undef NDEBUG  // these tests rely on assert(); never compile them out
#endif
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <limits>
#include <map>
#include <utility>

#include "modules/itch_book.hpp"
#include "modules/parser_itch50.hpp"
#include "support/test_rng.hpp"

using namespace fpga_sim;
using fpga_sim_test::TestRng;

namespace {

constexpr std::int64_t int32_max = std::numeric_limits<std::int32_t>::max();

struct RefOrder { bool bid; std::int32_t price; std::int64_t remaining; bool in_book; };

// Independent model: plain std::map, 64-bit arithmetic, no code shared with the modules.
struct Reference {
    std::map<std::uint64_t, RefOrder> orders;
    std::map<std::int32_t, std::int64_t> window[2];   // [0] asks, [1] bids: quantity resting in the 5-level book
    std::map<std::int32_t, std::int64_t> full[2];     // full-depth aggregate over all live orders

    ApplyStatus apply(const ItchEvent& e) {
        if (e.shares == 0 || e.shares > int32_max) return ApplyStatus::out_of_range;
        if (e.type == ItchType::add) {
            if (e.price_ticks > int32_max) return ApplyStatus::out_of_range;
            if (orders.count(e.order_reference) != 0 || orders.size() == OrderTable::capacity)
                return ApplyStatus::duplicate_or_full;
            const bool bid = e.side == 'B';
            const auto price = static_cast<std::int32_t>(e.price_ticks);
            RefOrder o{bid, price, e.shares, true};
            full[bid][price] += e.shares;
            auto& w = window[bid];
            ApplyStatus st = ApplyStatus::applied;
            if (auto it = w.find(price); it != w.end()) {
                if (it->second + e.shares > int32_max) { st = ApplyStatus::book_rejected; o.in_book = false; }
                else it->second += e.shares;
            } else if (w.size() < FiveLevelOrderBook::levels) {
                w[price] = e.shares;
            } else {
                st = ApplyStatus::book_overflow;
                o.in_book = false;
            }
            orders[e.order_reference] = o;
            return st;
        }
        auto it = orders.find(e.order_reference);
        if (it == orders.end()) return ApplyStatus::unknown_order;
        RefOrder& o = it->second;
        const std::int64_t applied = std::min<std::int64_t>(e.shares, o.remaining);
        o.remaining -= applied;
        if (o.in_book) {
            auto& q = window[o.bid][o.price];
            q -= applied;
            assert(q >= 0);
            if (q == 0) window[o.bid].erase(o.price);
        }
        auto& f = full[o.bid][o.price];
        f -= applied;
        if (f == 0) full[o.bid].erase(o.price);
        if (o.remaining == 0) orders.erase(it);
        return ApplyStatus::applied;
    }
    [[nodiscard]] bool all_in_book() const {
        for (const auto& kv : orders) if (!kv.second.in_book) return false;
        return true;
    }
};

void check_side(const FiveLevelOrderBook& book, bool bid, const std::map<std::int32_t, std::int64_t>& want) {
    std::map<std::int32_t, std::int64_t> got;
    for (std::size_t i = 0; i < FiveLevelOrderBook::levels; ++i) {
        const BookLevel& l = bid ? book.bid(i) : book.ask(i);
        if (!l.valid) { assert(l.price == 0 && l.quantity == 0); continue; }
        assert(l.quantity > 0);
        assert(got.count(l.price) == 0);             // distinct prices
        got[l.price] = l.quantity;
    }
    assert(got == want);
    const BookLevel best = bid ? book.best_bid() : book.best_ask();
    assert(best.valid == !want.empty());
    if (!want.empty()) {
        const auto& extreme = bid ? *want.rbegin() : *want.begin();
        assert(best.price == extreme.first && best.quantity == extreme.second);
    }
}

void check_invariants(const ItchBookApplier& a, const Reference& ref) {
    const FiveLevelOrderBook& book = a.book();
    check_side(book, true, ref.window[1]);
    check_side(book, false, ref.window[0]);

    // Conservation, computed from the OrderTable itself (not the reference).
    std::map<std::pair<bool, std::int32_t>, std::int64_t> resting;
    std::size_t live = 0;
    for (std::size_t i = 0; i < OrderTable::capacity; ++i) {
        const OrderTable::Entry& e = a.orders().slot(i);
        if (!e.live) continue;
        ++live;
        assert(e.remaining > 0);
        const auto it = ref.orders.find(e.reference);
        assert(it != ref.orders.end());
        assert(it->second.bid == e.bid && it->second.price == e.price &&
               it->second.remaining == e.remaining && it->second.in_book == e.in_book);
        if (e.in_book) resting[{e.bid, e.price}] += e.remaining;
    }
    assert(live == ref.orders.size() && live == a.orders().live_count());
    std::size_t levels_seen = 0;
    for (bool bid : {false, true}) {
        for (std::size_t i = 0; i < FiveLevelOrderBook::levels; ++i) {
            const BookLevel& l = bid ? book.bid(i) : book.ask(i);
            if (!l.valid) continue;
            ++levels_seen;
            const auto it = resting.find({bid, l.price});
            assert(it != resting.end() && it->second == l.quantity);
        }
    }
    assert(levels_seen == resting.size());

    // Not crossed; quote() agrees with best_bid()/best_ask().
    const BookLevel b = book.best_bid(), s = book.best_ask();
    if (b.valid && s.valid) assert(b.price < s.price);
    const BookQuote q = a.quote();
    assert(q.bid_price == (b.valid ? b.price : ItchBookApplier::empty_bid_price));
    assert(q.bid_qty == (b.valid ? b.quantity : 0));
    assert(q.ask_price == (s.valid ? s.price : ItchBookApplier::empty_ask_price));
    assert(q.ask_qty == (s.valid ? s.quantity : 0));

    // With nothing off-book, the 5-level window is the full-depth book.
    if (ref.all_in_book()) assert(ref.window[0] == ref.full[0] && ref.window[1] == ref.full[1]);
}

struct Stats {
    std::size_t by_status[7] = {};
    std::size_t malformed = 0;
    std::size_t max_live = 0;
};

// Sends one event through encode -> parse -> apply and checks everything.
void feed(ItchBookApplier& a, Reference& ref, const ItchEvent& e, Stats& st, bool corrupt_side = false) {
    std::uint8_t wire[Itch50Layout::max_length];
    const std::size_t n = Itch50Encoder::encode(e, wire, sizeof wire);
    assert(n != 0);
    if (corrupt_side) wire[19] = 'Q';               // not 'B'/'S': the parser must drop it
    Itch50Parser parser;
    const std::size_t events = parser.parse(wire, n);
    assert(parser.status() == ParseStatus::ok && parser.bytes_consumed() == n);
    if (corrupt_side) {
        assert(events == 0 && parser.malformed_count() == 1);
        ++st.malformed;
        check_invariants(a, ref);
        return;
    }
    assert(events == 1 && parser.event(0) == e);
    const ApplyStatus want = ref.apply(e);
    const ApplyStatus got = a.apply(parser.event(0));
    assert(got == want);
    ++st.by_status[static_cast<std::size_t>(got)];
    if (ref.orders.size() > st.max_live) st.max_live = ref.orders.size();
    check_invariants(a, ref);
}

Stats replay(std::uint64_t seed, std::size_t count) {
    TestRng rng(seed);
    ItchBookApplier applier;
    Reference ref;
    Stats st;
    std::uint64_t next_ref = 1;
    std::int32_t center = 100000;
    auto best_full = [&](bool bid, std::int32_t& price) {
        if (ref.full[bid].empty()) return false;
        price = bid ? ref.full[bid].rbegin()->first : ref.full[bid].begin()->first;
        return true;
    };
    auto random_live = [&]() -> std::uint64_t {
        auto it = ref.orders.begin();
        std::advance(it, static_cast<std::ptrdiff_t>(rng.below(ref.orders.size())));
        return it->first;
    };
    std::size_t burst_extra = 0;   // > 0: filling the order table, then this many adds against a full table
    for (std::size_t i = 0; i < count; ++i) {
        if (i % 10000 == 5000) burst_extra = 3;
        if (burst_extra > 0 && ref.orders.size() == OrderTable::capacity) --burst_extra;
        if (rng.chance(2)) center += static_cast<std::int32_t>(rng.below(7)) - 3;   // slow drift
        ItchEvent e{};
        e.tracking_number = static_cast<std::uint16_t>(i);
        e.timestamp_ns = i;
        const std::uint64_t pick = rng.below(100);
        const bool crowded = ref.orders.size() >= 110;
        if (burst_extra == 0 && !ref.orders.empty() && (crowded || pick >= 45)) {
            // Executed / Cancel of a live order (partial or full, sometimes more than remains).
            e.type = rng.chance(60) ? ItchType::executed : ItchType::cancel;
            e.order_reference = random_live();
            const std::int64_t rem = ref.orders[e.order_reference].remaining;
            const std::uint64_t r = rng.below(10);
            e.shares = static_cast<std::uint32_t>(r < 4 ? rem : r < 8 ? 1 + static_cast<std::int64_t>(rng.below(static_cast<std::uint64_t>(rem)))
                                                                    : rem + 1 + static_cast<std::int64_t>(rng.below(1000)));
            if (e.shares > int32_max) e.shares = static_cast<std::uint32_t>(int32_max);
            if (e.type == ItchType::executed) e.match_number = i;
        } else if (burst_extra > 0 || pick < 38 || ref.orders.empty()) {
            // Uncrossed add: a bid below the full-depth best ask, an ask above the best bid.
            e.type = ItchType::add;
            const bool bid = rng.chance(50);
            e.side = bid ? 'B' : 'S';
            std::int32_t price = center + (bid ? -static_cast<std::int32_t>(rng.below(12))
                                              : static_cast<std::int32_t>(rng.below(12)) + 1);
            std::int32_t opp = 0;
            if (best_full(!bid, opp)) price = bid ? std::min(price, opp - 1) : std::max(price, opp + 1);
            e.price_ticks = static_cast<std::uint32_t>(price);
            e.shares = static_cast<std::uint32_t>(100 * (1 + rng.below(9)));
            if (rng.chance(3)) e.shares = static_cast<std::uint32_t>(int32_max - static_cast<std::int64_t>(rng.below(3)));
            e.order_reference = next_ref++;
            std::memcpy(e.stock.data(), "TEST    ", 8);
        } else {
            // Faults.
            e.type = ItchType::add;
            e.side = 'B';
            e.price_ticks = static_cast<std::uint32_t>(center - 1);
            e.shares = 100;
            e.order_reference = next_ref++;
            std::memcpy(e.stock.data(), "TEST    ", 8);
            switch (rng.below(7)) {
            case 0: e.type = ItchType::cancel; e.side = 0; e.price_ticks = 0; e.stock = {};
                    e.order_reference = 0xF000000000000000ULL + i; break;             // unknown reference
            case 1: if (ref.orders.empty()) e.shares = 0; else e.order_reference = random_live(); break;  // duplicate reference
            case 2: e.shares = 0; break;                                               // zero shares
            case 3: e.shares = 0x80000000U + static_cast<std::uint32_t>(rng.below(1000)); break;  // > INT32_MAX
            case 4: e.price_ticks = 0x80000000U; break;                                // price > INT32_MAX
            case 5: feed(applier, ref, e, st, true); continue;                         // invalid side byte
            default: e.type = ItchType::executed; e.side = 0; e.price_ticks = 0; e.stock = {};
                     e.order_reference = ref.orders.empty() ? 1 : random_live(); e.shares = 0; break;
            }
        }
        feed(applier, ref, e, st);
    }
    return st;
}

void a_dropped_order_never_touches_a_level_created_later_at_its_price() {
    // Regression: the pipeline used to apply every Executed/Cancel to the book
    // level at the order's price, including orders the book had dropped when
    // the side was full. If a level at that price was created later by another
    // order, the dropped order's reductions were taken from it.
    ItchBookApplier a;
    auto add = [&](std::uint64_t ref, std::uint32_t price, std::uint32_t shares) {
        ItchEvent e{}; e.type = ItchType::add; e.side = 'B'; e.order_reference = ref;
        e.price_ticks = price; e.shares = shares;
        return a.apply(e);
    };
    auto cancel = [&](std::uint64_t ref, std::uint32_t shares) {
        ItchEvent e{}; e.type = ItchType::cancel; e.order_reference = ref; e.shares = shares;
        return a.apply(e);
    };
    for (std::uint32_t i = 0; i < 5; ++i) assert(add(1 + i, 100 + i, 10) == ApplyStatus::applied);
    assert(add(50, 90, 300) == ApplyStatus::book_overflow);   // side full: order 50 is off-book
    assert(cancel(1, 10) == ApplyStatus::applied);            // frees the level at 100
    assert(add(60, 90, 100) == ApplyStatus::applied);         // new level at 90 holds order 60 only
    assert(cancel(50, 300) == ApplyStatus::applied);          // order 50 recognised and removed...
    bool found = false;                                       // ...but order 60's 100 shares remain
    for (std::size_t i = 0; i < FiveLevelOrderBook::levels; ++i)
        if (a.book().bid(i).valid && a.book().bid(i).price == 90) { found = true; assert(a.book().bid(i).quantity == 100); }
    assert(found);
    assert(a.orders().lookup(50) == nullptr);
}

} // namespace

int main() {
    a_dropped_order_never_touches_a_level_created_later_at_its_price();

    const std::uint64_t seeds[] = {0xB00C0001ULL, 0xB00C0002ULL, 0xB00C0003ULL, 0xB00C0004ULL};
    for (std::uint64_t seed : seeds) {
        const Stats st = replay(seed, 50000);
        std::printf("test_book_invariants: seed %#llx: applied=%zu overflow=%zu rejected=%zu out_of_range=%zu "
                    "dup_or_full=%zu unknown=%zu malformed=%zu max_live=%zu\n",
                    static_cast<unsigned long long>(seed), st.by_status[0], st.by_status[1], st.by_status[2],
                    st.by_status[3], st.by_status[4], st.by_status[5], st.malformed, st.max_live);
        // Every fault class and every book outcome must actually have been exercised.
        for (std::size_t k = 0; k < 6; ++k) assert(st.by_status[k] > 0);
        assert(st.malformed > 0 && st.max_live == OrderTable::capacity);
    }
    std::puts("test_book_invariants: all assertions passed");
    return 0;
}
