#include "../../include/server/tick_generator.h"
#include <cmath>
#include <cstring>
#include <random>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// Xoshiro256++ implementation
// ─────────────────────────────────────────────────────────────────────────────

Xoshiro256pp::Xoshiro256pp(uint64_t seed) {
    // Seed the state using SplitMix64
    auto splitmix = [](uint64_t& x) -> uint64_t {
        x += 0x9e3779b97f4a7c15ULL;
        uint64_t z = x;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31);
    };
    s_[0] = splitmix(seed);
    s_[1] = splitmix(seed);
    s_[2] = splitmix(seed);
    s_[3] = splitmix(seed);
}

uint64_t Xoshiro256pp::next() {
    uint64_t result = rotl(s_[0] + s_[3], 23) + s_[0];
    uint64_t t = s_[1] << 17;
    s_[2] ^= s_[0];
    s_[3] ^= s_[1];
    s_[1] ^= s_[2];
    s_[0] ^= s_[3];
    s_[2] ^= t;
    s_[3] = rotl(s_[3], 45);
    return result;
}

double Xoshiro256pp::uniform01() {
    // 53-bit mantissa → [0, 1)
    return static_cast<double>(next() >> 11) * 0x1.0p-53;
}

void Xoshiro256pp::jump() {
    static const uint64_t JUMP[] = {
        0x180ec6d33cfd0abaULL, 0xd5a61266f0c9392cULL,
        0xa9582618e03fc9aaULL, 0x39abdc4529b1661cULL
    };
    uint64_t s0 = 0, s1 = 0, s2 = 0, s3 = 0;
    for (int i = 0; i < 4; i++) {
        for (int b = 0; b < 64; b++) {
            if (JUMP[i] & (1ULL << b)) {
                s0 ^= s_[0]; s1 ^= s_[1];
                s2 ^= s_[2]; s3 ^= s_[3];
            }
            next();
        }
    }
    s_[0] = s0; s_[1] = s1; s_[2] = s2; s_[3] = s3;
}

// ─────────────────────────────────────────────────────────────────────────────
// Box-Muller transform
// ─────────────────────────────────────────────────────────────────────────────

NormalPair box_muller(Xoshiro256pp& rng) {
    double u1, u2;
    do { u1 = rng.uniform01(); } while (u1 <= 1e-300);
    u2 = rng.uniform01();
    double mag = std::sqrt(-2.0 * std::log(u1));
    return {
        mag * std::cos(2.0 * M_PI * u2),
        mag * std::sin(2.0 * M_PI * u2)
    };
}

// ─────────────────────────────────────────────────────────────────────────────
// TickGenerator implementation
// ─────────────────────────────────────────────────────────────────────────────

static constexpr double TICK_SIZE = 0.05;  // NSE minimum price tick (₹)

// Round to nearest tick
static double round_tick(double price) {
    return std::round(price / TICK_SIZE) * TICK_SIZE;
}

TickGenerator::TickGenerator(size_t num_symbols, uint64_t seed)
    : rng_(seed)
{
    // NSE-like symbol names for top symbols, rest are SYM000..SYM499
    const char* nse_names[] = {
        "RELIANCE", "TCS", "INFY", "HDFC", "ICICIBANK",
        "SBIN", "BAJFINANCE", "BHARTIARTL", "WIPRO", "HCLTECH",
        "KOTAKBANK", "ASIANPAINT", "AXISBANK", "MARUTI", "TITAN",
        "SUNPHARMA", "ULTRACEMCO", "NESTLEIND", "POWERGRID", "NTPC"
    };
    constexpr int NUM_NAMED = 20;

    processes_.reserve(num_symbols);
    Xoshiro256pp param_rng(seed ^ 0xabcdef1234567890ULL);

    for (size_t i = 0; i < num_symbols; ++i) {
        SymbolProcess p{};
        p.symbol_id     = static_cast<uint16_t>(i);

        // Name
        if (i < static_cast<size_t>(NUM_NAMED)) {
            std::strncpy(p.name, nse_names[i], sizeof(p.name) - 1);
        } else {
            std::snprintf(p.name, sizeof(p.name), "SYM%03zu", i);
        }

        // Initial price: U(100, 5000)
        p.initial_price = 100.0 + param_rng.uniform01() * 4900.0;
        p.price         = round_tick(p.initial_price);

        // Drift: U(-0.02, +0.02) annualised
        p.mu = (param_rng.uniform01() - 0.5) * 0.04;

        // Volatility: U(0.01, 0.06) annualised
        p.sigma = 0.01 + param_rng.uniform01() * 0.05;

        // Spread: U(0.0005, 0.002) fraction of price
        p.spread_pct = 0.0005 + param_rng.uniform01() * 0.0015;

        processes_.push_back(p);
    }
}

