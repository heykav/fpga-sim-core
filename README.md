# fpga-sim-core

Deterministic C++20 cycle model for a tick-to-trade FPGA NIC datapath. The
simulator uses a discrete 322.265625 MHz clock (`3.1032 ns/cycle`) and never
uses wall-clock time.

The model includes:

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
