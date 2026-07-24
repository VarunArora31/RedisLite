# RedisLite
A concurrent, in-memory key-value store in C++ supporting TCP client connections, LRU eviction, and TTL-based expiration.

---

## Project Structure

```text
redis-lite/
├── src/
│   ├── cache.hpp/.cpp        # Core data structure + locking
│   ├── shard.hpp/.cpp        # Sharded map wrapper
│   ├── server.hpp/.cpp       # Socket accept loop, per-client handling
│   ├── protocol.hpp/.cpp     # Command parsing (RESP or custom)
│   ├── expiry.hpp/.cpp       # TTL sweep thread
│   ├── eviction.hpp/.cpp     # LRU logic
│   └── persistence.hpp/.cpp  # Snapshot/AOF
├── tests/
├── benchmarks/
└── client/
    └── cli.cpp