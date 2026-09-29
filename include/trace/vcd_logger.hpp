#pragma once

#include <cstdint>
#include <cstdio>

namespace fpga_sim {

// Minimal VCD writer. Time unit is 100 fs so one 3.1032 ns cycle is exactly
// 31032 units (no rounding drift). Each cycle() call emits a rising clock edge
// at the start of the cycle and a falling edge half a cycle later.
class VcdLogger {
public:
    static constexpr std::uint64_t units_per_cycle = 31032;  // 3.1032 ns / 100 fs
    static constexpr std::uint64_t units_per_half_cycle = units_per_cycle / 2;

    explicit VcdLogger(const char* path) : file_(std::fopen(path, "w")) {
        if (file_ != nullptr) {
            std::fprintf(file_, "$timescale 100 fs $end\n$scope module fpga_sim $end\n");
            std::fprintf(file_, "$var wire 1 ! clk $end\n$var wire 1 \" tick_valid $end\n");
            std::fprintf(file_, "$var wire 32 # ofi $end\n$upscope $end\n$enddefinitions $end\n");
            std::fprintf(file_, "#0\n$dumpvars\n0!\n0\"\nb0 #\n$end\n");
            last_time_ = 0;
        }
    }
    ~VcdLogger() { if (file_ != nullptr) std::fclose(file_); }
    VcdLogger(const VcdLogger&) = delete;
    VcdLogger& operator=(const VcdLogger&) = delete;

    void cycle(std::uint64_t cycle, bool tick_valid, std::int32_t ofi) noexcept {
        if (file_ == nullptr) return;
        const std::uint64_t t = cycle * units_per_cycle;
        stamp(t);
        std::fprintf(file_, "1!\n%c\"\n", tick_valid ? '1' : '0');
        write_vector(static_cast<std::uint32_t>(ofi));
        stamp(t + units_per_half_cycle);
        std::fputs("0!\n", file_);
    }

private:
    void stamp(std::uint64_t t) noexcept {
        if (t != last_time_) {
            std::fprintf(file_, "#%llu\n", static_cast<unsigned long long>(t));
            last_time_ = t;
        }
    }
    void write_vector(std::uint32_t value) noexcept {
        char bits[34];
        bits[0] = 'b';
        for (int i = 0; i < 32; ++i) bits[1 + i] = ((value >> (31 - i)) & 1U) ? '1' : '0';
        bits[33] = '\0';
        std::fprintf(file_, "%s #\n", bits);
    }

    std::FILE* file_ = nullptr;
    std::uint64_t last_time_ = 0;
};

} // namespace fpga_sim
