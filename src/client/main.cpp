#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <cstring>
#include <vector>
#include <stdexcept>
#include "../../include/client/feed_handler.h"

static FeedHandler* g_handler = nullptr;

static void signal_handler(int sig) {
    std::fprintf(stderr, "\n[client] Received signal %d — shutting down...\n", sig);
    if (g_handler) g_handler->stop();
}

static void print_usage(const char* prog) {
    std::fprintf(stderr,
        "Usage: %s [options]\n"
        "  -h HOST       Server host (default: 127.0.0.1)\n"
        "  -p PORT       Server port (default: %u)\n"
        "  -s SYMBOLS    Number of symbols in cache (default: %u)\n"
        "  -S SYM_IDS    Comma-separated symbol IDs to subscribe (e.g. 0,1,2)\n"
        "  -e FILE       Export latency histogram to CSV on exit\n"
        "  --help        Show this help\n",
        prog, DEFAULT_PORT, DEFAULT_SYMBOLS);
}

int main(int argc, char* argv[]) {
    std::string host         = "127.0.0.1";
    uint16_t    port         = DEFAULT_PORT;
    uint16_t    num_symbols  = DEFAULT_SYMBOLS;
    std::string csv_export;
    std::vector<uint16_t> subscribe_ids;

    for (int i = 1; i < argc; ++i) {
        if ((std::strcmp(argv[i], "-h") == 0) && i + 1 < argc) {
            host = argv[++i];
        } else if (std::strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            num_symbols = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "-S") == 0 && i + 1 < argc) {
            // Parse comma-separated symbol IDs
            char* tok = std::strtok(argv[++i], ",");
            while (tok) {
                subscribe_ids.push_back(static_cast<uint16_t>(std::atoi(tok)));
                tok = std::strtok(nullptr, ",");
            }
        } else if (std::strcmp(argv[i], "-e") == 0 && i + 1 < argc) {
            csv_export = argv[++i];
        } else if (std::strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
    }

    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);

    try {
        FeedHandler handler(host, port, num_symbols);
        g_handler = &handler;

        if (!subscribe_ids.empty()) {
            handler.set_symbols(subscribe_ids);
        }

        handler.start();
        handler.run();  // blocks until 'q' or signal

        if (!csv_export.empty()) {
            if (handler.tracker().export_csv(csv_export)) {
                std::fprintf(stderr, "[client] Latency histogram saved to %s\n",
                             csv_export.c_str());
            }
        }

    } catch (const std::exception& e) {
        std::fprintf(stderr, "[client] Fatal: %s\n", e.what());
        return 1;
    }

    return 0;
}
