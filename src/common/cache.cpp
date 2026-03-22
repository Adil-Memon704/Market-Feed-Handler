#include "../../include/common/cache.h"
#include <cstdlib>
#include <stdexcept>
#include <thread>

// ─────────────────────────────────────────────────────────────────────────────
// SymbolCache implementation
// ─────────────────────────────────────────────────────────────────────────────

SymbolCache::SymbolCache(uint16_t num_symbols)
    : num_symbols_(num_symbols)
{
    // Allocate cache-line-aligned array
    void* ptr = nullptr;
    if (posix_memalign(&ptr, 64, sizeof(CacheEntry) * num_symbols) != 0) {
        throw std::bad_alloc();
    }
    entries_ = new (ptr) CacheEntry[num_symbols]();
}

SymbolCache::~SymbolCache() {
    for (uint16_t i = 0; i < num_symbols_; ++i) {
        entries_[i].~CacheEntry();
    }
    std::free(entries_);
}

// ─────────────────────────────────────────────────────────────────────────────
// Seqlock helpers
// ─────────────────────────────────────────────────────────────────────────────

inline void SymbolCache::seqlock_write_begin(CacheEntry& e) {
    uint32_t s = e.seq.load(std::memory_order_relaxed);
    e.seq.store(s + 1, std::memory_order_release);  // mark dirty (odd)
}

inline void SymbolCache::seqlock_write_end(CacheEntry& e) {
    uint32_t s = e.seq.load(std::memory_order_relaxed);
    e.seq.store(s + 1, std::memory_order_release);  // mark clean (even)
}

// ─────────────────────────────────────────────────────────────────────────────
// Writer API
// ─────────────────────────────────────────────────────────────────────────────

void SymbolCache::update_quote(uint16_t symbol_id,
                               double   bid_price, uint32_t bid_qty,
                               double   ask_price, uint32_t ask_qty,
                               uint64_t ts_ns)
{
    if (symbol_id >= num_symbols_) return;
    CacheEntry& e = entries_[symbol_id];

    seqlock_write_begin(e);
    e.state.best_bid      = bid_price;
    e.state.best_ask      = ask_price;
    e.state.bid_quantity  = bid_qty;
    e.state.ask_quantity  = ask_qty;
    e.state.last_update_time = ts_ns;
    ++e.state.update_count;
    if (e.state.open_price == 0.0) {
        e.state.open_price = (bid_price + ask_price) * 0.5;
    }
    seqlock_write_end(e);
}

void SymbolCache::update_trade(uint16_t symbol_id,
                               double   price, uint32_t qty,
                               uint64_t ts_ns)
{
    if (symbol_id >= num_symbols_) return;
    CacheEntry& e = entries_[symbol_id];

    seqlock_write_begin(e);
    e.state.last_traded_price    = price;
    e.state.last_traded_quantity = qty;
    e.state.last_update_time     = ts_ns;
    ++e.state.update_count;
    if (e.state.open_price == 0.0) {
        e.state.open_price = price;
    }
    seqlock_write_end(e);
}

// ─────────────────────────────────────────────────────────────────────────────
// Reader API
// ─────────────────────────────────────────────────────────────────────────────

MarketState SymbolCache::get_snapshot(uint16_t symbol_id) const {
    if (symbol_id >= num_symbols_) return MarketState{};
    const CacheEntry& e = entries_[symbol_id];

    MarketState s;
    uint32_t seq1, seq2;
    do {
        seq1 = e.seq.load(std::memory_order_acquire);
        if (seq1 & 1u) {
            // Write in progress — spin (with pause hint)
#if defined(__x86_64__)
            __asm__ volatile("pause" ::: "memory");
#elif defined(__aarch64__)
            __asm__ volatile("yield" ::: "memory");
#endif
            continue;
        }
        s    = e.state;  // copy
        seq2 = e.seq.load(std::memory_order_acquire);
    } while (seq1 != seq2);

    return s;
}

uint64_t SymbolCache::get_update_count(uint16_t symbol_id) const {
    if (symbol_id >= num_symbols_) return 0;
    // Relaxed read of update_count — acceptable for display purposes
    return entries_[symbol_id].state.update_count;
}
