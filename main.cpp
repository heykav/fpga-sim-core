#include "bus/axi4_stream.hpp"
#include "core/discrete_engine.hpp"
#include "modules/mac_framer.hpp"
#include "modules/bram_memory.hpp"
#include "modules/ofi_dsp.hpp"
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

void put_u32(std::uint8_t* p, std::uint32_t value) {
    p[0] = static_cast<std::uint8_t>(value >> 24U); p[1] = static_cast<std::uint8_t>(value >> 16U);
    p[2] = static_cast<std::uint8_t>(value >> 8U); p[3] = static_cast<std::uint8_t>(value);
}
void put_u64(std::uint8_t* p, std::uint64_t value) {
    for (int i = 7; i >= 0; --i) { p[i] = static_cast<std::uint8_t>(value); value >>= 8U; }
}

} // namespace

int main() {
    using namespace fpga_sim;
    std::array<std::uint8_t, 67> itch{};
    itch[0] = 'A'; put_u64(itch.data() + 11, 0x1122334455667788ULL); itch[19] = 'B';
    put_u32(itch.data() + 20, 100); std::memcpy(itch.data() + 24, "AAPL    ", 8); put_u32(itch.data() + 32, 1892500);
    itch[36] = 'E'; put_u64(itch.data() + 47, 0x1122334455667788ULL); put_u32(itch.data() + 55, 25);
    Itch50Parser parser;
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

    DualPortBram<BookQuote, 32, 1> book;
    OfiDsp ofi;
    VcdLogger vcd("fpga_sim_core.vcd");
    DiscreteEngine engine;
    std::array<std::uint64_t, 69> latency_histogram{};
    std::int32_t last_ofi = 0;
    bool tick_seen = false;
    bool trade_seen = false;

    engine.on_posedge([&](std::uint64_t cycle) {
        if (cycle == 13 && stream[0].transfer() && stream[1].transfer()) {
            parser.parse(itch.data(), itch.size());
            tick_seen = parser.event_count() > 0;
        }
        if (cycle == 23 && tick_seen) {
            const auto& add = parser.event(0);
            book.posedge({false, true, 0, BookQuote{static_cast<std::int32_t>(add.price_ticks), static_cast<std::int32_t>(add.shares), 1893000, 80}}, {});
            ofi.posedge(BookQuote{static_cast<std::int32_t>(add.price_ticks), static_cast<std::int32_t>(add.shares), 1893000, 80});
        } else {
            book.posedge({}, {});
            ofi.posedge(BookQuote{1892500, 75, 1893000, 80});
        }
    });
    engine.on_negedge([&](std::uint64_t cycle) {
        book.negedge(); ofi.negedge();
        if (ofi.output_valid()) last_ofi = ofi.output();
        if (cycle == 68 && parser.event_count() > 1 && parser.event(1).type == ItchType::executed) {
            trade_seen = true;
            latency_histogram[cycle] = 1;
        }
        vcd.cycle(cycle, tick_seen, last_ofi);
    });
    engine.run(69);

    if (!trade_seen) return 1;
    std::cout << "fpga-sim-core: discrete clock = " << DiscreteEngine::cycle_ns << " ns/cycle\n";
    std::cout << "OFI (signed int32) = " << last_ofi << "\n";
    std::cout << "tick-to-trade latency histogram (cycles -> count):\n";
    for (std::size_t cycles = 0; cycles < latency_histogram.size(); ++cycles) {
        if (latency_histogram[cycles] != 0) std::cout << "  " << cycles << " -> " << latency_histogram[cycles] << "\n";
    }
    std::cout << "VCD trace: fpga_sim_core.vcd\n";
    return 0;
}
