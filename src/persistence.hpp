// persistence.hpp
#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// Snapshot persistence — saves and loads the entire ShardedCache to/from disk.
//
// File format  (plain text, human-readable)
// ──────────────────────────────────────────
//   Line 1:   REDISLITE-SNAPSHOT v1
//   Per entry: <key_len> <val_len> <ttl_ms>\n<key><value>
//
//   key_len  — byte length of key   (unsigned decimal)
//   val_len  — byte length of value (unsigned decimal)
//   ttl_ms   — remaining TTL in ms, or -1 for persistent
//
//   The key and value are written as raw bytes immediately after the header
//   line (no separator), so binary-safe values are supported even though
//   the header itself is ASCII.
//
// Design decisions
// ─────────────────
//   • Snapshotting only (no AOF). Simple, sufficient for a this project.
//   • Writes to a temp file first, then atomically renames to the target path
//     (atomic on POSIX; best-effort on Windows via MoveFileEx).
//   • On load, expired entries are silently skipped (their TTL elapsed while
//     the server was down).
//   • save() / load() are free functions — no state, easy to test.
// ─────────────────────────────────────────────────────────────────────────────

#include "cache.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

namespace persistence {

static constexpr const char* MAGIC   = "REDISLITE-SNAPSHOT";
static constexpr int         VERSION = 1;

// Save 

// Serialise `cache` to `path`. Throws std::runtime_error on I/O failure.
// Writes to a temp file then renames atomically so a crash mid-write
// never leaves a half-written snapshot.
inline void save(cache::ShardedCache& cache, const std::string& path) {
    std::string tmp = path + ".tmp";

    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("persistence::save: cannot open " + tmp);

    // Header
    out << MAGIC << " v" << VERSION << "\n";

    // Entries
    auto entries = cache.snapshot();
    for (auto& e : entries) {
        // Skip any entry whose TTL has already elapsed (ttlMs == 0)
        if (e.ttlMs == 0) continue;

        out << e.key.size()   << ' '
            << e.value.size() << ' '
            << e.ttlMs        << '\n'
            << e.key
            << e.value;
    }

    out.flush();
    if (!out) throw std::runtime_error("persistence::save: write error to " + tmp);
    out.close();

    // Atomic rename temp → final
#ifdef _WIN32
    // MoveFileEx with REPLACE_EXISTING is the closest Windows has to atomic rename
    if (!::MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        throw std::runtime_error("persistence::save: rename failed for " + path);
    }
#else
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        throw std::runtime_error("persistence::save: rename failed for " + path);
    }
#endif
}

// Load 

// Deserialise a snapshot file into `cache`. Throws std::runtime_error if the
// file is malformed. Returns the number of entries loaded.
// Silently skips entries whose TTL elapsed while the server was offline.
inline std::size_t load(cache::ShardedCache& cache, const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("persistence::load: cannot open " + path);

    // Read and validate header line
    std::string header;
    if (!std::getline(in, header)) {
        throw std::runtime_error("persistence::load: empty file " + path);
    }

    // Expected: "REDISLITE-SNAPSHOT v1"
    std::istringstream hss(header);
    std::string magic, ver;
    hss >> magic >> ver;
    if (magic != MAGIC || ver != "v" + std::to_string(VERSION)) {
        throw std::runtime_error(
            "persistence::load: invalid header in " + path);
    }

    std::size_t count = 0;
    std::string metaLine;

    while (std::getline(in, metaLine)) {
        if (metaLine.empty()) continue;

        // Parse: key_len val_len ttl_ms
        std::istringstream mss(metaLine);
        std::size_t keyLen = 0, valLen = 0;
        long long   ttlMs  = 0;
        if (!(mss >> keyLen >> valLen >> ttlMs)) {
            throw std::runtime_error(
                "persistence::load: malformed entry in " + path);
        }

        // Read key bytes
        std::string key(keyLen, '\0');
        if (!in.read(key.data(), static_cast<std::streamsize>(keyLen))) {
            throw std::runtime_error(
                "persistence::load: truncated key in " + path);
        }

        // Read value bytes
        std::string value(valLen, '\0');
        if (!in.read(value.data(), static_cast<std::streamsize>(valLen))) {
            throw std::runtime_error(
                "persistence::load: truncated value in " + path);
        }

        // Skip already-expired entries
        if (ttlMs == 0) continue;

        if (ttlMs > 0) {
            cache.setWithTTL(key, value,
                             std::chrono::milliseconds{ttlMs});
        } else {
            // ttlMs == -1 → persistent
            cache.set(key, value);
        }
        ++count;
    }

    return count;
}

}  // namespace persistence
