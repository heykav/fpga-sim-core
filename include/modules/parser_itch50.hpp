#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace fpga_sim {

// A/E/X are the synthetic stream's types. F, C, D, U are added so real NASDAQ files can be
// replayed: F = Add with MPID (the book treats it as A), C = Executed with price (as E),
// D = Delete (removes all remaining shares), U = Replace (delete + add under a new reference).
enum class ItchType : std::uint8_t {
    add = 'A', add_mpid = 'F', executed = 'E', executed_price = 'C', cancel = 'X',
    delete_order = 'D', replace = 'U', unknown = 0
};

struct ItchEvent {
    ItchType type = ItchType::unknown;
    std::uint64_t order_reference = 0;
    std::uint64_t new_reference = 0;   // Replace only: the reference of the replacement order
    std::uint32_t shares = 0;
    std::uint32_t price_ticks = 0;
    char side = 0;
    std::array<char, 9> stock{};
};

// Why the last parse() call stopped.
enum class ParseStatus : std::uint8_t {
    ok,            // whole buffer consumed
    unknown_type,  // stopped at a message type this parser has no verified length for
    truncated,     // stopped at a known type whose message runs past the buffer end
    event_limit    // stopped because max_events events were already stored
};

class Itch50Parser {
public:
    static constexpr std::size_t max_events = 32;

    void reset() noexcept {
        event_count_ = 0;
        byte_count_ = 0;
        consumed_ = 0;
        status_ = ParseStatus::ok;
    }

    std::size_t parse(const std::uint8_t* bytes, std::size_t length) noexcept {
        event_count_ = 0;
        byte_count_ = length;
        status_ = ParseStatus::ok;
        std::size_t offset = 0;
        while (offset < length) {
            if (event_count_ >= max_events) { status_ = ParseStatus::event_limit; break; }
            const std::size_t message_length = length_for(bytes[offset]);
            if (message_length == 0) { status_ = ParseStatus::unknown_type; break; }
            if (length - offset < message_length) { status_ = ParseStatus::truncated; break; }
            ItchEvent event{};
            if (decode(bytes + offset, message_length, event)) events_[event_count_++] = event;
            offset += message_length;
        }
        consumed_ = offset;
        return event_count_;
    }

    [[nodiscard]] const ItchEvent& event(std::size_t index) const noexcept { return events_[index]; }
    [[nodiscard]] std::size_t event_count() const noexcept { return event_count_; }
    [[nodiscard]] std::size_t byte_count() const noexcept { return byte_count_; }
    // Bytes of the last buffer actually consumed; less than byte_count() when status() != ok.
    [[nodiscard]] std::size_t bytes_consumed() const noexcept { return consumed_; }
    [[nodiscard]] ParseStatus status() const noexcept { return status_; }

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
        case 'F': return 40;
        case 'E': return 31;
        case 'C': return 36;
        case 'X': return 23;
        case 'D': return 19;
        case 'U': return 35;
        default: return 0;
        }
    }
    static bool decode(const std::uint8_t* p, std::size_t length, ItchEvent& out) noexcept {
        if ((length == 36 && p[0] == 'A') || (length == 40 && p[0] == 'F')) {
            out.type = p[0] == 'A' ? ItchType::add : ItchType::add_mpid;
            out.order_reference = u64be(p + 11);
            out.side = static_cast<char>(p[19]);
            out.shares = u32be(p + 20);
            for (int i = 0; i < 8; ++i) out.stock[static_cast<std::size_t>(i)] = static_cast<char>(p[24 + i]);
            out.stock[8] = '\0';
            out.price_ticks = u32be(p + 32);
            return true;
        }
        if ((length == 31 && p[0] == 'E') || (length == 36 && p[0] == 'C')) {
            out.type = p[0] == 'E' ? ItchType::executed : ItchType::executed_price;
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
        if (length == 19 && p[0] == 'D') {
            out.type = ItchType::delete_order;
            out.order_reference = u64be(p + 11);
            return true;
        }
        if (length == 35 && p[0] == 'U') {
            out.type = ItchType::replace;
            out.order_reference = u64be(p + 11);
            out.new_reference = u64be(p + 19);
            out.shares = u32be(p + 27);
            out.price_ticks = u32be(p + 31);
            return true;
        }
        return false;
    }

    std::array<ItchEvent, max_events> events_{};
    std::size_t event_count_ = 0;
    std::size_t byte_count_ = 0;
    std::size_t consumed_ = 0;
    ParseStatus status_ = ParseStatus::ok;
};

} // namespace fpga_sim
