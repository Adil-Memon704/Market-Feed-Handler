# GBM.md — Geometric Brownian Motion

## Table of Contents
1. [Mathematical Background](#1-mathematical-background)
2. [Implementation Details](#2-implementation-details)
3. [Realism Considerations](#3-realism-considerations)

---

## 1. Mathematical Background

### 1a. SDE Formulation

Stock prices are modelled as a **Geometric Brownian Motion (GBM)**, the continuous-time stochastic process described by the stochastic differential equation (SDE):

```
dS = μ S dt + σ S dW_t
```

where:

| Symbol | Meaning | Typical value |
|--------|---------|---------------|
| `S` | Current mid price (₹) | ₹100 – ₹5000 |
| `μ` | Drift — expected instantaneous return per unit time | ±0.02 (annualised) |
| `σ` | Volatility — standard deviation of instantaneous return per unit time | 0.01 – 0.06 |
| `dW_t` | Wiener increment — `W_t` is a standard Brownian motion | `ε √dt`, ε ~ N(0,1) |
| `dt` | Infinitesimal time step | 1/tick_rate (e.g. 10 µs at 100K/s) |

The term `μ S dt` is the **deterministic drift** — the expected price change over `dt` in the absence of randomness. The term `σ S dW_t` is the **stochastic diffusion** — the random noise term scaled by the current price (making volatility proportional to price, a key property of GBM).

### 1b. Discretisation for Simulation

Applying Itô's lemma to `f(S) = ln S` yields the exact solution:

```
S(t + Δt) = S(t) · exp( (μ - σ²/2) Δt + σ · ε · √Δt )
```

This is the **log-normal form** and is used in all tick generation. It is preferred over the naive Euler form:

```
# Euler (naive) — DO NOT USE:
S(t + Δt) = S(t) + μ·S(t)·Δt + σ·S(t)·ε·√Δt
```

The Euler form has two problems:
1. It can produce negative prices if `σ·ε·√Δt` is sufficiently negative.
2. Its discretisation error is O(Δt), while the exact log-normal form has zero discretisation error (it is the true solution to the SDE for constant μ and σ).

**Step-by-step computation per tick:**

```
1. Draw ε from N(0,1)  [Box-Muller, see §2a]
2. Compute exponent = (μ - 0.5·σ²)·Δt + σ·ε·√Δt
3. S_new = S_old · exp(exponent)
4. bid = S_new · (1 - spread_pct/2)
5. ask = S_new · (1 + spread_pct/2)
```

### 1c. Why GBM for Stock Prices?

GBM has three properties that make it a natural baseline for equity simulation:

1. **Positivity:** `S(t) > 0` for all `t`, since the exponential is always positive. Real stock prices cannot go negative.
2. **Log-normal returns:** `ln(S(t+Δt)/S(t))` is normally distributed. Empirically, short-horizon equity log-returns are approximately normal, making GBM a reasonable first-order model.
3. **Markov property:** The future price depends only on the current price, not the history. This simplifies implementation — each symbol's state is a single `double`.

**Limitations (acknowledged):**
- Real markets exhibit **fat tails** (more extreme moves than normal). A Student-t or jump-diffusion model would be more realistic but also more complex.
- Real volatility is **stochastic**, not constant (motivates Heston/SABR models).
- NSE stocks show **overnight gaps** and **intraday seasonality** not captured by plain GBM.

For the purposes of a market data feed simulator (stress-testing the feed handler, not performing risk calculations), plain GBM is entirely adequate.

---

## 2. Implementation Details

### 2a. Box–Muller Transform

The Box–Muller transform converts two independent uniform random variables `U₁, U₂ ~ U(0,1)` into two independent standard normal variables:

```
Z₀ = √(-2 ln U₁) · cos(2π U₂)
Z₁ = √(-2 ln U₁) · sin(2π U₂)
```

Both `Z₀` and `Z₁` are used: even-indexed symbols use `Z₀` and odd-indexed symbols use `Z₁`, so every two uniform draws produce two normal samples with no waste.

**C++ implementation:**

```cpp
// Xoshiro256++ PRNG — faster than std::mt19937, passes BigCrush
class Xoshiro256pp {
    uint64_t s[4];
public:
    uint64_t next() {
        uint64_t result = rotl(s[0] + s[3], 23) + s[0];
        uint64_t t = s[1] << 17;
        s[2] ^= s[0]; s[3] ^= s[1]; s[1] ^= s[2]; s[0] ^= s[3];
        s[2] ^= t;
        s[3] = rotl(s[3], 45);
        return result;
    }
    double uniform01() {
        return (next() >> 11) * 0x1.0p-53;  // 53-bit mantissa
    }
};

// Box-Muller — returns two standard normals at once
std::pair<double, double> box_muller(Xoshiro256pp& rng) {
    double u1, u2;
    do { u1 = rng.uniform01(); } while (u1 <= 1e-300);  // guard ln(0)
    u2 = rng.uniform01();
    double mag = std::sqrt(-2.0 * std::log(u1));
    double z0  = mag * std::cos(2.0 * M_PI * u2);
    double z1  = mag * std::sin(2.0 * M_PI * u2);
    return {z0, z1};
}
```

**Why Xoshiro256++ over `std::mt19937`?**

| Property | `mt19937` | Xoshiro256++ |
|----------|-----------|--------------|
| State size | 2496 bytes | 32 bytes |
| Throughput | ~300 MB/s | ~800 MB/s |
| Cache friendly | No (large state) | Yes |
| Statistical quality | High | High (passes BigCrush) |
| Seedability | 624 uint32 seeds | 4 uint64 seeds |

At 500K ticks/s with 100 symbols, the RNG is called ~50M times/second. Xoshiro256++ comfortably sustains this on a single core.

### 2b. Parameter Selection Rationale

```cpp
struct SymbolParams {
    double initial_price;   // U(100, 5000) — mimics NSE price range
    double mu;              // U(-0.02, +0.02) annualised — slight per-symbol drift
    double sigma;           // U(0.01, 0.06) annualised — low to high volatility
    double spread_pct;      // U(0.0005, 0.002) — 0.05% to 0.20% of price
};
```

Annualised parameters are converted to per-tick parameters:

```
sigma_tick = sigma * sqrt(dt)
   where dt = 1 / tick_rate   (e.g. 1e-5 s at 100K ticks/s)

mu_tick = (mu - 0.5 * sigma^2) * dt
```

**Example at 100K ticks/s, σ=0.02 (low volatility):**
```
dt = 1e-5 s
sigma_tick = 0.02 × √(1e-5) = 0.02 × 0.00316 ≈ 6.3 × 10⁻⁵
```
A price of ₹2,450 moves by ±₹0.15 per tick on average — a realistic sub-penny quote update.

### 2c. Time Step Considerations

The time step `Δt` must be chosen consistently with the tick rate:

- **Too large `Δt`:** Prices make unrealistically large jumps per message. A ₹2,450 stock does not move ₹25 in a single quote update.
- **Too small `Δt`:** Prices barely move between ticks, producing a stream of identical quotes — unrealistic and unhelpful for testing.

At a tick rate of 100K messages/second distributed across 100 symbols, each symbol receives approximately 1,000 updates/second. The effective time step per symbol tick is therefore `1/1000 s = 1 ms`, giving:

```
sigma_tick(per-symbol) = sigma × sqrt(0.001) ≈ sigma × 0.0316
```

For σ=0.02 and S=₹2,450: one-tick move ~ ±₹1.55. This is realistic for a mid-cap NSE stock during active trading.

---

## 3. Realism Considerations

### 3a. How Spread Relates to Price

The bid–ask spread is set as a **fixed percentage of the current mid price**, not a fixed absolute amount. This is consistent with real market microstructure: market makers quote tighter spreads on liquid, lower-priced stocks and wider spreads on illiquid, higher-priced ones.

```cpp
double half_spread = price * spread_pct / 2.0;
double bid = price - half_spread;
double ask = price + half_spread;

// Round to nearest tick size (NSE uses ₹0.05 minimum tick)
bid = std::round(bid / TICK_SIZE) * TICK_SIZE;
ask = std::round(ask / TICK_SIZE) * TICK_SIZE;

// Ensure minimum 1-tick spread
if (ask - bid < TICK_SIZE) ask = bid + TICK_SIZE;
```

During high-volatility ticks (when `|ε| > 2.0`), the spread is widened by 50% to simulate liquidity withdrawal during stress:

```cpp
if (std::abs(epsilon) > 2.0) spread_pct_effective *= 1.5;
```

### 3b. Volume Generation

Trade quantity is drawn from a **log-normal distribution** with parameters calibrated to produce volumes in the range typical of NSE mid-cap stocks (100–100,000 shares):

```cpp
// Base volume: log-normal with mean=1000, std=2000
double log_vol = mu_vol + sigma_vol * epsilon_vol;
uint32_t quantity = static_cast<uint32_t>(std::exp(log_vol));
quantity = std::clamp(quantity, 1u, 1'000'000u);
```

Parameters: `mu_vol = ln(500) ≈ 6.21`, `sigma_vol = 0.8`.

Quote updates have bid and ask quantities independently drawn from the same distribution but scaled by a **depth factor** (asks tend to be larger than bids in a rising market, simulating a supply overhang).

### 3c. Correlation Between Price and Volume

An optional enhancement applies the **leverage effect**: higher absolute price returns tend to be accompanied by higher volume (increased participation during large moves).

```cpp
// Scale volume up when absolute return is large
double abs_return = std::abs(epsilon);
double volume_multiplier = 1.0 + std::max(0.0, abs_return - 1.0);
// For |ε| > 1 sigma: volume increases by (|ε|-1)× baseline
```

This is disabled by default (`--realistic-volume` flag) to keep baseline tick generation computationally simple. When enabled, it improves the visual realism of the terminal display (large price moves coincide with volume spikes).

### 3d. Sequence Number and Timestamp Assignment

- **Sequence numbers** are assigned by the broadcast thread from a single global `std::atomic<uint32_t>` incremented with `fetch_add(1, relaxed)`. This guarantees strictly increasing, contiguous sequence numbers across all symbols in broadcast order — matching the NSE feed specification.
- **Timestamps** use `clock_gettime(CLOCK_REALTIME)` and are encoded as nanoseconds since the Unix epoch (fits in `uint64_t` until year 2554).
- The tick generator assigns an **event timestamp** (when the GBM formula ran); the broadcast thread assigns a **send timestamp** (when the message was written to the socket). Both are included in the message to enable end-to-end latency measurement.
