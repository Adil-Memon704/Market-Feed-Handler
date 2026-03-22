#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include "../protocol.h"

// ─────────────────────────────────────────────────────────────────────────────
// MarketState — the latest market data for one symbol.
// ─────────────────────────────────────────────────────────────────────────────

struct MarketState {
    double   best_bid            = 0.0;
    double   best_ask            = 0.0;
    uint32_t bid_quantity        = 0;
    uint32_t ask_quantity        = 0;
    double   last_traded_price   = 0.0;
    uint32_t last_traded_quantity= 0;
    uint64_t last_update_time    = 0;  // ns since epoch
    uint64_t update_count        = 0;  // total updates (for stats)
    double   open_price          = 0.0; // first price seen this session
};

// ─────────────────────────────────────────────────────────────────────────────
// SymbolCache — seqlock-protected array of MarketState.
//
// Single writer (parse thread) + multiple readers (visualizer, strategies).
// Each CacheEntry is padded to 2 cache lines:
//   - Cache line 0: seq counter (written by writer, read by both)
//   - Cache lines 1-2: MarketState (written by writer, read by reader)
//
// Writers: O(1), non-blocking.
// Readers: O(1) average, spin on write contention (rare with single writer).
// ─────────────────────────────────────────────────────────────────────────────

struct alignas(64) CacheEntry {
    std::atomic<uint32_t> seq{0};  // odd = write in progress
    char                  _pad[60];
    MarketState           state;
};

class SymbolCache {
public:
    explicit SymbolCache(uint16_t num_symbols = MAX_SYMBOLS);
    ~SymbolCache();

    // Writer API (call from parse thread only)
    void update_quote(uint16_t symbol_id,
                      double bid_price, uint32_t bid_qty,
                      double ask_price, uint32_t ask_qty,
                      uint64_t ts_ns);

    void update_trade(uint16_t symbol_id,
                      double price, uint32_t qty,
                      uint64_t ts_ns);

    // Reader API (lock-free, safe from any thread)
    MarketState get_snapshot(uint16_t symbol_id) const;

    // Convenience: returns update_count without full snapshot
    uint64_t get_update_count(uint16_t symbol_id) const;

    uint16_t num_symbols() const { return num_symbols_; }

private:
    void seqlock_write_begin(CacheEntry& e);
    void seqlock_write_end(CacheEntry& e);

    CacheEntry* entries_;
    uint16_t    num_symbols_;
};
