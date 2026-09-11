#pragma once

#include "core/aligned_slab.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace fpga_sim {

struct PcieTlp {
    std::uint64_t address = 0;
    std::uint32_t byte_count = 0;
    std::uint32_t tag = 0;
    std::array<std::uint8_t, 256> payload{};
};

class PcieGen4x16Dma {
public:
    static constexpr std::size_t ring_depth = 64;

    void reset() noexcept {
        ring_ = {};
        posted_ = 0;
        msix_ = false;
    }
    bool post_write(const PcieTlp& tlp) noexcept {
        if (!ring_.push(tlp)) return false;
        ++posted_;
        msix_ = true;
        return true;
    }
    bool consume(PcieTlp& tlp) noexcept { return ring_.pop(tlp); }
    void clear_msix() noexcept { msix_ = false; }
    [[nodiscard]] bool msix_asserted() const noexcept { return msix_; }
    [[nodiscard]] std::size_t posted_count() const noexcept { return posted_; }

private:
    RingBuffer<PcieTlp, ring_depth> ring_{};
    std::size_t posted_ = 0;
    bool msix_ = false;
};

} // namespace fpga_sim
