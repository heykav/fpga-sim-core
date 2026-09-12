# fpga-sim-core

The honest reason this exists: I wanted to know exactly how many
nanoseconds it costs, cycle by cycle, for a packet to go from wire to
order-book decision on a tick-to-trade FPGA path — and "wanted to know"
means "wanted a number I could defend," not a vibe. Real hardware doesn't
let you single-step a clock domain and print intermediate state; a
software model with no wall-clock time in it does. So: a deterministic
C++20 cycle model, clocked at 322.265625 MHz (`3.1032 ns/cycle`), where
"deterministic" isn't marketing — there is no `std::chrono` anywhere in
the hot path, only a cycle counter.

The scheduler (`core/discrete_engine.hpp`) is the part I'm actually proud
of: posedge/negedge callbacks live in a fixed `std::array<Slot, 16>`, not
a `std::vector` or `std::function`, so registering a module's clock edge
costs zero heap allocations and zero virtual dispatch — the callback type
is a bare `void(*)(void*, uint64_t) noexcept` function pointer. If you've
ever had a "cycle-accurate" simulator get quietly less accurate under
`-O2` because the allocator started making its own timing decisions, this
is the fix.

The datapath itself models:

- 64b/66b PCS block-lock and CRC32 validation primitives.
- AXI4-Stream 512-bit framing with IPG verification.
- Zero-copy big-endian NASDAQ ITCH 5.0 parsing for `A`, `E`, and `X`.
- Five-level synchronous dual-port BRAM with collision assertions.
- Three-cycle signed 32-bit OFI pipeline and compile-time micro-price LUT.
- Preallocated 64-byte-aligned host rings and a PCIe Gen4 x16 TLP/MSI-X model.
- Minimal IEEE 1364 VCD output suitable for GTKWave.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/fpga-sim-demo
```

The demo reports the deterministic latency histogram and writes
`fpga_sim_core.vcd`.

## What "tests" actually means here

`ctest` currently runs one target: the demo binary itself, which asserts
on the way through that PCS block-lock and CRC came back valid, the MAC
framer's IPG check and both packet accepts passed, the DMA's `post_write`
and MSI-X assert fired, the consumed byte count matched the source ITCH
message, and the order book actually saw a trade. That's a real
end-to-end invariant check, not just "didn't segfault" — but it's one
integration path, not a unit-test matrix over each module's edge cases
(what happens on a mid-frame CRC flip, a BRAM read/write collision on the
exact same cycle, a zero-length ITCH message). Worth knowing before you
trust this the way you'd trust a project with per-module unit tests.
