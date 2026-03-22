#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <cstring>
#include <cinttypes>
#include <stdexcept>
#include <time.h>
#include "../../include/server/exchange_simulator.h"

static ExchangeSimulator* g_sim = nullptr;

static void signal_handler(int sig) {
    std::fprintf(stderr, "\n[server] Received signal %d — shutting down...\n", sig);
    if (g_sim) g_sim->stop();
}

static void print_usage(const char* prog) {
    std::fprintf(stderr,
        "Usage: %s [options]\n"
        "  -p PORT       Listen port (default: %u)\n"
        "  -s SYMBOLS    Number of symbols (default: %u, max: %u)\n"
        "  -r RATE       Tick rate messages/sec (default: 100000)\n"
        "  -f            Enable fault injection (sequence gaps)\n"
        "  -h            Show this help\n",
        prog, DEFAULT_PORT, DEFAULT_SYMBOLS, MAX_SYMBOLS);
}

int main(int argc, char* argv[]) {
    uint16_t port        = DEFAULT_PORT;
    uint32_t num_symbols = DEFAULT_SYMBOLS;
    uint32_t tick_rate   = 100'000;
    bool     fault_inj   = false;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            num_symbols = static_cast<uint32_t>(std::atoi(argv[++i]));
            if (num_symbols > MAX_SYMBOLS) num_symbols = MAX_SYMBOLS;
        } else if (std::strcmp(argv[i], "-r") == 0 && i + 1 < argc) {
            tick_rate = static_cast<uint32_t>(std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "-f") == 0) {
            fault_inj = true;
        } else if (std::strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        }
    }

    // Ignore SIGPIPE — handle in send() return values
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);

    try {
        ExchangeSimulator sim(port, num_symbols);
        g_sim = &sim;

        sim.set_tick_rate(tick_rate);
        sim.enable_fault_injection(fault_inj);
        sim.start();

        std::fprintf(stderr,
            "[server] Started — port=%u symbols=%u rate=%u/s fault=%s\n",
            port, num_symbols, tick_rate, fault_inj ? "ON" : "OFF");

        // Print stats every 10 seconds
        uint64_t last_ticks = 0;
        while (true) {
            struct timespec ts{ 10, 0 };
            nanosleep(&ts, nullptr);

            uint64_t ticks = sim.ticks_generated();
            uint64_t delta = ticks - last_ticks;
            last_ticks     = ticks;
            std::fprintf(stderr,
                "[server] ticks_total=%llu rate=%llu/s msgs_broadcast=%llu\n",
                (unsigned long long)ticks,
                (unsigned long long)(delta / 10),
                (unsigned long long)sim.msgs_broadcast());
        }

        sim.run();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[server] Fatal: %s\n", e.what());
        return 1;
    }

    return 0;
}
