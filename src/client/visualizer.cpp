#include "../../include/client/visualizer.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <vector>
#include <sstream>
#include <iomanip>
#include <sys/ioctl.h>
#include <unistd.h>
#include <cinttypes>
#include <time.h>

// ─────────────────────────────────────────────────────────────────────────────
// ANSI helpers
// ─────────────────────────────────────────────────────────────────────────────

#define ANSI_RESET      "\033[0m"
#define ANSI_BOLD       "\033[1m"
#define ANSI_RED        "\033[31m"
#define ANSI_GREEN      "\033[32m"
#define ANSI_YELLOW     "\033[33m"
#define ANSI_CYAN       "\033[36m"
#define ANSI_WHITE      "\033[37m"
#define ANSI_CLEAR      "\033[2J\033[H"
#define ANSI_CLEAR_LINE "\033[K"

// ─────────────────────────────────────────────────────────────────────────────
// Visualizer
// ─────────────────────────────────────────────────────────────────────────────

Visualizer::Visualizer(const SymbolCache&    cache,
                       const LatencyTracker& tracker,
                       const DisplayStats&   stats)
    : cache_(cache), tracker_(tracker), stats_(stats)
{
    std::memset(open_price_, 0, sizeof(open_price_));
    std::memset(open_set_,   0, sizeof(open_set_));
}

Visualizer::~Visualizer() {
    stop();
}

void Visualizer::start() {
    if (running_.exchange(true)) return;
    start_time_ns_ = mono_ns();
    last_ts_ns_    = start_time_ns_;
    thread_ = std::thread(&Visualizer::render_loop, this);
}

void Visualizer::stop() {
    running_.store(false);
    if (thread_.joinable()) thread_.join();
    // Restore cursor
    std::fputs("\033[?25h\n", stdout);
    std::fflush(stdout);
}

// ─────────────────────────────────────────────────────────────────────────────
// Render loop
// ─────────────────────────────────────────────────────────────────────────────