double TickGenerator::next_normal() {
    if (has_spare_normal_) {
        has_spare_normal_ = false;
        return spare_normal_;
    }
    auto [z0, z1] = box_muller(rng_);
    spare_normal_     = z1;
    has_spare_normal_ = true;
    return z0;
}

void TickGenerator::generate_tick(uint16_t symbol_id, uint32_t seq_num) {
    if (symbol_id >= processes_.size()) return;
    SymbolProcess& proc = processes_[symbol_id];

    // dt per tick for this symbol (approximate: tick_rate / num_symbols)
    double dt = 1.0 / static_cast<double>(
        std::max(1u, tick_rate_ / static_cast<uint32_t>(processes_.size())));

    double eps = next_normal();

    // Exact log-normal GBM:  S(t+dt) = S(t) * exp((μ - σ²/2)*dt + σ*ε*√dt)
    double exponent = (proc.mu - 0.5 * proc.sigma * proc.sigma) * dt
                    + proc.sigma * eps * std::sqrt(dt);
    double new_price = proc.price * std::exp(exponent);

    // Floor guard
    if (new_price < 1.0 || !std::isfinite(new_price)) {
        new_price = proc.initial_price;
    }
    proc.price = new_price;

    // Determine message type: 70% quote, 30% trade
    double r = rng_.uniform01();

    if (r < 0.70) {
        // QUOTE UPDATE
        double spread_eff = proc.spread_pct;
        if (std::abs(eps) > 2.0) spread_eff *= 1.5;  // widen on high vol

        double half = proc.price * spread_eff / 2.0;
        double bid  = round_tick(proc.price - half);
        double ask  = round_tick(proc.price + half);
        if (ask - bid < TICK_SIZE) ask = bid + TICK_SIZE;

        // Volume: log-normal
        double lv_eps = next_normal();
        uint32_t bid_qty = static_cast<uint32_t>(
            std::max(1.0, std::exp(6.21 + 0.8 * lv_eps)));
        lv_eps = next_normal();
        uint32_t ask_qty = static_cast<uint32_t>(
            std::max(1.0, std::exp(6.21 + 0.8 * lv_eps)));

        emit_quote(proc, seq_num, bid, bid_qty, ask, ask_qty);
    } else {
        // TRADE EXECUTION — price somewhere between bid and ask
        double spread_eff = proc.spread_pct;
        double half  = proc.price * spread_eff / 2.0;
        double trade_price = round_tick(
            proc.price + (rng_.uniform01() - 0.5) * half * 2.0);

        double lv_eps = next_normal();
        uint32_t qty  = static_cast<uint32_t>(
            std::max(1.0, std::exp(6.21 + 0.8 * lv_eps)));

        // Volume multiplier on large moves
        if (std::abs(eps) > 1.5) {
            qty = static_cast<uint32_t>(qty * (1.0 + std::abs(eps) - 1.5));
        }
        qty = std::min(qty, 1'000'000u);

        emit_trade(proc, seq_num, trade_price, qty);
    }
}

void TickGenerator::generate_heartbeat(uint32_t seq_num) {
    HeartbeatMsg msg{};
    msg.header.msg_type    = MSG_HEARTBEAT;
    msg.header.seq_num     = seq_num;
    msg.header.timestamp_ns= now_ns();
    msg.header.symbol_id   = 0;
    set_checksum(&msg, sizeof(msg));
    if (callback_) callback_(&msg, sizeof(msg));
}

void TickGenerator::emit_trade(const SymbolProcess& proc, uint32_t seq_num,
                               double price, uint32_t qty) {
    TradeMsg msg{};
    msg.header.msg_type     = MSG_TRADE;
    msg.header.seq_num      = seq_num;
    msg.header.timestamp_ns = now_ns();
    msg.header.symbol_id    = proc.symbol_id;
    msg.payload.price       = price;
    msg.payload.quantity    = qty;
    set_checksum(&msg, sizeof(msg));
    if (callback_) callback_(&msg, sizeof(msg));
}

void TickGenerator::emit_quote(const SymbolProcess& proc, uint32_t seq_num,
                               double bid, uint32_t bid_qty,
                               double ask, uint32_t ask_qty) {
    QuoteMsg msg{};
    msg.header.msg_type     = MSG_QUOTE;
    msg.header.seq_num      = seq_num;
    msg.header.timestamp_ns = now_ns();
    msg.header.symbol_id    = proc.symbol_id;
    msg.payload.bid_price   = bid;
    msg.payload.bid_qty     = bid_qty;
    msg.payload.ask_price   = ask;
    msg.payload.ask_qty     = ask_qty;
    set_checksum(&msg, sizeof(msg));
    if (callback_) callback_(&msg, sizeof(msg));
}
