#include "../../include/common/latency_tracker.h"
#include <fstream>
#include <cstring>
#include <limits>
#include <algorithm>

// ─────────────────────────────────────────────────────────────────────────────
// Bucket layout:
//   [0,  64):  1 ns per bucket   → indices 0..63
//   [64, 184): 8 ns per bucket   → indices 64..183  (offset 64 ns)
//   [184,304): 64 ns per bucket  → indices 184..303 (offset 64+120*8=1024 ns)
//   [304,424): 512 ns per bucket → indices 304..423 (offset 1024+120*64=8704 ns)
//   [424,544): 4096 ns           → indices 424..543 (offset 8704+120*512=70144 ns)
//   [544,608): 32768 ns          → indices 544..607 (offset 70144+120*4096=560384 ns)
// ─────────────────────────────────────────────────────────────────────────────

LatencyTracker::LatencyTracker()
    : total_ns_(0), sample_count_(0),
      min_ns_(std::numeric_limits<uint64_t>::max()), max_ns_(0)
{
    for (auto& b : buckets_) b.store(0, std::memory_order_relaxed);
}

static const uint64_t TIER_START_NS[]  = { 0, 64, 1024, 8704, 70144, 560384 };
static const uint64_t TIER_STEP_NS[]   = { 1, 8, 64, 512, 4096, 32768 };
static const int      TIER_FIRST_IDX[] = { 0, 64, 184, 304, 424, 544 };
static const int      NUM_TIERS        = 6;

int LatencyTracker::bucket_for(uint64_t ns) {
    for (int t = NUM_TIERS - 1; t >= 0; --t) {
        if (ns >= TIER_START_NS[t]) {
            int offset = static_cast<int>((ns - TIER_START_NS[t]) / TIER_STEP_NS[t]);
            int idx    = TIER_FIRST_IDX[t] + offset;
            if (idx >= static_cast<int>(NUM_BUCKETS)) idx = NUM_BUCKETS - 1;
            return idx;
        }
    }
    return 0;
}

uint64_t LatencyTracker::bucket_to_ns(int idx) {
    for (int t = NUM_TIERS - 1; t >= 0; --t) {
        if (idx >= TIER_FIRST_IDX[t]) {
            return TIER_START_NS[t] +
                   static_cast<uint64_t>(idx - TIER_FIRST_IDX[t]) * TIER_STEP_NS[t];
        }
    }
    return 0;
}

void LatencyTracker::record(uint64_t ns) {
    int idx = bucket_for(ns);
    buckets_[idx].fetch_add(1, std::memory_order_relaxed);
    total_ns_.fetch_add(ns, std::memory_order_relaxed);
    sample_count_.fetch_add(1, std::memory_order_relaxed);

    // Update min/max with relaxed CAS loop
    uint64_t cur_min = min_ns_.load(std::memory_order_relaxed);
    while (ns < cur_min &&
           !min_ns_.compare_exchange_weak(cur_min, ns,
               std::memory_order_relaxed, std::memory_order_relaxed)) {}

    uint64_t cur_max = max_ns_.load(std::memory_order_relaxed);
    while (ns > cur_max &&
           !max_ns_.compare_exchange_weak(cur_max, ns,
               std::memory_order_relaxed, std::memory_order_relaxed)) {}
}

LatencyStats LatencyTracker::get_stats() const {
    LatencyStats s;
    s.sample_count = sample_count_.load(std::memory_order_acquire);
    if (s.sample_count == 0) return s;

    s.min_ns  = min_ns_.load(std::memory_order_relaxed);
    s.max_ns  = max_ns_.load(std::memory_order_relaxed);
    uint64_t total = total_ns_.load(std::memory_order_relaxed);
    s.mean_ns = total / s.sample_count;

    // Compute percentiles by scanning buckets
    uint64_t counts[NUM_BUCKETS];
    for (int i = 0; i < static_cast<int>(NUM_BUCKETS); ++i) {
        counts[i] = buckets_[i].load(std::memory_order_relaxed);
    }

    const double pct_targets[] = { 0.50, 0.95, 0.99, 0.999 };
    uint64_t* pct_out[] = { &s.p50_ns, &s.p95_ns, &s.p99_ns, &s.p999_ns };

    uint64_t acc = 0;
    int      pi  = 0;
    for (int i = 0; i < static_cast<int>(NUM_BUCKETS) && pi < 4; ++i) {
        acc += counts[i];
        while (pi < 4 &&
               static_cast<double>(acc) >=
               pct_targets[pi] * static_cast<double>(s.sample_count))
        {
            *pct_out[pi] = bucket_to_ns(i);
            ++pi;
        }
    }
    // Fill any remaining percentiles with max
    for (; pi < 4; ++pi) *pct_out[pi] = s.max_ns;

    return s;
}

void LatencyTracker::reset() {
    for (auto& b : buckets_) b.store(0, std::memory_order_relaxed);
    total_ns_.store(0, std::memory_order_relaxed);
    sample_count_.store(0, std::memory_order_relaxed);
    min_ns_.store(std::numeric_limits<uint64_t>::max(), std::memory_order_relaxed);
    max_ns_.store(0, std::memory_order_relaxed);
}

bool LatencyTracker::export_csv(const std::string& path) const {
    std::ofstream f(path);
    if (!f) return false;

    f << "bucket_idx,lower_ns,upper_ns,count\n";
    for (int i = 0; i < static_cast<int>(NUM_BUCKETS); ++i) {
        uint64_t cnt = buckets_[i].load(std::memory_order_relaxed);
        if (cnt == 0) continue;
        uint64_t lower = bucket_to_ns(i);
        uint64_t upper = (i + 1 < static_cast<int>(NUM_BUCKETS))
                         ? bucket_to_ns(i + 1) : lower * 2;
        f << i << ',' << lower << ',' << upper << ',' << cnt << '\n';
    }
    return true;
}
