#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace fpga_sim {

class Crc32 {
public:
    static constexpr std::uint32_t polynomial = 0xEDB88320U;
    static std::uint32_t compute(const std::uint8_t* data, std::size_t length) noexcept {
        std::uint32_t crc = 0xFFFFFFFFU;
        for (std::size_t i = 0; i < length; ++i) {
            crc ^= data[i];
            for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1U) ^ (polynomial & (0U - (crc & 1U)));
        }
        return ~crc;
    }
};

struct PcsResult {
    bool block_lock = false;
    bool crc_valid = false;
    std::size_t payload_length = 0;
};

class Deserializer66b {
public:
    static constexpr std::size_t bytes_per_block = 7;

    static std::size_t encode(const std::uint8_t* payload, std::size_t length,
                              std::uint64_t* blocks, std::size_t capacity) noexcept {
        if (payload == nullptr || blocks == nullptr || capacity == 0) return 0;
        const std::size_t total = length + sizeof(std::uint32_t);
        const std::size_t count = (total + bytes_per_block - 1) / bytes_per_block;
        if (count > capacity) return 0;
        std::uint32_t crc = Crc32::compute(payload, length);
        for (std::size_t block_index = 0; block_index < count; ++block_index) {
            std::uint64_t block = 0x1ULL << 56U;
            for (std::size_t byte = 0; byte < bytes_per_block; ++byte) {
                const std::size_t index = block_index * bytes_per_block + byte;
                std::uint8_t value = 0;
                if (index < length) value = payload[index];
                else if (index == length) value = static_cast<std::uint8_t>(crc >> 24U);
                else if (index == length + 1) value = static_cast<std::uint8_t>(crc >> 16U);
                else if (index == length + 2) value = static_cast<std::uint8_t>(crc >> 8U);
                else if (index == length + 3) value = static_cast<std::uint8_t>(crc);
                block |= static_cast<std::uint64_t>(value) << ((6U - byte) * 8U);
            }
            blocks[block_index] = block;
        }
        return count;
    }

    PcsResult decode(const std::uint64_t* blocks, std::size_t count,
                     std::uint8_t* payload, std::size_t capacity,
                     std::size_t expected_payload_length = 0) noexcept {
        PcsResult result{};
        if (count == 0 || blocks == nullptr || payload == nullptr) return result;
        result.block_lock = true;
        std::size_t written = 0;
        for (std::size_t i = 0; i < count && written < capacity; ++i) {
            const std::uint64_t block = blocks[i];
            if ((block >> 56U) != 0x1U) continue;
            for (int byte = 6; byte >= 0 && written < capacity; --byte)
                payload[written++] = static_cast<std::uint8_t>(block >> (byte * 8));
        }
        const std::size_t payload_length = expected_payload_length == 0
            ? (written >= 4 ? written - 4 : 0) : expected_payload_length;
        if (payload_length + 4 <= written && payload_length <= capacity) {
            const std::size_t crc_offset = payload_length;
            const std::uint32_t expected = (static_cast<std::uint32_t>(payload[crc_offset]) << 24U) |
                (static_cast<std::uint32_t>(payload[crc_offset + 1]) << 16U) |
                (static_cast<std::uint32_t>(payload[crc_offset + 2]) << 8U) | payload[crc_offset + 3];
            result.payload_length = payload_length;
            result.crc_valid = Crc32::compute(payload, result.payload_length) == expected;
        }
        return result;
    }
};

} // namespace fpga_sim
