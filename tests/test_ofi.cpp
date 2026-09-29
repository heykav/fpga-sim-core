// OfiDsp against a hand-computed sequence of best-quote updates.
// e_n = [Pb>=Pb'] qb - [Pb<=Pb'] qb' - [Pa<=Pa'] qa + [Pa>=Pa'] qa'  (primes = previous quote)
#ifdef NDEBUG
#undef NDEBUG  // these tests rely on assert(); never compile them out
#endif
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <limits>

#include "modules/ofi_dsp.hpp"

using fpga_sim::BookQuote;
using fpga_sim::OfiDsp;

namespace {

void hand_computed_sequence() {
    struct Step { BookQuote q; std::int32_t expected; };
    const Step steps[] = {
        // first quote: bid +10, ask -20                                   = -10
        {{100, 10, 101, 20}, -10},
        // bid same price: 15-10 = +5; ask same price: 20-20 = 0          = +5
        {{100, 15, 101, 20}, 5},
        // bid price up: +7; ask price up: +previous ask qty 20            = +27
        {{101, 7, 102, 20}, 27},
        // bid same: 4-7 = -3; ask same: 20-12 = +8                        = +5
        {{101, 4, 102, 12}, 5},
        // bid price down: -previous bid qty 4; ask same: 12-12 = 0        = -4
        {{100, 9, 102, 12}, -4},
        // bid same: 9-9 = 0; ask price down: -new ask qty 30              = -30
        {{100, 9, 101, 30}, -30},
    };
    constexpr std::size_t n = sizeof(steps) / sizeof(steps[0]);
    std::int64_t cumulative = 0;
    OfiDsp ofi;
    std::size_t seen = 0;
    // Feed n quotes, then flush with idle cycles; the value for quote k appears at the
    // negedge of cycle k + 2 (3-stage pipeline), never earlier.
    for (std::size_t cycle = 0; cycle < n + OfiDsp::pipeline_stages; ++cycle) {
        if (cycle < n) ofi.posedge(steps[cycle].q); else ofi.posedge_idle();
        ofi.negedge();
        if (cycle < OfiDsp::pipeline_stages - 1) {
            assert(!ofi.output_valid());
        } else if (cycle - (OfiDsp::pipeline_stages - 1) < n) {
            assert(ofi.output_valid());
            const std::size_t k = cycle - (OfiDsp::pipeline_stages - 1);
            assert(ofi.output() == steps[k].expected);
            cumulative += ofi.output();
            ++seen;
        } else {
            assert(!ofi.output_valid());
        }
    }
    assert(seen == n);
    assert(cumulative == -7);  // -10 + 5 + 27 + 5 - 4 - 30
}

void idle_cycles_insert_bubbles_and_keep_previous_quote() {
    OfiDsp ofi;
    ofi.posedge({100, 10, 101, 20}); ofi.negedge();
    ofi.posedge_idle(); ofi.negedge();
    ofi.posedge({100, 13, 101, 20}); ofi.negedge();   // +3 relative to the quote before the idle cycle
    assert(ofi.output_valid() && ofi.output() == -10);
    ofi.posedge_idle(); ofi.negedge();
    assert(!ofi.output_valid());
    ofi.posedge_idle(); ofi.negedge();
    assert(ofi.output_valid() && ofi.output() == 3);
}

void output_saturates_to_int32() {
    constexpr std::int32_t big = std::numeric_limits<std::int32_t>::max();
    OfiDsp ofi;
    ofi.posedge({100, 1, 101, big});   ofi.negedge();
    ofi.posedge({101, big, 102, 1});   ofi.negedge();  // bid up: +big; ask up: +big => saturates
    ofi.posedge_idle();                ofi.negedge();
    ofi.posedge_idle();                ofi.negedge();
    assert(ofi.output_valid() && ofi.output() == big);
}

} // namespace

int main() {
    hand_computed_sequence();
    idle_cycles_insert_bubbles_and_keep_previous_quote();
    output_saturates_to_int32();
    std::puts("test_ofi: all assertions passed");
    return 0;
}
