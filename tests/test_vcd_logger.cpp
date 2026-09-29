#ifdef NDEBUG
#undef NDEBUG  // these tests rely on assert(); never compile them out
#endif
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "core/discrete_engine.hpp"
#include "trace/vcd_logger.hpp"

int main() {
    const char* path = "test_vcd_logger.vcd";
    {
        fpga_sim::VcdLogger vcd(path);
        vcd.cycle(0, false, 0);
        vcd.cycle(1, true, 5);
        vcd.cycle(2, true, -1);
        vcd.cycle(3, false, -2147483647 - 1);
    }
    std::ifstream in(path);
    assert(in.good());
    std::string line;
    bool header_done = false, in_dumpvars = false;
    std::vector<std::uint64_t> times;
    std::set<std::uint64_t> seen;
    std::vector<char> clk;
    std::vector<std::string> ofi;
    bool saw_timescale = false;
    while (std::getline(in, line)) {
        if (line.rfind("$timescale 1 fs", 0) == 0) saw_timescale = true;
        if (line == "$enddefinitions $end") { header_done = true; continue; }
        if (!header_done) continue;
        if (line == "$dumpvars") { in_dumpvars = true; continue; }
        if (line == "$end") { in_dumpvars = false; continue; }
        if (line[0] == '#') {
            const std::uint64_t t = std::strtoull(line.c_str() + 1, nullptr, 10);
            assert(times.empty() || t > times.back());  // strictly increasing => no duplicate #0
            times.push_back(t);
            continue;
        }
        if (line[0] == 'b') {
            const auto sp = line.find(' ');
            assert(sp != std::string::npos && line.substr(sp + 1) == "#");
            for (std::size_t i = 1; i < sp; ++i) assert(line[i] == '0' || line[i] == '1');  // binary only
            if (!in_dumpvars) ofi.push_back(line.substr(1, sp - 1));
            continue;
        }
        if (line.size() == 2 && line[1] == '!') { assert(line[0] == '0' || line[0] == '1'); if (!in_dumpvars) clk.push_back(line[0]); }
    }
    assert(saw_timescale && header_done);
    // 4 cycles -> 8 distinct timestamps (rise + fall each), starting at #0.
    assert(times.size() == 8 && times.front() == 0);
    // Cycle N rises at N * 3103030 fs (1/322.265625 MHz = 3.10303.. ns, rounded to whole fs); falls half a cycle later.
    for (std::uint64_t c = 0; c < 4; ++c) {
        assert(times[2 * c] == c * 3103030);
        assert(times[2 * c + 1] == c * 3103030 + 1551515);
    }
    static_assert(fpga_sim::VcdLogger::units_per_cycle == 3103030);
    // Clock actually toggles.
    assert(clk.size() == 8);
    for (std::size_t i = 0; i < clk.size(); ++i) assert(clk[i] == (i % 2 == 0 ? '1' : '0'));
    // Vector values are correct 32-bit two's complement binary.
    assert(ofi.size() == 4);
    assert(ofi[0] == std::string(32, '0'));
    assert(ofi[1] == std::string(29, '0') + "101");
    assert(ofi[2] == std::string(32, '1'));
    assert(ofi[3] == "1" + std::string(31, '0'));
    std::remove(path);
    std::puts("test_vcd_logger: all assertions passed");
    return 0;
}
