#ifdef NDEBUG
#undef NDEBUG  // these tests rely on assert(); never compile them out
#endif
#include <cassert>
#include <cstdio>

#include "core/discrete_engine.hpp"

using fpga_sim::DiscreteEngine;

namespace {
void tick(void* ctx, std::uint64_t) noexcept { ++*static_cast<int*>(ctx); }

void registration_is_bounds_checked() {
    DiscreteEngine engine;
    int pos = 0, neg = 0;
    for (std::size_t i = 0; i < DiscreteEngine::max_callbacks; ++i) {
        assert(engine.on_posedge(&tick, &pos));
        assert(engine.on_negedge(&tick, &neg));
    }
    assert(!engine.on_posedge(&tick, &pos));  // 17th is rejected, not UB
    assert(!engine.on_negedge(&tick, &neg));
    assert(!engine.on_posedge(nullptr, &pos));
    engine.run(3);
    assert(pos == 3 * static_cast<int>(DiscreteEngine::max_callbacks));  // rejected ones never ran
    assert(neg == pos);
    assert(engine.cycle() == 3);
}
} // namespace

int main() {
    registration_is_bounds_checked();
    std::puts("test_discrete_engine: all assertions passed");
    return 0;
}
