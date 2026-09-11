#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace fpga_sim {

template <typename T, std::size_t Count, std::size_t Alignment = 64>
struct alignas(Alignment) AlignedSlab {
    static_assert(Count > 0 && (Alignment & (Alignment - 1)) == 0);
    static_assert(std::is_trivially_copyable_v<T>);
    std::array<T, Count> entries{};
};

template <typename T, std::size_t Count, std::size_t Alignment = 64>
class RingBuffer {
public:
    bool push(const T& value) noexcept {
        if (size_ == Count) return false;
        slab_.entries[tail_] = value;
        tail_ = (tail_ + 1) % Count;
        ++size_;
        return true;
    }
    bool pop(T& value) noexcept {
        if (size_ == 0) return false;
        value = slab_.entries[head_];
        head_ = (head_ + 1) % Count;
        --size_;
        return true;
    }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] static constexpr std::size_t capacity() noexcept { return Count; }

private:
    AlignedSlab<T, Count, Alignment> slab_{};
    std::size_t head_ = 0;
    std::size_t tail_ = 0;
    std::size_t size_ = 0;
};

} // namespace fpga_sim
