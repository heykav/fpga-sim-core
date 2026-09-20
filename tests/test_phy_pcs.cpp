// Unit tests for Crc32 and Deserializer66b (include/modules/phy_pcs.hpp).
// Plain assert()-based, no external framework, matching the project's own
// zero-extra-dependency ethos.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

#include "modules/phy_pcs.hpp"

using fpga_sim::Crc32;
using fpga_sim::Deserializer66b;

namespace {

void crc32_matches_the_standard_check_value() {
    // "123456789" is the official CRC-32/ISO-HDLC ("zlib") check vector;
    // every correct implementation of this polynomial/init/refin/refout
    // combination must produce 0xCBF43926 for it.
    const std::uint8_t input[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    assert(Crc32::compute(input, sizeof(input)) == 0xCBF43926U);
}

void crc32_of_empty_input_is_the_documented_zlib_value() {
    // crc32("") == 0 for the same standard, since init (0xFFFFFFFF) run
    // through zero bytes and then complemented cancels out to zero.
    assert(Crc32::compute(nullptr, 0) == 0U);
}

void pcs_round_trips_a_payload_through_encode_and_decode() {
    const std::uint8_t payload[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04, 0x05};
    std::uint64_t blocks[8] = {};
    const std::size_t count = Deserializer66b::encode(payload, sizeof(payload), blocks, 8);
    assert(count > 0);

    Deserializer66b decoder;
    std::uint8_t out[64] = {};
    const auto result = decoder.decode(blocks, count, out, sizeof(out), sizeof(payload));

    assert(result.block_lock);
    assert(result.crc_valid);
    assert(result.payload_length == sizeof(payload));
    assert(std::memcmp(out, payload, sizeof(payload)) == 0);
}

void pcs_round_trips_a_payload_not_aligned_to_the_block_size() {
    // 7 bytes/block: a 3-byte payload plus 4-byte trailing CRC spans a
    // single partially-filled block, which is the boundary case most
    // likely to be off-by-one.
    const std::uint8_t payload[] = {0xAA, 0xBB, 0xCC};
    std::uint64_t blocks[4] = {};
    const std::size_t count = Deserializer66b::encode(payload, sizeof(payload), blocks, 4);
    assert(count == 1); // 3 + 4 = 7 bytes = exactly one block

    Deserializer66b decoder;
    std::uint8_t out[16] = {};
    const auto result = decoder.decode(blocks, count, out, sizeof(out), sizeof(payload));
    assert(result.crc_valid);
    assert(result.payload_length == sizeof(payload));
    assert(std::memcmp(out, payload, sizeof(payload)) == 0);
}

void pcs_detects_a_bit_flip_in_the_payload() {
    const std::uint8_t payload[] = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80};
    std::uint64_t blocks[8] = {};
    const std::size_t count = Deserializer66b::encode(payload, sizeof(payload), blocks, 8);
    assert(count > 0);

    // Flip one bit inside the first block's payload byte (bits 48-55,
    // i.e. the top payload byte after the sync-marker byte).
    blocks[0] ^= (std::uint64_t{1} << 48U);

    Deserializer66b decoder;
    std::uint8_t out[64] = {};
    const auto result = decoder.decode(blocks, count, out, sizeof(out), sizeof(payload));
    assert(result.block_lock);
    assert(!result.crc_valid); // corruption must be caught, not silently accepted
}

void pcs_loses_block_lock_when_the_sync_marker_is_missing() {
    // A block whose top byte isn't the 0x1 sync marker must be skipped by
    // the decoder rather than misread as payload.
    std::uint64_t blocks[1] = {0x00AABBCCDDEEFF00ULL}; // top byte 0x00, not 0x01
    Deserializer66b decoder;
    std::uint8_t out[16] = {};
    const auto result = decoder.decode(blocks, 1, out, sizeof(out));
    assert(result.block_lock); // decode() was called on real blocks...
    assert(result.payload_length == 0); // ...but nothing was extracted from this one
}

void pcs_encode_reports_failure_when_the_output_buffer_is_too_small() {
    const std::uint8_t payload[32] = {};
    std::uint64_t blocks[1] = {}; // needs 6 blocks (32+4 bytes / 7), only 1 given
    const std::size_t count = Deserializer66b::encode(payload, sizeof(payload), blocks, 1);
    assert(count == 0);
}

} // namespace

int main() {
    crc32_matches_the_standard_check_value();
    crc32_of_empty_input_is_the_documented_zlib_value();
    pcs_round_trips_a_payload_through_encode_and_decode();
    pcs_round_trips_a_payload_not_aligned_to_the_block_size();
    pcs_detects_a_bit_flip_in_the_payload();
    pcs_loses_block_lock_when_the_sync_marker_is_missing();
    pcs_encode_reports_failure_when_the_output_buffer_is_too_small();
    std::puts("test_phy_pcs: all assertions passed");
    return 0;
}
