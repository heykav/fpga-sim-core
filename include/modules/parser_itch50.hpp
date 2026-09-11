#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace fpga_sim {

enum class ItchType : std::uint8_t { add = 'A', executed = 'E', cancel = 'X', unknown = 0 };

struct ItchEvent {
    ItchType type = ItchType::unknown;
    std::uint64_t order_reference = 0;
    std::uint32_t shares = 0;
    std::uint32_t price_ticks = 0;
    char side = 0;
    std::array<char, 9> stock{};
};

class Itch50Parser {
public:
    static constexpr std::size_t max_events = 32;

    void reset() noexcept {
        event_count_ = 0;
        byte_count_ = 0;
    }

    std::size_t parse(const std::uint8_t* bytes, std::size_t length) noexcept {
        event_count_ = 0;
        byte_count_ = length;
        std::size_t offset = 0;
        while (offset < length && event_count_ < max_events) {
            const std::size_t message_length = length_for(bytes[offset]);
            if (message_length == 0 || length - offset < message_length) break;
            ItchEvent event{};
            if (decode(bytes + offset, message_length, event)) events_[event_count_++] = event;
            offset += message_length;
        }
        return event_count_;
    }

    [[nodiscard]] const ItchEvent& event(std::size_t index) const noexcept { return events_[index]; }
    [[nodiscard]] std::size_t event_count() const noexcept { return event_count_; }
    [[nodiscard]] std::size_t byte_count() const noexcept { return byte_count_; }

private:
    static std::uint64_t u64be(const std::uint8_t* p) noexcept {
        std::uint64_t value = 0;
        for (int i = 0; i < 8; ++i) value = (value << 8U) | p[i];
        return value;
    }
    static std::uint32_t u32be(const std::uint8_t* p) noexcept {
        return (static_cast<std::uint32_t>(p[0]) << 24U) | (static_cast<std::uint32_t>(p[1]) << 16U) |
               (static_cast<std::uint32_t>(p[2]) << 8U) | p[3];
    }
    static std::size_t length_for(std::uint8_t type) noexcept {
        switch (type) {
        case 'A': return 36;
        case 'E': return 31;
        case 'X': return 23;
        default: return 0;
        }
    }
    static bool decode(const std::uint8_t* p, std::size_t length, ItchEvent& out) noexcept {
        if (length == 36 && p[0] == 'A') {
            out.type = ItchType::add;
            out.order_reference = u64be(p + 11);
            out.side = static_cast<char>(p[19]);
            out.shares = u32be(p + 20);
            for (int i = 0; i < 8; ++i) out.stock[static_cast<std::size_t>(i)] = static_cast<char>(p[24 + i]);
            out.stock[8] = '\0';
            out.price_ticks = u32be(p + 32);
            return true;
        }
        if (length == 31 && p[0] == 'E') {
            out.type = ItchType::executed;
            out.order_reference = u64be(p + 11);
            out.shares = u32be(p + 19);
            return true;
        }
        if (length == 23 && p[0] == 'X') {
            out.type = ItchType::cancel;
            out.order_reference = u64be(p + 11);
            out.shares = u32be(p + 19);
            return true;
        }
        return false;
    }

    std::array<ItchEvent, max_events> events_{};
    std::size_t event_count_ = 0;
    std::size_t byte_count_ = 0;
};

} // namespace fpga_sim
