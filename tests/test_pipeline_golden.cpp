// Golden-output test for the synthetic tick-to-trade pipeline. The stream is
// SYNTHETIC (fixed-seed integer PRNG) and all decision paths are integer-only,
// so the statistics below must be bit-identical on every run and platform.
// A change to any stage latency, the generator, or the modules changes these
// numbers on purpose: update them together with the README.
#ifdef NDEBUG
#undef NDEBUG  // these tests rely on assert(); never compile them out
#endif
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>

#include "core/discrete_engine.hpp"
#include "pipeline/synthetic_itch.hpp"
#include "pipeline/tick_to_trade.hpp"

using namespace fpga_sim;

namespace {

struct Run {
    SyntheticItchStream stream{};
    TickToTradePipeline pipeline{stream};
    std::uint64_t cycles = 0;
    explicit Run() {
        DiscreteEngine engine;
        const bool registered = engine.on_posedge(&TickToTradePipeline::posedge_callback, &pipeline) &&
                                engine.on_negedge(&TickToTradePipeline::negedge_callback, &pipeline);
        assert(registered);
        (void)registered;
        while (!pipeline.done() && engine.cycle() < 100000) engine.step();
        cycles = engine.cycle();
    }
};

// Independent reference implementation of the Cont-Kukanov-Stoikov event OFI.
std::int64_t reference_ofi(const BookQuote& p, const BookQuote& q, bool first) {
    if (first) return static_cast<std::int64_t>(q.bid_qty) - q.ask_qty;
    std::int64_t e = 0;
    if (q.bid_price >= p.bid_price) e += q.bid_qty;
    if (q.bid_price <= p.bid_price) e -= p.bid_qty;
    if (q.ask_price <= p.ask_price) e -= q.ask_qty;
    if (q.ask_price >= p.ask_price) e += p.ask_qty;
    return e;
}

std::uint64_t fnv1a(const std::uint32_t* v, std::size_t n) {
    std::uint64_t h = 1469598103934665603ULL;
    for (std::size_t i = 0; i < n; ++i)
        for (int b = 0; b < 4; ++b) { h ^= (v[i] >> (8 * b)) & 0xFFU; h *= 1099511628211ULL; }
    return h;
}

} // namespace

int main() {
    static Run a;
    static Run b;
    assert(a.pipeline.ok() && b.pipeline.ok());
    assert(a.pipeline.completed() == SyntheticItchStream::message_count);
    assert(a.pipeline.dma_posted() == SyntheticItchStream::message_count);
    assert(a.pipeline.book_overflow_count() == 0);

    // Determinism: two independent runs agree exactly.
    for (std::size_t i = 0; i < SyntheticItchStream::message_count; ++i) {
        assert(a.pipeline.latencies()[i] == b.pipeline.latencies()[i]);
        assert(a.pipeline.message_ofi(i) == b.pipeline.message_ofi(i));
    }
    assert(a.cycles == b.cycles);

    // Module outputs match the generator's independent bookkeeping.
    std::int64_t expected_cumulative = 0;
    BookQuote previous{};
    for (std::size_t i = 0; i < SyntheticItchStream::message_count; ++i) {
        const BookQuote& want = a.stream.expected_quote(i);
        const BookQuote& got = a.pipeline.message_quote(i);
        assert(got.bid_price == want.bid_price && got.bid_qty == want.bid_qty);
        assert(got.ask_price == want.ask_price && got.ask_qty == want.ask_qty);
        const std::int64_t e = reference_ofi(previous, want, i == 0);
        assert(a.pipeline.message_ofi(i) == e);
        expected_cumulative += e;
        previous = want;
        // Lower bound: wire blocks + MAC 1 + parse 2 + book 1 + RAM read 1 + OFI (issue+3 stages) + DMA 2.
        const std::size_t len = a.stream.message(i).length;
        const std::size_t blocks = (len + 4 + 6) / 7;
        assert(a.pipeline.latencies()[i] >= blocks + 10);
    }
    assert(a.pipeline.cumulative_ofi() == expected_cumulative);
    assert(expected_cumulative != 0);

    // Golden statistics.
    const LatencyStats s = a.pipeline.stats();
    std::printf("golden: cycles=%llu min=%u median=%u p99=%u max=%u ofi=%lld hash=%016llx\n",
                static_cast<unsigned long long>(a.cycles), s.min, s.median, s.p99, s.max,
                static_cast<long long>(a.pipeline.cumulative_ofi()),
                static_cast<unsigned long long>(fnv1a(a.pipeline.latencies(), s.count)));
    std::fflush(stdout);
    assert(s.count == 256);
    assert(a.stream.count_of('A') == 116 && a.stream.count_of('E') == 100 &&
           a.stream.count_of('X') == 40);
    assert(a.cycles == 5339ULL);
    assert(s.min == 14 && s.median == 15 && s.p99 == 19 && s.max == 20);
    assert(a.pipeline.cumulative_ofi() == 400);
    assert(fnv1a(a.pipeline.latencies(), s.count) == 0x4db4895597976b86ULL);
    constexpr std::array<std::uint32_t, LatencyStats::histogram_bins> golden_hist = {{0, 0, 0, 0, 0, 0, 0, 130, 121, 4, 1}};
    for (std::size_t i = 0; i < golden_hist.size(); ++i) assert(s.histogram[i] == golden_hist[i]);

    // ns conversion is exact integer arithmetic: 512/165 ns per cycle, four decimals.
    assert(DiscreteEngine::cycles_to_ns_e4(165) == 5120000);   // 165 cycles = 512 ns exactly
    assert(DiscreteEngine::cycles_to_ns_e4(1) == 31030);       // 3.1030 ns (3.10303 rounded)
    assert(DiscreteEngine::cycles_to_ns_e4(14) == 434424);     // 43.4424 ns

    std::puts("test_pipeline_golden: all assertions passed");
    return 0;
}
