#pragma once

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>

namespace fpga_sim {

template <typename T, std::size_t Depth, std::size_t Latency = 1>
class DualPortBram {
    static_assert(Depth > 0 && Latency > 0);

public:
    struct PortRequest {
        bool read = false;
        bool write = false;
        std::size_t address = 0;
        T write_data{};
    };

    struct PortResponse {
        bool valid = false;
        T data{};
    };

    void reset() noexcept {
        memory_.fill(T{});
        for (auto& p : pending_) p = {};
        response_a_ = {};
        response_b_ = {};
        collision_ = false;
    }

    void posedge(const PortRequest& a, const PortRequest& b) noexcept {
        collision_ = false;
        if (a.address >= Depth || b.address >= Depth) {
            assert((!a.read && !a.write) || a.address < Depth);
            assert((!b.read && !b.write) || b.address < Depth);
        }
        if ((a.write && b.write && a.address == b.address) ||
            (a.write && b.read && a.address == b.address) ||
            (b.write && a.read && a.address == b.address)) {
            collision_ = true;
            assert(false && "dual-port BRAM same-cycle collision");
        }
        pending_[0] = {a, b};
        for (std::size_t i = 1; i < Latency; ++i) pending_[i] = pending_[i - 1];
    }

    void negedge() noexcept {
        const auto request = pending_[Latency - 1];
        response_a_ = {};
        response_b_ = {};
        if (request.a.write && request.a.address < Depth) memory_[request.a.address] = request.a.write_data;
        if (request.b.write && request.b.address < Depth) memory_[request.b.address] = request.b.write_data;
        if (request.a.read && request.a.address < Depth) {
            response_a_ = {true, memory_[request.a.address]};
        }
        if (request.b.read && request.b.address < Depth) {
            response_b_ = {true, memory_[request.b.address]};
        }
    }

    [[nodiscard]] PortResponse response_a() const noexcept { return response_a_; }
    [[nodiscard]] PortResponse response_b() const noexcept { return response_b_; }
    [[nodiscard]] bool collision() const noexcept { return collision_; }

private:
    struct Pair {
        PortRequest a{};
        PortRequest b{};
    };
    std::array<T, Depth> memory_{};
    std::array<Pair, Latency> pending_{};
    PortResponse response_a_{};
    PortResponse response_b_{};
    bool collision_ = false;
};

} // namespace fpga_sim
