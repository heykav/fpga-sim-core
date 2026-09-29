# fpga-sim-core

The honest reason this exists: I wanted to know exactly how many
nanoseconds it costs, cycle by cycle, for a packet to go from wire to
order-book decision on a tick-to-trade FPGA path — and "wanted to know"
means "wanted a number I could defend," not a vibe. Real hardware doesn't
let you single-step a clock domain and print intermediate state; a
software model with no wall-clock time in it does. That is the goal; the
current demo does not deliver it yet: its tick/trade cycles are a scripted
stimulus timeline, not a measured tick-to-trade latency. So: a deterministic
C++20 cycle model, clocked at 322.265625 MHz (`3.1032 ns/cycle`), where
"deterministic" isn't marketing — there is no `std::chrono` anywhere in
the hot path, only a cycle counter.

The scheduler (`core/discrete_engine.hpp`) is the part I'm actually proud
of: posedge/negedge callbacks live in a fixed `std::array<Slot, 16>`, not
a `std::vector` or `std::function`, so registering a module's clock edge
costs no heap allocation and no virtual dispatch — the callback type
is a bare `void(*)(void*, uint64_t) noexcept` function pointer. If you've
ever had a "cycle-accurate" simulator get quietly less accurate under
`-O2` because the allocator started making its own timing decisions, this
is the fix. No-allocation is by construction (no `new`/`malloc`/`std::vector`
in `include/`) and is checked on the stepping/parse/book/OFI/BRAM path by
`test-hot-path-alloc`, which counts global `operator new` calls (skipped under
ASan). Registration is bounds-checked: `on_posedge`/`on_negedge` return `false`
once the 16 slots are full. Timing claims beyond that (e.g. "no allocator
jitter under `-O2`") are not measured.

The datapath itself models:

- PCS block-lock and CRC32 validation primitives, in a block shape inspired
  by 64b/66b (`Deserializer66b`) but without its scrambler or control-block
  sync header — see the doc comment on that class before assuming 802.3
  conformance.
- AXI4-Stream 512-bit framing with IPG verification.
- Zero-copy big-endian NASDAQ ITCH 5.0 parsing for `A`, `E`, and `X`.
- A generic synchronous dual-port BRAM model (`DualPortBram<T, Depth, Latency>`)
  with same-cycle collision assertions, and a separate five-level order book
  (`FiveLevelOrderBook`) that counts adds dropped when a side is full.
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

The demo prints its scripted stimulus timeline (tick and trade cycles taken
from the engine's cycle counter, converted at 3.1032 ns/cycle) and writes
`fpga_sim_core.vcd` (binary vectors, toggling clock, 100 fs timescale so a
cycle is exactly 31032 units).

## What "tests" actually means here

`ctest` runs several targets, including unit tests for the engine, order
book, VCD writer (the emitted file is parsed) and the hot-path allocation
check. CI also runs an ASan+UBSan job. `fpga-sim-demo` is the end-to-end integration
path: it asserts that PCS block-lock and CRC came back valid, the MAC
framer's IPG check and both packet accepts passed, the DMA's `post_write`
and MSI-X assert fired, the consumed byte count matched the source ITCH
message, and the order book actually saw a trade. `test-phy-pcs` and
`test-parser-itch50` are real per-module unit tests: the CRC32 is checked
against the official CRC-32/ISO-HDLC check vector (`crc32("123456789") ==
0xCBF43926`), the PCS block encode/decode is round-tripped (including a
non-block-aligned payload and a deliberately corrupted block to confirm
`crc_valid` actually goes false), and the ITCH parser is checked against
hand-built messages at the documented NASDAQ TotalView-ITCH 5.0 byte
offsets, including truncated and unknown-type inputs. The parser exposes why it
stopped (`status()`, `bytes_consumed()`); it still stops at the first unknown
message type because only the `A`/`E`/`X` lengths are implemented.

Still not covered: BRAM read/write collision on the exact same cycle, and
the PCIe/DMA and MAC-framer modules beyond what the integration path
exercises. That's real remaining scope, not swept under "tests pass."

---

Made with ❤️ in India by [Krishna Anubhav](https://github.com/heykav).
