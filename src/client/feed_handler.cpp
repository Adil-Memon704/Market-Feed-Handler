#include "../../include/client/feed_handler.h"
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <stdexcept>

// Helper: fire-and-forget write/read on eventfd (errors are non-critical)
static inline void efd_write(int fd) { uint64_t v=1; int r=write(fd,&v,8); (void)r; }
static inline void efd_read(int fd)  { uint64_t v=0; int r=read(fd,&v,8);  (void)r; }

// ─────────────────────────────────────────────────────────────────────────────
// FeedHandler
// ─────────────────────────────────────────────────────────────────────────────

FeedHandler::FeedHandler(const std::string& host,
                         uint16_t           port,
                         uint16_t           num_symbols)
    : host_(host), port_(port), num_symbols_(num_symbols)
{
    cache_   = std::make_unique<SymbolCache>(num_symbols);
    tracker_ = std::make_unique<LatencyTracker>();
    socket_  = std::make_unique<MarketDataSocket>();
    parser_  = std::make_unique<Parser>();
    stats_   = std::make_unique<DisplayStats>();

    parser_->set_latency_tracker(tracker_.get());

    // Parser callback → update SymbolCache and stats
    parser_->set_callback([this](const ParsedMessage& pm) {
        if (pm.msg_type == MSG_QUOTE) {
            cache_->update_quote(pm.symbol_id,
                                 pm.quote.bid_price, pm.quote.bid_qty,
                                 pm.quote.ask_price, pm.quote.ask_qty,
                                 pm.timestamp_ns);
        } else if (pm.msg_type == MSG_TRADE) {
            cache_->update_trade(pm.symbol_id,
                                 pm.trade.price, pm.trade.quantity,
                                 pm.timestamp_ns);
        }
        stats_->total_messages.fetch_add(1, std::memory_order_relaxed);
        last_recv_ns_.store(mono_ns(), std::memory_order_relaxed);
    });

    // epoll instance for network thread
    epfd_ = epoll_create1(EPOLL_CLOEXEC);
    if (epfd_ < 0) throw std::runtime_error("epoll_create1 failed");

    // eventfd for reconnect signalling
    event_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (event_fd_ < 0) throw std::runtime_error("eventfd failed");

    epoll_event ev{};
    ev.events  = EPOLLIN;
    ev.data.fd = event_fd_;
    epoll_ctl(epfd_, EPOLL_CTL_ADD, event_fd_, &ev);

    visualizer_ = std::make_unique<Visualizer>(*cache_, *tracker_, *stats_);
}

FeedHandler::~FeedHandler() {
    stop();
    if (epfd_ >= 0)    close(epfd_);
    if (event_fd_ >= 0) close(event_fd_);
}

// ─────────────────────────────────────────────────────────────────────────────
// Start / stop / run
// ─────────────────────────────────────────────────────────────────────────────

void FeedHandler::start() {
    running_.store(true);
    visualizer_->start();
    reconnect_thread_ = std::thread(&FeedHandler::reconnect_loop, this);
    network_thread_   = std::thread(&FeedHandler::network_loop,   this);
    parse_thread_     = std::thread(&FeedHandler::parse_loop,     this);
}

void FeedHandler::stop() {
    running_.store(false);
    // Wake epoll_wait via eventfd
    if (event_fd_ >= 0) {

        efd_write(event_fd_);
    }
    if (visualizer_) visualizer_->stop();
    if (network_thread_.joinable())   network_thread_.join();
    if (parse_thread_.joinable())     parse_thread_.join();
    if (reconnect_thread_.joinable()) reconnect_thread_.join();
}

