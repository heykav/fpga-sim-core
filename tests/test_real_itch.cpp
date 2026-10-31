// Tests for the real-ITCH replay path. Every message here is HAND-BUILT at the byte offsets of
// the NASDAQ TotalView-ITCH 5.0 specification; no NASDAQ data is in the repository.
#ifdef NDEBUG
#undef NDEBUG  // these tests rely on assert(); never compile them out
#endif
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "core/discrete_engine.hpp"
#include "itch_analysis.hpp"
#include "modules/parser_itch50.hpp"
#include "pipeline/real_itch.hpp"
#include "pipeline/tick_to_trade.hpp"

using namespace fpga_sim;

namespace {

using Bytes = std::vector<std::uint8_t>;

void put(Bytes& b, std::size_t off, std::uint64_t v, int n) {
    for (int i = 0; i < n; ++i) b[off + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v >> ((n - 1 - i) * 8));
}
Bytes base(char type, std::size_t len, std::uint16_t locate, std::uint64_t ts) {
    Bytes b(len, 0);
    b[0] = static_cast<std::uint8_t>(type);
    put(b, 1, locate, 2);
    put(b, 5, ts, 6);
    return b;
}
Bytes add(char type, std::uint16_t loc, std::uint64_t ts, std::uint64_t ref, char side, std::uint32_t sh, std::uint32_t px) {
    Bytes b = base(type, type == 'F' ? 40 : 36, loc, ts);
    put(b, 11, ref, 8); b[19] = static_cast<std::uint8_t>(side); put(b, 20, sh, 4);
    const char* sym = "TEST    ";
    for (int i = 0; i < 8; ++i) b[24 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(sym[i]);
    put(b, 32, px, 4);
    if (type == 'F') { const char* mp = "MPID"; for (int i = 0; i < 4; ++i) b[36 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(mp[i]); }
    return b;
}
Bytes exec(char type, std::uint16_t loc, std::uint64_t ts, std::uint64_t ref, std::uint32_t sh) {
    Bytes b = base(type, type == 'E' ? 31 : (type == 'C' ? 36 : 23), loc, ts);
    put(b, 11, ref, 8); put(b, 19, sh, 4);
    return b;
}
Bytes del(std::uint16_t loc, std::uint64_t ts, std::uint64_t ref) { Bytes b = base('D', 19, loc, ts); put(b, 11, ref, 8); return b; }
Bytes repl(std::uint16_t loc, std::uint64_t ts, std::uint64_t old_ref, std::uint64_t new_ref, std::uint32_t sh, std::uint32_t px) {
    Bytes b = base('U', 35, loc, ts); put(b, 11, old_ref, 8); put(b, 19, new_ref, 8); put(b, 27, sh, 4); put(b, 31, px, 4); return b;
}
Bytes directory(std::uint16_t loc) {
    Bytes b = base('R', 39, loc, 1);
    const char* sym = "TEST    ";
    for (int i = 0; i < 8; ++i) b[11 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(sym[i]);
    return b;
}
void frame(Bytes& out, const Bytes& m) { out.push_back(static_cast<std::uint8_t>(m.size() >> 8)); out.push_back(static_cast<std::uint8_t>(m.size())); out.insert(out.end(), m.begin(), m.end()); }

// The scripted session used by several tests (stock locate 7, plus one message for locate 9).
std::vector<Bytes> session() {
    return {
        base('S', 12, 0, 1), directory(7),
        add('A', 7, 100, 1, 'B', 100, 1000000),   // bid 100 @ 100.0000
        add('F', 7, 200, 2, 'S', 50, 1010000),    // ask 50 @ 101.0000 (with MPID)
        add('A', 9, 250, 99, 'B', 5, 5),          // other stock, ignored by the locate-7 book
        exec('E', 7, 300, 1, 40),                 // bid -> 60
        exec('C', 7, 400, 2, 20),                 // ask -> 30 (executed with price)
        exec('X', 7, 500, 1, 10),                 // bid -> 50
        repl(7, 600, 2, 3, 70, 1020000),          // ask order 2 replaced by order 3: 70 @ 102.0000
        add('A', 7, 700, 4, 'B', 25, 990000),     // second bid level
        del(7, 800, 1),                           // bid order 1 deleted (50 @ 100.0000 gone)
    };
}

void parser_decodes_the_new_types_at_the_spec_offsets() {
    Itch50Parser p;
    Bytes f = add('F', 7, 1, 0x1122334455667788ULL, 'B', 300, 1234567);
    assert(p.parse(f.data(), f.size()) == 1);
    assert(p.event(0).type == ItchType::add_mpid && p.event(0).order_reference == 0x1122334455667788ULL);
    assert(p.event(0).shares == 300 && p.event(0).price_ticks == 1234567 && p.event(0).side == 'B');
    Bytes c = exec('C', 7, 1, 42, 17);
    assert(p.parse(c.data(), c.size()) == 1 && p.event(0).type == ItchType::executed_price && p.event(0).shares == 17);
    Bytes d = del(7, 1, 77);
    assert(p.parse(d.data(), d.size()) == 1 && p.event(0).type == ItchType::delete_order && p.event(0).order_reference == 77);
    Bytes u = repl(7, 1, 5, 6, 88, 999);
    assert(p.parse(u.data(), u.size()) == 1 && p.event(0).type == ItchType::replace);
    assert(p.event(0).order_reference == 5 && p.event(0).new_reference == 6 && p.event(0).shares == 88 && p.event(0).price_ticks == 999);
    // A wrong-length F is not decoded (length comes from the type byte, so it reads as truncated).
    Bytes shortf(30, 0); shortf[0] = 'F';
    assert(p.parse(shortf.data(), shortf.size()) == 0 && p.status() == ParseStatus::truncated);
    std::puts("parser F/C/D/U: ok");
}

void walker_counts_the_mix_and_flags_bad_framing() {
    Bytes buf;
    for (const Bytes& m : session()) frame(buf, m);
    itch_tools::Walker w(buf.data(), buf.size());
    std::size_t len = 0, n = 0;
    std::array<int, 256> mix{};
    while (const std::uint8_t* m = w.next(len)) { ++n; ++mix[m[0]]; }
    assert(w.stop == itch_tools::Walker::Stop::end && n == 11);
    assert(mix['A'] == 3 && mix['F'] == 1 && mix['E'] == 1 && mix['C'] == 1 && mix['X'] == 1 && mix['D'] == 1 && mix['U'] == 1 && mix['S'] == 1 && mix['R'] == 1);
    // Truncated tail is reported, not misread.
    Bytes cut(buf.begin(), buf.end() - 5);
    itch_tools::Walker wt(cut.data(), cut.size());
    n = 0; while (wt.next(len)) ++n;
    assert(wt.stop == itch_tools::Walker::Stop::truncated_tail && n == 10);
    // A length that disagrees with the spec is bad framing.
    Bytes bad; Bytes m = add('A', 7, 1, 1, 'B', 1, 1); m.pop_back(); frame(bad, m);
    itch_tools::Walker wb(bad.data(), bad.size());
    assert(wb.next(len) == nullptr && wb.stop == itch_tools::Walker::Stop::bad_framing);
    std::puts("walker: ok");
}

void real_book_follows_the_session_by_hand() {
    itch_tools::RealBook book;
    std::int64_t p = 0;
    std::size_t step = 0;
    for (const Bytes& m : session()) {
        if (m[0] == 'S' || m[0] == 'R' || itch_tools::locate_of(m.data()) != 7) continue;
        book.apply(m.data());
        ++step;
        if (step == 2) { assert(book.bid_levels() == 1 && book.ask_levels() == 1 && book.orders() == 2); }
        if (step == 3) { assert(book.orders() == 2); }                       // E: bid 100 -> 60, still resting
        if (step == 5) { assert(book.best_bid(p) && p == 1000000); }         // X: bid 60 -> 50
        if (step == 6) { assert(book.best_ask(p) && p == 1020000 && book.orders() == 2); }  // U moved the ask
    }
    assert(book.bid_levels() == 1 && book.ask_levels() == 1 && book.orders() == 2);   // bid @100 deleted, @99 added
    assert(book.best_bid(p) && p == 990000);
    assert(book.best_ask(p) && p == 1020000);
    assert(book.unknown_refs() == 0);
    std::puts("real book: ok");
}

void the_model_replays_the_session_and_matches_the_hand_book() {
    using Stream = ReplayStream<16, 8>;
    static Stream stream;
    std::uint64_t arrival = 0;
    for (const Bytes& m : session()) {
        if (m[0] == 'S' || m[0] == 'R' || itch_tools::locate_of(m.data()) != 7) continue;
        assert(stream.push(m.data(), m.size(), arrival));
        arrival += 40;
    }
    assert(stream.count() == 8);
    static BasicTickToTradePipeline<Stream> pipeline(stream);
    DiscreteEngine engine;
    assert(engine.on_posedge(&BasicTickToTradePipeline<Stream>::posedge_callback, &pipeline));
    assert(engine.on_negedge(&BasicTickToTradePipeline<Stream>::negedge_callback, &pipeline));
    while (!pipeline.done() && engine.cycle() < 100000) engine.step();
    assert(pipeline.ok() && pipeline.completed() == 8 && pipeline.dma_posted() == 8);
    // Final quote after the last message: bid 25 @ 99.0000, ask 70 @ 102.0000.
    const BookQuote& q = pipeline.message_quote(7);
    assert(q.bid_price == 990000 && q.bid_qty == 25 && q.ask_price == 1020000 && q.ask_qty == 70);
    assert(pipeline.book_overflow_count() == 0);
    std::array<std::uint32_t, 16> scratch{};
    const LatencyStats s = pipeline.stats_with(scratch.data());
    assert(s.count == 8 && s.min >= 10 && s.max < 64);   // finite, small, and well formed
    std::puts("model replay: ok");
}

void a_reference_the_model_cannot_resolve_stops_it_visibly() {
    using Stream = ReplayStream<4, 4>;
    static Stream stream;
    Bytes e = exec('E', 7, 1, 12345, 10);   // executes an order that was never added
    assert(stream.push(e.data(), e.size(), 0));
    static BasicTickToTradePipeline<Stream> pipeline(stream);
    DiscreteEngine engine;
    assert(engine.on_posedge(&BasicTickToTradePipeline<Stream>::posedge_callback, &pipeline));
    assert(engine.on_negedge(&BasicTickToTradePipeline<Stream>::negedge_callback, &pipeline));
    while (!pipeline.done() && engine.cycle() < 1000) engine.step();
    assert(!pipeline.ok());
    std::puts("unresolved reference: ok");
}

} // namespace

int main() {
    parser_decodes_the_new_types_at_the_spec_offsets();
    walker_counts_the_mix_and_flags_bad_framing();
    real_book_follows_the_session_by_hand();
    the_model_replays_the_session_and_matches_the_hand_book();
    a_reference_the_model_cannot_resolve_stops_it_visibly();
    std::puts("test-real-itch: all passed");
    return 0;
}
