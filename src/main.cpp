// main.cpp — RedisLite server entry point
//
// Usage:
//   redis-lite-server [port] [snapshot-file] [capacity] [shards]
//
//   port           TCP port to listen on           (default: 6379)
//   snapshot-file  Path to snapshot file           (default: dump.rdb)
//   capacity       Total cache capacity (keys)     (default: 100000)
//   shards         Number of shards (power of 2)   (default: 16)
//
// Startup sequence:
//   1. Parse CLI args
//   2. Load snapshot if it exists
//   3. Start server (bind + listen + expiry worker)
//   4. Block until Ctrl-C / SIGINT / SIGTERM
//   5. Stop server, save snapshot, exit
//
// Shutdown is signal-driven: SIGINT (Ctrl-C) or SIGTERM sets a flag
// that the main thread polls, then triggers a clean save + stop.

#include "server.hpp"
#include "persistence.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

using namespace std::chrono_literals;

// Signal handling 

static std::atomic<bool> gShutdown{false};

static void onSignal(int) {
    gShutdown.store(true, std::memory_order_relaxed);
}

// Helpers 

static void printUsage(const char* prog) {
    std::cerr
        << "Usage: " << prog
        << " [port] [snapshot] [capacity] [shards]\n"
        << "\n"
        << "  port      - listen port          (default 6379)\n"
        << "  snapshot  - snapshot file path   (default dump.rdb)\n"
        << "  capacity  - max keys in cache    (default 100000)\n"
        << "  shards    - number of shards     (default 16, must be power of 2)\n";
}

// main

int main(int argc, char* argv[]) {
    // Parse arguments 
    uint16_t    port     = 6379;
    std::string snapFile = "dump.rdb";
    std::size_t capacity = 100'000;
    std::size_t shards   = 16;

    if (argc > 1) port     = static_cast<uint16_t>(std::atoi(argv[1]));
    if (argc > 2) snapFile = argv[2];
    if (argc > 3) capacity = static_cast<std::size_t>(std::atoi(argv[3]));
    if (argc > 4) shards   = static_cast<std::size_t>(std::atoi(argv[4]));

    if (port == 0) {
        std::cerr << "Invalid port.\n";
        printUsage(argv[0]);
        return 1;
    }

    // Register signal handlers 
    std::signal(SIGINT,  onSignal);
    std::signal(SIGTERM, onSignal);

    // Create server 
    server::Server srv(capacity, shards, /*expiryInterval=*/100ms);

    // Load snapshot 
    {
        std::ifstream check(snapFile);
        if (check.good()) {
            check.close();
            try {
                std::size_t n = persistence::load(srv.cache(), snapFile);
                std::cout << "[startup] loaded " << n
                          << " keys from " << snapFile << "\n";
            } catch (const std::exception& e) {
                std::cerr << "[startup] warning: could not load snapshot: "
                          << e.what() << "\n"
                          << "          starting with empty cache.\n";
            }
        } else {
            std::cout << "[startup] no snapshot found at " << snapFile
                      << " — starting with empty cache.\n";
        }
    }

    // Start server
    if (!srv.start(port)) {
        std::cerr << "[fatal] failed to bind on port " << port
                  << " — is another process using it?\n";
        return 1;
    }

    std::cout << "[ready] RedisLite listening on port " << port        << "\n"
              << "        capacity : " << capacity                      << " keys\n"
              << "        shards   : " << shards                        << "\n"
              << "        snapshot : " << snapFile                      << "\n"
              << "        press Ctrl-C to stop\n";

    // Wait for shutdown signal
    while (!gShutdown.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(100ms);
    }

    // Graceful shutdown 
    std::cout << "\n[shutdown] stopping server...\n";
    srv.stop();

    std::cout << "[shutdown] saving snapshot to " << snapFile << "...\n";
    try {
        persistence::save(srv.cache(), snapFile);
        std::cout << "[shutdown] snapshot saved ("
                  << srv.cache().size() << " keys).\n";
    } catch (const std::exception& e) {
        std::cerr << "[shutdown] warning: could not save snapshot: "
                  << e.what() << "\n";
    }

    std::cout << "[shutdown] done.\n";
    return 0;
}
