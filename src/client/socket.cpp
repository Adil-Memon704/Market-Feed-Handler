#include "../../include/client/socket.h"
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <csignal>

// ─────────────────────────────────────────────────────────────────────────────
// DisconnectReason helpers
// ─────────────────────────────────────────────────────────────────────────────

const char* disconnect_reason_str(DisconnectReason r) {
    switch (r) {
        case DisconnectReason::PEER_CLOSED:       return "PEER_CLOSED";
        case DisconnectReason::RECV_ERROR:        return "RECV_ERROR";
        case DisconnectReason::SEND_ERROR:        return "SEND_ERROR";
        case DisconnectReason::HEARTBEAT_TIMEOUT: return "HEARTBEAT_TIMEOUT";
        case DisconnectReason::PROTOCOL_ERROR:    return "PROTOCOL_ERROR";
        case DisconnectReason::USER_REQUESTED:    return "USER_REQUESTED";
        default:                                  return "UNKNOWN";
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// ReconnectPolicy
// ─────────────────────────────────────────────────────────────────────────────

int ReconnectPolicy::next_delay_ms() {
    int shift = (attempt_ < 8) ? attempt_ : 8;
    int delay = std::min(BASE_MS * (1 << shift), MAX_MS);
    delay += rand() % 100;  // jitter: 0-100 ms
    ++attempt_;
    return delay;
}

// ─────────────────────────────────────────────────────────────────────────────
// MarketDataSocket
// ─────────────────────────────────────────────────────────────────────────────

MarketDataSocket::MarketDataSocket() {
    signal(SIGPIPE, SIG_IGN);
}

MarketDataSocket::~MarketDataSocket() {
    disconnect();
}

bool MarketDataSocket::configure_socket() {
    // Non-blocking
    int flags = fcntl(fd_, F_GETFL, 0);
    if (flags < 0 || fcntl(fd_, F_SETFL, flags | O_NONBLOCK) < 0) {
        last_errno_ = errno;
        return false;
    }

    set_tcp_nodelay(true);
    set_recv_buffer_size(4 * 1024 * 1024);

    // TCP keepalive
    int opt = 1;
    setsockopt(fd_, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt));
    int idle  = 10, intvl = 5, cnt = 3;
    setsockopt(fd_, IPPROTO_TCP, TCP_KEEPIDLE,  &idle,  sizeof(idle));
    setsockopt(fd_, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    setsockopt(fd_, IPPROTO_TCP, TCP_KEEPCNT,   &cnt,   sizeof(cnt));

    return true;
}

bool MarketDataSocket::connect(const std::string& host, uint16_t port,
                               uint32_t timeout_ms)
{
    disconnect();

    fd_ = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd_ < 0) { last_errno_ = errno; return false; }

    if (!configure_socket()) { ::close(fd_); fd_ = -1; return false; }

    // Resolve host
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char port_str[8];
    std::snprintf(port_str, sizeof(port_str), "%u", port);

    if (getaddrinfo(host.c_str(), port_str, &hints, &res) != 0 || !res) {
        last_errno_ = errno;
        ::close(fd_); fd_ = -1;
        return false;
    }

    // Non-blocking connect
    int rc = ::connect(fd_, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);

    if (rc == 0) {
        connected_  = true;
        last_errno_ = 0;
        return true;
    }

    if (errno != EINPROGRESS) {
        last_errno_ = errno;
        ::close(fd_); fd_ = -1;
        return false;
    }

    // Wait for connection with select() timeout
    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(fd_, &wfds);
    struct timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    rc = select(fd_ + 1, nullptr, &wfds, nullptr, &tv);
    if (rc <= 0) {
        last_errno_ = (rc == 0) ? ETIMEDOUT : errno;
        ::close(fd_); fd_ = -1;
        return false;
    }

    // Check for connection error
    int err = 0;
    socklen_t elen = sizeof(err);
    getsockopt(fd_, SOL_SOCKET, SO_ERROR, &err, &elen);
    if (err != 0) {
        last_errno_ = err;
        ::close(fd_); fd_ = -1;
        return false;
    }

    connected_  = true;
    last_errno_ = 0;
    return true;
}

ssize_t MarketDataSocket::receive(void* buffer, size_t max_len) {
    if (fd_ < 0) { errno = EBADF; return -1; }
    ssize_t n = recv(fd_, buffer, max_len, 0);
    if (n < 0) last_errno_ = errno;
    if (n == 0) { connected_ = false; }
    return n;
}

bool MarketDataSocket::send_subscription(const std::vector<uint16_t>& symbol_ids) {
    if (fd_ < 0 || !connected_) return false;

    // Build: [0xFF][count:u16][id:u16 ...]
    size_t total = 3 + symbol_ids.size() * 2;
    std::vector<uint8_t> buf(total);
    buf[0] = 0xFF;
    uint16_t count = static_cast<uint16_t>(symbol_ids.size());
    std::memcpy(buf.data() + 1, &count, 2);
    for (size_t i = 0; i < symbol_ids.size(); ++i) {
        std::memcpy(buf.data() + 3 + i * 2, &symbol_ids[i], 2);
    }

    size_t sent = 0;
    while (sent < total) {
        ssize_t n = send(fd_, buf.data() + sent, total - sent, MSG_NOSIGNAL);
        if (n <= 0) { last_errno_ = errno; return false; }
        sent += n;
    }
    return true;
}

bool MarketDataSocket::is_connected() const {
    return fd_ >= 0 && connected_;
}

void MarketDataSocket::disconnect() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    connected_ = false;
}

bool MarketDataSocket::set_tcp_nodelay(bool enable) {
    if (fd_ < 0) return false;
    int opt = enable ? 1 : 0;
    return setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt)) == 0;
}

bool MarketDataSocket::set_recv_buffer_size(size_t bytes) {
    if (fd_ < 0) return false;
    int sz = static_cast<int>(bytes);
    return setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &sz, sizeof(sz)) == 0;
}

bool MarketDataSocket::set_socket_priority(int priority) {
    if (fd_ < 0) return false;
    return setsockopt(fd_, SOL_SOCKET, SO_PRIORITY, &priority, sizeof(priority)) == 0;
}
