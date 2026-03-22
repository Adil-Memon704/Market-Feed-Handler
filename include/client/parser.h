#pragma once

#include <cstdint>
#include <functional>
#include <atomic>
#include <memory>
#include "../protocol.h"
#include "../common/ring_buffer.h"
#include "../common/latency_tracker.h"

// ─────────────────────────────────────────────────────────────────────────────
// ParsedMessage — describes one fully parsed message (no heap allocation).
// Points into a scratch buffer managed by the parser.
// ─────────────────────────────────────────────────────────────────────────────

struct ParsedMessage {
    uint16_t    msg_type;
    uint32_t    seq_num;
    uint64_t    timestamp_ns;
    uint16_t    symbol_id;
    uint64_t    recv_time_ns;  // when bytes arrived in userspace

    // Union for payload
    union {
        struct { double price; uint32_t quantity; } trade;
        struct { double bid_price; uint32_t bid_qty;
                 double ask_price; uint32_t ask_qty; } quote;
    };
};

using MessageCallback = std::function<void(const ParsedMessage&)>;

// ─────────────────────────────────────────────────────────────────────────────
// Parser — stateful binary protocol parser.
//
// Feed raw bytes via feed(). Complete messages are dispatched via callback.
// The 8 MB ring buffer is heap-allocated to avoid stack overflow when Parser
// is used as a local variable.
// ─────────────────────────────────────────────────────────────────────────────

class Parser {
public:
    static constexpr size_t RECV_BUF_SIZE = 8 * 1024 * 1024; // 8 MB

    using RingBuf = ByteRingBuffer<RECV_BUF_SIZE>;

    Parser();

    void set_callback(MessageCallback cb) { callback_ = std::move(cb); }
    void set_latency_tracker(LatencyTracker* lt) { latency_ = lt; }

    // Feed raw bytes from recv(). recv_time is mono_ns() recorded at recv().
    void feed(const uint8_t* data, size_t len, uint64_t recv_time_ns);

    // Parse all complete messages currently in the ring buffer.
    // Returns number of messages processed.
    int process();

    // Stats
    uint64_t msgs_parsed()         const { return msgs_parsed_.load(); }
    uint64_t checksum_errors()     const { return checksum_errors_.load(); }
    uint64_t sequence_gaps()       const { return sequence_gaps_.load(); }
    uint64_t total_gap_size()      const { return total_gap_size_.load(); }

    void reset_stats();

private:
    enum class State { READING_HEADER, READING_PAYLOAD };

    // Heap-allocated to avoid 8 MB stack overflow when Parser is a local var
    std::unique_ptr<RingBuf> ring_;

    State    state_          = State::READING_HEADER;
    uint32_t expected_seq_   = 0;
    bool     seq_initialized_= false;

    // Scratch buffer for wrap-around messages
    uint8_t  scratch_[MAX_MSG_SIZE + 4];

    // Last recv time (set by feed(), used for all messages in that batch)
    uint64_t batch_recv_time_ns_ = 0;

    MessageCallback  callback_;
    LatencyTracker*  latency_ = nullptr;

    std::atomic<uint64_t> msgs_parsed_{0};
    std::atomic<uint64_t> checksum_errors_{0};
    std::atomic<uint64_t> sequence_gaps_{0};
    std::atomic<uint64_t> total_gap_size_{0};
};
