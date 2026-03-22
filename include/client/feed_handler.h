#pragma once

#include <string>
#include <thread>
#include <atomic>
#include <memory>
#include <vector>
#include "../common/cache.h"
#include "../common/latency_tracker.h"
#include "socket.h"
#include "parser.h"
#include "visualizer.h"

// ─────────────────────────────────────────────────────────────────────────────
// FeedHandler — orchestrates the client-side feed processing.
//
// Threads:
//   network_thread   : epoll loop — recv bytes, feed to parser ring buffer
//   parse_thread     : drains parser, updates SymbolCache
//   visualize_thread : (managed by Visualizer)
//   reconnect_thread : sleeps for backoff, triggers reconnect
// ─────────────────────────────────────────────────────────────────────────────

class FeedHandler {
public:
    FeedHandler(const std::string& host,
                uint16_t           port,
                uint16_t           num_symbols = DEFAULT_SYMBOLS);
    ~FeedHandler();

    void start();
    void stop();
    void run();   // Blocks until stopped (handles Ctrl-C / 'q')

    void set_symbols(const std::vector<uint16_t>& symbol_ids) {
        symbol_ids_ = symbol_ids;
    }

    // Access for stats
    const SymbolCache&    cache()   const { return *cache_; }
    const LatencyTracker& tracker() const { return *tracker_; }

private:
    void network_loop();
    void parse_loop();
    void reconnect_loop();

    bool do_connect();
    void on_disconnect(DisconnectReason reason);

    std::string host_;
    uint16_t    port_;
    uint16_t    num_symbols_;
    std::vector<uint16_t> symbol_ids_;

    std::unique_ptr<SymbolCache>    cache_;
    std::unique_ptr<LatencyTracker> tracker_;
    std::unique_ptr<MarketDataSocket> socket_;
    std::unique_ptr<Parser>         parser_;
    std::unique_ptr<Visualizer>     visualizer_;
    std::unique_ptr<DisplayStats>   stats_;

    int  epfd_          = -1;
    int  event_fd_      = -1;  // eventfd for reconnect signalling

    std::thread network_thread_;
    std::thread parse_thread_;
    std::thread reconnect_thread_;

    std::atomic<bool>            running_{false};
    std::atomic<bool>            connected_{false};
    std::atomic<bool>            reconnect_pending_{false};
    std::atomic<DisconnectReason> disconnect_reason_{DisconnectReason::USER_REQUESTED};

    ReconnectPolicy reconnect_policy_;

    // Heartbeat detection
    std::atomic<uint64_t> last_recv_ns_{0};
    static constexpr uint64_t HEARTBEAT_TIMEOUT_NS = 30ULL * 1'000'000'000ULL;
};
