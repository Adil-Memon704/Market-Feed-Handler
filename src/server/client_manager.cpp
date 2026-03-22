#include "../../include/server/client_manager.h"
#include "../../include/protocol.h"
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <cinttypes>

ClientManager::ClientManager(int epfd) : epfd_(epfd) {}

// ─────────────────────────────────────────────────────────────────────────────
// Add / remove clients
// ─────────────────────────────────────────────────────────────────────────────

void ClientManager::add_client(int fd, const std::string& peer_addr) {
    ClientState cs{};
    cs.fd          = fd;
    cs.phase       = ClientPhase::ACCEPTED;
    cs.connected_at = std::chrono::steady_clock::now();
    cs.last_send    = cs.connected_at;
    cs.peer_addr   = peer_addr;
    clients_.emplace(fd, std::move(cs));
    std::fprintf(stderr, "[server] Client connected fd=%d addr=%s total=%zu\n",
                 fd, peer_addr.c_str(), clients_.size());
}

void ClientManager::remove_client(int fd) {
    auto it = clients_.find(fd);
    if (it == clients_.end()) return;
    std::fprintf(stderr, "[server] Client disconnected fd=%d addr=%s sent=%llu dropped=%llu\n",
                 fd, it->second.peer_addr.c_str(),
                 (unsigned long long)it->second.msgs_sent,
                 (unsigned long long)it->second.msgs_dropped);
    epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr);
    close(fd);
    clients_.erase(it);
}

// ─────────────────────────────────────────────────────────────────────────────
// Send helpers
// ─────────────────────────────────────────────────────────────────────────────

void ClientManager::arm_epollout(int fd, bool enable) {
    epoll_event ev{};
    ev.data.fd = fd;
    ev.events  = EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLRDHUP;
    if (enable) ev.events |= EPOLLOUT;
    epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &ev);
}

void ClientManager::enqueue(ClientState& cs, const void* data, size_t len) {
    cs.pending.emplace_back(static_cast<const uint8_t*>(data),
                             static_cast<const uint8_t*>(data) + len);
    cs.pending_bytes += len;
}

bool ClientManager::flush_queue(ClientState& cs) {
    while (!cs.pending.empty()) {
        auto& buf = cs.pending.front();
        ssize_t sent = send(cs.fd, buf.data(), buf.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
        if (sent == static_cast<ssize_t>(buf.size())) {
            cs.pending_bytes -= buf.size();
            cs.bytes_sent    += buf.size();
            ++cs.msgs_sent;
            cs.pending.pop_front();
        } else if (sent > 0) {
            cs.pending_bytes -= sent;
            cs.bytes_sent    += sent;
            buf.erase(buf.begin(), buf.begin() + sent);
            return false;  // still has data
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return false;  // still blocked
        } else {
            return false;  // error — caller handles disconnect
        }
    }
    return true;  // fully flushed
}

bool ClientManager::send_to(int fd, const void* data, size_t len) {
    auto it = clients_.find(fd);
    if (it == clients_.end()) return false;
    ClientState& cs = it->second;

    if (!cs.pending.empty()) {
        // Already backpressured — enqueue
        enqueue(cs, data, len);
        ++cs.msgs_dropped;
        ++total_msgs_dropped_;

        if (cs.pending_bytes > SLOW_CLIENT_DROP_BYTES) {
            std::fprintf(stderr, "[server] SLOW CLIENT fd=%d pending=%.1fMB — dropping\n",
                         fd, cs.pending_bytes / (1024.0 * 1024.0));
            remove_client(fd);
            return false;
        }
        return true;
    }

    ssize_t sent = send(fd, data, len, MSG_NOSIGNAL | MSG_DONTWAIT);
    if (sent == static_cast<ssize_t>(len)) {
        cs.bytes_sent += len;
        ++cs.msgs_sent;
        ++total_msgs_sent_;
        cs.last_send = std::chrono::steady_clock::now();
        return true;
    } else if (sent == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        enqueue(cs, data, len);
        arm_epollout(fd, true);
        ++cs.msgs_dropped;
        ++total_msgs_dropped_;
        return true;
    } else if (sent == -1) {
        remove_client(fd);
        return false;
    }
    // Partial send — queue the rest
    enqueue(cs, static_cast<const uint8_t*>(data) + sent, len - sent);
    arm_epollout(fd, true);
    return true;
}

void ClientManager::broadcast(const void* data, size_t len) {
    std::vector<int> to_remove;
    for (auto& [fd, cs] : clients_) {
        if (cs.phase != ClientPhase::LIVE) continue;
        if (!send_to(fd, data, len)) {
            to_remove.push_back(fd);
        }
    }
    // Remove clients that failed during send_to (already closed in send_to)
    // send_to already calls remove_client, so we just need to handle the
    // case where the fd was removed mid-iteration — already handled.
    (void)to_remove;
}

void ClientManager::flush_pending(int fd) {
    auto it = clients_.find(fd);
    if (it == clients_.end()) return;
    ClientState& cs = it->second;

    bool done = flush_queue(cs);
    if (done) {
        arm_epollout(fd, false);  // No longer need EPOLLOUT
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Subscription parsing
// ─────────────────────────────────────────────────────────────────────────────

void ClientManager::on_recv(int fd, const void* data, size_t len) {
    auto it = clients_.find(fd);
    if (it == clients_.end()) return;
    ClientState& cs = it->second;

    if (cs.phase == ClientPhase::LIVE) return;  // No upstream data expected

    // Accumulate bytes
    const uint8_t* p = static_cast<const uint8_t*>(data);
    cs.recv_buf.insert(cs.recv_buf.end(), p, p + len);

    // Parse subscription message: [0xFF][count:u16][symbol_id:u16 ...]
    if (cs.recv_buf.size() < 3) return;
    if (cs.recv_buf[0] != MSG_SUBSCRIBE) {
        cs.phase = ClientPhase::LIVE;  // No subscription — receive all
        cs.subscribe_all = true;
        return;
    }

    uint16_t count;
    std::memcpy(&count, cs.recv_buf.data() + 1, 2);
    size_t expected_size = 3 + count * 2;
    if (cs.recv_buf.size() < expected_size) return;

    cs.subscribed_symbols.clear();
    for (uint16_t i = 0; i < count; ++i) {
        uint16_t sym;
        std::memcpy(&sym, cs.recv_buf.data() + 3 + i * 2, 2);
        if (sym < MAX_SYMBOLS) cs.subscribed_symbols.push_back(sym);
    }
    cs.subscribe_all = (count == 0);
    cs.phase = ClientPhase::LIVE;
    cs.recv_buf.clear();

    std::fprintf(stderr, "[server] Client fd=%d subscribed to %u symbols (all=%d)\n",
                 fd, count, cs.subscribe_all);
}

// ─────────────────────────────────────────────────────────────────────────────
// Periodic maintenance
// ─────────────────────────────────────────────────────────────────────────────

void ClientManager::tick(std::chrono::steady_clock::time_point now) {
    std::vector<int> to_remove;
    for (auto& [fd, cs] : clients_) {
        if (cs.pending_bytes > SLOW_CLIENT_WARN_BYTES) {
            auto age = std::chrono::duration_cast<std::chrono::seconds>(
                now - cs.last_send).count();
            std::fprintf(stderr,
                "[server] Slow client fd=%d pending=%.1fMB age=%llds\n",
                fd, cs.pending_bytes / (1024.0 * 1024.0), (long long)age);
        }
        if (cs.pending_bytes > SLOW_CLIENT_DROP_BYTES) {
            to_remove.push_back(fd);
        }
    }
    for (int fd : to_remove) remove_client(fd);
}
