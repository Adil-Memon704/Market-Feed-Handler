#include <cstdio>
#include <vector>
#include <thread>
#include <atomic>
#include <numeric>
#include <algorithm>
#include "../include/common/cache.h"

static uint64_t percentile(std::vector<uint64_t>& v, double pct) {
    size_t idx = static_cast<size_t>(pct * v.size());
    if (idx >= v.size()) idx = v.size() - 1;
    std::nth_element(v.begin(), v.begin() + idx, v.end());
    return v[idx];
}

int main() {
    std::fprintf(stdout, "SymbolCache Benchmark\n");
    std::fprintf(stdout, "=====================\n\n");

    SymbolCache cache(DEFAULT_SYMBOLS);
    const int N = 1'000'000;

    // ── Write benchmark ─────────────────────────────────────────────────────
    {
        uint64_t t0 = mono_ns();
        for (int i = 0; i < N; ++i) {
            uint16_t sym = static_cast<uint16_t>(i % DEFAULT_SYMBOLS);
            cache.update_quote(sym,
                100.0 + i * 0.01, 500,
                100.5 + i * 0.01, 300,
                static_cast<uint64_t>(i));
        }
        uint64_t t1 = mono_ns();
        double rate  = N / ((t1 - t0) / 1e9);
        double ns_ea = (t1 - t0) / static_cast<double>(N);
        std::fprintf(stdout, "Write throughput: %.0f updates/s (%.1f ns each)\n",
                     rate, ns_ea);
    }

    // ── Read latency benchmark (single-threaded, no contention) ─────────────
    {
        std::vector<uint64_t> samples;
        samples.reserve(N);

        for (int i = 0; i < N; ++i) {
            uint16_t sym = static_cast<uint16_t>(i % DEFAULT_SYMBOLS);
            uint64_t t0 = mono_ns();
            volatile MarketState s = cache.get_snapshot(sym);
            uint64_t t1 = mono_ns();
            (void)s;
            samples.push_back(t1 - t0);
        }

        std::fprintf(stdout, "Read latency (no contention):\n");
        std::fprintf(stdout, "  p50  = %llu ns\n", (unsigned long long)percentile(samples, 0.50));
        std::fprintf(stdout, "  p99  = %llu ns\n", (unsigned long long)percentile(samples, 0.99));
        std::fprintf(stdout, "  p999 = %llu ns\n", (unsigned long long)percentile(samples, 0.999));
    }

    // ── Read latency under write contention ──────────────────────────────────
    {
        std::atomic<bool> stop{false};
        // Background writer at 100K updates/s
        std::thread writer([&] {
            int i = 0;
            while (!stop.load()) {
                cache.update_quote(
                    static_cast<uint16_t>(i % DEFAULT_SYMBOLS),
                    100.0 + i, 500, 100.5 + i, 300,
                    static_cast<uint64_t>(i));
                ++i;
                struct timespec ts{ 0, 10'000L };
                clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, nullptr);
            }
        });

        std::vector<uint64_t> samples;
        samples.reserve(100'000);
        for (int i = 0; i < 100'000; ++i) {
            uint16_t sym = static_cast<uint16_t>(i % DEFAULT_SYMBOLS);
            uint64_t t0  = mono_ns();
            volatile MarketState s = cache.get_snapshot(sym);
            uint64_t t1  = mono_ns();
            (void)s;
            samples.push_back(t1 - t0);
        }
        stop.store(true);
        writer.join();

        std::fprintf(stdout, "\nRead latency (with 100K/s writer contention):\n");
        std::fprintf(stdout, "  p50  = %llu ns\n", (unsigned long long)percentile(samples, 0.50));
        std::fprintf(stdout, "  p99  = %llu ns\n", (unsigned long long)percentile(samples, 0.99));
        std::fprintf(stdout, "  p999 = %llu ns\n", (unsigned long long)percentile(samples, 0.999));

        // Verify target: < 50 ns average
        double mean = std::accumulate(samples.begin(), samples.end(), 0ULL) /
                      static_cast<double>(samples.size());
        std::fprintf(stdout, "  mean = %.1f ns\n", mean);
        if (mean > 50.0) {
            std::fprintf(stderr, "WARN: Mean read latency %.1f ns exceeds 50 ns target\n", mean);
            return 1;
        }
    }

    return 0;
}
