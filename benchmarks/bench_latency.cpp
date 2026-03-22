#include <cstdio>
#include <vector>
#include <thread>
#include <random>
#include "../include/common/latency_tracker.h"
#include "../include/protocol.h"

int main() {
    std::fprintf(stdout, "LatencyTracker Benchmark\n");
    std::fprintf(stdout, "========================\n\n");

    LatencyTracker tracker;
    const int N = 1'000'000;

    // ── Record overhead ──────────────────────────────────────────────────────
    {
        uint64_t t0 = mono_ns();
        for (int i = 0; i < N; ++i) {
            tracker.record(static_cast<uint64_t>(i % 100'000));
        }
        uint64_t t1  = mono_ns();
        double ns_ea = (t1 - t0) / static_cast<double>(N);
        std::fprintf(stdout, "record() cost: %.1f ns per call (target < 30 ns)\n", ns_ea);
        if (ns_ea > 30.0) {
            std::fprintf(stderr, "WARN: record() overhead %.1f ns exceeds 30 ns target\n", ns_ea);
        }
    }

    // ── Percentile accuracy ───────────────────────────────────────────────────
    {
        LatencyStats s = tracker.get_stats();
        std::fprintf(stdout, "\nStats for 1M samples [0, 100K) ns:\n");
        std::fprintf(stdout, "  count = %llu\n", (unsigned long long)s.sample_count);
        std::fprintf(stdout, "  min   = %llu ns\n", (unsigned long long)s.min_ns);
        std::fprintf(stdout, "  mean  = %llu ns\n", (unsigned long long)s.mean_ns);
        std::fprintf(stdout, "  p50   = %llu ns\n", (unsigned long long)s.p50_ns);
        std::fprintf(stdout, "  p99   = %llu ns\n", (unsigned long long)s.p99_ns);
        std::fprintf(stdout, "  p999  = %llu ns\n", (unsigned long long)s.p999_ns);
        std::fprintf(stdout, "  max   = %llu ns\n", (unsigned long long)s.max_ns);

        // p50 should be around 50,000 ns
        if (s.p50_ns < 45'000 || s.p50_ns > 55'000) {
            std::fprintf(stderr, "WARN: p50 = %llu ns far from expected ~50000 ns\n",
                         (unsigned long long)s.p50_ns);
        }
    }

    // ── CSV export ────────────────────────────────────────────────────────────
    bool ok = tracker.export_csv("/tmp/latency_histogram.csv");
    std::fprintf(stdout, "\nCSV export: %s\n", ok ? "OK (/tmp/latency_histogram.csv)" : "FAILED");

    return 0;
}
