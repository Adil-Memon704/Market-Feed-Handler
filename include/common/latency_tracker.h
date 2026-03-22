#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <array>

// ─────────────────────────────────────────────────────────────────────────────
// LatencyTracker — lock-free histogram-based latency measurement.
//
// Buckets (720 total, ~5.7 KB — fits in L1 cache):
//   Bucket  0-63:   1 ns each       (0–63 ns)
//   Bucket 64-183:  8 ns each       (64 ns–1,024 ns)
//   Bucket 184-303: 64 ns each      (1,024 ns–8,768 ns)
//   Bucket 304-423: 512 ns each     (8,768 ns–70,400 ns)
//   Bucket 424-543: 4,096 ns each   (70,400 ns–562,688 ns)
//   Bucket 544-607: 32,768 ns each  (562 µs–2,621 µs ... overflow bucket)
// ─────────────────────────────────────────────────────────────────────────────

struct LatencyStats {
    uint64_t min_ns    = 0;
    uint64_t max_ns    = 0;
    uint64_t mean_ns   = 0;
    uint64_t p50_ns    = 0;
    uint64_t p95_ns    = 0;
    uint64_t p99_ns    = 0;
    uint64_t p999_ns   = 0;
    uint64_t sample_count = 0;
};

class LatencyTracker {
public:
    static constexpr size_t NUM_BUCKETS = 608;

    LatencyTracker();

    // Thread-safe recording (atomic increment)
    void record(uint64_t latency_ns);

    // Compute stats (reads all buckets — call from stats/vis thread only)
    LatencyStats get_stats() const;

    // Reset all counters
    void reset();

    // Export histogram to CSV
    bool export_csv(const std::string& path) const;

private:
    // Map nanoseconds to bucket index
    static int bucket_for(uint64_t ns);
    // Map bucket index back to representative nanosecond value
    static uint64_t bucket_to_ns(int idx);

    alignas(64) std::atomic<uint64_t> buckets_[NUM_BUCKETS];
    alignas(64) std::atomic<uint64_t> total_ns_;     // sum for mean
    alignas(64) std::atomic<uint64_t> sample_count_;
    alignas(64) std::atomic<uint64_t> min_ns_;
    alignas(64) std::atomic<uint64_t> max_ns_;
};
