// SPSC End-to-End Latency Benchmark
// Producer: writes timestamped bytes to ring
// Consumer: reads from ring and parses via byte-serial FSM
// Latency: measured from last byte entering ring to parse completion
//
// Compile: g++ -O3 -march=native -std=c++17 -pthread spsc_bench.cpp -o spsc_bench
// Run:     ./spsc_bench [producer_core] [consumer_core]

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

// Read timestamp counter (assumes invariant TSC)
static inline uint64_t rdtsc_now() {
    _mm_lfence();
    uint64_t t = __rdtsc();
    _mm_lfence();
    return t;
}

// Calibrate TSC frequency (GHz) using steady_clock
static double calibrate_tsc_ghz() {
    using clock = std::chrono::steady_clock;
    
    (void)rdtsc_now(); // Warmup
    const auto wall_start = clock::now();
    const uint64_t tsc_start = rdtsc_now();

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    const uint64_t tsc_end = rdtsc_now();
    const auto wall_end = clock::now();

    const double elapsed_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(wall_end - wall_start).count();
    const double cycles = (double)(tsc_end - tsc_start);
    return cycles / elapsed_ns;
}

// Lock-free SPSC ring buffer
template <typename T, size_t CAPACITY>
class SPSCRingBuffer {
    static_assert((CAPACITY & (CAPACITY - 1)) == 0, "CAPACITY must be a power of 2");
    static constexpr size_t MASK = CAPACITY - 1;

    alignas(64) std::atomic<size_t> head_{0}; // Written by consumer
    alignas(64) std::atomic<size_t> tail_{0}; // Written by producer
    alignas(64) T buffer_[CAPACITY];

public:
    // Returns false if full
    bool push(const T& item) {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        const size_t next = (tail + 1) & MASK;
        if (next == head_.load(std::memory_order_acquire)) return false; 
        buffer_[tail] = item;
        tail_.store(next, std::memory_order_release);
        return true;
    }

    // Returns false if empty
    bool pop(T& out) {
        const size_t head = head_.load(std::memory_order_relaxed);
        if (head == tail_.load(std::memory_order_acquire)) return false; 
        out = buffer_[head];
        head_.store((head + 1) & MASK, std::memory_order_release);
        return true;
    }
};

// Ring buffer data element
struct TimedByte {
    uint64_t tsc;     // Production timestamp
    uint8_t  byte;
    uint8_t  is_last; // End of message flag
};

static constexpr uint8_t  SYNC     = 0xAA;
static constexpr size_t   RING_CAP = 1024;
static constexpr int      N_MSGS   = 5'000'000;

SPSCRingBuffer<TimedByte, RING_CAP> g_ring;
std::atomic<bool> g_producer_done{false};

std::vector<uint64_t> g_latencies; // Measured latencies (cycles)
std::vector<uint32_t> g_msg_index; // Message indices for diagnostics
double g_tsc_ghz = 0.0;            // Calibrated TSC frequency

// Pin thread to specific CPU core
static bool pin_to_core(std::thread& th, int core) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core, &cpuset);
    int rc = pthread_setaffinity_np(th.native_handle(), sizeof(cpu_set_t), &cpuset);
    return rc == 0;
}