void FeedHandler::run() {
    bool is_tty = isatty(STDIN_FILENO);

    struct termios orig_termios {};
    if (is_tty) {
        tcgetattr(STDIN_FILENO, &orig_termios);
        struct termios raw = orig_termios;
        raw.c_lflag &= ~(static_cast<tcflag_t>(ECHO) | static_cast<tcflag_t>(ICANON));
        raw.c_cc[VMIN]  = 0;
        raw.c_cc[VTIME] = 1;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    }

    while (running_.load(std::memory_order_relaxed)) {
        if (is_tty) {
            char c = 0;
            if (read(STDIN_FILENO, &c, 1) == 1) {
                if (c == 'q' || c == 'Q') {
                    std::fprintf(stderr, "\n[client] User requested quit\n");
                    break;
                } else if (c == 'r' || c == 'R') {
                    tracker_->reset();
                    parser_->reset_stats();
                    std::fprintf(stderr, "[client] Stats reset\n");
                }
            }
        }
        struct timespec ts{ 0, 100'000'000L };  // 100ms
        clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, nullptr);
    }
    if (is_tty) tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
    stop();
}

// ─────────────────────────────────────────────────────────────────────────────
// Connect / disconnect
// ─────────────────────────────────────────────────────────────────────────────

bool FeedHandler::do_connect() {
    socket_ = std::make_unique<MarketDataSocket>();
    socket_->set_socket_priority(6);

    bool ok = socket_->connect(host_, port_, 5000);
    if (!ok) {
        std::fprintf(stderr, "[client] connect() to %s:%u failed: %s\n",
                     host_.c_str(), port_,
                     strerror(socket_->last_error()));
        return false;
    }

    // Register socket fd with epoll (edge-triggered)
    epoll_event ev{};
    ev.events  = EPOLLIN | EPOLLET | EPOLLERR | EPOLLHUP | EPOLLRDHUP;
    ev.data.fd = socket_->fd();
    epoll_ctl(epfd_, EPOLL_CTL_ADD, socket_->fd(), &ev);

    // Send subscription
    if (!symbol_ids_.empty()) {
        socket_->send_subscription(symbol_ids_);
        std::fprintf(stderr, "[client] Subscribed to %zu symbols\n",
                     symbol_ids_.size());
    } else {
        // Subscribe to all: send empty subscription
        socket_->send_subscription({});
    }

    connected_.store(true);
    reconnect_policy_.reset();
    last_recv_ns_.store(mono_ns(), std::memory_order_relaxed);

    // Update display stats
    stats_->connected.store(true, std::memory_order_relaxed);
    char addr[64];
    std::snprintf(addr, sizeof(addr), "%s:%u", host_.c_str(), port_);
    std::strncpy(stats_->server_addr, addr, sizeof(stats_->server_addr) - 1);

    std::fprintf(stderr, "[client] Connected to %s\n", addr);
    return true;
}

