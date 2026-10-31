#pragma once

// Offline analysis helpers for real NASDAQ ITCH 5.0 files (BinaryFILE framing: a 2-byte
// big-endian length before every message). Uses the standard containers freely: this is a
// tool, not part of the allocation-audited model in include/.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace itch_tools {

// Message lengths from the NASDAQ TotalView-ITCH 5.0 specification. 0 = not a 5.0 type.
inline std::size_t spec_length(std::uint8_t t) noexcept {
    switch (t) {
    case 'S': return 12; case 'R': return 39; case 'H': return 25; case 'Y': return 20;
    case 'L': return 26; case 'V': return 35; case 'W': return 12; case 'K': return 28;
    case 'J': return 35; case 'h': return 21; case 'A': return 36; case 'F': return 40;
    case 'E': return 31; case 'C': return 36; case 'X': return 23; case 'D': return 19;
    case 'U': return 35; case 'P': return 44; case 'Q': return 40; case 'B': return 19;
    case 'I': return 50; case 'N': return 20;
    default: return 0;
    }
}

inline const char* type_name(std::uint8_t t) noexcept {
    switch (t) {
    case 'S': return "System Event"; case 'R': return "Stock Directory"; case 'H': return "Stock Trading Action";
    case 'Y': return "Reg SHO Restriction"; case 'L': return "Market Participant Position";
    case 'V': return "MWCB Decline Level"; case 'W': return "MWCB Status"; case 'K': return "IPO Quoting Period Update";
    case 'J': return "LULD Auction Collar"; case 'h': return "Operational Halt"; case 'A': return "Add Order";
    case 'F': return "Add Order (MPID)"; case 'E': return "Order Executed"; case 'C': return "Order Executed With Price";
    case 'X': return "Order Cancel"; case 'D': return "Order Delete"; case 'U': return "Order Replace";
    case 'P': return "Trade (non-cross)"; case 'Q': return "Cross Trade"; case 'B': return "Broken Trade";
    case 'I': return "NOII"; case 'N': return "Retail Price Improvement"; default: return "?";
    }
}

inline std::uint64_t be(const std::uint8_t* p, int n) noexcept {
    std::uint64_t v = 0;
    for (int i = 0; i < n; ++i) v = (v << 8U) | p[i];
    return v;
}
inline std::uint16_t locate_of(const std::uint8_t* m) noexcept { return static_cast<std::uint16_t>(be(m + 1, 2)); }
inline std::uint64_t timestamp_ns(const std::uint8_t* m) noexcept { return be(m + 5, 6); }

// Walks length-framed messages. Stops at the first framing problem (unknown type, length
// that disagrees with the spec) or at a truncated tail; `stop` says which.
struct Walker {
    enum class Stop { end, truncated_tail, bad_framing };
    const std::uint8_t* data;
    std::size_t size;
    std::size_t offset = 0;
    Stop stop = Stop::end;
    Walker(const std::uint8_t* d, std::size_t n) : data(d), size(n) {}
    // Returns pointer to message body or nullptr when finished; `len` is set to its length.
    const std::uint8_t* next(std::size_t& len) {
        if (offset >= size) { stop = Stop::end; return nullptr; }
        if (size - offset < 2) { stop = Stop::truncated_tail; return nullptr; }
        const std::size_t framed = static_cast<std::size_t>(be(data + offset, 2));
        if (size - offset - 2 < framed) { stop = Stop::truncated_tail; return nullptr; }
        const std::uint8_t* m = data + offset + 2;
        if (framed == 0 || spec_length(m[0]) != framed) { stop = Stop::bad_framing; return nullptr; }
        offset += 2 + framed;
        len = framed;
        return m;
    }
};

// Full-depth book for ONE stock locate: every price level, every resting order.
class RealBook {
public:
    // Applies one message for this stock; ignores types that do not touch the displayed book.
    // Returns true if the book changed.
    bool apply(const std::uint8_t* m) {
        switch (m[0]) {
        case 'A': case 'F': add(be(m + 11, 8), m[19] == 'B', static_cast<std::int64_t>(be(m + 32, 4)), static_cast<std::int64_t>(be(m + 20, 4))); return true;
        case 'E': case 'C': case 'X': reduce(be(m + 11, 8), static_cast<std::int64_t>(be(m + 19, 4))); return true;
        case 'D': remove(be(m + 11, 8)); return true;
        case 'U': {
            const std::uint64_t old = be(m + 11, 8);
            auto it = orders_.find(old);
            if (it == orders_.end()) { ++unknown_refs_; return true; }
            const bool bid = it->second.bid;
            remove(old);
            add(be(m + 19, 8), bid, static_cast<std::int64_t>(be(m + 31, 4)), static_cast<std::int64_t>(be(m + 27, 4)));
            return true;
        }
        default: return false;
        }
    }
    [[nodiscard]] std::size_t bid_levels() const noexcept { return bids_.size(); }
    [[nodiscard]] std::size_t ask_levels() const noexcept { return asks_.size(); }
    [[nodiscard]] std::size_t orders() const noexcept { return orders_.size(); }
    [[nodiscard]] std::uint64_t unknown_refs() const noexcept { return unknown_refs_; }
    // Best prices in ITCH 1/10000 units; false if the side is empty.
    bool best_bid(std::int64_t& p) const noexcept { if (bids_.empty()) return false; p = bids_.rbegin()->first; return true; }
    bool best_ask(std::int64_t& p) const noexcept { if (asks_.empty()) return false; p = asks_.begin()->first; return true; }

private:
    struct Order { bool bid; std::int64_t price; std::int64_t shares; };
    void add(std::uint64_t ref, bool bid, std::int64_t price, std::int64_t shares) {
        orders_[ref] = {bid, price, shares};
        (bid ? bids_ : asks_)[price] += shares;
    }
    void reduce(std::uint64_t ref, std::int64_t shares) {
        auto it = orders_.find(ref);
        if (it == orders_.end()) { ++unknown_refs_; return; }
        Order& o = it->second;
        const std::int64_t n = std::min(shares, o.shares);
        take(o, n);
        o.shares -= n;
        if (o.shares <= 0) orders_.erase(it);
    }
    void remove(std::uint64_t ref) {
        auto it = orders_.find(ref);
        if (it == orders_.end()) { ++unknown_refs_; return; }
        take(it->second, it->second.shares);
        orders_.erase(it);
    }
    void take(const Order& o, std::int64_t n) {
        auto& side = o.bid ? bids_ : asks_;
        auto lv = side.find(o.price);
        if (lv == side.end()) return;
        lv->second -= n;
        if (lv->second <= 0) side.erase(lv);
    }
    std::map<std::int64_t, std::int64_t> bids_, asks_;
    std::unordered_map<std::uint64_t, Order> orders_;
    std::uint64_t unknown_refs_ = 0;
};

// Nearest-rank percentile on a sorted copy (same convention as the pipeline's LatencyStats).
inline std::uint64_t percentile(std::vector<std::uint64_t> v, unsigned pct) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[(pct * v.size() + 99) / 100 - 1];
}

} // namespace itch_tools
