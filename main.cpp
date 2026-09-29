// fpga-sim-demo: drives a SYNTHETIC, deterministic ITCH 5.0 stream through the
// PCS -> MAC -> parser -> order book -> quote RAM -> OFI -> DMA model under the
// discrete engine's cycle counter and reports per-message tick-to-trade latency
// in SIMULATED cycles. This is a software model of a pipeline with documented
// stage latencies (see pipeline/tick_to_trade.hpp); it is not a hardware
// measurement and says nothing about any real FPGA.
#include "core/discrete_engine.hpp"
#include "pipeline/synthetic_itch.hpp"
#include "pipeline/tick_to_trade.hpp"
#include "trace/vcd_logger.hpp"

#include <cstdint>
#include <cstdio>

namespace {

using namespace fpga_sim;

struct Ns { unsigned long long whole; unsigned long long frac; };
Ns to_ns(std::uint64_t cycles) {
    const std::uint64_t e4 = DiscreteEngine::cycles_to_ns_e4(cycles);
    return {e4 / 10000ULL, e4 % 10000ULL};
}
void print_cycles_ns(const char* label, std::uint32_t cycles) {
    const Ns ns = to_ns(cycles);
    std::printf("  %-7s %4u cycles = %llu.%04llu ns\n", label, cycles, ns.whole, ns.frac);
}

struct Rig {
    SyntheticItchStream stream{};
    TickToTradePipeline pipeline{stream};
    VcdLogger vcd{"fpga_sim_core.vcd"};
    static void vcd_negedge(void* context, std::uint64_t cycle) noexcept {
        auto* r = static_cast<Rig*>(context);
        r->vcd.cycle(cycle, r->pipeline.wire_busy(), r->pipeline.last_ofi());
    }
};

} // namespace

int main() {
    static Rig rig;  // large fixed-size state: static storage, no heap
    DiscreteEngine engine;
    if (!engine.on_posedge(&TickToTradePipeline::posedge_callback, &rig.pipeline) ||
        !engine.on_negedge(&TickToTradePipeline::negedge_callback, &rig.pipeline) ||
        !engine.on_negedge(&Rig::vcd_negedge, &rig)) return 1;
    constexpr std::uint64_t cycle_limit = 100000;
    while (!rig.pipeline.done() && engine.cycle() < cycle_limit) engine.step();
    if (!rig.pipeline.ok()) { std::fprintf(stderr, "pipeline failed at cycle %llu\n",
                                           static_cast<unsigned long long>(engine.cycle())); return 1; }

    const LatencyStats s = rig.pipeline.stats();
    const SyntheticItchStream& st = rig.stream;
    std::printf("fpga-sim-core: discrete clock = 322.265625 MHz = 512/165 ns/cycle (%.5f)\n",
                DiscreteEngine::cycle_ns);
    std::printf("SYNTHETIC ITCH 5.0 stream (fixed-seed PRNG 0x%08X, not market data): %zu messages "
                "(A=%zu E=%zu X=%zu)\n", SyntheticItchStream::seed, SyntheticItchStream::message_count,
                st.count_of('A'), st.count_of('E'), st.count_of('X'));
    std::printf("simulated %llu cycles; %zu decision records posted to the DMA ring\n",
                static_cast<unsigned long long>(engine.cycle()), rig.pipeline.dma_posted());
    std::printf("tick-to-trade latency, simulated cycles (model with documented stage latencies; "
                "not a hardware measurement):\n");
    print_cycles_ns("min", s.min);
    print_cycles_ns("median", s.median);
    print_cycles_ns("p99", s.p99);
    print_cycles_ns("max", s.max);

    std::uint32_t peak = 0;
    for (auto c : s.histogram) if (c > peak) peak = c;
    std::printf("histogram (2-cycle bins, last bin includes overflow):\n");
    for (std::size_t b = 0; b < LatencyStats::histogram_bins; ++b) {
        if (s.histogram[b] == 0) continue;
        const std::uint32_t lo = static_cast<std::uint32_t>(b) * LatencyStats::bin_width_cycles;
        const std::uint32_t bar = (s.histogram[b] * 40U + peak - 1U) / peak;
        std::printf("  %3u-%3u cycles %4u ", lo, lo + LatencyStats::bin_width_cycles - 1U, s.histogram[b]);
        for (std::uint32_t i = 0; i < bar; ++i) std::putchar('#');
        std::putchar('\n');
    }

    std::size_t nonzero = 0;
    std::int32_t lo = 0, hi = 0;
    for (std::size_t i = 0; i < SyntheticItchStream::message_count; ++i) {
        const std::int32_t v = rig.pipeline.message_ofi(i);
        if (v != 0) ++nonzero;
        if (v < lo) lo = v;
        if (v > hi) hi = v;
    }
    std::printf("OFI from best-level changes (Cont-Kukanov-Stoikov, shares): cumulative = %lld, "
                "per-message min/max = %d/%d, nonzero on %zu of %zu messages\n",
                static_cast<long long>(rig.pipeline.cumulative_ofi()), lo, hi, nonzero,
                SyntheticItchStream::message_count);
    std::printf("order book adds dropped (side full): %llu\n",
                static_cast<unsigned long long>(rig.pipeline.book_overflow_count()));
    std::printf("VCD trace: fpga_sim_core.vcd\n");
    return rig.pipeline.book_overflow_count() == 0 && rig.pipeline.cumulative_ofi() != 0 ? 0 : 1;
}