int main(int argc, char** argv) {
    int producer_core = 2;
    int consumer_core = 3;
    if (argc == 3) {
        producer_core = std::atoi(argv[1]);
        consumer_core = std::atoi(argv[2]);
    }

    // Calibrate TSC frequency
    g_tsc_ghz = calibrate_tsc_ghz();
    printf("Calibrated TSC frequency: %.4f GHz\n", g_tsc_ghz);

    // Generate test data in advance to avoid overhead during measurement
    // Format: [garbage bytes] SYNC TYPE PRICE_HI PRICE_LO QTY_HI QTY_LO
    std::mt19937 rng(12345);
    std::uniform_int_distribution<int> bd(0, 255), gd(0, 2);

    std::vector<TimedByte> input;
    input.reserve(N_MSGS * 8);
    for (int m = 0; m < N_MSGS; ++m) {
        int garbage = gd(rng);
        for (int g = 0; g < garbage; ++g) {
            uint8_t x = bd(rng);
            if (x == SYNC) x = 0x00; // Avoid accidental SYNC
            input.push_back({0, x, 0});
        }
        input.push_back({0, SYNC, 0});
        input.push_back({0, (uint8_t)bd(rng), 0}); // type
        input.push_back({0, (uint8_t)bd(rng), 0}); // price hi
        input.push_back({0, (uint8_t)bd(rng), 0}); // price lo
        input.push_back({0, (uint8_t)bd(rng), 0}); // qty hi
        input.push_back({0, (uint8_t)bd(rng), 1}); // qty lo (last byte)
    }
    
    const size_t total_bytes = input.size();
    g_latencies.reserve(N_MSGS);
    g_msg_index.reserve(N_MSGS);

    printf("Test data: %zu bytes, %d messages\n", total_bytes, N_MSGS);
    printf("Producer core: %d, Consumer core: %d\n", producer_core, consumer_core);
    printf("Ring capacity: %zu\n\n", RING_CAP);

    // Consumer: Read bytes and parse via state machine
    auto consumer_fn = [&]() {
        enum State { IDLE, T, PH, PL, QH, QL };
        State st = IDLE;
        TimedByte tb;
        int parsed = 0;

        while (parsed < N_MSGS) {
            if (!g_ring.pop(tb)) {
                if (g_producer_done.load(std::memory_order_acquire)) {
                    if (!g_ring.pop(tb)) break; 
                } else {
                    continue; // Busy-spin
                }
            }
            
            const uint8_t b = tb.byte;
            switch (st) {
                case IDLE: if (b == SYNC) st = T; break;
                case T:    st = PH;               break;
                case PH:   st = PL;               break;
                case PL:   st = QH;               break;
                case QH:   st = QL;               break;
                case QL: {
                    const uint64_t now = rdtsc_now();
                    if (tb.is_last) {
                        g_latencies.push_back(now - tb.tsc);
                        g_msg_index.push_back((uint32_t)parsed);
                        ++parsed;
                    }
                    st = IDLE;
                    break;
                }
            }
        }
    };

    // Producer: Timestamp and push bytes to ring
    auto producer_fn = [&]() {
        for (size_t i = 0; i < total_bytes; ++i) {
            TimedByte tb = input[i];
            tb.tsc = rdtsc_now(); 
            while (!g_ring.push(tb)) {
                // Busy-spin (backpressure)
            }
        }
        g_producer_done.store(true, std::memory_order_release);
    };

    // Start and pin threads
    std::thread consumer(consumer_fn);
    std::thread producer(producer_fn);

    if (!pin_to_core(consumer, consumer_core))
        fprintf(stderr, "WARNING: Failed to pin consumer to core %d\n", consumer_core);
    if (!pin_to_core(producer, producer_core))
        fprintf(stderr, "WARNING: Failed to pin producer to core %d\n", producer_core);

    producer.join();
    consumer.join();

    if (g_latencies.empty()) { printf("ERROR: No latency measured\n"); return 1; }

    // DIAGNOSTIC 1: Top 20 slowest messages
    {
        std::vector<std::pair<uint64_t,uint32_t>> pairs;
        pairs.reserve(g_latencies.size());
        for (size_t i = 0; i < g_latencies.size(); ++i)
            pairs.push_back({g_latencies[i], g_msg_index[i]});

        std::sort(pairs.begin(), pairs.end(),
                  [](auto& a, auto& b){ return a.first > b.first; });

        printf("=== DIAGNOSTIC 1: TOP 20 SLOWEST MESSAGES (index / latency) ===\n");
        printf("(index = message number, 0..%d)\n", N_MSGS-1);
        for (int i = 0; i < 20 && i < (int)pairs.size(); ++i) {
            printf("  #%2d: msg index=%8u   latency=%8lu cyc  (%.1f ns)\n",
                   i+1, pairs[i].second, pairs[i].first,
                   (double)pairs[i].first / g_tsc_ghz);
        }
        printf("\n");
    }

    // DIAGNOSTIC 2: Latency distribution over time (10 slices)
    {
        const int SLICES = 10;
        const size_t slice_sz = g_latencies.size() / SLICES;
        printf("=== DIAGNOSTIC 2: LATENCY TIME SLICES (%zu msgs/slice) ===\n", slice_sz);
        printf("slice    avg(ns)    max(ns)    max_index\n");
        for (int s = 0; s < SLICES; ++s) {
            size_t start = s * slice_sz;
            size_t end = (s == SLICES-1) ? g_latencies.size() : start + slice_sz;
            uint64_t sum = 0, mx = 0; uint32_t mx_idx = 0;
            for (size_t i = start; i < end; ++i) {
                sum += g_latencies[i];
                if (g_latencies[i] > mx) { mx = g_latencies[i]; mx_idx = g_msg_index[i]; }
            }
            double avg = (double)sum / (end - start);
            printf("  %2d    %8.1f  %9.1f   %u\n",
                   s, avg / g_tsc_ghz, (double)mx / g_tsc_ghz, mx_idx);
        }
        printf("\n");
    }

    // DIAGNOSTIC 3: Threshold analysis
    {
        std::sort(g_latencies.begin(), g_latencies.end());
        uint64_t median = g_latencies[g_latencies.size()/2];
        uint64_t threshold = median * 10;
        printf("=== DIAGNOSTIC 3: median=%lu cyc, threshold(10x)=%lu cyc ===\n",
               median, threshold);
        printf("(check index list above for detailed interval analysis)\n\n");
    }

    auto pct = [&](double p) {
        size_t idx = (size_t)(p / 100.0 * g_latencies.size());
        if (idx >= g_latencies.size()) idx = g_latencies.size() - 1;
        return g_latencies[idx];
    };

    auto ns = [&](uint64_t c) { return (double)c / g_tsc_ghz; };

    printf("Parsed messages: %zu\n\n", g_latencies.size());
    printf("=== SPSC END-TO-END LATENCY ===\n");
    printf("            cycles         ns (@%.4f GHz)\n", g_tsc_ghz);
    printf("  min:    %8lu    %8.1f\n", g_latencies.front(), ns(g_latencies.front()));
    printf("  p50:    %8lu    %8.1f\n", pct(50),    ns(pct(50)));
    printf("  p90:    %8lu    %8.1f\n", pct(90),    ns(pct(90)));
    printf("  p99:    %8lu    %8.1f\n", pct(99),    ns(pct(99)));
    printf("  p99.9:  %8lu    %8.1f\n", pct(99.9),  ns(pct(99.9)));
    printf("  p99.99: %8lu    %8.1f\n", pct(99.99), ns(pct(99.99)));
    printf("  max:    %8lu    %8.1f\n", g_latencies.back(), ns(g_latencies.back()));

    printf("\n=== JITTER ===\n");
    printf("  body spread (p99.9 - p50): %lu cyc  (%.1f ns)\n",
           pct(99.9) - pct(50), ns(pct(99.9) - pct(50)));
    printf("  worst-case (max - p50):     %lu cyc  (%.1f ns)\n",
           g_latencies.back() - pct(50), ns(g_latencies.back() - pct(50)));
    printf("  max / p50 ratio:            %.1fx\n",
           (double)g_latencies.back() / pct(50));

    return 0;
}