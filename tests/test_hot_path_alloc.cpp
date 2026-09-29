// Counts global heap allocations across the simulation hot path (engine
// stepping, ITCH parsing, order book, OFI pipeline, BRAM). Fails if any occur.
// Skipped under AddressSanitizer, which owns operator new.
#ifdef NDEBUG
#undef NDEBUG  // these tests rely on assert(); never compile them out
#endif
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <new>

#if defined(__SANITIZE_ADDRESS__)
#define FPGA_SIM_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define FPGA_SIM_ASAN 1
#endif
#endif

#ifndef FPGA_SIM_ASAN
namespace { std::size_t g_allocs = 0; }
void* operator new(std::size_t n) { ++g_allocs; if (void* p = std::malloc(n ? n : 1)) return p; throw std::bad_alloc(); }
void* operator new[](std::size_t n) { ++g_allocs; if (void* p = std::malloc(n ? n : 1)) return p; throw std::bad_alloc(); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
#endif

#include "core/discrete_engine.hpp"
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
} // namespace

int main() {
#ifdef FPGA_SIM_ASAN
    std::puts("test_hot_path_alloc: SKIPPED under AddressSanitizer");
    return 0;
#else
    static Rig rig;
    DiscreteEngine engine;
    assert(engine.on_posedge(&Rig::pos, &rig) && engine.on_negedge(&Rig::neg, &rig));
    std::uint8_t msg[36] = {'A'};
    const std::size_t before = g_allocs;
    for (int i = 0; i < 100; ++i) rig.parser.parse(msg, sizeof msg);
    engine.run(1000);
    const std::size_t used = g_allocs - before;
    std::printf("test_hot_path_alloc: %zu allocations on hot path\n", used);
    assert(used == 0);
    return 0;
#endif
}
