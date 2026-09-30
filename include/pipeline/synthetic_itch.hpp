#pragma once

// SYNTHETIC ITCH 5.0 STREAM. Not market data: every byte is produced by a
// fixed-seed integer PRNG (xorshift32), so the stream is identical on every
// run and platform. Messages use the real NASDAQ TotalView-ITCH 5.0 byte
// layouts also used by the unit tests: Add Order (No MPID) 'A' = 36 bytes,
// Order Executed 'E' = 31 bytes, Order Cancel 'X' = 23 bytes, order
// reference at offset 11, integer big-endian fields.
//
// The generator also keeps its own independent bookkeeping of the resting
// orders and publishes the best bid/ask after every message (`expected_quote`),
// which tests use as an oracle against the modules under test.

#include "modules/ofi_dsp.hpp"
#include "modules/parser_itch50.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace fpga_sim {

struct SyntheticMessage {
    std::array<std::uint8_t, 36> bytes{};
    std::uint8_t length = 0;
    char type = 0;
    std::uint64_t arrival_cycle = 0;  // cycle at which the first PCS block reaches the receiver
};

class SyntheticItchStream {
public:
    static constexpr std::size_t message_count = 256;
    static constexpr std::uint32_t seed = 0x5EEDF00DU;
    static constexpr std::int32_t base_bid = 1892500;   // 189.2500 in ITCH 1/10000 price units
    static constexpr std::int32_t base_ask = 1893000;
    static constexpr std::int32_t tick = 100;
    static constexpr std::size_t price_levels = 5;      // distinct prices per side (matches the 5-level book)
    static constexpr std::int32_t empty_ask_price = std::numeric_limits<std::int32_t>::max();

    SyntheticItchStream() noexcept { generate(); }

    [[nodiscard]] const SyntheticMessage& message(std::size_t i) const noexcept { return messages_[i]; }
    // Best bid/ask after message i, from the generator's own bookkeeping.
    // Empty bid side is {0,0}; empty ask side is {empty_ask_price, 0}.
    [[nodiscard]] const BookQuote& expected_quote(std::size_t i) const noexcept { return quotes_[i]; }
    [[nodiscard]] std::size_t count_of(char type) const noexcept {
        return type == 'A' ? adds_ : (type == 'E' ? executes_ : cancels_);
    }

private:
    struct Live { std::uint64_t reference = 0; std::int32_t remaining = 0; std::size_t level = 0; bool bid = false; };

    std::uint32_t next() noexcept {
        rng_ ^= rng_ << 13U; rng_ ^= rng_ >> 17U; rng_ ^= rng_ << 5U;
        return rng_;
    }
    static std::int32_t level_price(bool bid, std::size_t level) noexcept {
        const auto step = static_cast<std::int32_t>(level) * tick;
        return bid ? base_bid - step : base_ask + step;
    }

    // Header fields common to every synthetic message: stock locate 0,
    // tracking number 1, timestamp = message index (ns since midnight).
    static ItchEvent header(ItchType type, std::size_t seq) noexcept {
        ItchEvent e{};
        e.type = type;
        e.tracking_number = 1;
        e.timestamp_ns = seq;
        return e;
    }
    static void emit(SyntheticMessage& m, const ItchEvent& e) noexcept {
        m.length = static_cast<std::uint8_t>(Itch50Encoder::encode(e, m.bytes.data(), m.bytes.size()));
        m.type = static_cast<char>(e.type);
    }

    void remove_live(std::size_t index) noexcept {
        live_[index] = live_[live_count_ - 1];
        --live_count_;
    }

    void update_quote(std::size_t i) noexcept {
        BookQuote q{0, 0, empty_ask_price, 0};
        for (std::size_t l = 0; l < price_levels; ++l) {
            if (bid_qty_[l] > 0) { q.bid_price = level_price(true, l); q.bid_qty = bid_qty_[l]; break; }
        }
        for (std::size_t l = 0; l < price_levels; ++l) {
            if (ask_qty_[l] > 0) { q.ask_price = level_price(false, l); q.ask_qty = ask_qty_[l]; break; }
        }
        quotes_[i] = q;
    }

    void generate() noexcept {
        std::uint64_t arrival = 5;
        for (std::size_t i = 0; i < message_count; ++i) {
            SyntheticMessage& m = messages_[i];
            const std::uint32_t pick = next() % 100U;
            // The first four messages seed both sides of the book.
            const bool must_add = i < 4 || live_count_ == 0;
            const bool must_reduce = live_count_ >= 100;
            if (must_add || (!must_reduce && pick < 45U)) {
                add_order(m, i, i < 4 ? (i & 1U) == 0U : (next() & 1U) == 0U);
            } else if (pick < 80U) {
                reduce_order(m, i, 'E');
            } else {
                reduce_order(m, i, 'X');
            }
            m.arrival_cycle = arrival;
            arrival += 3U + next() % 38U;  // inter-arrival gap: 3..40 cycles
            update_quote(i);
        }
    }

    void add_order(SyntheticMessage& m, std::size_t i, bool bid) noexcept {
        const std::uint32_t a = next(), b = next();
        const std::size_t level = static_cast<std::size_t>((a % price_levels) < (b % price_levels) ? a % price_levels : b % price_levels);
        const auto shares = static_cast<std::int32_t>(100U * (1U + next() % 5U));
        const std::uint64_t reference = 1000U + i;
        ItchEvent e = header(ItchType::add, i);
        e.order_reference = reference;
        e.side = bid ? 'B' : 'S';
        e.shares = static_cast<std::uint32_t>(shares);
        std::memcpy(e.stock.data(), "AAPL    ", 8);
        e.price_ticks = static_cast<std::uint32_t>(level_price(bid, level));
        emit(m, e);
        live_[live_count_++] = {reference, shares, level, bid};
        (bid ? bid_qty_ : ask_qty_)[level] += shares;
        ++adds_;
    }

    void reduce_order(SyntheticMessage& m, std::size_t i, char type) noexcept {
        const std::size_t index = next() % live_count_;
        Live& o = live_[index];
        const bool full = (next() & 1U) != 0U;
        const std::int32_t shares = (full || o.remaining <= 100) ? o.remaining : 100;
        ItchEvent e = header(type == 'E' ? ItchType::executed : ItchType::cancel, i);
        e.order_reference = o.reference;
        e.shares = static_cast<std::uint32_t>(shares);
        emit(m, e);   // Executed: match number 0
        (o.bid ? bid_qty_ : ask_qty_)[o.level] -= shares;
        o.remaining -= shares;
        if (o.remaining == 0) remove_live(index);
        if (type == 'E') ++executes_; else ++cancels_;
    }

    std::array<SyntheticMessage, message_count> messages_{};
    std::array<BookQuote, message_count> quotes_{};
    std::array<Live, 128> live_{};
    std::array<std::int32_t, price_levels> bid_qty_{};
    std::array<std::int32_t, price_levels> ask_qty_{};
    std::size_t live_count_ = 0;
    std::size_t adds_ = 0, executes_ = 0, cancels_ = 0;
    std::uint32_t rng_ = seed;
};

} // namespace fpga_sim
