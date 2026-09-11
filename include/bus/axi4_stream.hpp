#pragma once

#include <array>
#include <cstdint>

namespace fpga_sim {

struct Axi4Stream512 {
    std::array<std::uint8_t, 64> tdata{};
    std::uint64_t tkeep = 0;
    bool tvalid = false;
    bool tready = false;
    bool tlast = false;

    [[nodiscard]] bool transfer() const noexcept { return tvalid && tready; }
    void clear() noexcept {
        tdata.fill(0);
        tkeep = 0;
        tvalid = false;
        tready = false;
        tlast = false;
    }
};

} // namespace fpga_sim
