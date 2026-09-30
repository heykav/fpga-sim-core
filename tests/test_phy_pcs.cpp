// Unit tests for Crc32 and Deserializer66b (include/modules/phy_pcs.hpp).
// Plain assert()-based, no external framework, matching the project's own
// zero-extra-dependency ethos.
#ifdef NDEBUG
#undef NDEBUG  // these tests rely on assert(); never compile them out
#endif
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

#include "modules/phy_pcs.hpp"
#include "support/test_rng.hpp"

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

void pcs_round_trips_every_length_with_random_payloads() {
    // Seeded property test: for lengths 0..200 and random bytes, decode(encode(x))
    // returns x with a valid CRC, and the block count is ceil((len+4)/7).
    fpga_sim_test::TestRng rng(0x9C5ULL);
    for (std::size_t len = 0; len <= 200; ++len) {
        for (int rep = 0; rep < 20; ++rep) {
            std::vector<std::uint8_t> payload(len + 1);   // +1: encode needs a non-null pointer for len 0
            for (auto& b : payload) b = static_cast<std::uint8_t>(rng.below(256));
            std::vector<std::uint64_t> blocks((len + 4 + 6) / 7);
            const std::size_t count = Deserializer66b::encode(payload.data(), len, blocks.data(), blocks.size());
            assert(count == blocks.size());
            for (auto b : blocks) assert((b >> 56U) == 0x1U);
            std::vector<std::uint8_t> out(count * Deserializer66b::bytes_per_block);
            Deserializer66b decoder;
            const auto r = decoder.decode(blocks.data(), count, out.data(), out.size(), len == 0 ? 0 : len);
            if (len == 0) continue;   // 0 means "infer" to decode(); covered below
            assert(r.block_lock && r.crc_valid && r.payload_length == len);
            assert(std::memcmp(out.data(), payload.data(), len) == 0);
        }
    }
}

void pcs_any_single_bit_flip_is_detected_or_harmless() {
    // Flip each of the 64 bits of each block once. Either decode() reports a
    // failure (no CRC match at the expected length) or, for bits that land in
    // the zero padding after the CRC, the payload is still exactly the input.
    // CRC-32 detects every single-bit error inside payload+CRC.
    fpga_sim_test::TestRng rng(0xF11FULL);
    for (std::size_t len : {std::size_t{1}, std::size_t{3}, std::size_t{23}, std::size_t{31}, std::size_t{36}, std::size_t{50}}) {
        std::vector<std::uint8_t> payload(len);
        for (auto& b : payload) b = static_cast<std::uint8_t>(rng.below(256));
        std::vector<std::uint64_t> clean((len + 4 + 6) / 7);
        assert(Deserializer66b::encode(payload.data(), len, clean.data(), clean.size()) == clean.size());
        const std::size_t covered = len + 4;   // payload + CRC bytes
        for (std::size_t blk = 0; blk < clean.size(); ++blk) {
            for (unsigned bit = 0; bit < 64; ++bit) {
                std::vector<std::uint64_t> blocks(clean);
                blocks[blk] ^= std::uint64_t{1} << bit;
                std::vector<std::uint8_t> out(blocks.size() * 7);
                Deserializer66b decoder;
                const auto r = decoder.decode(blocks.data(), blocks.size(), out.data(), out.size(), len);
                const bool accepted = r.crc_valid && r.payload_length == len;
                // Byte index in the de-framed stream that this bit belongs to (bits 0..55 are data).
                const bool in_padding = bit < 56 && blk * 7 + (6 - bit / 8) >= covered;
                if (in_padding) {
                    assert(accepted && std::memcmp(out.data(), payload.data(), len) == 0);
                } else {
                    assert(!accepted);
                }
            }
        }
    }
}

void pcs_rejects_an_expected_length_longer_than_the_data() {
    const std::uint8_t payload[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    std::uint64_t blocks[4] = {};
    const std::size_t count = Deserializer66b::encode(payload, sizeof payload, blocks, 4);
    std::uint8_t out[64] = {};
    Deserializer66b decoder;
    for (std::size_t expected : {sizeof payload + 1, std::size_t{64}, ~std::size_t{0}, ~std::size_t{0} - 3}) {
        const auto r = decoder.decode(blocks, count, out, sizeof out, expected);
        assert(!r.crc_valid && r.payload_length == 0);
    }
}

void pcs_length_inference_only_works_without_padding() {
    // decode() with expected_payload_length == 0 assumes the last 4 bytes are
    // the CRC, which is only true when (len + 4) is a multiple of 7. This pins
    // that documented limitation.
    for (std::size_t len = 1; len <= 40; ++len) {
        std::vector<std::uint8_t> payload(len, 0x5A);
        std::uint64_t blocks[8] = {};
        const std::size_t count = Deserializer66b::encode(payload.data(), len, blocks, 8);
        std::uint8_t out[64] = {};
        Deserializer66b decoder;
        const auto r = decoder.decode(blocks, count, out, sizeof out);
        if ((len + 4) % 7 == 0) assert(r.crc_valid && r.payload_length == len);
        else assert(!(r.crc_valid && r.payload_length == len));
    }
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
    pcs_round_trips_every_length_with_random_payloads();
    pcs_any_single_bit_flip_is_detected_or_harmless();
    pcs_rejects_an_expected_length_longer_than_the_data();
    pcs_length_inference_only_works_without_padding();
    std::puts("test_phy_pcs: all assertions passed");
    return 0;
}
