#pragma once

#include <cstdint>
#include <cstddef>
#include <array>
#include <functional>
#include "../protocol.h"

// ─────────────────────────────────────────────────────────────────────────────
// Xoshiro256++ PRNG — fast, passes BigCrush, 256-bit state.
// ─────────────────────────────────────────────────────────────────────────────

class Xoshiro256pp {
public:
    explicit Xoshiro256pp(uint64_t seed = 0xdeadbeefcafe1234ULL);

    uint64_t next();
    double   uniform01();          // [0,1)
    void     jump();               // advance 2^128 steps (for parallel streams)

private:
    static uint64_t rotl(uint64_t x, int k) {
        return (x << k) | (x >> (64 - k));
    }
    uint64_t s_[4];
};

// ─────────────────────────────────────────────────────────────────────────────
// Box-Muller transform — produces two N(0,1) samples per call.
// ─────────────────────────────────────────────────────────────────────────────

struct NormalPair {
    double z0, z1;
};
NormalPair box_muller(Xoshiro256pp& rng);

// ─────────────────────────────────────────────────────────────────────────────
// SymbolProcess — per-symbol GBM state.
// ─────────────────────────────────────────────────────────────────────────────

struct SymbolProcess {
    uint16_t symbol_id;
    double   price;           // current mid price (₹)
    double   initial_price;   // for open_price reference
    double   mu;              // annualised drift
    double   sigma;           // annualised volatility
    double   spread_pct;      // bid-ask spread as fraction of price
    char     name[16];        // e.g. "SYM042"
};

// ─────────────────────────────────────────────────────────────────────────────
// TickGenerator — generates Trade and Quote messages using GBM.
// ─────────────────────────────────────────────────────────────────────────────

using TickCallback = std::function<void(const void* msg, size_t len)>;

class TickGenerator {
public:
    explicit TickGenerator(size_t num_symbols = DEFAULT_SYMBOLS,
                           uint64_t seed = 0xdeadbeef12345678ULL);

    void set_callback(TickCallback cb) { callback_ = std::move(cb); }
    void set_tick_rate(uint32_t ticks_per_second) { tick_rate_ = ticks_per_second; }

    // Generate one tick for the given symbol. Calls callback with the serialised message.
    void generate_tick(uint16_t symbol_id, uint32_t seq_num);

    // Generate a heartbeat message for all connected clients.
    void generate_heartbeat(uint32_t seq_num);

    size_t num_symbols() const { return processes_.size(); }

    const SymbolProcess& get_process(uint16_t id) const {
        return processes_[id];
    }

private:
    void emit_trade(const SymbolProcess& proc, uint32_t seq_num,
                    double price, uint32_t qty);
    void emit_quote(const SymbolProcess& proc, uint32_t seq_num,
                    double bid, uint32_t bid_qty,
                    double ask, uint32_t ask_qty);

    std::vector<SymbolProcess> processes_;
    Xoshiro256pp               rng_;
    TickCallback               callback_;
    uint32_t                   tick_rate_ = 100'000;

    // Box-Muller buffering (generate 2 normals at once, cache the spare)
    bool   has_spare_normal_ = false;
    double spare_normal_     = 0.0;

    double next_normal();
};
