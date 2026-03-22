#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cassert>

// ─────────────────────────────────────────────────────────────────────────────
// ByteRingBuffer — single-producer single-consumer byte stream ring buffer.
// Used as the application-level receive buffer between the network thread
// (writer) and the parse thread (reader).
//
// Capacity must be a power of two. Thread-safe for one writer and one reader.
// ─────────────────────────────────────────────────────────────────────────────

template<size_t Capacity>
class ByteRingBuffer {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2");
    static constexpr size_t MASK = Capacity - 1;

public:
    ByteRingBuffer() : write_pos_(0), read_pos_(0) {}

    // Number of bytes available to write
    size_t writable() const {
        return Capacity - (write_pos_.load(std::memory_order_relaxed)
                         - read_pos_.load(std::memory_order_acquire));
    }

    // Number of bytes available to read
    size_t readable() const {
        return write_pos_.load(std::memory_order_acquire)
             - read_pos_.load(std::memory_order_relaxed);
    }

    // Write up to len bytes; returns bytes actually written
    size_t write(const void* src, size_t len) {
        size_t avail = writable();
        if (len > avail) len = avail;
        if (len == 0) return 0;

        size_t wp   = write_pos_.load(std::memory_order_relaxed) & MASK;
        size_t tail = Capacity - wp;
        if (len <= tail) {
            std::memcpy(buf_ + wp, src, len);
        } else {
            std::memcpy(buf_ + wp, src, tail);
            std::memcpy(buf_, static_cast<const uint8_t*>(src) + tail, len - tail);
        }
        write_pos_.fetch_add(len, std::memory_order_release);
        return len;
    }

    // Peek at up to len bytes without consuming; copies into dst.
    // Returns number of bytes copied (may be less than len).
    size_t peek(void* dst, size_t len) const {
        size_t avail = readable();
        if (len > avail) len = avail;
        if (len == 0) return 0;

        size_t rp   = read_pos_.load(std::memory_order_relaxed) & MASK;
        size_t tail = Capacity - rp;
        if (len <= tail) {
            std::memcpy(dst, buf_ + rp, len);
        } else {
            std::memcpy(dst, buf_ + rp, tail);
            std::memcpy(static_cast<uint8_t*>(dst) + tail, buf_, len - tail);
        }
        return len;
    }

    // Consume (advance read pointer by n bytes). Must call peek first.
    void consume(size_t n) {
        read_pos_.fetch_add(n, std::memory_order_release);
    }

    // Read and consume up to len bytes. Returns bytes read.
    size_t read(void* dst, size_t len) {
        size_t n = peek(dst, len);
        consume(n);
        return n;
    }

    void reset() {
        read_pos_.store(0, std::memory_order_relaxed);
        write_pos_.store(0, std::memory_order_relaxed);
    }

    size_t capacity() const { return Capacity; }

private:
    alignas(64) uint8_t buf_[Capacity];
    alignas(64) std::atomic<size_t> write_pos_;
    alignas(64) std::atomic<size_t> read_pos_;
};

// ─────────────────────────────────────────────────────────────────────────────
// SPSCQueue<T> — single-producer single-consumer object queue.
// Used between tick_thread and broadcast_thread.
// ─────────────────────────────────────────────────────────────────────────────

template<typename T, size_t Capacity>
class SPSCQueue {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2");
    static constexpr size_t MASK = Capacity - 1;

public:
    SPSCQueue() : head_(0), tail_(0) {}

    bool push(const T& item) {
        size_t tail = tail_.load(std::memory_order_relaxed);
        size_t next = (tail + 1) & MASK;
        if (next == head_.load(std::memory_order_acquire)) return false; // full
        slots_[tail] = item;
        tail_.store(next, std::memory_order_release);
        return true;
    }

    bool pop(T& item) {
        size_t head = head_.load(std::memory_order_relaxed);
        if (head == tail_.load(std::memory_order_acquire)) return false; // empty
        item = slots_[head];
        head_.store((head + 1) & MASK, std::memory_order_release);
        return true;
    }

    bool empty() const {
        return head_.load(std::memory_order_acquire) ==
               tail_.load(std::memory_order_acquire);
    }

private:
    alignas(64) T slots_[Capacity];
    alignas(64) std::atomic<size_t> head_;
    alignas(64) std::atomic<size_t> tail_;
};
