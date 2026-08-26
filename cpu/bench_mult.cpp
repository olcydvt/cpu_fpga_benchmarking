// SPSC end-to-end latency benchmark.
// Producer (core A): timestamps each byte, pushes to ring.
// Consumer (core B): pops bytes, parses with a byte-serial FSM.
// Latency = last byte pushed -> message parsed (queue crossing + parse).
// FPGA analog: one clock domain writes, another reads = async FIFO.
//
// Build: g++ -O3 -march=native -std=c++17 -pthread spsc_bench.cpp -o spsc_bench
// Run:   ./spsc_bench 0 1        (producer core, consumer core)
// Tip:   use two separate physical cores (check lscpu -e); sibling
//        hyperthreads share cache and give misleadingly low numbers.

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <algorithm>
#include <random>
#include <thread>
#include <chrono>
#include <pthread.h>
#include <sched.h>
#include <x86intrin.h>

// rdtsc with lfence on both sides so it isn't reordered around the work.
// Assumes invariant TSC (constant_tsc + nonstop_tsc), single socket.
static inline uint64_t rdtsc_now() {
    _mm_lfence();
    uint64_t t = __rdtsc();
    _mm_lfence();
    return t;
}

// Measure the real TSC rate (GHz) instead of hardcoding it, so ns is correct
// on any machine. Count TSC cycles over a known steady_clock interval.
static double calibrate_tsc_ghz() {
    using clock = std::chrono::steady_clock;
    (void)rdtsc_now();                          // warmup
    const auto wall_start = clock::now();
    const uint64_t tsc_start = rdtsc_now();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const uint64_t tsc_end = rdtsc_now();
    const auto wall_end = clock::now();
    const double elapsed_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(wall_end - wall_start).count();
    const double cycles = (double)(tsc_end - tsc_start);
    return cycles / elapsed_ns;                 // cycle/ns = GHz
}

// Lock-free SPSC ring. C++ analog of the FPGA async FIFO.
// head_/tail_ on separate cache lines to avoid false sharing.
template <typename T, size_t CAPACITY>
class SPSCRingBuffer {
    static_assert((CAPACITY & (CAPACITY - 1)) == 0, "CAPACITY must be power of two");
    static constexpr size_t MASK = CAPACITY - 1;

    alignas(64) std::atomic<size_t> head_{0};   // consumer writes
    alignas(64) std::atomic<size_t> tail_{0};   // producer writes
    alignas(64) T buffer_[CAPACITY];

public:
    bool push(const T& item) {                  // false if full (backpressure)
        const size_t tail = tail_.load(std::memory_order_relaxed);
        const size_t next = (tail + 1) & MASK;
        if (next == head_.load(std::memory_order_acquire)) return false;
        buffer_[tail] = item;
        tail_.store(next, std::memory_order_release);   // data before index
        return true;
    }

    bool pop(T& out) {                          // false if empty
        const size_t head = head_.load(std::memory_order_relaxed);
        if (head == tail_.load(std::memory_order_acquire)) return false;
        out = buffer_[head];
        head_.store((head + 1) & MASK, std::memory_order_release);
        return true;
    }
};

// One byte carried through the ring: value + production timestamp + end flag.
struct TimedByte {
    uint64_t tsc;       // when the producer wrote this byte
    uint8_t  byte;
    uint8_t  is_last;   // last byte of a message (qty_lo)
};

static constexpr uint8_t  SYNC     = 0xAA;
static constexpr size_t   RING_CAP = 1024;          // power of two
static constexpr int      N_MSGS   = 5'000'000;

SPSCRingBuffer<TimedByte, RING_CAP> g_ring;
std::atomic<bool> g_producer_done{false};

// Results. resize() maps pages up front so page faults don't hit the hot path;
// the hot path writes by index (no push_back).
std::vector<uint64_t> g_latencies;   // per-message latency (cycles)
std::vector<uint64_t> g_wall_tsc;    // per-message absolute TSC (end instant)
std::vector<uint32_t> g_msg_index;   // message index

double g_tsc_ghz = 0.0;              // calibrated at startup

// Pin the calling thread to a core as its first action, before any work.
// Avoids the race where a thread starts on the wrong core and migrates.
static bool pin_self_to_core(int core) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core, &cpuset);
    return pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) == 0;
}

