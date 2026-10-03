// Seeded, deterministic property tests for Itch50Encoder / Itch50Parser:
// encode -> decode round trips, decode -> encode byte identity, chunked stream
// parsing, and a random-byte fuzz loop whose buffers are exactly sized so that
// any over-read is caught by AddressSanitizer in the sanitizer build.
#ifdef NDEBUG
#undef NDEBUG  // these tests rely on assert(); never compile them out
#endif
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "modules/parser_itch50.hpp"
#include "support/test_rng.hpp"

using fpga_sim::Itch50Encoder;
using fpga_sim::Itch50Layout;
using fpga_sim::Itch50Parser;
using fpga_sim::ItchEvent;
using fpga_sim::ItchType;
using fpga_sim::ParseStatus;
using fpga_sim_test::TestRng;

namespace {

constexpr std::uint64_t seed = 0x1705C0DEULL;

// Biased towards the boundary values where overflow and sign bugs live.
std::uint64_t field(TestRng& rng, unsigned bits) {
    const std::uint64_t max = bits == 64 ? ~0ULL : (1ULL << bits) - 1ULL;
    switch (rng.below(6)) {
    case 0: return 0;
    case 1: return max;
    case 2: return max >> 1U;              // INT_MAX-style value
    case 3: return (max >> 1U) + 1U;       // first value with the top bit set
    default: return rng.next() & max;
    }
}

ItchEvent random_event(TestRng& rng) {
    ItchEvent e{};
    const std::uint64_t t = rng.below(3);
    e.type = t == 0 ? ItchType::add : (t == 1 ? ItchType::executed : ItchType::cancel);
    e.stock_locate = static_cast<std::uint16_t>(field(rng, 16));
    e.tracking_number = static_cast<std::uint16_t>(field(rng, 16));
    e.timestamp_ns = field(rng, 48);
    e.order_reference = field(rng, 64);
    e.shares = static_cast<std::uint32_t>(field(rng, 32));
    if (e.type == ItchType::add) {
        e.side = rng.chance(50) ? 'B' : 'S';
        e.price_ticks = static_cast<std::uint32_t>(field(rng, 32));
        for (std::size_t i = 0; i < 8; ++i) e.stock[i] = static_cast<char>(rng.below(256));
    } else if (e.type == ItchType::executed) {
        e.match_number = field(rng, 64);
    }
    return e;
}

void encode_then_decode_is_the_identity() {
    TestRng rng(seed);
    std::uint8_t buf[Itch50Layout::max_length];
    for (int i = 0; i < 100000; ++i) {
        const ItchEvent e = random_event(rng);
        const std::size_t n = Itch50Encoder::encode(e, buf, sizeof buf);
        assert(n == Itch50Layout::length_for(static_cast<std::uint8_t>(e.type)));
        std::vector<std::uint8_t> exact(buf, buf + n);   // exactly sized: over-reads trip ASan
        ItchEvent back{};
        assert(Itch50Parser::decode(exact.data(), exact.size(), back));
        assert(back == e);
    }
}

void decode_then_encode_reproduces_every_byte() {
    // Every byte of the A/E/X layouts belongs to a field, so decode is a
    // bijection on valid messages: random valid bytes must survive a round trip.
    TestRng rng(seed + 1);
    const std::uint8_t types[] = {'A', 'E', 'X'};
    for (int i = 0; i < 100000; ++i) {
        const std::uint8_t type = types[rng.below(3)];
        std::vector<std::uint8_t> msg(Itch50Layout::length_for(type));
        for (auto& b : msg) b = static_cast<std::uint8_t>(rng.below(256));
        msg[0] = type;
        if (type == 'A') msg[19] = rng.chance(50) ? 'B' : 'S';
        ItchEvent e{};
        assert(Itch50Parser::decode(msg.data(), msg.size(), e));
        std::uint8_t out[Itch50Layout::max_length] = {};
        assert(Itch50Encoder::encode(e, out, sizeof out) == msg.size());
        assert(std::memcmp(out, msg.data(), msg.size()) == 0);
    }
}

void a_long_stream_parses_identically_in_event_limited_chunks() {
    TestRng rng(seed + 2);
    for (int round = 0; round < 200; ++round) {
        std::vector<ItchEvent> want;
        std::vector<std::uint8_t> stream;
        const std::size_t count = 1 + rng.below(150);
        for (std::size_t i = 0; i < count; ++i) {
            want.push_back(random_event(rng));
            std::uint8_t buf[Itch50Layout::max_length];
            const std::size_t n = Itch50Encoder::encode(want.back(), buf, sizeof buf);
            stream.insert(stream.end(), buf, buf + n);
        }
        Itch50Parser parser;
        std::size_t offset = 0, got = 0;
        while (offset < stream.size()) {
            const std::size_t n = parser.parse(stream.data() + offset, stream.size() - offset);
            assert(n > 0 && n <= Itch50Parser::max_events);
            assert(parser.status() == ParseStatus::ok || parser.status() == ParseStatus::event_limit);
            for (std::size_t k = 0; k < n; ++k) assert(parser.event(k) == want[got + k]);
            got += n;
            offset += parser.bytes_consumed();
        }
        assert(got == want.size() && offset == stream.size());
    }
}

void random_bytes_never_over_read_and_the_accounting_adds_up() {
    TestRng rng(seed + 3);
    Itch50Parser parser;
    for (int i = 0; i < 20000; ++i) {
        std::vector<std::uint8_t> buf(rng.below(300));
        for (auto& b : buf) {
            // Mostly plausible type bytes so that multi-message paths are reached.
            const std::uint64_t r = rng.below(8);
            b = r < 2 ? 'A' : r < 4 ? 'E' : r < 6 ? 'X' : static_cast<std::uint8_t>(rng.below(256));
        }
        const std::size_t n = parser.parse(buf.empty() ? nullptr : buf.data(), buf.size());
        const std::size_t consumed = parser.bytes_consumed();
        assert(consumed <= buf.size());
        assert(parser.byte_count() == buf.size());
        // Walk the same buffer independently using only the length table.
        std::size_t walked = 0, messages = 0;
        while (walked < consumed) {
            const std::size_t len = Itch50Layout::length_for(buf[walked]);
            assert(len != 0);
            walked += len;
            ++messages;
        }
        assert(walked == consumed);
        assert(n + parser.malformed_count() == messages);
        switch (parser.status()) {
        case ParseStatus::ok: assert(consumed == buf.size()); break;
        case ParseStatus::unknown_type: assert(Itch50Layout::length_for(buf[consumed]) == 0); break;
        case ParseStatus::truncated: {
            const std::size_t len = Itch50Layout::length_for(buf[consumed]);
            assert(len != 0 && buf.size() - consumed < len);
            break;
        }
        case ParseStatus::event_limit: assert(n == Itch50Parser::max_events && consumed < buf.size()); break;
        case ParseStatus::null_buffer: assert(false); break;
        }
        // Every event re-encodes to exactly the bytes it came from.
        std::size_t off = 0, k = 0;
        while (off < consumed) {
            const std::size_t len = Itch50Layout::length_for(buf[off]);
            ItchEvent e{};
            if (Itch50Parser::decode(buf.data() + off, len, e)) {
                assert(parser.event(k) == e);
                std::uint8_t out[Itch50Layout::max_length];
                assert(Itch50Encoder::encode(e, out, sizeof out) == len);
                assert(std::memcmp(out, buf.data() + off, len) == 0);
                ++k;
            }
            off += len;
        }
        assert(k == n);
    }
}

void the_encoder_rejects_what_the_wire_cannot_carry() {
    std::uint8_t buf[Itch50Layout::max_length] = {};
    ItchEvent e{};
    e.type = ItchType::add;
    e.side = 'B';
    assert(Itch50Encoder::encode(e, buf, sizeof buf) == Itch50Layout::add_length);
    assert(Itch50Encoder::encode(e, buf, Itch50Layout::add_length - 1) == 0);   // capacity
    assert(Itch50Encoder::encode(e, nullptr, sizeof buf) == 0);                // null output
    e.timestamp_ns = Itch50Layout::timestamp_limit;                             // 49 bits
    assert(Itch50Encoder::encode(e, buf, sizeof buf) == 0);
    e.timestamp_ns = Itch50Layout::timestamp_limit - 1;
    assert(Itch50Encoder::encode(e, buf, sizeof buf) == Itch50Layout::add_length);
    e.side = 'Q';                                                                // invalid side
    assert(Itch50Encoder::encode(e, buf, sizeof buf) == 0);
    ItchEvent unknown{};                                                         // type unknown
    assert(Itch50Encoder::encode(unknown, buf, sizeof buf) == 0);
    // A rejected encode writes nothing.
    std::uint8_t sentinel[Itch50Layout::max_length];
    std::memset(sentinel, 0xAB, sizeof sentinel);
    e.side = 'Q';
    assert(Itch50Encoder::encode(e, sentinel, sizeof sentinel) == 0);
    for (auto b : sentinel) assert(b == 0xAB);
}

} // namespace

int main() {
    encode_then_decode_is_the_identity();
    decode_then_encode_reproduces_every_byte();
    a_long_stream_parses_identically_in_event_limited_chunks();
    random_bytes_never_over_read_and_the_accounting_adds_up();
    the_encoder_rejects_what_the_wire_cannot_carry();
    std::puts("test_itch_roundtrip: all assertions passed");
    return 0;
}
