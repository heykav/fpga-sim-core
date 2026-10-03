// Unit tests for Itch50Parser (include/modules/parser_itch50.hpp), checked
// against the real NASDAQ TotalView-ITCH 5.0 byte layout: Add Order (No
// MPID) = 36 bytes, Order Executed = 31 bytes, Order Cancel = 23 bytes,
// all with Order Reference Number at offset 11.
#ifdef NDEBUG
#undef NDEBUG  // these tests rely on assert(); never compile them out
#endif
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

#include "modules/parser_itch50.hpp"

using fpga_sim::Itch50Parser;
using fpga_sim::ItchType;

namespace {

void put_u64be(std::vector<std::uint8_t>& buf, std::size_t offset, std::uint64_t value) {
    for (std::size_t i = 0; i < 8; ++i) buf[offset + i] = static_cast<std::uint8_t>(value >> ((7 - i) * 8));
}
void put_u32be(std::vector<std::uint8_t>& buf, std::size_t offset, std::uint32_t value) {
    for (std::size_t i = 0; i < 4; ++i) buf[offset + i] = static_cast<std::uint8_t>(value >> ((3 - i) * 8));
}

std::vector<std::uint8_t> make_add_order(std::uint64_t ref, char side, std::uint32_t shares,
                                          const char* stock, std::uint32_t price_ticks) {
    std::vector<std::uint8_t> msg(36, 0);
    msg[0] = 'A';
    put_u64be(msg, 11, ref);
    msg[19] = static_cast<std::uint8_t>(side);
    put_u32be(msg, 20, shares);
    std::memcpy(&msg[24], stock, std::strlen(stock));
    put_u32be(msg, 32, price_ticks);
    return msg;
}

std::vector<std::uint8_t> make_executed(std::uint64_t ref, std::uint32_t shares) {
    std::vector<std::uint8_t> msg(31, 0);
    msg[0] = 'E';
    put_u64be(msg, 11, ref);
    put_u32be(msg, 19, shares);
    return msg;
}

std::vector<std::uint8_t> make_cancel(std::uint64_t ref, std::uint32_t shares) {
    std::vector<std::uint8_t> msg(23, 0);
    msg[0] = 'X';
    put_u64be(msg, 11, ref);
    put_u32be(msg, 19, shares);
    return msg;
}

void parses_a_single_add_order_message_at_the_documented_offsets() {
    auto msg = make_add_order(0x1122334455667788ULL, 'B', 500, "AAPL", 1234500);
    Itch50Parser parser;
    const std::size_t n = parser.parse(msg.data(), msg.size());
    assert(n == 1);
    const auto& ev = parser.event(0);
    assert(ev.type == ItchType::add);
    assert(ev.order_reference == 0x1122334455667788ULL);
    assert(ev.side == 'B');
    assert(ev.shares == 500);
    assert(std::strncmp(ev.stock.data(), "AAPL", 4) == 0);
    assert(ev.price_ticks == 1234500);
}

void parses_an_executed_order_message() {
    auto msg = make_executed(0xAABBCCDDULL, 250);
    Itch50Parser parser;
    assert(parser.parse(msg.data(), msg.size()) == 1);
    const auto& ev = parser.event(0);
    assert(ev.type == ItchType::executed);
    assert(ev.order_reference == 0xAABBCCDDULL);
    assert(ev.shares == 250);
}

void parses_a_cancel_message() {
    auto msg = make_cancel(0x99ULL, 100);
    Itch50Parser parser;
    assert(parser.parse(msg.data(), msg.size()) == 1);
    const auto& ev = parser.event(0);
    assert(ev.type == ItchType::cancel);
    assert(ev.order_reference == 0x99ULL);
    assert(ev.shares == 100);
}

void parses_multiple_concatenated_messages_in_one_buffer() {
    auto a = make_add_order(1, 'B', 10, "MSFT", 100);
    auto e = make_executed(1, 5);
    auto x = make_cancel(1, 5);
    std::vector<std::uint8_t> buf;
    buf.insert(buf.end(), a.begin(), a.end());
    buf.insert(buf.end(), e.begin(), e.end());
    buf.insert(buf.end(), x.begin(), x.end());

    Itch50Parser parser;
    const std::size_t n = parser.parse(buf.data(), buf.size());
    assert(n == 3);
    assert(parser.event(0).type == ItchType::add);
    assert(parser.event(1).type == ItchType::executed);
    assert(parser.event(2).type == ItchType::cancel);
    assert(parser.byte_count() == buf.size());
}

void stops_cleanly_on_a_truncated_trailing_message() {
    // A full Add followed by only 10 bytes of a second Add (needs 36) -
    // the parser must return exactly the one complete event, not
    // over-read past the buffer or fabricate a second event.
    auto a = make_add_order(1, 'B', 10, "GOOG", 100);
    std::vector<std::uint8_t> buf(a.begin(), a.end());
    buf.insert(buf.end(), 10, 0xFF);
    buf[a.size()] = 'A'; // looks like the start of another Add

    Itch50Parser parser;
    assert(parser.parse(buf.data(), buf.size()) == 1);
}

void rejects_an_unknown_message_type_without_crashing() {
    std::vector<std::uint8_t> buf(10, 0);
    buf[0] = 'Z'; // not A/E/X
    Itch50Parser parser;
    assert(parser.parse(buf.data(), buf.size()) == 0);
}

void reports_why_parsing_stopped() {
    Itch50Parser parser;
    auto add = make_add_order(1, 'B', 10, "AMZN", 100);
    parser.parse(add.data(), add.size());
    assert(parser.status() == fpga_sim::ParseStatus::ok);
    assert(parser.bytes_consumed() == add.size());

    // Valid add followed by an unknown type: the add is kept, the stop is visible.
    auto buf = add;
    buf.push_back('Z');
    buf.resize(buf.size() + 9, 0);
    assert(parser.parse(buf.data(), buf.size()) == 1);
    assert(parser.status() == fpga_sim::ParseStatus::unknown_type);
    assert(parser.bytes_consumed() == add.size());
    assert(parser.bytes_consumed() < parser.byte_count());

    // Truncated trailing message.
    auto trunc = add;
    trunc.insert(trunc.end(), add.begin(), add.begin() + 10);
    assert(parser.parse(trunc.data(), trunc.size()) == 1);
    assert(parser.status() == fpga_sim::ParseStatus::truncated);
    assert(parser.bytes_consumed() == add.size());

    // More messages than max_events.
    std::vector<std::uint8_t> many;
    for (std::size_t i = 0; i < Itch50Parser::max_events + 1; ++i) many.insert(many.end(), add.begin(), add.end());
    assert(parser.parse(many.data(), many.size()) == Itch50Parser::max_events);
    assert(parser.status() == fpga_sim::ParseStatus::event_limit);
    assert(parser.bytes_consumed() == add.size() * Itch50Parser::max_events);

    parser.reset();
    assert(parser.status() == fpga_sim::ParseStatus::ok);
}

void handles_a_zero_length_buffer() {
    Itch50Parser parser;
    assert(parser.parse(nullptr, 0) == 0);
    assert(parser.byte_count() == 0);
}

void reset_clears_prior_state() {
    auto msg = make_add_order(1, 'B', 10, "AMZN", 100);
    Itch50Parser parser;
    parser.parse(msg.data(), msg.size());
    assert(parser.event_count() == 1);
    parser.reset();
    assert(parser.event_count() == 0);
    assert(parser.byte_count() == 0);
}

// ---- robustness (table-driven) --------------------------------------------

void every_type_byte_is_classified_by_the_documented_length_table() {
    // Only A/E/X have a verified length. Every other byte value must stop the
    // parse with unknown_type and consume nothing, whatever follows it.
    std::vector<std::uint8_t> buf(64, 0);
    Itch50Parser parser;
    for (unsigned t = 0; t < 256; ++t) {
        buf[0] = static_cast<std::uint8_t>(t);
        const std::size_t n = parser.parse(buf.data(), buf.size());
        if (t == 'A' || t == 'E' || t == 'X') {
            // The first message is stepped over (an Add with side 0 counts as
            // malformed); parsing then stops at the zero byte that follows.
            assert(n + parser.malformed_count() == 1);
            assert(parser.bytes_consumed() == fpga_sim::Itch50Layout::length_for(static_cast<std::uint8_t>(t)));
        } else {
            assert(n == 0);
            assert(parser.status() == fpga_sim::ParseStatus::unknown_type);
            assert(parser.bytes_consumed() == 0);
        }
    }
}

void every_truncation_of_every_type_is_reported_and_never_over_read() {
    // Each prefix [1, len-1] of a valid message is copied into an exactly
    // sized heap buffer, so any read past the end is caught by ASan.
    const std::vector<std::vector<std::uint8_t>> table = {
        make_add_order(42, 'S', 7, "IBM", 99), make_executed(42, 3), make_cancel(42, 4)};
    Itch50Parser parser;
    for (const auto& msg : table) {
        for (std::size_t cut = 1; cut < msg.size(); ++cut) {
            std::vector<std::uint8_t> prefix(msg.begin(), msg.begin() + static_cast<std::ptrdiff_t>(cut));
            assert(parser.parse(prefix.data(), prefix.size()) == 0);
            assert(parser.status() == fpga_sim::ParseStatus::truncated);
            assert(parser.bytes_consumed() == 0);
            assert(parser.byte_count() == cut);
        }
        std::vector<std::uint8_t> exact(msg);
        assert(parser.parse(exact.data(), exact.size()) == 1);
        assert(parser.status() == fpga_sim::ParseStatus::ok);
    }
}

void a_null_buffer_with_nonzero_length_is_rejected_without_reading() {
    // Regression: parse(nullptr, n > 0) used to dereference the null pointer.
    Itch50Parser parser;
    assert(parser.parse(nullptr, 36) == 0);
    assert(parser.status() == fpga_sim::ParseStatus::null_buffer);
    assert(parser.bytes_consumed() == 0);
}

void an_add_with_an_invalid_side_is_skipped_counted_and_parsing_continues() {
    // Regression: any side byte other than 'B' used to be accepted and was
    // then treated as a sell by the book update.
    const char sides[] = {0, 'b', 's', 'X', ' ', static_cast<char>(0xFF)};
    for (char side : sides) {
        auto bad = make_add_order(5, side, 10, "AMD", 100);
        auto good = make_cancel(6, 1);
        std::vector<std::uint8_t> buf(bad);
        buf.insert(buf.end(), good.begin(), good.end());
        Itch50Parser parser;
        assert(parser.parse(buf.data(), buf.size()) == 1);
        assert(parser.malformed_count() == 1);
        assert(parser.status() == fpga_sim::ParseStatus::ok);
        assert(parser.bytes_consumed() == buf.size());
        assert(parser.event(0).type == ItchType::cancel && parser.event(0).order_reference == 6);
        fpga_sim::ItchEvent untouched{};
        untouched.shares = 77;
        assert(!Itch50Parser::decode(bad.data(), bad.size(), untouched) && untouched.shares == 77);
    }
    for (char side : {'B', 'S'}) {
        auto ok = make_add_order(5, side, 10, "AMD", 100);
        Itch50Parser parser;
        assert(parser.parse(ok.data(), ok.size()) == 1 && parser.event(0).side == side);
    }
}

void decode_rejects_a_length_that_does_not_match_the_type() {
    auto add = make_add_order(1, 'B', 1, "A", 1);
    fpga_sim::ItchEvent ev{};
    assert(Itch50Parser::decode(add.data(), 36, ev));
    for (std::size_t len : {std::size_t{0}, std::size_t{23}, std::size_t{31}, std::size_t{35}})
        assert(!Itch50Parser::decode(add.data(), len, ev));
    assert(!Itch50Parser::decode(nullptr, 36, ev));
}

void fields_are_big_endian_regardless_of_host_byte_order() {
    // Distinct byte in every position: a little-endian or misaligned read
    // produces a different value.
    std::vector<std::uint8_t> msg(36, 0);
    for (std::size_t i = 0; i < msg.size(); ++i) msg[i] = static_cast<std::uint8_t>(0x10 + i);
    msg[0] = 'A';
    msg[19] = 'B';
    Itch50Parser parser;
    assert(parser.parse(msg.data(), msg.size()) == 1);
    const auto& ev = parser.event(0);
    assert(ev.stock_locate == 0x1112);
    assert(ev.tracking_number == 0x1314);
    assert(ev.timestamp_ns == 0x15161718191AULL);
    assert(ev.order_reference == 0x1B1C1D1E1F202122ULL);
    assert(ev.shares == 0x24252627U);
    assert(ev.price_ticks == 0x30313233U);
}

void extreme_field_values_decode_exactly() {
    auto msg = make_add_order(0xFFFFFFFFFFFFFFFFULL, 'S', 0xFFFFFFFFU, "ZZZZZZZZ", 0xFFFFFFFFU);
    for (std::size_t i = 5; i < 11; ++i) msg[i] = 0xFF;   // timestamp = 2^48 - 1
    Itch50Parser parser;
    assert(parser.parse(msg.data(), msg.size()) == 1);
    const auto& ev = parser.event(0);
    assert(ev.order_reference == 0xFFFFFFFFFFFFFFFFULL);
    assert(ev.shares == 0xFFFFFFFFU && ev.price_ticks == 0xFFFFFFFFU);
    assert(ev.timestamp_ns == (1ULL << 48U) - 1U);
    assert(std::strcmp(ev.stock.data(), "ZZZZZZZZ") == 0);
}

void input_alignment_does_not_change_the_result() {
    auto a = make_add_order(0x0102030405060708ULL, 'B', 123, "QQQ", 456);
    auto e = make_executed(0x0102030405060708ULL, 23);
    std::vector<std::uint8_t> msgs(a);
    msgs.insert(msgs.end(), e.begin(), e.end());
    Itch50Parser reference;
    assert(reference.parse(msgs.data(), msgs.size()) == 2);
    for (std::size_t shift = 0; shift < 16; ++shift) {
        std::vector<std::uint8_t> buf(shift + msgs.size(), 0xEE);
        std::memcpy(buf.data() + shift, msgs.data(), msgs.size());
        Itch50Parser parser;
        assert(parser.parse(buf.data() + shift, msgs.size()) == 2);
        assert(parser.event(0) == reference.event(0));
        assert(parser.event(1) == reference.event(1));
    }
}

void executed_carries_the_match_number() {
    auto e = make_executed(9, 1);
    put_u64be(e, 23, 0xDEADBEEFCAFEF00DULL);
    Itch50Parser parser;
    assert(parser.parse(e.data(), e.size()) == 1);
    assert(parser.event(0).match_number == 0xDEADBEEFCAFEF00DULL);
}

} // namespace

int main() {
    parses_a_single_add_order_message_at_the_documented_offsets();
    parses_an_executed_order_message();
    parses_a_cancel_message();
    parses_multiple_concatenated_messages_in_one_buffer();
    stops_cleanly_on_a_truncated_trailing_message();
    rejects_an_unknown_message_type_without_crashing();
    reports_why_parsing_stopped();
    handles_a_zero_length_buffer();
    reset_clears_prior_state();
    every_type_byte_is_classified_by_the_documented_length_table();
    every_truncation_of_every_type_is_reported_and_never_over_read();
    a_null_buffer_with_nonzero_length_is_rejected_without_reading();
    an_add_with_an_invalid_side_is_skipped_counted_and_parsing_continues();
    decode_rejects_a_length_that_does_not_match_the_type();
    fields_are_big_endian_regardless_of_host_byte_order();
    extreme_field_values_decode_exactly();
    input_alignment_does_not_change_the_result();
    executed_carries_the_match_number();
    std::puts("test_parser_itch50: all assertions passed");
    return 0;
}