int main(int argc, char** argv) {
    int producer_core = 2;
    int consumer_core = 3;
    if (argc == 3) {
        producer_core = std::atoi(argv[1]);
        consumer_core = std::atoi(argv[2]);
    }

    g_tsc_ghz = calibrate_tsc_ghz();
    printf("Calibrated TSC: %.4f GHz\n", g_tsc_ghz);

    // Pre-generate test data so no RNG/allocation happens during measurement.
    // Layout: [garbage bytes] SYNC TYPE PRICE_HI PRICE_LO QTY_HI QTY_LO
    std::mt19937 rng(12345);
    std::uniform_int_distribution<int> bd(0, 255), gd(0, 2);

    std::vector<TimedByte> input;               // tsc filled by the producer
    input.reserve(N_MSGS * 8);
    for (int m = 0; m < N_MSGS; ++m) {
        int garbage = gd(rng);
        for (int g = 0; g < garbage; ++g) {
            uint8_t x = bd(rng);
            if (x == SYNC) x = 0x00;            // don't emit a stray SYNC
            input.push_back({0, x, 0});
        }
        input.push_back({0, SYNC, 0});
        input.push_back({0, (uint8_t)bd(rng), 0}); // type
        input.push_back({0, (uint8_t)bd(rng), 0}); // price hi
        input.push_back({0, (uint8_t)bd(rng), 0}); // price lo
        input.push_back({0, (uint8_t)bd(rng), 0}); // qty hi
        input.push_back({0, (uint8_t)bd(rng), 1}); // qty lo = last byte
    }
    const size_t total_bytes = input.size();

    // resize() sizes and touches every page, so minor page faults fire now,
    // not in the hot path. Hot path writes by index.
    g_latencies.resize(N_MSGS);
    g_wall_tsc.resize(N_MSGS);
    g_msg_index.resize(N_MSGS);

    printf("Test data: %zu bytes, %d messages\n", total_bytes, N_MSGS);
    printf("Producer core: %d, Consumer core: %d\n", producer_core, consumer_core);
    printf("Ring capacity: %zu\n\n", RING_CAP);

    // CONSUMER: pop bytes, parse with the byte-serial FSM (IDLE->T->PH->PL->QH->QL).
    // On the last byte, latency = now - that byte's production tsc.
    auto consumer_fn = [&]() {
        if (!pin_self_to_core(consumer_core))   // pin first, before any work
            fprintf(stderr, "WARN: could not pin consumer to core %d\n", consumer_core);

        enum State { IDLE, T, PH, PL, QH, QL };
        State st = IDLE;
        TimedByte tb;
        int parsed = 0;

        while (parsed < N_MSGS) {
            if (!g_ring.pop(tb)) {
                if (g_producer_done.load(std::memory_order_acquire)) {
                    if (!g_ring.pop(tb)) break;  // one last try
                } else {
                    continue;                    // busy-spin
                }
            }
            const uint8_t b = tb.byte;
            switch (st) {
                case IDLE: if (b == SYNC) st = T;  break;
                case T:    st = PH;                break;
                case PH:   st = PL;                break;
                case PL:   st = QH;                break;
                case QH:   st = QL;                break;
                case QL: {
                    if (tb.is_last) {            // timestamp only what we record
                        const uint64_t now = rdtsc_now();
                        g_latencies[parsed] = now - tb.tsc;   // write by index
                        g_wall_tsc[parsed]  = now;            // absolute end time
                        g_msg_index[parsed] = (uint32_t)parsed;
                        ++parsed;
                    }
                    st = IDLE;
                    break;
                }
            }
        }
    };

    // PRODUCER: timestamp each byte at the moment of the write, push to ring.
    auto producer_fn = [&]() {
        if (!pin_self_to_core(producer_core))   // pin first, before any work
            fprintf(stderr, "WARN: could not pin producer to core %d\n", producer_core);

        for (size_t i = 0; i < total_bytes; ++i) {
            TimedByte tb = input[i];
            tb.tsc = rdtsc_now();               // real production instant
            while (!g_ring.push(tb)) { }        // spin on backpressure
        }
        g_producer_done.store(true, std::memory_order_release);
    };

    // Each thread self-pins as its first action, so no post-hoc pinning needed.
    std::thread consumer(consumer_fn);
    std::thread producer(producer_fn);
    producer.join();
    consumer.join();

    if (g_latencies.empty()) { printf("ERROR: no latencies measured\n"); return 1; }

    // PERIODICITY CHECK: take the slowest messages' absolute times (g_wall_tsc)
    // and measure the real gap between consecutive stall peaks. Consistent gaps
    // prove the stall is periodic. Done before sort (index order preserved).
    {
        struct Rec { uint64_t lat; uint32_t idx; uint64_t wall; };
        std::vector<Rec> recs(g_latencies.size());
        for (size_t i = 0; i < g_latencies.size(); ++i)
            recs[i] = { g_latencies[i], g_msg_index[i], g_wall_tsc[i] };

        std::sort(recs.begin(), recs.end(),
                  [](const Rec& a, const Rec& b){ return a.lat > b.lat; });

        // A single stall hits many consecutive messages; keep only distinct
        // peaks (indices far apart), then sort those by absolute time.
        std::vector<Rec> peaks;
        for (const auto& r : recs) {
            bool near = false;
            for (const auto& p : peaks)
                if ((r.idx > p.idx ? r.idx - p.idx : p.idx - r.idx) < 1000) { near = true; break; }
            if (!near) peaks.push_back(r);
            if (peaks.size() >= 12) break;
        }
        std::sort(peaks.begin(), peaks.end(),
                  [](const Rec& a, const Rec& b){ return a.wall < b.wall; });

        printf("=== PERIODICITY: distinct stall peaks (gap in absolute time) ===\n");
        printf("  peak   msg_index    latency(ns)    gap from prev (ms)\n");
        for (size_t i = 0; i < peaks.size(); ++i) {
            double gap_ms = 0.0;
            if (i > 0)
                gap_ms = (double)(peaks[i].wall - peaks[i-1].wall) / g_tsc_ghz / 1e6;
            printf("  %3zu   %10u    %9.1f    %s%.2f\n",
                   i, peaks[i].idx, (double)peaks[i].lat / g_tsc_ghz,
                   (i==0 ? "(first) " : ""), gap_ms);
        }
        printf("  -> consistent gaps => the stall is periodic.\n\n");
    }

    std::sort(g_latencies.begin(), g_latencies.end());
    auto pct = [&](double p) {
        size_t idx = (size_t)(p / 100.0 * g_latencies.size());
        if (idx >= g_latencies.size()) idx = g_latencies.size() - 1;
        return g_latencies[idx];
    };
    auto ns = [&](uint64_t c) { return (double)c / g_tsc_ghz; };   // calibrated

    printf("Messages parsed: %zu\n\n", g_latencies.size());
    printf("=== SPSC END-TO-END LATENCY (last byte pushed -> parsed) ===\n");
    printf("           cycles        ns (@%.4f GHz)\n", g_tsc_ghz);
    printf("  min:    %8lu    %8.1f\n", g_latencies.front(), ns(g_latencies.front()));
    printf("  p50:    %8lu    %8.1f\n", pct(50),   ns(pct(50)));
    printf("  p90:    %8lu    %8.1f\n", pct(90),   ns(pct(90)));
    printf("  p99:    %8lu    %8.1f\n", pct(99),   ns(pct(99)));
    printf("  p99.9:  %8lu    %8.1f\n", pct(99.9), ns(pct(99.9)));
    printf("  p99.99: %8lu    %8.1f\n", pct(99.99),ns(pct(99.99)));
    printf("  max:    %8lu    %8.1f\n", g_latencies.back(), ns(g_latencies.back()));

    printf("\n=== JITTER ===\n");
    printf("  body spread (p99.9 - p50): %lu cyc  (%.1f ns)\n",
           pct(99.9) - pct(50), ns(pct(99.9) - pct(50)));
    printf("  worst-case (max - p50):    %lu cyc  (%.1f ns)\n",
           g_latencies.back() - pct(50), ns(g_latencies.back() - pct(50)));
    printf("  max / p50 ratio:           %.1fx\n",
           (double)g_latencies.back() / pct(50));

    return 0;
}
