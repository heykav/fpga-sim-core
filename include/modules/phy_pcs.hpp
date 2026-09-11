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
    PcsResult decode(const std::uint64_t* blocks, std::size_t count,
                     std::uint8_t* payload, std::size_t capacity) noexcept {
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
        if (written >= 4) {
            const std::uint32_t expected = (static_cast<std::uint32_t>(payload[written - 4]) << 24U) |
                (static_cast<std::uint32_t>(payload[written - 3]) << 16U) |
                (static_cast<std::uint32_t>(payload[written - 2]) << 8U) | payload[written - 1];
            result.payload_length = written - 4;
            result.crc_valid = Crc32::compute(payload, result.payload_length) == expected;
        }
        return result;
    }
};

} // namespace fpga_sim
