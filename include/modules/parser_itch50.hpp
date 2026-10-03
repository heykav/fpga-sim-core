#pragma once

// NASDAQ TotalView-ITCH 5.0 parser for the three message types this model
// uses: Add Order (No MPID) 'A' = 36 bytes, Order Executed 'E' = 31 bytes and
// Order Cancel 'X' = 23 bytes. All integer fields are big-endian and are read
// byte by byte (no pointer casts), so there is no alignment requirement on the
// input buffer, no strict-aliasing hazard and no dependence on host byte order.
//
// Messages are framed by type: this parser has no MoldUDP64 length prefix, so
// it can only step over a message whose type it knows. On an unknown type it
// stops (it cannot know where the next message starts). A message of a known
// type whose fields are invalid (see `validate`) is stepped over, counted in
// malformed_count(), and produces no event.

#include <array>
#include <cstddef>
#include <cstdint>

namespace fpga_sim {

enum class ItchType : std::uint8_t { add = 'A', executed = 'E', cancel = 'X', unknown = 0 };

struct ItchEvent {
    ItchType type = ItchType::unknown;
    std::uint16_t stock_locate = 0;
    std::uint16_t tracking_number = 0;
    std::uint64_t timestamp_ns = 0;      // 48-bit on the wire (nanoseconds since midnight)
    std::uint64_t order_reference = 0;
    std::uint32_t shares = 0;            // A: shares, E: executed shares, X: cancelled shares
    std::uint32_t price_ticks = 0;       // A only: Price(4), 4 implied decimals
    std::uint64_t match_number = 0;      // E only
    char side = 0;                       // A only: 'B' or 'S'
    std::array<char, 9> stock{};         // A only: 8 bytes, space padded, NUL terminated here

    friend bool operator==(const ItchEvent&, const ItchEvent&) = default;
};

// Why the last parse() call stopped.
enum class ParseStatus : std::uint8_t {
    ok,            // whole buffer consumed
    unknown_type,  // stopped at a message type this parser has no verified length for
    truncated,     // stopped at a known type whose message runs past the buffer end
    event_limit,   // stopped because max_events events were already stored
    null_buffer    // bytes == nullptr with length > 0: nothing was read
};

struct Itch50Layout {
    static constexpr std::size_t add_length = 36;
    static constexpr std::size_t executed_length = 31;
    static constexpr std::size_t cancel_length = 23;
    static constexpr std::size_t max_length = add_length;
    static constexpr std::uint64_t timestamp_limit = 1ULL << 48U;  // exclusive

    // Wire length for a message type byte, 0 if this model does not know it.
    static constexpr std::size_t length_for(std::uint8_t type) noexcept {
        switch (type) {
        case 'A': return add_length;
        case 'E': return executed_length;
        case 'X': return cancel_length;
        default: return 0;
        }
    }
};

class Itch50Parser {
public:
    static constexpr std::size_t max_events = 32;

    void reset() noexcept {
        event_count_ = 0;
        byte_count_ = 0;
        consumed_ = 0;
        malformed_ = 0;
        status_ = ParseStatus::ok;
    }

    std::size_t parse(const std::uint8_t* bytes, std::size_t length) noexcept {
        event_count_ = 0;
        byte_count_ = length;
        consumed_ = 0;
        malformed_ = 0;
        status_ = ParseStatus::ok;
        if (bytes == nullptr && length != 0) { status_ = ParseStatus::null_buffer; return 0; }
        std::size_t offset = 0;
        while (offset < length) {
            if (event_count_ >= max_events) { status_ = ParseStatus::event_limit; break; }
            const std::size_t message_length = Itch50Layout::length_for(bytes[offset]);
            if (message_length == 0) { status_ = ParseStatus::unknown_type; break; }
            if (length - offset < message_length) { status_ = ParseStatus::truncated; break; }
            ItchEvent event{};
            if (decode(bytes + offset, message_length, event)) events_[event_count_++] = event;
            else ++malformed_;
            offset += message_length;
        }
        consumed_ = offset;
        return event_count_;
    }

    // Decodes exactly one message of `length` bytes. False (and `out`
    // untouched) if the type is not A/E/X, the length does not match the type,
    // or a field is invalid.
    static bool decode(const std::uint8_t* p, std::size_t length, ItchEvent& out) noexcept {
        if (p == nullptr || length == 0 || Itch50Layout::length_for(p[0]) != length) return false;
        ItchEvent e{};
        e.stock_locate = u16be(p + 1);
        e.tracking_number = u16be(p + 3);
        e.timestamp_ns = u48be(p + 5);
        e.order_reference = u64be(p + 11);
        switch (p[0]) {
        case 'A':
            e.type = ItchType::add;
            e.side = static_cast<char>(p[19]);
            e.shares = u32be(p + 20);
            for (std::size_t i = 0; i < 8; ++i) e.stock[i] = static_cast<char>(p[24 + i]);
            e.stock[8] = '\0';
            e.price_ticks = u32be(p + 32);
            break;
        case 'E':
            e.type = ItchType::executed;
            e.shares = u32be(p + 19);
            e.match_number = u64be(p + 23);
            break;
        default:  // 'X' (length_for already rejected every other type)
            e.type = ItchType::cancel;
            e.shares = u32be(p + 19);
            break;
        }
        if (!validate(e)) return false;
        out = e;
        return true;
    }