void FeedHandler::on_disconnect(DisconnectReason reason) {
    if (!connected_.exchange(false)) return;  // already disconnected

    stats_->connected.store(false, std::memory_order_relaxed);
    stats_->reconnect_count.fetch_add(1, std::memory_order_relaxed);

    std::fprintf(stderr, "[client] Disconnected: %s\n",
                 disconnect_reason_str(reason));

    if (socket_->fd() >= 0) {
        epoll_ctl(epfd_, EPOLL_CTL_DEL, socket_->fd(), nullptr);
        socket_->disconnect();
    }

    if (running_.load()) {
        reconnect_pending_.store(true, std::memory_order_release);
        // Wake reconnect_thread via eventfd

        efd_write(event_fd_);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Network loop — edge-triggered epoll, drains recv buffer
// ─────────────────────────────────────────────────────────────────────────────

void FeedHandler::network_loop() {
    // Initial connect
    if (!do_connect()) {
        reconnect_pending_.store(true);
    }

    constexpr int   MAX_EVENTS  = 16;
    constexpr size_t RECV_CHUNK = 65536;
    uint8_t recv_buf[RECV_CHUNK];
    epoll_event events[MAX_EVENTS];

    while (running_.load(std::memory_order_relaxed)) {
        int n = epoll_wait(epfd_, events, MAX_EVENTS, 1000);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }

        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;

            // ── eventfd: reconnect signal or shutdown ────────────────────
            if (fd == event_fd_) {
                efd_read(event_fd_);

                if (reconnect_pending_.exchange(false)) {
                    // Attempt reconnect from network thread
                    // (reconnect_thread signals us, we do the actual connect)
                    if (!do_connect()) {
                        // Will be retried by reconnect_thread
                        reconnect_pending_.store(true);
                    }
                }
                continue;
            }

            // ── Socket events ────────────────────────────────────────────
            if (events[i].events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
                on_disconnect(DisconnectReason::RECV_ERROR);
                continue;
            }

            if (events[i].events & EPOLLIN) {
                // Edge-triggered: MUST drain to EAGAIN
                while (true) {
                    uint64_t t0 = mono_ns();
                    ssize_t bytes = socket_->receive(recv_buf, RECV_CHUNK);

                    if (bytes > 0) {
                        parser_->feed(recv_buf,
                                      static_cast<size_t>(bytes),
                                      t0);
                        // Heartbeat timeout reset
                        last_recv_ns_.store(mono_ns(), std::memory_order_relaxed);
                    } else if (bytes == 0) {
                        on_disconnect(DisconnectReason::PEER_CLOSED);
                        break;
                    } else {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        if (errno == EINTR) continue;
                        on_disconnect(DisconnectReason::RECV_ERROR);
                        break;
                    }
                }
            }
        }

        // Heartbeat timeout check
        if (connected_.load()) {
            uint64_t now = mono_ns();
            uint64_t last = last_recv_ns_.load(std::memory_order_relaxed);
            if (last > 0 && now - last > HEARTBEAT_TIMEOUT_NS) {
                std::fprintf(stderr, "[client] Heartbeat timeout (%.1fs)\n",
                             (now - last) / 1e9);
                on_disconnect(DisconnectReason::HEARTBEAT_TIMEOUT);
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Parse loop — drains parser ring buffer, updates symbol cache
// ─────────────────────────────────────────────────────────────────────────────

void FeedHandler::parse_loop() {
    while (running_.load(std::memory_order_relaxed)) {
        int processed = parser_->process();

        // Update gap/checksum stats
        stats_->sequence_gaps.store(parser_->sequence_gaps(),
                                    std::memory_order_relaxed);
        stats_->checksum_errors.store(parser_->checksum_errors(),
                                      std::memory_order_relaxed);

        if (processed == 0) {
            // Nothing to parse — yield briefly
            struct timespec ts{ 0, 50'000L };  // 50 µs
            clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, nullptr);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Reconnect loop — sleeps for backoff, signals network_thread via eventfd
// ─────────────────────────────────────────────────────────────────────────────

void FeedHandler::reconnect_loop() {
    while (running_.load(std::memory_order_relaxed)) {
        if (!reconnect_pending_.load(std::memory_order_acquire)) {
            struct timespec ts{ 0, 100'000'000L };  // 100 ms polling
            clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, nullptr);
            continue;
        }

        if (reconnect_policy_.give_up()) {
            std::fprintf(stderr,
                "[client] FATAL: Max reconnect attempts (%d) reached — giving up\n",
                ReconnectPolicy::MAX_ATTEMPT);
            running_.store(false);
            return;
        }

        int delay_ms = reconnect_policy_.next_delay_ms();
        std::fprintf(stderr,
            "[client] Reconnecting in %dms (attempt %d)...\n",
            delay_ms, reconnect_policy_.attempt());

        struct timespec ts;
        ts.tv_sec  = delay_ms / 1000;
        ts.tv_nsec = (delay_ms % 1000) * 1'000'000L;
        clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, nullptr);

        if (!running_.load()) return;

        // Signal network_thread to attempt connect

        efd_write(event_fd_);
    }
}
