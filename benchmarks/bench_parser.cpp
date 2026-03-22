#include <cstdio>
#include <cstring>
#include <vector>
#include <memory>
#include <numeric>
#include "../include/client/parser.h"
#include "../include/protocol.h"

static std::vector<uint8_t> make_quote(uint32_t seq, uint16_t sym) {
    QuoteMsg msg{};
    msg.header.msg_type     = MSG_QUOTE;
    msg.header.seq_num      = seq;
    msg.header.timestamp_ns = now_ns();
    msg.header.symbol_id    = sym;
    msg.payload.bid_price   = 1000.0 + seq * 0.01;
    msg.payload.bid_qty     = 500;
    msg.payload.ask_price   = 1000.5 + seq * 0.01;
    msg.payload.ask_qty     = 300;
    set_checksum(&msg, sizeof(msg));
    return { reinterpret_cast<uint8_t*>(&msg),
             reinterpret_cast<uint8_t*>(&msg) + sizeof(msg) };
}

int main() {
    std::fprintf(stdout, "Parser Throughput Benchmark\n");
    std::fprintf(stdout, "============================\n\n");

    const int WARMUP = 10'000;
    const int N      = 1'000'000;

    // Build stream
    std::vector<uint8_t> stream;
    stream.reserve((WARMUP + N) * MSG_QUOTE_SIZE);
    for (int i = 0; i < WARMUP + N; ++i) {
        auto m = make_quote(static_cast<uint32_t>(i),
                            static_cast<uint16_t>(i % DEFAULT_SYMBOLS));
        stream.insert(stream.end(), m.begin(), m.end());
    }

    auto pp = std::make_unique<Parser>();
    Parser& p = *pp;
    uint64_t count = 0;
    p.set_callback([&](const ParsedMessage&) { ++count; });

    // Warmup
    p.feed(stream.data(), WARMUP * MSG_QUOTE_SIZE, mono_ns());
    p.process();
    count = 0;
    p.reset_stats();

    // Measure
    const uint8_t* start_ptr = stream.data() + WARMUP * MSG_QUOTE_SIZE;
    const size_t   total_bytes = N * MSG_QUOTE_SIZE;

    uint64_t t0 = mono_ns();
    p.feed(start_ptr, total_bytes, t0);
    p.process();
    uint64_t t1 = mono_ns();

    double elapsed_s  = (t1 - t0) / 1e9;
    double throughput  = count / elapsed_s;
    double mbps        = (total_bytes / elapsed_s) / (1024.0 * 1024.0);
    double ns_per_msg  = (t1 - t0) / static_cast<double>(count);

    std::fprintf(stdout, "Messages parsed:  %llu\n",  (unsigned long long)count);
    std::fprintf(stdout, "Elapsed:          %.3f ms\n", elapsed_s * 1000.0);
    std::fprintf(stdout, "Throughput:       %.0f msg/s\n", throughput);
    std::fprintf(stdout, "Bandwidth:        %.1f MB/s\n",  mbps);
    std::fprintf(stdout, "Per-message cost: %.1f ns\n",    ns_per_msg);
    std::fprintf(stdout, "Checksum errors:  %llu\n", (unsigned long long)p.checksum_errors());
    std::fprintf(stdout, "Sequence gaps:    %llu\n", (unsigned long long)p.sequence_gaps());

    if (throughput < 500'000) {
        std::fprintf(stderr, "WARN: Throughput below 500K msg/s target!\n");
        return 1;
    }
    return 0;
}
