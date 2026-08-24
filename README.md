# cpu_fpga_benchmarking

Measuring the latency of the same market-data feed parser built two ways: in C++
on a server CPU, and in hardware on a small FPGA.

The goal is not to find the fastest median. It is to compare the **worst case**.
In high-frequency trading you need to know when your order will go out, not just
how fast it usually is.

## Result in one table

| Metric  | C++ same core | C++ separate cores | FPGA (Trion T20) |
|---------|---------------|--------------------|------------------|
| p50     | 62.5 ns       | 359 ns             | 60 ns            |
| p99     | 84 ns         | 1,028 ns           | 60 ns            |
| p99.9   | 7,115 ns      | 11,865 ns          | 60 ns            |
| p99.99  | 359,000 ns    | 358,000 ns         | 60 ns            |
| max     | 379,000 ns    | 374,000 ns         | 60 ns            |

- The CPU has a good median but a long tail (~360 us at p99.99).
- The FPGA is a single value: 60 ns, every message. No tail.

The CPU tail is a periodic system stall (~373 us every few hundred ms). It is not
warm-up, and it is not steal time. Core isolation (`isolcpus`, `nohz_full`) did
not remove it — it only lowered the clock on the isolated cores. Software on an OS
cannot fully remove this; the FPGA has no OS, so the tail is not there.

## What "latency" means here

The full path, not just the parse step: from the moment the last byte of a
message is written into the queue, to the moment the parser finishes that message.
That includes the queue write, the crossing between the two domains (two CPU cores
/ two clock domains), the read, and the parse.

## Message format

Fixed 6-byte message:

```
SYNC(0xAA) | TYPE(1B) | PRICE(2B, big-endian) | QTY(2B, big-endian)
```

## Repository layout

```
cpu/    C++ SPSC benchmark
fpga/   Efinix Trion T20 design (SystemVerilog)
```

### cpu/

A single-producer / single-consumer benchmark. Two threads pinned to two cores:
the producer builds messages, timestamps the last byte with `rdtsc`, and pushes
bytes into a lock-free ring buffer; the consumer pops bytes, runs the parser
state machine, and timestamps when each message is done.

Notes:
- The TSC rate is calibrated at startup against `steady_clock` (not hardcoded).
  The chip has `constant_tsc` and `nonstop_tsc`, so the TSC counts real time.
- There is a diagnostic build that logs which message index each slow latency
  belongs to — that is how the periodic stall was identified.

Build and run:

```bash
g++ -O3 -march=native -std=c++17 -pthread bench.cpp -o bench

# same physical core (two hyperthreads) — best case
./bench 0 4

# separate physical cores — realistic case, fair match for the FPGA
./bench 0 1
```

Pass the two core IDs as arguments. Check `lscpu -e` to see which CPUs are
separate physical cores vs sibling hyperthreads.

Test machine: AWS `c7i.2xlarge` (4 physical cores, 8 vCPUs, base 2.4 GHz),
5,000,000 messages per run.

### fpga/

The same pipeline in hardware, four blocks:

```
test_generator --> async FIFO (CDC) --> feed_parser_core --> latency_counter
  (wr_clk 50MHz)                          (rd_clk 100MHz)
```

- **test_generator** — builds messages, writes bytes into the FIFO, pulses on the
  last byte.
- **async FIFO** — the clock-domain crossing (the hardware ring buffer).
- **feed_parser_core** — the framer state machine; raises `msg_valid` when a
  message is decoded.
- **latency_counter** — counts read-clock cycles from the last-byte pulse to
  `msg_valid`.

Hardware:
- Chip: Efinix Trion **T20Q144C3** (entry-level, 144-pin).
- Board: TriPi development board, 50 MHz oscillator into a PLL.
- Clocks: PLL produces 100 MHz read clock and 50 MHz write clock. 250 MHz did not
  meet timing on this chip (max ~121 MHz), so the design runs at 100/50 MHz.
- Tool: Efinity. Measured on the running chip with an on-chip logic analyzer over
  JTAG. The analyzer taps the latency signal from the side, so it does not add to
  the measured number.

Result: every message measured **6 cycles = 60 ns**, with no variation.

## Why the FPGA is flat

The 6 cycles are fixed hardware: a couple to cross the clock domain, a couple for
the state machine. No OS, no scheduler, no cache that can miss. The same logic
runs every time, so the answer is always 6.

## Where each one fits

This is not "FPGA good, CPU bad." Most software should stay on the CPU — it wins
when logic is complex or changes often, when you need general-purpose work, or
when the median matters more than the tail. The FPGA wins when the worst case is
what you are paying for, and the work is simple, fixed, and streaming. A common
design uses both: FPGA for the fixed, latency-critical front end (decode, filter,
timestamp), CPU for the flexible work behind it.

## Write-up

Full write-up with figures: see the article (link to your blog post here).