    // Field checks applied by decode(): an Add must have side 'B' or 'S'.
    // Share counts and prices are passed through as the unsigned wire values;
    // range checks against the book's int32 arithmetic are the consumer's job
    // (see ItchBookApplier).
    static constexpr bool validate(const ItchEvent& e) noexcept {
        if (e.type == ItchType::add) return e.side == 'B' || e.side == 'S';
        return e.type == ItchType::executed || e.type == ItchType::cancel;
    }

    [[nodiscard]] const ItchEvent& event(std::size_t index) const noexcept { return events_[index]; }
    [[nodiscard]] std::size_t event_count() const noexcept { return event_count_; }
    [[nodiscard]] std::size_t byte_count() const noexcept { return byte_count_; }
    // Bytes of the last buffer actually consumed; less than byte_count() when status() != ok.
    [[nodiscard]] std::size_t bytes_consumed() const noexcept { return consumed_; }
    // Known-type messages in the last buffer that were stepped over because a field was invalid.
    [[nodiscard]] std::size_t malformed_count() const noexcept { return malformed_; }
    [[nodiscard]] ParseStatus status() const noexcept { return status_; }

private:
    static std::uint16_t u16be(const std::uint8_t* p) noexcept {
        return static_cast<std::uint16_t>((static_cast<unsigned>(p[0]) << 8U) | p[1]);
    }
    static std::uint32_t u32be(const std::uint8_t* p) noexcept {
        return (static_cast<std::uint32_t>(p[0]) << 24U) | (static_cast<std::uint32_t>(p[1]) << 16U) |
               (static_cast<std::uint32_t>(p[2]) << 8U) | p[3];
    }
    static std::uint64_t u48be(const std::uint8_t* p) noexcept {
        std::uint64_t value = 0;
        for (std::size_t i = 0; i < 6; ++i) value = (value << 8U) | p[i];
        return value;
    }
    static std::uint64_t u64be(const std::uint8_t* p) noexcept {
        std::uint64_t value = 0;
        for (std::size_t i = 0; i < 8; ++i) value = (value << 8U) | p[i];
        return value;
    }

    std::array<ItchEvent, max_events> events_{};
    std::size_t event_count_ = 0;
    std::size_t byte_count_ = 0;
    std::size_t consumed_ = 0;
    std::size_t malformed_ = 0;
    ParseStatus status_ = ParseStatus::ok;
};

// Inverse of Itch50Parser::decode for the same three types. Writes the wire
// bytes and returns the message length, or 0 (writing nothing) if the event
// is not encodable: unknown type, invalid side, timestamp >= 2^48, null output
// or capacity too small. Fields a type does not carry are not written (their
// bytes do not exist in that layout); stock[8] is never written.
struct Itch50Encoder {
    static std::size_t encode(const ItchEvent& e, std::uint8_t* out, std::size_t capacity) noexcept {
        if (out == nullptr || !Itch50Parser::validate(e) || e.timestamp_ns >= Itch50Layout::timestamp_limit)
            return 0;
        const std::size_t length = Itch50Layout::length_for(static_cast<std::uint8_t>(e.type));
        if (length == 0 || capacity < length) return 0;
        out[0] = static_cast<std::uint8_t>(e.type);
        put(out + 1, e.stock_locate, 2);
        put(out + 3, e.tracking_number, 2);
        put(out + 5, e.timestamp_ns, 6);
        put(out + 11, e.order_reference, 8);
        if (e.type == ItchType::add) {
            out[19] = static_cast<std::uint8_t>(e.side);
            put(out + 20, e.shares, 4);
            for (std::size_t i = 0; i < 8; ++i) out[24 + i] = static_cast<std::uint8_t>(e.stock[i]);
            put(out + 32, e.price_ticks, 4);
        } else {
            put(out + 19, e.shares, 4);
            if (e.type == ItchType::executed) put(out + 23, e.match_number, 8);
        }
        return length;
    }

private:
    static void put(std::uint8_t* p, std::uint64_t value, std::size_t bytes) noexcept {
        for (std::size_t i = 0; i < bytes; ++i)
            p[i] = static_cast<std::uint8_t>(value >> ((bytes - 1U - i) * 8U));
    }
};

} // namespace fpga_sim
