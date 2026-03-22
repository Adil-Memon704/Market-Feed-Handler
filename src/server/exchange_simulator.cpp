#include "../../include/server/exchange_simulator.h"
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <stdexcept>
#include <signal.h>

// ─────────────────────────────────────────────────────────────────────────────
// ExchangeSimulator
// ─────────────────────────────────────────────────────────────────────────────

ExchangeSimulator::ExchangeSimulator(uint16_t port, size_t num_symbols)
    : port_(port),
      tick_gen_(std::make_unique<TickGenerator>(num_symbols)),
      tick_rate_(100'000)
{
    // epoll instance
    epfd_ = epoll_create1(EPOLL_CLOEXEC);
    if (epfd_ < 0) throw std::runtime_error("epoll_create1 failed");

    client_mgr_ = std::make_unique<ClientManager>(epfd_);
}

ExchangeSimulator::~ExchangeSimulator() {
    stop();
    if (epfd_ >= 0)     close(epfd_);
    if (listen_fd_ >= 0) close(listen_fd_);
}

// ─────────────────────────────────────────────────────────────────────────────
// Socket setup
// ─────────────────────────────────────────────────────────────────────────────

int ExchangeSimulator::setup_listen_socket() {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) throw std::runtime_error("socket() failed");

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port_);

    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd);
        throw std::runtime_error(std::string("bind() failed: ") + strerror(errno));
    }
    if (listen(fd, SOMAXCONN) < 0) {
        close(fd);
        throw std::runtime_error("listen() failed");
    }

    // Register with epoll
    epoll_event ev{};
    ev.events  = EPOLLIN;
    ev.data.fd = fd;
    epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev);

    return fd;
}

// ─────────────────────────────────────────────────────────────────────────────
// Start / stop
// ─────────────────────────────────────────────────────────────────────────────

void ExchangeSimulator::start() {
    listen_fd_ = setup_listen_socket();

    running_.store(true);

    // Register tick callback — called by tick_thread, writes to all clients
    tick_gen_->set_tick_rate(tick_rate_.load());
    tick_gen_->set_callback([this](const void* data, size_t len) {
        client_mgr_->broadcast(data, len);
        msgs_broadcast_.fetch_add(1, std::memory_order_relaxed);
    });

    accept_thread_ = std::thread(&ExchangeSimulator::accept_loop, this);
    tick_thread_   = std::thread(&ExchangeSimulator::tick_loop,   this);

    std::fprintf(stderr, "[server] Listening on port %u with %zu symbols\n",
                 port_, tick_gen_->num_symbols());
}

void ExchangeSimulator::stop() {
    running_.store(false);
    // Closing the listen fd makes epoll_wait return with EPOLLERR on it,
    // breaking the accept loop within one iteration (max 1s timeout anyway).
    if (listen_fd_ >= 0) {
        shutdown(listen_fd_, SHUT_RDWR);
    }
}

void ExchangeSimulator::run() {
    if (accept_thread_.joinable()) accept_thread_.join();
    if (tick_thread_.joinable())   tick_thread_.join();
}

void ExchangeSimulator::set_tick_rate(uint32_t tps) {
    tick_rate_.store(tps);
    tick_gen_->set_tick_rate(tps);
}

void ExchangeSimulator::enable_fault_injection(bool enable) {
    fault_injection_.store(enable);
}

// ─────────────────────────────────────────────────────────────────────────────
// Accept loop — handles connections and subscription messages
// ─────────────────────────────────────────────────────────────────────────────

void ExchangeSimulator::accept_loop() {
    constexpr int MAX_EVENTS = 64;
    epoll_event   events[MAX_EVENTS];

    while (running_.load(std::memory_order_relaxed)) {
        int n = epoll_wait(epfd_, events, MAX_EVENTS, 1000 /* ms timeout */);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }

        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;

            if (fd == listen_fd_) {
                // Accept new connections
                while (true) {
                    sockaddr_in peer{};
                    socklen_t   peer_len = sizeof(peer);
                    int cfd = accept4(listen_fd_,
                                      reinterpret_cast<sockaddr*>(&peer),
                                      &peer_len,
                                      SOCK_NONBLOCK | SOCK_CLOEXEC);
                    if (cfd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        break;
                    }

                    // Configure client socket
                    int nodelay = 1;
                    setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
                    int sndbuf = 4 * 1024 * 1024;
                    setsockopt(cfd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

                    // Register with epoll
                    epoll_event cev{};
                    cev.events  = EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLRDHUP;
                    cev.data.fd = cfd;
                    epoll_ctl(epfd_, EPOLL_CTL_ADD, cfd, &cev);

                    char peer_str[64];
                    std::snprintf(peer_str, sizeof(peer_str), "%s:%d",
                                  inet_ntoa(peer.sin_addr), ntohs(peer.sin_port));
                    client_mgr_->add_client(cfd, peer_str);
                }
            } else {
                // Existing client activity
                if (events[i].events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
                    client_mgr_->remove_client(fd);
                } else if (events[i].events & EPOLLOUT) {
                    client_mgr_->flush_pending(fd);
                } else if (events[i].events & EPOLLIN) {
                    uint8_t buf[512];
                    ssize_t n_recv = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
                    if (n_recv > 0) {
                        client_mgr_->on_recv(fd, buf, n_recv);
                    } else if (n_recv == 0 || (n_recv < 0 && errno != EAGAIN)) {
                        client_mgr_->remove_client(fd);
                    }
                }
            }
        }

        // Periodic maintenance
        auto now = std::chrono::steady_clock::now();
        client_mgr_->tick(now);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Tick loop — generates ticks at the configured rate
// ─────────────────────────────────────────────────────────────────────────────

void ExchangeSimulator::tick_loop() {
    uint32_t num_symbols = static_cast<uint32_t>(tick_gen_->num_symbols());
    uint32_t symbol_idx  = 0;
    uint32_t seq         = 0;

    while (running_.load(std::memory_order_relaxed)) {
        uint32_t rate = tick_rate_.load(std::memory_order_relaxed);
        if (rate == 0) { rate = 1; }

        // Time per tick in nanoseconds
        uint64_t ns_per_tick = 1'000'000'000ULL / rate;

        uint64_t batch_start = mono_ns();
        uint32_t batch_size  = std::max(1u, rate / 100u);  // 10ms batches

        for (uint32_t b = 0; b < batch_size && running_.load(std::memory_order_relaxed); ++b) {
            // Fault injection: skip sequence numbers occasionally
            if (fault_injection_.load() && (seq % 100 == 99)) {
                seq += 2;  // artificially create a gap
            }

            uint16_t sym = static_cast<uint16_t>(symbol_idx % num_symbols);
            tick_gen_->generate_tick(sym, seq++);
            ticks_generated_.fetch_add(1, std::memory_order_relaxed);
            symbol_idx++;
        }

        // Heartbeat every second
        uint64_t now = mono_ns();
        if (now - last_heartbeat_ns_ >= 1'000'000'000ULL) {
            tick_gen_->generate_heartbeat(seq++);
            last_heartbeat_ns_ = now;
        }

        // Rate limiting: sleep for the remainder of the batch window
        uint64_t batch_end = mono_ns();
        uint64_t elapsed   = batch_end - batch_start;
        uint64_t expected  = ns_per_tick * batch_size;
        if (elapsed < expected) {
            struct timespec ts;
            ts.tv_sec  = 0;
            ts.tv_nsec = static_cast<long>(expected - elapsed);
            clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, nullptr);
        }
    }
}
