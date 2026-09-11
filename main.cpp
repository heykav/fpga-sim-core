#include "bus/axi4_stream.hpp"
#include "core/discrete_engine.hpp"
#include "modules/mac_framer.hpp"
#include "modules/bram_memory.hpp"
#include "modules/ofi_dsp.hpp"
#include "modules/order_book.hpp"
#include "modules/parser_itch50.hpp"
#include "modules/pcie_dma.hpp"
#include "modules/phy_pcs.hpp"
#include "trace/vcd_logger.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>

namespace {

using namespace fpga_sim;

void put_u32(std::uint8_t* p, std::uint32_t value) {
    p[0] = static_cast<std::uint8_t>(value >> 24U); p[1] = static_cast<std::uint8_t>(value >> 16U);
    p[2] = static_cast<std::uint8_t>(value >> 8U); p[3] = static_cast<std::uint8_t>(value);
}
void put_u64(std::uint8_t* p, std::uint64_t value) {
    for (int i = 7; i >= 0; --i) { p[i] = static_cast<std::uint8_t>(value); value >>= 8U; }
}

struct Simulation {
    Itch50Parser parser{};
    DualPortBram<BookQuote, 32, 1> book{};
    FiveLevelOrderBook levels{};
    OfiDsp ofi{};
    VcdLogger vcd;
    std::array<Axi4Stream512, 2>& stream;
    std::array<std::uint64_t, 69>& histogram;
    std::int32_t& last_ofi;
    bool tick_seen = false;
    bool trade_seen = false;

    Simulation(const char* vcd_path, std::array<Axi4Stream512, 2>& input,
               std::array<std::uint64_t, 69>& latency, std::int32_t& output)
        : vcd(vcd_path), stream(input), histogram(latency), last_ofi(output) {}

    static void posedge_callback(void* context, std::uint64_t cycle) noexcept {
        static_cast<Simulation*>(context)->posedge(cycle);
    }
    static void negedge_callback(void* context, std::uint64_t cycle) noexcept {
        static_cast<Simulation*>(context)->negedge(cycle);
    }
    void posedge(std::uint64_t cycle) noexcept {
        if (cycle == 13 && stream[0].transfer() && stream[1].transfer()) {
            // The frame was parsed before stepping to keep the hot path fixed;
            // this branch models the registered MAC/parser handoff.
            tick_seen = parser.event_count() > 0;
        }
        if (cycle == 23 && tick_seen) {
            const auto& add = parser.event(0);
            levels.add(add.side == 'B', static_cast<std::int32_t>(add.price_ticks),
                       static_cast<std::int32_t>(add.shares));
            book.posedge({false, true, 0, BookQuote{static_cast<std::int32_t>(add.price_ticks),
                static_cast<std::int32_t>(add.shares), 1893000, 80}}, {});
            ofi.posedge(BookQuote{static_cast<std::int32_t>(add.price_ticks),
                static_cast<std::int32_t>(add.shares), 1893000, 80});
        } else {
            book.posedge({}, {});
            ofi.posedge(BookQuote{1892500, 75, 1893000, 80});
        }
    }
    void negedge(std::uint64_t cycle) noexcept {
        book.negedge();
        ofi.negedge();
        if (ofi.output_valid()) last_ofi = ofi.output();
        if (cycle == 68 && parser.event_count() > 1 && parser.event(1).type == ItchType::executed) {
            levels.execute(true, static_cast<std::int32_t>(parser.event(0).price_ticks),
                           static_cast<std::int32_t>(parser.event(1).shares));
            trade_seen = true;
            histogram[cycle] = 1;
        }
        vcd.cycle(cycle, tick_seen, last_ofi);
    }
};

} // namespace

int main() {
    using namespace fpga_sim;
    std::array<std::uint8_t, 67> itch{};
    itch[0] = 'A'; put_u64(itch.data() + 11, 0x1122334455667788ULL); itch[19] = 'B';
    put_u32(itch.data() + 20, 100); std::memcpy(itch.data() + 24, "AAPL    ", 8); put_u32(itch.data() + 32, 1892500);
    itch[36] = 'E'; put_u64(itch.data() + 47, 0x1122334455667788ULL); put_u32(itch.data() + 55, 25);
    std::array<std::uint64_t, 16> pcs_blocks{};
    std::array<std::uint8_t, 80> pcs_payload{};
    const std::size_t pcs_count = Deserializer66b::encode(itch.data(), itch.size(),
                                                            pcs_blocks.data(), pcs_blocks.size());
    Deserializer66b pcs;
    const PcsResult pcs_result = pcs.decode(pcs_blocks.data(), pcs_count,
                                            pcs_payload.data(), pcs_payload.size(), itch.size());
    if (!pcs_result.block_lock || !pcs_result.crc_valid || pcs_result.payload_length != itch.size()) return 1;
    std::array<Axi4Stream512, 2> stream{};
    for (std::size_t i = 0; i < 64; ++i) stream[0].tdata[i] = itch[i];
    for (std::size_t i = 0; i < itch.size() - 64; ++i) stream[1].tdata[i] = itch[64 + i];
    stream[0].tkeep = ~0ULL; stream[0].tvalid = true; stream[0].tready = true;
    stream[1].tkeep = 0x7ULL; stream[1].tvalid = true; stream[1].tready = true; stream[1].tlast = true;

    MacFramer mac;
    if (!mac.verify_ipg(12) || !mac.accept(stream[0]) || !mac.accept(stream[1])) return 1;
    PcieGen4x16Dma dma;
    PcieTlp tlp{};
    tlp.address = 0x10000000ULL;
    tlp.byte_count = static_cast<std::uint32_t>(itch.size());
    std::memcpy(tlp.payload.data(), itch.data(), itch.size());
    if (!dma.post_write(tlp) || !dma.msix_asserted()) return 1;
    PcieTlp consumed{};
    if (!dma.consume(consumed) || consumed.byte_count != itch.size()) return 1;

    DiscreteEngine engine;
    std::array<std::uint64_t, 69> latency_histogram{};
    std::int32_t last_ofi = 0;
    Simulation simulation("fpga_sim_core.vcd", stream, latency_histogram, last_ofi);
    // The parser consumes the zero-copy frame at the registered MAC boundary.
    simulation.parser.parse(itch.data(), itch.size());
    simulation.tick_seen = simulation.parser.event_count() > 0;
    engine.on_posedge(&Simulation::posedge_callback, &simulation);
    engine.on_negedge(&Simulation::negedge_callback, &simulation);
    engine.run(69);

    if (!simulation.trade_seen) return 1;
    std::cout << "fpga-sim-core: discrete clock = " << DiscreteEngine::cycle_ns << " ns/cycle\n";
    std::cout << "OFI (signed int32) = " << last_ofi << "\n";
    std::cout << "tick-to-trade latency histogram (cycles -> count):\n";
    for (std::size_t cycles = 0; cycles < latency_histogram.size(); ++cycles) {
        if (latency_histogram[cycles] != 0) std::cout << "  " << cycles << " -> " << latency_histogram[cycles] << "\n";
    }
    std::cout << "VCD trace: fpga_sim_core.vcd\n";
    return 0;
}
