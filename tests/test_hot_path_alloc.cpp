// Counts global heap allocations across the simulation hot paths and fails if
// any occur. Covered paths (each counted separately, see the README table):
//   engine stepping, ITCH parse, order book (+best-level queries), order table,
//   OFI, BRAM, DMA model (post/consume/MSI-X/reset, incl. full-ring reject),
//   AlignedSlab + RingBuffer, PCS encode/decode, CRC32, MAC framer, and a full
//   end-to-end run of the synthetic tick-to-trade pipeline.
// NOT covered: VCD logger (stdio), program output, anything outside the above.
// All global allocation functions are replaced (plain, array, aligned,
// nothrow), and a positive control proves the counter works. Skipped under
// AddressSanitizer, which owns operator new.
#ifdef NDEBUG
#undef NDEBUG  // these tests rely on assert(); never compile them out
#endif
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <stdlib.h>

#if defined(__SANITIZE_ADDRESS__)
#define FPGA_SIM_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define FPGA_SIM_ASAN 1
#endif
#endif

#ifndef FPGA_SIM_ASAN
namespace {
std::size_t g_allocs = 0;
void* counted_alloc(std::size_t n) { ++g_allocs; if (void* p = std::malloc(n ? n : 1)) return p; throw std::bad_alloc(); }
void* counted_aligned(std::size_t n, std::size_t align) {
    ++g_allocs;
    void* p = nullptr;
    if (align < sizeof(void*)) align = sizeof(void*);
    if (posix_memalign(&p, align, n ? n : 1) != 0) throw std::bad_alloc();
    return p;
}
} // namespace
void* operator new(std::size_t n) { return counted_alloc(n); }
void* operator new[](std::size_t n) { return counted_alloc(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { ++g_allocs; return std::malloc(n ? n : 1); }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { ++g_allocs; return std::malloc(n ? n : 1); }
void* operator new(std::size_t n, std::align_val_t a) { return counted_aligned(n, static_cast<std::size_t>(a)); }
void* operator new[](std::size_t n, std::align_val_t a) { return counted_aligned(n, static_cast<std::size_t>(a)); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
#endif

#include "bus/axi4_stream.hpp"
#include "core/aligned_slab.hpp"
#include "core/discrete_engine.hpp"
#include "modules/mac_framer.hpp"
#include "modules/order_table.hpp"
#include "modules/pcie_dma.hpp"
#include "modules/phy_pcs.hpp"
#include "pipeline/synthetic_itch.hpp"
#include "pipeline/tick_to_trade.hpp"
#include "modules/bram_memory.hpp"
#include "modules/ofi_dsp.hpp"
#include "modules/order_book.hpp"
#include "modules/parser_itch50.hpp"

using namespace fpga_sim;

namespace {
struct Rig {
    Itch50Parser parser;
    DualPortBram<BookQuote, 32, 1> bram;
    FiveLevelOrderBook book;
    OfiDsp ofi;
    static void pos(void* c, std::uint64_t cycle) noexcept {
        auto* r = static_cast<Rig*>(c);
        r->book.add(cycle % 2 == 0, static_cast<std::int32_t>(100 + cycle % 9), 5);
        r->book.execute(true, 100, 1);
        r->bram.posedge({false, true, cycle % 32, BookQuote{1, 2, 3, 4}}, {});
        r->ofi.posedge(BookQuote{static_cast<std::int32_t>(cycle), 7, 9, 8});
    }
    static void neg(void* c, std::uint64_t) noexcept {
        auto* r = static_cast<Rig*>(c);
        r->bram.negedge();
        r->ofi.negedge();
    }
};

#ifndef FPGA_SIM_ASAN
struct Path { const char* name; std::size_t allocs; };
std::size_t checked = 0;
Path results[16];

template <typename F>
void audit(const char* name, F&& body) {
    const std::size_t before = g_allocs;
    body();
    results[checked++] = {name, g_allocs - before};
}
#endif
} // namespace

int main() {
#ifdef FPGA_SIM_ASAN
    std::puts("test_hot_path_alloc: SKIPPED under AddressSanitizer");
    return 0;
#else
    // Positive control: the counter must see a direct allocation.
    {
        const std::size_t before = g_allocs;
        ::operator delete(::operator new(8));
        ::operator delete(::operator new(8, std::align_val_t{64}), std::align_val_t{64});
        assert(g_allocs - before == 2);
    }

    static Rig rig;
    static PcieGen4x16Dma dma;
    static RingBuffer<PcieTlp, 4> small_ring;
    static AlignedSlab<std::uint64_t, 16> slab;
    static OrderTable orders;
    static MacFramer mac;
    static SyntheticItchStream stream;
    static TickToTradePipeline pipeline(stream);
    static std::uint8_t msg[36] = {'A'};
    static std::uint8_t payload[40];
    for (std::size_t i = 0; i < sizeof payload; ++i) payload[i] = static_cast<std::uint8_t>(i * 7);

    audit("engine + parse + book + OFI + BRAM", [&] {
        DiscreteEngine engine;
        assert(engine.on_posedge(&Rig::pos, &rig) && engine.on_negedge(&Rig::neg, &rig));
        for (int i = 0; i < 100; ++i) rig.parser.parse(msg, sizeof msg);
        engine.run(1000);
        for (int i = 0; i < 100; ++i) { (void)rig.book.best_bid(); (void)rig.book.best_ask(); }
        rig.ofi.posedge_idle();
    });
    audit("order table", [&] {
        bool bid = false; std::int32_t price = 0, applied = 0;
        for (std::uint64_t r = 0; r < 300; ++r) {
            (void)orders.insert(r, (r & 1U) != 0U, 100, 10);
            (void)orders.reduce(r / 2, 3, bid, price, applied);
        }
    });
    audit("DMA model", [&] {
        PcieTlp tlp{}, out{};
        for (int i = 0; i < 200; ++i) {   // overfills the 64-deep ring: rejected posts must not allocate either
            tlp.tag = static_cast<std::uint32_t>(i);
            (void)dma.post_write(tlp);
            if (i % 3 == 0) { (void)dma.consume(out); dma.clear_msix(); }
        }
        dma.reset();
        assert(dma.post_write(tlp) && dma.consume(out));
    });
    audit("aligned slab + ring buffer", [&] {
        for (std::size_t i = 0; i < slab.entries.size(); ++i) slab.entries[i] = i;
        PcieTlp tlp{}, out{};
        for (int i = 0; i < 50; ++i) { (void)small_ring.push(tlp); if (i % 2) (void)small_ring.pop(out); }
        assert(reinterpret_cast<std::uintptr_t>(&slab) % 64 == 0);
    });
    audit("PCS encode/decode + CRC32", [&] {
        std::uint64_t blocks[16];
        std::uint8_t decoded[80];
        Deserializer66b pcs;
        for (int i = 0; i < 100; ++i) {
            const std::size_t n = Deserializer66b::encode(payload, sizeof payload, blocks, 16);
            const PcsResult r = pcs.decode(blocks, n, decoded, sizeof decoded, sizeof payload);
            assert(r.block_lock && r.crc_valid);
            assert(Crc32::compute(payload, sizeof payload) != 0);
        }
    });
    audit("MAC framer", [&] {
        Axi4Stream512 beat{};
        beat.tvalid = beat.tready = beat.tlast = true;
        for (int i = 0; i < 100; ++i) { (void)mac.verify_ipg(12); assert(mac.accept(beat)); }
    });
    audit("end-to-end synthetic pipeline", [&] {
        DiscreteEngine engine;
        assert(engine.on_posedge(&TickToTradePipeline::posedge_callback, &pipeline) &&
               engine.on_negedge(&TickToTradePipeline::negedge_callback, &pipeline));
        while (!pipeline.done() && engine.cycle() < 100000) engine.step();
        assert(pipeline.ok());
        (void)pipeline.stats();
    });

    std::size_t total = 0;
    for (std::size_t i = 0; i < checked; ++i) {
        std::printf("test_hot_path_alloc: %-40s %zu allocations\n", results[i].name, results[i].allocs);
        total += results[i].allocs;
    }
    std::printf("test_hot_path_alloc: %zu allocations on hot paths\n", total);
    assert(total == 0);
    return 0;
#endif
}
