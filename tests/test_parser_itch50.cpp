// Unit tests for Itch50Parser (include/modules/parser_itch50.hpp), checked
// against the real NASDAQ TotalView-ITCH 5.0 byte layout: Add Order (No
// MPID) = 36 bytes, Order Executed = 31 bytes, Order Cancel = 23 bytes,
// all with Order Reference Number at offset 11.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

#include "modules/parser_itch50.hpp"

using fpga_sim::Itch50Parser;
using fpga_sim::ItchType;

namespace {

void put_u64be(std::vector<std::uint8_t>& buf, std::size_t offset, std::uint64_t value) {
    for (int i = 0; i < 8; ++i) buf[offset + i] = static_cast<std::uint8_t>(value >> ((7 - i) * 8));
}
void put_u32be(std::vector<std::uint8_t>& buf, std::size_t offset, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) buf[offset + i] = static_cast<std::uint8_t>(value >> ((3 - i) * 8));
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

} // namespace

int main() {
    parses_a_single_add_order_message_at_the_documented_offsets();
    parses_an_executed_order_message();
    parses_a_cancel_message();
    parses_multiple_concatenated_messages_in_one_buffer();
    stops_cleanly_on_a_truncated_trailing_message();
    rejects_an_unknown_message_type_without_crashing();
    handles_a_zero_length_buffer();
    reset_clears_prior_state();
    std::puts("test_parser_itch50: all assertions passed");
    return 0;
}
