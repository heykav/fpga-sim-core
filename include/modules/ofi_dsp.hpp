#pragma once

#include <array>
#include <cstdint>
#include <limits>

namespace fpga_sim {

class MicroPriceLut {
public:
    static constexpr int imbalance_min = -16;
    static constexpr int imbalance_max = 16;
    static constexpr std::size_t spread_levels = 8;

    [[nodiscard]] static constexpr std::int32_t lookup(int imbalance, std::size_t spread_ticks) noexcept {
        const int clamped = imbalance < imbalance_min ? imbalance_min : (imbalance > imbalance_max ? imbalance_max : imbalance);
        const std::size_t spread = spread_ticks == 0 ? 1 : (spread_ticks > spread_levels ? spread_levels : spread_ticks);
        return table_[static_cast<std::size_t>(clamped - imbalance_min)][spread - 1];
    }

private:
    inline static constexpr auto table_ = [] {
        std::array<std::array<std::int32_t, spread_levels>, 33> table{};
        for (std::size_t i = 0; i < table.size(); ++i) {
            for (std::size_t s = 0; s < spread_levels; ++s) {
                table[i][s] = static_cast<std::int32_t>(
                    (static_cast<int>(i) - 16) * static_cast<int>(s + 1) * 1000 / 16);
            }
        }
        return table;
    }();
};

struct BookQuote {
    std::int32_t bid_price = 0;
    std::int32_t bid_qty = 0;
    std::int32_t ask_price = 0;
    std::int32_t ask_qty = 0;
};

class OfiDsp {
public:
    void reset() noexcept {
        previous_ = {};
        valid_previous_ = false;
        for (auto& stage : pipeline_) stage = {};
        output_ = {};
    }

    void posedge(const BookQuote& quote) noexcept {
        const std::int64_t eb = side_bid(quote);
        const std::int64_t ea = side_ask(quote);
        const std::int64_t value = eb - ea;
        for (std::size_t i = pipeline_.size() - 1; i > 0; --i) pipeline_[i] = pipeline_[i - 1];
        pipeline_[0] = {true, saturate(value)};
        previous_ = quote;
        valid_previous_ = true;
    }

    void negedge() noexcept {
        output_ = pipeline_.back();
    }

    [[nodiscard]] bool output_valid() const noexcept { return output_.valid; }
    [[nodiscard]] std::int32_t output() const noexcept { return output_.value; }

private:
    struct Stage { bool valid = false; std::int32_t value = 0; };
    std::int64_t side_bid(const BookQuote& q) const noexcept {
        if (!valid_previous_ || q.bid_price > previous_.bid_price) return q.bid_qty;
        if (q.bid_price == previous_.bid_price) return static_cast<std::int64_t>(q.bid_qty) - previous_.bid_qty;
        return -static_cast<std::int64_t>(previous_.bid_qty);
    }
    std::int64_t side_ask(const BookQuote& q) const noexcept {
        if (!valid_previous_ || q.ask_price < previous_.ask_price) return -static_cast<std::int64_t>(q.ask_qty);
        if (q.ask_price == previous_.ask_price) return static_cast<std::int64_t>(q.ask_qty) - previous_.ask_qty;
        return previous_.ask_qty;
    }
    static std::int32_t saturate(std::int64_t value) noexcept {
        if (value > std::numeric_limits<std::int32_t>::max()) return std::numeric_limits<std::int32_t>::max();
        if (value < std::numeric_limits<std::int32_t>::min()) return std::numeric_limits<std::int32_t>::min();
        return static_cast<std::int32_t>(value);
    }
    BookQuote previous_{};
    bool valid_previous_ = false;
    std::array<Stage, 3> pipeline_{};
    Stage output_{};
};

} // namespace fpga_sim
