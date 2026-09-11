#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace fpga_sim {

class VcdLogger {
public:
    explicit VcdLogger(const char* path) : file_(std::fopen(path, "w")) {
        if (file_ != nullptr) {
            std::fprintf(file_, "$timescale 1ns $end\n$scope module fpga_sim $end\n");
            std::fprintf(file_, "$var wire 1 ! clk $end\n$var wire 1 \" tick_valid $end\n");
            std::fprintf(file_, "$var wire 32 # ofi $end\n$upscope $end\n$enddefinitions $end\n");
            std::fprintf(file_, "#0\n0!\n0\"\n");
        }
    }
    ~VcdLogger() { if (file_ != nullptr) std::fclose(file_); }
    VcdLogger(const VcdLogger&) = delete;
    VcdLogger& operator=(const VcdLogger&) = delete;

    void cycle(std::uint64_t cycle, bool tick_valid, std::int32_t ofi) noexcept {
        if (file_ == nullptr) return;
        std::fprintf(file_, "#%llu\n%u!\n%u\"\nb%08x #\n",
                     static_cast<unsigned long long>(cycle), 1U, tick_valid ? 1U : 0U,
                     static_cast<unsigned>(ofi));
    }

private:
    std::FILE* file_ = nullptr;
};

} // namespace fpga_sim
