#pragma once

// Message source for replaying REAL NASDAQ ITCH 5.0 messages through the same pipeline model
// as the synthetic stream. Messages are copied byte-for-byte from the file; only the arrival
// schedule is derived (see tools/itch_replay.cpp). Fixed capacity, no heap.

#include "modules/ofi_dsp.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace fpga_sim {

struct ReplayMessage {
    std::array<std::uint8_t, 40> bytes{};   // longest supported message is F = 40 bytes
    std::uint8_t length = 0;
    char type = 0;
    std::uint64_t arrival_cycle = 0;
};

template <std::size_t Capacity, std::size_t OrderCapacity>
class ReplayStream {
public:
    static constexpr std::size_t message_count = Capacity;
    static constexpr std::size_t order_capacity = OrderCapacity;
    static constexpr std::int32_t empty_ask_price = std::numeric_limits<std::int32_t>::max();

    // False if full or the message does not fit.
    bool push(const std::uint8_t* bytes, std::size_t length, std::uint64_t arrival_cycle) noexcept {
        if (count_ >= Capacity || length == 0 || length > 40) return false;
        ReplayMessage& m = messages_[count_++];
        std::memcpy(m.bytes.data(), bytes, length);
        m.length = static_cast<std::uint8_t>(length);
        m.type = static_cast<char>(bytes[0]);
        m.arrival_cycle = arrival_cycle;
        return true;
    }
    [[nodiscard]] std::size_t count() const noexcept { return count_; }
    [[nodiscard]] const ReplayMessage& message(std::size_t i) const noexcept { return messages_[i]; }

private:
    std::array<ReplayMessage, Capacity> messages_{};
    std::size_t count_ = 0;
};

} // namespace fpga_sim