void Visualizer::render_loop() {
    // Hide cursor
    std::fputs("\033[?25l", stdout);
    std::fflush(stdout);

    while (running_.load(std::memory_order_relaxed)) {
        uint64_t now_t     = mono_ns();
        uint64_t uptime_s  = (now_t - start_time_ns_) / 1'000'000'000ULL;

        // Calculate message rate
        uint64_t total_msgs = stats_.total_messages.load(std::memory_order_relaxed);
        double   dt_s = static_cast<double>(now_t - last_ts_ns_) / 1e9;
        double   rate = (dt_s > 0) ? (total_msgs - last_msg_count_) / dt_s : 0.0;
        last_msg_count_ = total_msgs;
        last_ts_ns_     = now_t;

        std::string frame = build_frame(uptime_s, rate);
        std::fwrite(frame.data(), 1, frame.size(), stdout);
        std::fflush(stdout);

        // Sleep 500 ms
        struct timespec ts{ 0, UPDATE_INTERVAL_MS * 1'000'000L };
        clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, nullptr);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Frame builder
// ─────────────────────────────────────────────────────────────────────────────

std::string Visualizer::format_price(double p) const {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%10.2f", p);
    return buf;
}

std::string Visualizer::format_volume(uint64_t v) const {
    char buf[24];
    if (v >= 10'000'000) std::snprintf(buf, sizeof(buf), "%7.1fM", v / 1e6);
    else if (v >= 10'000) std::snprintf(buf, sizeof(buf), "%7.1fK", v / 1e3);
    else std::snprintf(buf, sizeof(buf), "%8llu", (unsigned long long)v);
    return buf;
}

std::string Visualizer::color_pct(double pct) const {
    char buf[48];
    const char* color = (pct >= 0) ? ANSI_GREEN : ANSI_RED;
    std::snprintf(buf, sizeof(buf), "%s%+6.2f%%%s", color, pct, ANSI_RESET);
    return buf;
}

std::string Visualizer::build_frame(uint64_t uptime_s, double msg_rate) const {
    std::ostringstream out;

    // Move cursor to top-left (no full clear to avoid flicker)
    out << "\033[H";

    // ── Header ──────────────────────────────────────────────────────────────
    char uptime_str[32];
    std::snprintf(uptime_str, sizeof(uptime_str), "%02llu:%02llu:%02llu",
                  (unsigned long long)(uptime_s / 3600),
                  (unsigned long long)((uptime_s % 3600) / 60),
                  (unsigned long long)(uptime_s % 60));

    out << ANSI_BOLD << ANSI_CYAN
        << "═══════════════════ NSE Market Data Feed Handler ═══════════════════"
        << ANSI_RESET << ANSI_CLEAR_LINE << "\n";

    bool conn = stats_.connected.load(std::memory_order_relaxed);
    out << " Status: " << (conn ? ANSI_GREEN "● CONNECTED" : ANSI_RED "● DISCONNECTED")
        << ANSI_RESET << "  to: " << stats_.server_addr
        << ANSI_CLEAR_LINE << "\n";

    char rate_str[64];
    std::snprintf(rate_str, sizeof(rate_str), "%.0f", msg_rate);
    out << " Uptime: " << ANSI_YELLOW << uptime_str << ANSI_RESET
        << "  │  Messages: " << ANSI_YELLOW
        << stats_.total_messages.load(std::memory_order_relaxed)
        << ANSI_RESET
        << "  │  Rate: " << ANSI_YELLOW << rate_str << " msg/s"
        << ANSI_RESET
        << "  │  Gaps: " << ANSI_RED
        << stats_.sequence_gaps.load(std::memory_order_relaxed)
        << ANSI_RESET
        << ANSI_CLEAR_LINE << "\n";

    out << ANSI_BOLD
        << "──────────────────────────────────────────────────────────────────────"
        << ANSI_RESET << ANSI_CLEAR_LINE << "\n";

    // ── Column headers ───────────────────────────────────────────────────────
    out << ANSI_BOLD
        << std::left  << std::setw(12) << " Symbol"
        << std::right << std::setw(11) << "Bid"
        << std::right << std::setw(11) << "Ask"
        << std::right << std::setw(11) << "LTP"
        << std::right << std::setw(10) << "Volume"
        << std::right << std::setw(9)  << "Chg%"
        << std::right << std::setw(10) << "Updates"
        << ANSI_RESET << ANSI_CLEAR_LINE << "\n";

    out << "──────────────────────────────────────────────────────────────────────"
        << ANSI_CLEAR_LINE << "\n";

    // ── Collect top-N symbols by update_count ────────────────────────────────
    struct Row {
        uint16_t    id;
        MarketState state;
    };
    std::vector<Row> rows;
    rows.reserve(cache_.num_symbols());

    for (uint16_t i = 0; i < cache_.num_symbols(); ++i) {
        uint64_t cnt = cache_.get_update_count(i);
        if (cnt == 0) continue;
        rows.push_back({i, cache_.get_snapshot(i)});
    }

    // Sort by update_count descending
    std::sort(rows.begin(), rows.end(),
              [](const Row& a, const Row& b) {
                  return a.state.update_count > b.state.update_count;
              });

    if (static_cast<int>(rows.size()) > TOP_N_SYMBOLS)
        rows.resize(TOP_N_SYMBOLS);

    // ── Symbol rows ──────────────────────────────────────────────────────────
    for (const auto& row : rows) {
        const MarketState& s = row.state;

        // Calculate % change (visualizer-local open price tracking)
        double open = open_price_[row.id];
        if (open == 0.0 && s.last_traded_price > 0.0) {
            // This is called on read thread — open_price_ is visualizer-local,
            // no synchronisation needed
            const_cast<Visualizer*>(this)->open_price_[row.id] = s.last_traded_price;
            const_cast<Visualizer*>(this)->open_set_[row.id]   = true;
            open = s.last_traded_price;
        }
        if (open == 0.0 && s.best_bid > 0.0) {
            const_cast<Visualizer*>(this)->open_price_[row.id] =
                (s.best_bid + s.best_ask) * 0.5;
            open = open_price_[row.id];
        }

        double ref = open > 0 ? open : s.last_traded_price;
        double ltp = s.last_traded_price > 0 ? s.last_traded_price
                                              : (s.best_bid + s.best_ask) * 0.5;
        double pct = (ref > 0) ? (ltp - ref) / ref * 100.0 : 0.0;

        // Symbol name from tick generator is stored in the SymbolProcess;
        // here we just use SYM+id as the cache doesn't store name
        char sym_name[16];
        // Named symbols for first 20
        const char* nse_names[] = {
            "RELIANCE", "TCS", "INFY", "HDFC", "ICICIBANK",
            "SBIN", "BAJFINANCE", "BHARTIARTL", "WIPRO", "HCLTECH",
            "KOTAKBANK", "ASIANPAINT", "AXISBANK", "MARUTI", "TITAN",
            "SUNPHARMA", "ULTRACEMCO", "NESTLEIND", "POWERGRID", "NTPC"
        };
        if (row.id < 20) {
            std::snprintf(sym_name, sizeof(sym_name), "%s", nse_names[row.id]);
        } else {
            std::snprintf(sym_name, sizeof(sym_name), "SYM%03u", row.id);
        }

        out << " "
            << ANSI_BOLD << std::left << std::setw(11) << sym_name << ANSI_RESET
            << std::right << std::setw(11) << format_price(s.best_bid)
            << std::right << std::setw(11) << format_price(s.best_ask)
            << std::right << std::setw(11) << format_price(ltp)
            << std::right << std::setw(10) << format_volume(s.last_traded_quantity)
            << "  " << std::setw(8)  << color_pct(pct)
            << std::right << std::setw(8)  << s.update_count
            << ANSI_CLEAR_LINE << "\n";
    }

    // Pad remaining rows to keep display stable
    for (int i = static_cast<int>(rows.size()); i < TOP_N_SYMBOLS; ++i) {
        out << ANSI_CLEAR_LINE << "\n";
    }

    // ── Statistics ───────────────────────────────────────────────────────────
    out << "──────────────────────────────────────────────────────────────────────"
        << ANSI_CLEAR_LINE << "\n";

    LatencyStats ls = tracker_.get_stats();
    char lat_buf[128];
    if (ls.sample_count > 0) {
        std::snprintf(lat_buf, sizeof(lat_buf),
                      "p50=%lluµs  p99=%lluµs  p999=%lluµs  samples=%llu",
                      (unsigned long long)(ls.p50_ns  / 1000),
                      (unsigned long long)(ls.p99_ns  / 1000),
                      (unsigned long long)(ls.p999_ns / 1000),
                      (unsigned long long)ls.sample_count);
    } else {
        std::snprintf(lat_buf, sizeof(lat_buf), "no samples yet");
    }

    out << " Latency: " << ANSI_YELLOW << lat_buf << ANSI_RESET
        << ANSI_CLEAR_LINE << "\n";
    out << " Checksum errors: " << stats_.checksum_errors.load()
        << "  │  Reconnects: " << stats_.reconnect_count.load()
        << ANSI_CLEAR_LINE << "\n";
    out << ANSI_BOLD << ANSI_CYAN
        << "──────────────────────────────────────────────────────────────────────"
        << ANSI_RESET << ANSI_CLEAR_LINE << "\n";
    out << " Press " << ANSI_BOLD << "'q'" << ANSI_RESET << " to quit  │  "
        << ANSI_BOLD << "'r'" << ANSI_RESET << " to reset stats"
        << ANSI_CLEAR_LINE << "\n";

    return out.str();
}
