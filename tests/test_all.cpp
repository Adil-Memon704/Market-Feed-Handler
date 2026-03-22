#include <gtest/gtest.h>
#include "protocol.h"
#include "common/ring_buffer.h"
#include "common/cache.h"
#include "common/latency_tracker.h"
#include "client/parser.h"
#include <cstring>
#include <thread>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// Protocol tests
// ─────────────────────────────────────────────────────────────────────────────

TEST(Protocol, MessageSizes) {
    EXPECT_EQ(sizeof(MsgHeader),     16u);
    EXPECT_EQ(sizeof(TradePayload),  12u);
    EXPECT_EQ(sizeof(QuotePayload),  24u);
    EXPECT_EQ(sizeof(TradeMsg),      32u);
    EXPECT_EQ(sizeof(QuoteMsg),      44u);
    EXPECT_EQ(sizeof(HeartbeatMsg),  20u);
}

TEST(Protocol, ChecksumRoundTrip) {
    TradeMsg msg{};
    msg.header.msg_type     = MSG_TRADE;
    msg.header.seq_num      = 42;
    msg.header.timestamp_ns = now_ns();
    msg.header.symbol_id    = 7;
    msg.payload.price       = 1234.56;
    msg.payload.quantity    = 100;
    set_checksum(&msg, sizeof(msg));
    EXPECT_TRUE(verify_checksum(&msg, sizeof(msg)));

    // Corrupt one byte
    reinterpret_cast<uint8_t*>(&msg)[5] ^= 0xFF;
    EXPECT_FALSE(verify_checksum(&msg, sizeof(msg)));
}

TEST(Protocol, PayloadSizeFromType) {
    EXPECT_EQ(payload_size_for_type(MSG_TRADE),      12);
    EXPECT_EQ(payload_size_for_type(MSG_QUOTE),      24);
    EXPECT_EQ(payload_size_for_type(MSG_HEARTBEAT),  0);
    EXPECT_EQ(payload_size_for_type(0xDEAD),         -1);
}

// ─────────────────────────────────────────────────────────────────────────────
// ByteRingBuffer tests
// ─────────────────────────────────────────────────────────────────────────────

TEST(RingBuffer, BasicWriteRead) {
    ByteRingBuffer<256> buf;
    EXPECT_EQ(buf.readable(), 0u);
    EXPECT_EQ(buf.writable(), 256u);

    const char* data = "HelloWorld";
    size_t n = buf.write(data, 10);
    EXPECT_EQ(n, 10u);
    EXPECT_EQ(buf.readable(), 10u);

    char out[16] = {};
    size_t r = buf.read(out, 10);
    EXPECT_EQ(r, 10u);
    EXPECT_EQ(std::string(out, 10), "HelloWorld");
    EXPECT_EQ(buf.readable(), 0u);
}

TEST(RingBuffer, WrapAround) {
    ByteRingBuffer<16> buf;
    // Fill 12 bytes
    uint8_t fill[12];
    std::memset(fill, 0xAA, 12);
    EXPECT_EQ(buf.write(fill, 12), 12u);

    // Consume 10 bytes — write ptr at 12, read ptr at 10
    uint8_t discard[10];
    buf.read(discard, 10);

    // Write 8 bytes — will wrap around the 16-byte boundary
    uint8_t pattern[8];
    for (int i = 0; i < 8; ++i) pattern[i] = static_cast<uint8_t>(i);
    size_t written = buf.write(pattern, 8);
    EXPECT_EQ(written, 8u);

    // Skip the 2 remaining 0xAA bytes
    uint8_t skip[2];
    buf.read(skip, 2);

    // Read back the 8-byte pattern
    uint8_t result[8];
    size_t  got = buf.read(result, 8);
    EXPECT_EQ(got, 8u);
    for (int i = 0; i < 8; ++i) {
        EXPECT_EQ(result[i], static_cast<uint8_t>(i));
    }
}

