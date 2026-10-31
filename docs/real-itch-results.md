# Real NASDAQ ITCH 5.0 replay results (aggregates only)

Produced by `fpga-sim-itch-replay` on a bounded slice of NASDAQ's public sample file. No NASDAQ messages are stored in this repository.

## Provenance

- File: `01302019.NASDAQ_ITCH50.gz` (trading day 2019-01-30), 4,764,426,091 bytes in full.
- Source: https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/ (NASDAQ's public sample directory).
- Fetched on 2026-10-31: bytes 0 to 134,217,727 only (134,217,728 bytes, 2.8% of the file) by HTTP range request. SHA-256 of that prefix: `95d6e2ef469c8e0615b6619590766e15ef957bd056832f42bc868467e0a86dee`.
- Decompressed prefix: 343,015,424 bytes, 10,672,014 messages, ending mid-message at the slice boundary. Exchange time covered: about 03:04 to 09:30:00 ET, so **pre-market through the opening bell**. Executions are rare in this window (0.41% of messages), which is why the mix is dominated by adds, deletes and cancels.
- Terms: the directory has no licence or readme; NASDAQ's US equities data policy lists Historical TotalView-ITCH as "Not Eligible" in its redistribution table. So the raw data is not committed and `scripts/fetch_itch_sample.sh` fetches it. This file contains only counts and cycle statistics, no message contents and no prices.

## How to read the latency numbers

Latency is **simulated cycles of this model driven by real messages**, not a hardware measurement. Stage latencies are the model's documented parameters (README). Arrival times come from the real message timestamps (1 cycle = 512/165 ns, integer arithmetic); idle gaps longer than 256 cycles are shortened to 256 because the pipeline drains in about 20 cycles (checked: caps of 64, 256 and 4096 give identical output on this slice).

The model follows **one stock** at a time (the model's book is single-symbol), so only that stock's messages reach it; queueing from other symbols' traffic is not simulated. The model book keeps 5 price levels per side and counts adds it drops; a real book is far deeper (below), so the model's book contents and OFI diverge from reality while latency, which depends on message length and wire queueing, does not.

## Run 1: busiest stock by add orders (default choice)

```
fpga-sim-itch-replay: REAL NASDAQ TotalView-ITCH 5.0 messages (not synthetic)
input: 343015424 bytes of decompressed ITCH; 10672014 messages parsed; framing stop: truncated final message (slice boundary)
message timestamps in slice: 23160.569 s of exchange time since midnight (first 11039.688 s, last 34200.257 s)

message mix (all symbols):
  A  Add Order                         3781651   35.44%
  D  Order Delete                      3642519   34.13%
  X  Order Cancel                      1071995   10.04%
  I  NOII                              1047885    9.82%
  U  Order Replace                      495476    4.64%
  F  Add Order (MPID)                   352697    3.30%
  L  Market Participant Position        193722    1.82%
  E  Order Executed                      44157    0.41%
  P  Trade (non-cross)                   14112    0.13%
  H  Stock Trading Action                 8716    0.08%
  R  Stock Directory                      8714    0.08%
  Y  Reg SHO Restriction                  8713    0.08%
  Q  Cross Trade                          1567    0.01%
  C  Order Executed With Price              86    0.00%
  S  System Event                            3    0.00%
  V  MWCB Decline Level                      1    0.00%
busiest 1 ms of exchange time: 1401 messages across all symbols

stock followed by the single-symbol book and model: RDS.B (stock locate 6537; 111682 add orders in the slice)

real order book for RDS.B over the slice (312645 book-changing messages, full depth, rebuilt from the messages):
  bid price levels       min 0  median 11  p99 32  max 47
  ask price levels       min 0  median 10  p99 35  max 49
  resting orders         min 1  median 48  p99 105  max 179
  after 99.94% of messages either side held more than the model's 5 levels
  messages referencing an order not seen in the slice: 0

cycle model on the first 20000 messages of RDS.B (arrival times from the real timestamps; idle gaps capped at 256 cycles):
  messages replayed: A=6898 D=6861 U=816 X=5425
  simulated cycles run: 5113898
tick-to-trade latency, SIMULATED CYCLES on real messages (not a hardware measurement):
  min       14 cycles = 43.4424 ns
  median    14 cycles = 43.4424 ns
  p99       16 cycles = 49.6485 ns
  max       16 cycles = 49.6485 ns
histogram (2-cycle bins, last bin includes overflow):
   14- 15 cycles  12286
   16- 17 cycles   7714
model order book (5 levels) adds dropped because a side was full: 3622
```

## Run 2: AAPL (`--symbol AAPL`), the stock-level section only

Same file, same slice, same message mix as run 1.

```
stock followed by the single-symbol book and model: AAPL (stock locate 14; 12515 add orders in the slice)

real order book for AAPL over the slice (25259 book-changing messages, full depth, rebuilt from the messages):
  bid price levels       min 1  median 136  p99 262  max 268
  ask price levels       min 0  median 143  p99 231  max 237
  resting orders         min 1  median 477  p99 1145  max 1162
  after 99.95% of messages either side held more than the model's 5 levels
  messages referencing an order not seen in the slice: 0

cycle model on the first 20000 messages of AAPL (arrival times from the real timestamps; idle gaps capped at 256 cycles):
  messages replayed: A=9944 D=7158 E=2862 F=18 U=1 X=17
  simulated cycles run: 4951452
tick-to-trade latency, SIMULATED CYCLES on real messages (not a hardware measurement):
  min       14 cycles = 43.4424 ns
  median    16 cycles = 49.6485 ns
  p99       21 cycles = 65.1636 ns
  max      105 cycles = 325.8182 ns
histogram (2-cycle bins, last bin includes overflow):
   14- 15 cycles   9451
   16- 17 cycles   9903
   18- 19 cycles      2
   20- 21 cycles    483
   24- 25 cycles     85
   26- 27 cycles     14
   30- 31 cycles     25
   34- 35 cycles     11
   36- 37 cycles      1
   40- 41 cycles      5
   44- 45 cycles      4
   50- 51 cycles      2
   54- 55 cycles      2
   60- 61 cycles      2
   62- 63 cycles     10
model order book (5 levels) adds dropped because a side was full: 9806
```

## Reproduce

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --parallel
scripts/fetch_itch_sample.sh            # fetches 128 MiB, prints the SHA-256, runs the replay
scripts/fetch_itch_sample.sh 134217728 01302019.NASDAQ_ITCH50.gz --symbol AAPL
```
