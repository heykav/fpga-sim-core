#pragma once

#include "bus/axi4_stream.hpp"

#include <cstddef>
#include <cstdint>

namespace fpga_sim {

class MacFramer {
public:
    static constexpr std::size_t minimum_ipg_bytes = 12;

    void reset() noexcept { previous_end_ = 0; ipg_valid_ = false; }
    bool verify_ipg(std::size_t idle_bytes) noexcept {
        ipg_valid_ = idle_bytes >= minimum_ipg_bytes;
        return ipg_valid_;
    }
    bool accept(const Axi4Stream512& beat) noexcept {
        if (!beat.transfer()) return false;
        previous_end_ = beat.tlast ? 0 : previous_end_ + 64;
        return ipg_valid_ || previous_end_ != 0;
    }
    [[nodiscard]] bool ipg_valid() const noexcept { return ipg_valid_; }

private:
    std::size_t previous_end_ = 0;
    bool ipg_valid_ = false;
};

} // namespace fpga_sim
