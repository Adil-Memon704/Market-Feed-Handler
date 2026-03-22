#include "../../include/client/parser.h"
#include <cstring>
#include <cstdio>

// ─────────────────────────────────────────────────────────────────────────────
// Parser
// ─────────────────────────────────────────────────────────────────────────────

Parser::Parser() : ring_(std::make_unique<RingBuf>()) {
    std::memset(scratch_, 0, sizeof(scratch_));
}

void Parser::reset_stats() {
    msgs_parsed_.store(0,       std::memory_order_relaxed);
    checksum_errors_.store(0,   std::memory_order_relaxed);
    sequence_gaps_.store(0,     std::memory_order_relaxed);
    total_gap_size_.store(0,    std::memory_order_relaxed);
    seq_initialized_ = false;
    expected_seq_    = 0;
    state_           = State::READING_HEADER;
    ring_->reset();
}

// ─────────────────────────────────────────────────────────────────────────────
// feed() — called by network thread: copies bytes into the ring buffer.
// ─────────────────────────────────────────────────────────────────────────────

void Parser::feed(const uint8_t* data, size_t len, uint64_t recv_time_ns) {
    batch_recv_time_ns_ = recv_time_ns;
    size_t written = ring_->write(data, len);
    if (written < len) {
        // Ring buffer overflow — this should not happen with correct sizing
        // but handle gracefully by dropping the overflow bytes
        std::fprintf(stderr, "[parser] WARN ring buffer overflow: dropped %zu bytes\n",
                     len - written);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// process() — called by parse thread: drains ring buffer, dispatches messages.
// ─────────────────────────────────────────────────────────────────────────────

int Parser::process() {
    int count = 0;

    while (true) {
        // ── Phase 1: need at least HEADER_SIZE bytes ──────────────────────
        if (ring_->readable() < HEADER_SIZE) break;

        // Peek header (may wrap around ring boundary — peek copies safely)
        MsgHeader hdr;
        ring_->peek(&hdr, HEADER_SIZE);

        uint16_t msg_type = hdr.msg_type;
        int payload_size  = payload_size_for_type(msg_type);

        if (payload_size < 0) {
            // Unknown message type — protocol error; skip 1 byte and retry
            std::fprintf(stderr, "[parser] Unknown msg_type=0x%04X — skipping byte\n",
                         msg_type);
            ring_->consume(1);
            continue;
        }

        size_t total_size = HEADER_SIZE
                          + static_cast<size_t>(payload_size)
                          + CHECKSUM_SIZE;

        // ── Phase 2: need full message ────────────────────────────────────
        if (ring_->readable() < total_size) break;

        // Peek entire message into scratch buffer (handles wrap-around)
        ring_->peek(scratch_, total_size);

        // ── Phase 3: verify checksum ──────────────────────────────────────
        if (!verify_checksum(scratch_, total_size)) {
            checksum_errors_.fetch_add(1, std::memory_order_relaxed);
            std::fprintf(stderr, "[parser] Checksum error seq=%u sym=%u — discarding\n",
                         hdr.seq_num, hdr.symbol_id);
            // Consume only 1 byte to try re-syncing
            ring_->consume(1);
            continue;
        }

        // ── Phase 4: consume the message ─────────────────────────────────
        ring_->consume(total_size);

        // ── Phase 5: sequence gap detection ──────────────────────────────
        if (seq_initialized_) {
            if (hdr.seq_num != expected_seq_) {
                if (hdr.seq_num > expected_seq_) {
                    uint32_t gap = hdr.seq_num - expected_seq_;
                    sequence_gaps_.fetch_add(1,   std::memory_order_relaxed);
                    total_gap_size_.fetch_add(gap, std::memory_order_relaxed);
                    std::fprintf(stderr,
                        "[parser] Sequence gap: expected=%u got=%u gap=%u\n",
                        expected_seq_, hdr.seq_num, gap);
                }
                // else: duplicate / old seq — silently ignore
            }
        } else {
            seq_initialized_ = true;
        }
        expected_seq_ = hdr.seq_num + 1;

        // ── Phase 6: validate symbol id ───────────────────────────────────
        if (hdr.symbol_id >= MAX_SYMBOLS && msg_type != MSG_HEARTBEAT) {
            // Out-of-range symbol — discard silently
            continue;
        }

        // ── Phase 7: build ParsedMessage and dispatch ─────────────────────
        if (msg_type == MSG_HEARTBEAT) {
            // Heartbeats are processed for timing but not dispatched
            msgs_parsed_.fetch_add(1, std::memory_order_relaxed);
            ++count;
            continue;
        }

        ParsedMessage pm{};
        pm.msg_type     = msg_type;
        pm.seq_num      = hdr.seq_num;
        pm.timestamp_ns = hdr.timestamp_ns;
        pm.symbol_id    = hdr.symbol_id;
        pm.recv_time_ns = batch_recv_time_ns_;

        if (msg_type == MSG_TRADE) {
            TradePayload tp;
            std::memcpy(&tp, scratch_ + HEADER_SIZE, TRADE_PAYLOAD);
            pm.trade.price    = tp.price;
            pm.trade.quantity = tp.quantity;
        } else if (msg_type == MSG_QUOTE) {
            QuotePayload qp;
            std::memcpy(&qp, scratch_ + HEADER_SIZE, QUOTE_PAYLOAD);
            pm.quote.bid_price = qp.bid_price;
            pm.quote.bid_qty   = qp.bid_qty;
            pm.quote.ask_price = qp.ask_price;
            pm.quote.ask_qty   = qp.ask_qty;
        }

        // Record end-to-end latency (message generation → parse completion)
        if (latency_ && pm.timestamp_ns > 0) {
            uint64_t now  = mono_ns();
            // Note: timestamp_ns is CLOCK_REALTIME, now is CLOCK_MONOTONIC.
            // For same-machine testing, use recv_time as proxy for latency.
            uint64_t lat  = (pm.recv_time_ns > 0 && now >= pm.recv_time_ns)
                            ? now - pm.recv_time_ns : 0;
            if (lat < 10'000'000ULL) {  // sanity: ignore > 10ms
                latency_->record(lat);
            }
        }

        msgs_parsed_.fetch_add(1, std::memory_order_relaxed);
        ++count;

        if (callback_) callback_(pm);
    }

    return count;
}
