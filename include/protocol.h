#pragma once

#include <cstdint>
#include <cstring>
#include <ctime>

// ─────────────────────────────────────────────────────────────────────────────
// Wire constants
// ─────────────────────────────────────────────────────────────────────────────

static constexpr uint16_t MSG_TRADE     = 0x0001;
static constexpr uint16_t MSG_QUOTE     = 0x0002;
static constexpr uint16_t MSG_HEARTBEAT = 0x0003;
static constexpr uint8_t  MSG_SUBSCRIBE = 0xFF;

static constexpr uint16_t MAX_SYMBOLS   = 500;
static constexpr uint16_t DEFAULT_SYMBOLS = 100;
static constexpr uint16_t DEFAULT_PORT  = 9876;

// Message size constants (bytes)
static constexpr size_t HEADER_SIZE     = 16;
static constexpr size_t CHECKSUM_SIZE   = 4;
static constexpr size_t TRADE_PAYLOAD   = 12;   // price(8) + qty(4)
static constexpr size_t QUOTE_PAYLOAD   = 24;   // bid_price(8)+bid_qty(4)+ask_price(8)+ask_qty(4)
static constexpr size_t HEARTBEAT_PAYLOAD = 0;

static constexpr size_t MSG_TRADE_SIZE     = HEADER_SIZE + TRADE_PAYLOAD   + CHECKSUM_SIZE; // 32
static constexpr size_t MSG_QUOTE_SIZE     = HEADER_SIZE + QUOTE_PAYLOAD   + CHECKSUM_SIZE; // 44
static constexpr size_t MSG_HEARTBEAT_SIZE = HEADER_SIZE + HEARTBEAT_PAYLOAD + CHECKSUM_SIZE; // 20
static constexpr size_t MAX_MSG_SIZE       = MSG_QUOTE_SIZE; // 44

// ─────────────────────────────────────────────────────────────────────────────
// Message structures (wire layout, little-endian)
// ─────────────────────────────────────────────────────────────────────────────

#pragma pack(push, 1)

struct MsgHeader {
    uint16_t msg_type;
    uint32_t seq_num;
    uint64_t timestamp_ns;
    uint16_t symbol_id;
};
static_assert(sizeof(MsgHeader) == HEADER_SIZE, "Header size mismatch");

struct TradePayload {
    double   price;
    uint32_t quantity;
};
static_assert(sizeof(TradePayload) == TRADE_PAYLOAD, "Trade payload size mismatch");

struct QuotePayload {
    double   bid_price;
    uint32_t bid_qty;
    double   ask_price;
    uint32_t ask_qty;
};
static_assert(sizeof(QuotePayload) == QUOTE_PAYLOAD, "Quote payload size mismatch");

struct TradeMsg {
    MsgHeader    header;
    TradePayload payload;
    uint32_t     checksum;
};
static_assert(sizeof(TradeMsg) == MSG_TRADE_SIZE, "TradeMsg size mismatch");

struct QuoteMsg {
    MsgHeader    header;
    QuotePayload payload;
    uint32_t     checksum;
};
static_assert(sizeof(QuoteMsg) == MSG_QUOTE_SIZE, "QuoteMsg size mismatch");

struct HeartbeatMsg {
    MsgHeader header;
    uint32_t  checksum;
};
static_assert(sizeof(HeartbeatMsg) == MSG_HEARTBEAT_SIZE, "HeartbeatMsg size mismatch");

struct SubscribeMsg {
    uint8_t  command;       // 0xFF
    uint16_t count;
    // followed by count × uint16_t symbol_ids
};

#pragma pack(pop)

// ─────────────────────────────────────────────────────────────────────────────
// Checksum helpers
// ─────────────────────────────────────────────────────────────────────────────

inline uint32_t compute_checksum(const void* data, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint32_t xor_val = 0;
    for (size_t i = 0; i + 3 < len; i += 4) {
        uint32_t word;
        std::memcpy(&word, p + i, 4);
        xor_val ^= word;
    }
    // Handle remaining bytes
    size_t rem = len & 3;
    if (rem) {
        uint32_t word = 0;
        std::memcpy(&word, p + (len - rem), rem);
        xor_val ^= word;
    }
    return xor_val;
}

inline void set_checksum(void* msg, size_t msg_size) {
    uint32_t cs = compute_checksum(msg, msg_size - CHECKSUM_SIZE);
    std::memcpy(static_cast<uint8_t*>(msg) + msg_size - CHECKSUM_SIZE, &cs, 4);
}

inline bool verify_checksum(const void* msg, size_t msg_size) {
    uint32_t expected = compute_checksum(msg, msg_size - CHECKSUM_SIZE);
    uint32_t actual;
    std::memcpy(&actual,
                static_cast<const uint8_t*>(msg) + msg_size - CHECKSUM_SIZE, 4);
    return expected == actual;
}

// ─────────────────────────────────────────────────────────────────────────────
// Timestamp helper
// ─────────────────────────────────────────────────────────────────────────────

inline uint64_t now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
}

inline uint64_t mono_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
}

// ─────────────────────────────────────────────────────────────────────────────
// Payload size from message type
// ─────────────────────────────────────────────────────────────────────────────

inline int payload_size_for_type(uint16_t msg_type) {
    switch (msg_type) {
        case MSG_TRADE:     return static_cast<int>(TRADE_PAYLOAD);
        case MSG_QUOTE:     return static_cast<int>(QUOTE_PAYLOAD);
        case MSG_HEARTBEAT: return 0;
        default:            return -1;  // unknown
    }
}

inline size_t total_size_for_type(uint16_t msg_type) {
    switch (msg_type) {
        case MSG_TRADE:     return MSG_TRADE_SIZE;
        case MSG_QUOTE:     return MSG_QUOTE_SIZE;
        case MSG_HEARTBEAT: return MSG_HEARTBEAT_SIZE;
        default:            return 0;
    }
}