TEST(RingBuffer, PartialWrite) {
    ByteRingBuffer<16> buf;
    uint8_t big[20];
    std::memset(big, 1, 20);
    size_t written = buf.write(big, 20);
    EXPECT_EQ(written, 16u);  // Only 16 fit
}

TEST(SPSCQueue, PushPop) {
    SPSCQueue<int, 8> q;
    EXPECT_TRUE(q.empty());

    for (int i = 0; i < 7; ++i) EXPECT_TRUE(q.push(i));
    EXPECT_FALSE(q.push(99));  // Full (capacity - 1 usable)

    for (int i = 0; i < 7; ++i) {
        int v;
        EXPECT_TRUE(q.pop(v));
        EXPECT_EQ(v, i);
    }
    int v;
    EXPECT_FALSE(q.pop(v));  // Empty
}

// ─────────────────────────────────────────────────────────────────────────────
// SymbolCache tests
// ─────────────────────────────────────────────────────────────────────────────

TEST(SymbolCache, QuoteUpdate) {
    SymbolCache cache(10);
    cache.update_quote(0, 100.0, 1000, 100.5, 500, now_ns());
    auto s = cache.get_snapshot(0);
    EXPECT_DOUBLE_EQ(s.best_bid, 100.0);
    EXPECT_DOUBLE_EQ(s.best_ask, 100.5);
    EXPECT_EQ(s.bid_quantity, 1000u);
    EXPECT_EQ(s.ask_quantity, 500u);
    EXPECT_EQ(s.update_count, 1u);
}

TEST(SymbolCache, TradeUpdate) {
    SymbolCache cache(10);
    cache.update_trade(5, 2450.75, 200, now_ns());
    auto s = cache.get_snapshot(5);
    EXPECT_DOUBLE_EQ(s.last_traded_price,    2450.75);
    EXPECT_EQ(s.last_traded_quantity, 200u);
    EXPECT_EQ(s.update_count, 1u);
}

TEST(SymbolCache, OpenPriceSetOnFirstUpdate) {
    SymbolCache cache(10);
    cache.update_trade(0, 1500.0, 100, now_ns());
    auto s = cache.get_snapshot(0);
    EXPECT_DOUBLE_EQ(s.open_price, 1500.0);
    // Second update should not change open_price
    cache.update_trade(0, 1600.0, 50, now_ns());
    s = cache.get_snapshot(0);
    EXPECT_DOUBLE_EQ(s.open_price, 1500.0);
}

TEST(SymbolCache, OutOfRangeIgnored) {
    SymbolCache cache(10);
    cache.update_quote(99, 1.0, 1, 2.0, 1, now_ns());  // symbol 99 out of range
    // Should not crash; snapshot returns empty state
    auto s = cache.get_snapshot(9);
    EXPECT_DOUBLE_EQ(s.best_bid, 0.0);
}

