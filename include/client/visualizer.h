#pragma once

#include <cstdint>
#include <string>
#include <thread>
#include <atomic>
#include <memory>
#include "../common/cache.h"
#include "../common/latency_tracker.h"

// ─────────────────────────────────────────────────────────────────────────────
// DisplayStats — snapshot of runtime metrics for the header line.
// Written by network/parse threads, read by visualizer.
// ─────────────────────────────────────────────────────────────────────────────

struct DisplayStats {
    std::atomic<uint64_t> total_messages{0};
    std::atomic<uint64_t> sequence_gaps{0};
    std::atomic<uint64_t> checksum_errors{0};
    std::atomic<uint64_t> reconnect_count{0};
    std::atomic<bool>     connected{false};
    char                  server_addr[64] = "disconnected";
};

// ─────────────────────────────────────────────────────────────────────────────
// Visualizer — terminal display that updates every 500 ms.
//
// Runs in its own thread. Reads from SymbolCache (lock-free) and
// DisplayStats (atomic). Never touches the network or parse threads directly.
// Uses raw ANSI escape codes — no ncurses dependency.
// ─────────────────────────────────────────────────────────────────────────────

class Visualizer {
public:
    static constexpr int  UPDATE_INTERVAL_MS = 500;
    static constexpr int  TOP_N_SYMBOLS      = 20;

    Visualizer(const SymbolCache& cache,
               const LatencyTracker& tracker,
               const DisplayStats& stats);
    ~Visualizer();

    void start();
    void stop();

    bool is_running() const { return running_.load(); }

private:
    void render_loop();
    std::string build_frame(uint64_t uptime_s, double msg_rate) const;
    std::string format_price(double p) const;
    std::string format_volume(uint64_t v) const;
    std::string color_pct(double pct) const;

    const SymbolCache&    cache_;
    const LatencyTracker& tracker_;
    const DisplayStats&   stats_;

    std::thread           thread_;
    std::atomic<bool>     running_{false};

    // Per-session open prices (visualizer-local, no sync needed)
    double   open_price_[MAX_SYMBOLS];
    bool     open_set_[MAX_SYMBOLS];

    uint64_t start_time_ns_  = 0;
    uint64_t last_msg_count_ = 0;
    uint64_t last_ts_ns_     = 0;
};
