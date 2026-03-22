#pragma once

#include <cstdint>
#include <thread>
#include <atomic>
#include <memory>
#include "../protocol.h"
#include "tick_generator.h"
#include "client_manager.h"

// ─────────────────────────────────────────────────────────────────────────────
// ExchangeSimulator — the main server class.
//
// Threads:
//   accept_thread   : epoll loop, accepts connections, reads subscriptions
//   tick_thread     : runs GBM for all symbols at configured tick_rate
//   broadcast_thread: drains SPSC queue, writes to client sockets
// ─────────────────────────────────────────────────────────────────────────────

class ExchangeSimulator {
public:
    explicit ExchangeSimulator(uint16_t port        = DEFAULT_PORT,
                               size_t   num_symbols = DEFAULT_SYMBOLS);
    ~ExchangeSimulator();

    // Non-copyable
    ExchangeSimulator(const ExchangeSimulator&) = delete;
    ExchangeSimulator& operator=(const ExchangeSimulator&) = delete;

    void start();  // Launch all threads
    void stop();   // Signal all threads to stop

    void run();    // Blocks until stopped (joins threads)

    void set_tick_rate(uint32_t ticks_per_second);
    void enable_fault_injection(bool enable);

    // Stats
    uint64_t ticks_generated()  const { return ticks_generated_.load(); }
    uint64_t msgs_broadcast()   const { return msgs_broadcast_.load(); }

private:
    void accept_loop();    // accept_thread function
    void tick_loop();      // tick_thread function

    int  setup_listen_socket();

    uint16_t port_;
    int      listen_fd_ = -1;
    int      epfd_      = -1;  // epoll fd (used by accept_thread)

    std::unique_ptr<TickGenerator>  tick_gen_;
    std::unique_ptr<ClientManager>  client_mgr_;

    std::thread accept_thread_;
    std::thread tick_thread_;

    std::atomic<bool>     running_{false};
    std::atomic<uint32_t> tick_rate_{100'000};
    std::atomic<bool>     fault_injection_{false};

    std::atomic<uint64_t> ticks_generated_{0};
    std::atomic<uint64_t> msgs_broadcast_{0};
    std::atomic<uint32_t> global_seq_{0};

    // Heartbeat timing
    uint64_t last_heartbeat_ns_ = 0;
};
