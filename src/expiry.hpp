// expiry.hpp
#pragma once

#include "cache.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace cache {

// ─────────────────────────────────────────────────────────────────────────────
// ExpiryWorker — background thread that periodically sweeps expired keys.
//
// Design
// ──────
//   Real Redis uses two expiry strategies simultaneously:
//     1. Lazy expiry   — check on access (done inside Shard::get())
//     2. Active expiry — periodic sweep (this class)
//
//   ExpiryWorker owns a std::thread that wakes every `intervalMs`
//   milliseconds and calls cache.purgeExpired() across all shards.
//   Between sweeps it waits on a condition_variable so it can be
//   woken early for a clean shutdown.
//
// Lifecycle
// ─────────
//   start()  — spawns the worker thread (idempotent: no-op if already running)
//   stop()   — signals the thread to exit and joins it (blocks until done)
//   Destructor calls stop() automatically (RAII).
//
// Thread-safety
// ─────────────
//   start()/stop() must not be called concurrently with each other.
//   The worker thread only calls cache_.purgeExpired(), which is itself
//   fully thread-safe.
// ─────────────────────────────────────────────────────────────────────────────

class ExpiryWorker {
public:
    using Millis = std::chrono::milliseconds;

    // cache     — the ShardedCache to sweep
    // intervalMs— how often to run a sweep (default 100 ms)
    explicit ExpiryWorker(ShardedCache& cache,
                          Millis interval = Millis{100})
        : cache_(cache)
        , interval_(interval)
    {
        if (interval_.count() <= 0) {
            throw std::invalid_argument(
                "ExpiryWorker: interval must be > 0 ms");
        }
    }

    // Non-copyable, non-movable.
    ExpiryWorker(const ExpiryWorker&)            = delete;
    ExpiryWorker& operator=(const ExpiryWorker&) = delete;
    ExpiryWorker(ExpiryWorker&&)                 = delete;
    ExpiryWorker& operator=(ExpiryWorker&&)      = delete;

    // RAII: stop the worker thread on destruction.
    ~ExpiryWorker() {
        stop();
    }

    // ── Control ───────────────────────────────────────────────────────────────

    // Spawn the background thread. No-op if already running.
    void start() {
        if (running_.load(std::memory_order_acquire)) return;
        running_.store(true, std::memory_order_release);
        thread_ = std::thread(&ExpiryWorker::loop, this);
    }

    // Signal the thread to stop and block until it has exited.
    // No-op if not running.
    void stop() {
        if (!running_.load(std::memory_order_acquire)) return;
        {
            std::lock_guard lock(mutex_);
            running_.store(false, std::memory_order_release);
        }
        cv_.notify_all();   // wake the sleeping thread immediately
        if (thread_.joinable()) thread_.join();
    }

    // ── Observers ────────────────────────────────────────────────────────────

    bool isRunning() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

    Millis interval() const noexcept { return interval_; }

    // Total keys purged since start() (useful for tests / metrics).
    std::size_t totalPurged() const noexcept {
        return totalPurged_.load(std::memory_order_relaxed);
    }

private:
    void loop() {
        while (running_.load(std::memory_order_acquire)) {
            // Sleep for `interval_`, but wake early if stop() is called.
            std::unique_lock lock(mutex_);
            cv_.wait_for(lock, interval_,
                [this] { return !running_.load(std::memory_order_acquire); });

            if (!running_.load(std::memory_order_acquire)) break;

            // Release lock before calling purgeExpired — no need to hold it
            // during the sweep (purgeExpired is internally thread-safe).
            lock.unlock();
            std::size_t purged = cache_.purgeExpired();
            totalPurged_.fetch_add(purged, std::memory_order_relaxed);
        }
    }

    // ── State ─────────────────────────────────────────────────────────────────
    ShardedCache&           cache_;
    const Millis            interval_;
    std::atomic<bool>       running_{false};
    std::atomic<std::size_t>totalPurged_{0};
    std::thread             thread_;
    std::mutex              mutex_;
    std::condition_variable cv_;
};

}  // namespace cache