TEST(SymbolCache, ConcurrentReadWrite) {
    SymbolCache cache(1);
    std::atomic<bool> stop{false};
    std::atomic<int>  reads{0};
    int               writes = 0;

    // Writer thread
    std::thread writer([&] {
        for (int i = 0; i < 100'000; ++i) {
            cache.update_quote(0,
                static_cast<double>(i),     1000,
                static_cast<double>(i) + 1, 500,
                now_ns());
            ++writes;
        }
        stop.store(true);
    });

    // Reader thread — checks for consistency
    std::thread reader([&] {
        while (!stop.load()) {
            auto s = cache.get_snapshot(0);
            // bid must be <= ask
            if (s.best_bid > 0 && s.best_ask > 0) {
                EXPECT_LE(s.best_bid, s.best_ask);
            }
            ++reads;
        }
    });

    writer.join();
    reader.join();

    EXPECT_EQ(writes, 100'000);
    EXPECT_GT(reads.load(), 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// LatencyTracker tests
// ─────────────────────────────────────────────────────────────────────────────

TEST(LatencyTracker, BasicRecord) {
    LatencyTracker t;
    t.record(100);
    t.record(200);
    t.record(300);

    auto s = t.get_stats();
    EXPECT_EQ(s.sample_count, 3u);
    EXPECT_EQ(s.min_ns, 100u);
    EXPECT_EQ(s.max_ns, 300u);
    EXPECT_EQ(s.mean_ns, 200u);
}

TEST(LatencyTracker, Percentiles) {
    LatencyTracker t;
    // Insert 1000 samples: 0, 1, 2, ... 999 ns
    for (uint64_t i = 0; i < 1000; ++i) t.record(i);

    auto s = t.get_stats();
    EXPECT_EQ(s.sample_count, 1000u);

    // p50 should be around 500 ns
    EXPECT_GE(s.p50_ns, 490u);
    EXPECT_LE(s.p50_ns, 510u);

    // p99 should be around 990 ns
    EXPECT_GE(s.p99_ns, 980u);
    EXPECT_LE(s.p99_ns, 1000u);
}

TEST(LatencyTracker, Reset) {
    LatencyTracker t;
    t.record(1000);
    t.reset();
    auto s = t.get_stats();
    EXPECT_EQ(s.sample_count, 0u);
}

TEST(LatencyTracker, ConcurrentRecord) {
    LatencyTracker t;
    const int N_THREADS = 4;
    const int N_SAMPLES = 10'000;

    std::vector<std::thread> threads;
    threads.reserve(N_THREADS);
    for (int i = 0; i < N_THREADS; ++i) {
        threads.emplace_back([&] {
            for (int j = 0; j < N_SAMPLES; ++j) t.record(j * 100);
        });
    }
    for (auto& th : threads) th.join();

    auto s = t.get_stats();
    EXPECT_EQ(s.sample_count, static_cast<uint64_t>(N_THREADS * N_SAMPLES));
}

// ─────────────────────────────────────────────────────────────────────────────
// Parser tests
// ─────────────────────────────────────────────────────────────────────────────

// Helper: build a valid QuoteMsg buffer
static std::vector<uint8_t> make_quote(uint32_t seq, uint16_t sym,
                                        double bid, double ask) {
    QuoteMsg msg{};
    msg.header.msg_type     = MSG_QUOTE;
    msg.header.seq_num      = seq;
    msg.header.timestamp_ns = now_ns();
    msg.header.symbol_id    = sym;
    msg.payload.bid_price   = bid;
    msg.payload.bid_qty     = 1000;
    msg.payload.ask_price   = ask;
    msg.payload.ask_qty     = 500;
    set_checksum(&msg, sizeof(msg));
    return { reinterpret_cast<uint8_t*>(&msg),
             reinterpret_cast<uint8_t*>(&msg) + sizeof(msg) };
}

static std::vector<uint8_t> make_trade(uint32_t seq, uint16_t sym, double price) {
    TradeMsg msg{};
    msg.header.msg_type     = MSG_TRADE;
    msg.header.seq_num      = seq;
    msg.header.timestamp_ns = now_ns();
    msg.header.symbol_id    = sym;
    msg.payload.price       = price;
    msg.payload.quantity    = 200;
    set_checksum(&msg, sizeof(msg));
    return { reinterpret_cast<uint8_t*>(&msg),
             reinterpret_cast<uint8_t*>(&msg) + sizeof(msg) };
}

TEST(Parser, SingleQuoteMessage) {
    Parser p;
    int    count = 0;
    p.set_callback([&](const ParsedMessage& pm) {
        EXPECT_EQ(pm.msg_type,  MSG_QUOTE);
        EXPECT_EQ(pm.seq_num,   1u);
        EXPECT_EQ(pm.symbol_id, 3u);
        EXPECT_DOUBLE_EQ(pm.quote.bid_price, 100.0);
        EXPECT_DOUBLE_EQ(pm.quote.ask_price, 100.5);
        ++count;
    });

    auto buf = make_quote(1, 3, 100.0, 100.5);
    p.feed(buf.data(), buf.size(), mono_ns());
    p.process();
    EXPECT_EQ(count, 1);
    EXPECT_EQ(p.msgs_parsed(), 1u);
}

TEST(Parser, MultipleMessagesOneFeed) {
    Parser p;
    int count = 0;
    p.set_callback([&](const ParsedMessage&) { ++count; });

    // Concatenate 10 messages in one feed call
    std::vector<uint8_t> combined;
    for (uint32_t i = 0; i < 10; ++i) {
        auto m = make_quote(i, 0, 100.0 + i, 100.5 + i);
        combined.insert(combined.end(), m.begin(), m.end());
    }
    p.feed(combined.data(), combined.size(), mono_ns());
    p.process();
    EXPECT_EQ(count, 10);
}

TEST(Parser, FragmentedMessage) {
    Parser p;
    int count = 0;
    p.set_callback([&](const ParsedMessage&) { ++count; });

    auto buf = make_quote(1, 0, 100.0, 100.5);

    // Feed 1 byte at a time
    for (size_t i = 0; i < buf.size(); ++i) {
        p.feed(buf.data() + i, 1, mono_ns());
        p.process();
    }
    EXPECT_EQ(count, 1);
}

TEST(Parser, CorruptChecksumDiscarded) {
    Parser p;
    int count = 0;
    p.set_callback([&](const ParsedMessage&) { ++count; });

    auto buf = make_quote(1, 0, 100.0, 100.5);
    buf[5] ^= 0xFF;  // Corrupt a byte in header

    p.feed(buf.data(), buf.size(), mono_ns());
    p.process();

    // The checksum error causes a resync attempt — message may be discarded
    EXPECT_EQ(p.checksum_errors(), 1u);
    EXPECT_EQ(count, 0);
}

TEST(Parser, SequenceGapDetected) {
    Parser p;
    p.set_callback([](const ParsedMessage&) {});

    auto m1 = make_quote(0, 0, 100.0, 100.5);
    auto m2 = make_quote(5, 0, 101.0, 101.5);  // Gap: seq 1-4 missing

    p.feed(m1.data(), m1.size(), mono_ns());
    p.process();
    p.feed(m2.data(), m2.size(), mono_ns());
    p.process();

    EXPECT_EQ(p.sequence_gaps(), 1u);
    EXPECT_EQ(p.total_gap_size(), 4u);  // expected 1, got 5 → gap of 4
}

TEST(Parser, TradeMessage) {
    Parser p;
    double recv_price = 0;
    p.set_callback([&](const ParsedMessage& pm) {
        EXPECT_EQ(pm.msg_type, MSG_TRADE);
        recv_price = pm.trade.price;
    });

    auto buf = make_trade(1, 7, 3678.50);
    p.feed(buf.data(), buf.size(), mono_ns());
    p.process();
    EXPECT_DOUBLE_EQ(recv_price, 3678.50);
}

TEST(Parser, HighThroughput) {
    Parser p;
    std::atomic<uint64_t> count{0};
    p.set_callback([&](const ParsedMessage&) { ++count; });

    const int N = 100'000;
    std::vector<uint8_t> stream;
    stream.reserve(N * MSG_QUOTE_SIZE);

    for (int i = 0; i < N; ++i) {
        auto m = make_quote(static_cast<uint32_t>(i), i % MAX_SYMBOLS,
                            100.0 + i * 0.01, 100.5 + i * 0.01);
        stream.insert(stream.end(), m.begin(), m.end());
    }

    auto t0 = mono_ns();
    p.feed(stream.data(), stream.size(), t0);
    p.process();
    auto t1 = mono_ns();

    EXPECT_EQ(count.load(), static_cast<uint64_t>(N));
    double elapsed_s = (t1 - t0) / 1e9;
    double throughput = N / elapsed_s;
    std::fprintf(stderr, "  Parser throughput: %.0f msgs/s (%.1f ms for %d msgs)\n",
                 throughput, elapsed_s * 1000, N);
    EXPECT_GT(throughput, 500'000.0);  // Must sustain > 500K msg/s
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
