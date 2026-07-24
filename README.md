# RedisLite

A multithreaded, in-memory cache server written in C++20 — built from scratch as a learning project. Supports TCP clients, LRU eviction, TTL expiry, sharded concurrency, snapshot persistence, and a CLI client.

---

## Architecture

The project is built in strict layers. Each layer depends only on the one below it, making every piece independently testable.

```
Client (TCP)
     │
     ▼
server.hpp          accept loop — one std::thread per connected client
     │
     ▼
protocol.hpp        parse raw bytes → Command{type, args}
                    format responses → RESP-compatible wire format
     │
     ▼
cache.hpp           ShardedCache — routes keys across N independent shards
     │                            using hash(key) & (N-1)
     ▼
shard.hpp           Shard — one bucket: std::shared_mutex + LRUCache + TTL map
     │
     ▼
eviction.hpp        LRUCache<K,V> — doubly-linked list + hashmap, O(1) all ops

expiry.hpp          ExpiryWorker — background std::thread, wakes every 100ms,
                    calls cache.purgeExpired() across all shards

persistence.hpp     save() / load() — atomic snapshot to disk (writes .tmp
                    then renames), skips expired entries on reload
```

### Why sharding matters

With a single lock, every `SET` from any client blocks every other `SET`/`GET` — even on completely unrelated keys. Sharding splits the keyspace into N independent maps, each with its own lock. A write to `user:1` and a write to `session:abc` can happen truly in parallel if they land in different shards. Throughput scales with shard count up to the number of CPU cores.

```
key → std::hash<string>(key) & (numShards - 1) → Shard[i]
```

### TTL — two strategies working together

Mirrors real Redis:

- **Lazy expiry** — checked on every `GET`. If the key is past its deadline, it is removed and `null` is returned. Free, but expired keys that are never accessed again stay in memory.
- **Active expiry** — `ExpiryWorker` wakes every 100ms and sweeps all shards for keys past their deadline. This is what actually frees memory over time.

### Lock design

| Operation | Lock type | Reason |
|---|---|---|
| `GET` | `unique_lock` | `LRUCache::get()` splices the list — it is a write |
| `SET` / `DEL` / `EXPIRE` | `unique_lock` | Mutates map and list |
| `EXISTS` / `TTL` / `size` | `shared_lock` | Reads only the TTL map, not LRU list |

---

## Project Structure

```
RedisLite/
├── src/
│   ├── eviction.hpp      LRUCache<K,V>     — generic, no locks, O(1) ops
│   ├── shard.hpp         Shard             — 1 thread-safe cache bucket + TTL
│   ├── cache.hpp         ShardedCache      — N shards, hash routing
│   ├── expiry.hpp        ExpiryWorker      — background TTL sweep thread
│   ├── protocol.hpp      parse / format    — RESP-compatible wire protocol
│   ├── server.hpp        Server            — TCP accept loop + command dispatch
│   ├── persistence.hpp   save / load       — atomic snapshot persistence
│   └── main.cpp          entry point       — wires everything together
│
├── client/
│   └── cli.cpp           redis-lite-cli    — interactive REPL client
│
├── tests/
│   ├── test_eviction.cpp     33 tests
│   ├── test_shard.cpp        42 tests
│   ├── test_cache.cpp        47 tests
│   ├── test_protocol.cpp     49 tests
│   ├── test_expiry.cpp       18 tests
│   ├── test_server.cpp       28 tests
│   ├── test_persistence.cpp  18 tests
│   └── test_client_helper.hpp
│
└── CMakeLists.txt
```

**Total: 235 tests, all passing.**

---

## Supported Commands

| Command | Description | Response |
|---|---|---|
| `PING` | Healthcheck | `+PONG` |
| `SET key value` | Set a persistent key | `+OK` |
| `SETEX key ms value` | Set key with TTL (milliseconds) | `+OK` |
| `GET key` | Get value | `$<n>\r\n<value>` or `$-1` (nil) |
| `DEL key` | Delete a key | `:1` (deleted) or `:0` (not found) |
| `EXISTS key` | Check if key exists | `:1` or `:0` |
| `EXPIRE key ms` | Set TTL on existing key (ms) | `:1` (set) or `:0` (not found) |
| `PERSIST key` | Remove TTL, make key permanent | `:1` or `:0` |
| `TTL key` | Remaining TTL in ms | `:<ms>`, `:-1` (persistent), `:-2` (missing) |
| `QUIT` | Close connection | `+OK` |

---

## Building

### Prerequisites

- GCC 16+ (or any C++20 compiler)
- On Windows: [MSYS2](https://www.msys2.org/) with the `ucrt64` toolchain

```powershell
# Install MSYS2 dependencies (one time)
C:\msys64\usr\bin\pacman.exe -S --noconfirm `
    mingw-w64-ucrt-x86_64-cmake `
    mingw-w64-ucrt-x86_64-ninja `
    mingw-w64-ucrt-x86_64-gtest
```

### Build the server and CLI

```powershell
$g = "C:\msys64\ucrt64\bin\g++.exe"

# Server
& $g -std=c++20 -O2 -I src src/main.cpp -lws2_32 -o redis-lite-server.exe

# CLI client
& $g -std=c++20 -O2 -I src client/cli.cpp -lws2_32 -o redis-lite-cli.exe
```

### Build and run all tests

```powershell
$g   = "C:\msys64\ucrt64\bin\g++.exe"
$inc = "C:\msys64\ucrt64\include"
$lib = "C:\msys64\ucrt64\lib"

$tests = @("test_eviction","test_shard","test_cache","test_protocol","test_expiry","test_persistence")
foreach ($t in $tests) {
    & $g -std=c++20 -I src -I tests -I $inc tests/$t.cpp -L $lib -lgtest -lgtest_main -lpthread -o $t.exe
    cmd /c "$t.exe --gtest_color=no"
}

# Server tests need ws2_32
& $g -std=c++20 -I src -I tests -I $inc tests/test_server.cpp -L $lib -lgtest -lgtest_main -lpthread -lws2_32 -o test_server.exe
cmd /c "test_server.exe --gtest_color=no"
```

### Build with CMake (optional)

```powershell
cmake -S . -B build -G Ninja `
      -DCMAKE_CXX_COMPILER=C:/msys64/ucrt64/bin/g++.exe `
      -DCMAKE_PREFIX_PATH=C:/msys64/ucrt64 `
      -DCMAKE_BUILD_TYPE=Release

cmake --build build
```

---

## Running

### Start the server

```powershell
# Default: port 6379, snapshot file dump.rdb, 100k key capacity, 16 shards
.\redis-lite-server.exe

# Custom: port 7000, custom snapshot, 50k capacity, 8 shards
.\redis-lite-server.exe 7000 mysnap.rdb 50000 8
```

Startup output:
```
[startup] no snapshot found at dump.rdb — starting with empty cache.
[ready] RedisLite listening on port 6379
        capacity : 100000 keys
        shards   : 16
        snapshot : dump.rdb
        press Ctrl-C to stop
```

On Ctrl-C:
```
[shutdown] stopping server...
[shutdown] saving snapshot to dump.rdb...
[shutdown] snapshot saved (42 keys).
[shutdown] done.
```

### Connect with the CLI

```powershell
# Default: 127.0.0.1:6379
.\redis-lite-cli.exe

# Custom host and port
.\redis-lite-cli.exe 127.0.0.1 7000
```

```
RedisLite CLI — connecting to 127.0.0.1:6379
Type commands and press Enter. QUIT to exit.

127.0.0.1:6379> SET user:1 Alice
+OK
127.0.0.1:6379> GET user:1
"Alice"
127.0.0.1:6379> SETEX session:abc 30000 token123
+OK
127.0.0.1:6379> TTL session:abc
:29997
127.0.0.1:6379> PERSIST session:abc
:1
127.0.0.1:6379> TTL session:abc
:-1
127.0.0.1:6379> DEL user:1
:1
127.0.0.1:6379> QUIT
+OK
```

### Connect with netcat or redis-cli

```bash
# netcat (Linux/WSL)
nc 127.0.0.1 6379

# redis-cli (points at your server, not Redis)
redis-cli -p 6379 ping
redis-cli -p 6379 set foo bar
redis-cli -p 6379 get foo
```

---

## Persistence

On startup the server loads `dump.rdb` if it exists. On shutdown (Ctrl-C) it saves a snapshot before exiting. The snapshot file is plain text and human-readable:

```
REDISLITE-SNAPSHOT v1
5 5 -1
helloworld
3 3 29500
foobaz
```

Each entry: `<key_len> <val_len> <ttl_ms_remaining>` followed immediately by the raw key and value bytes. TTL `-1` means persistent. Entries with `0` remaining TTL are skipped on save and load.

---

## Benchmark Results

Measured with `redis-benchmark` against RedisLite (16 shards, `-O2`) and real Redis 3.0 on the same machine. Numbers are requests/sec.

### 10 concurrent clients — 100k requests

| Command | RedisLite | Redis 3.0 | Ratio |
|---|---:|---:|---:|
| PING | 70,000 | 30,000 | **2.3×** |
| SET  | 77,000 | 29,000 | **2.7×** |
| GET  | 75,000 | 29,900 | **2.5×** |

### 1 client — baseline latency

| Command | RedisLite | Redis 3.0 | Ratio |
|---|---:|---:|---:|
| PING | 26,500 | 26,700 | **~1.0×** |
| SET  | 26,400 | 26,100 | **~1.0×** |
| GET  | 25,400 | 25,000 | **~1.0×** |

**What the numbers show:** with a single client there is no concurrency, so sharding provides no advantage — both servers score identically (~26k req/s), limited purely by localhost TCP round-trip latency. Under 10–50 concurrent clients, RedisLite's 16 independent shards allow parallel execution across CPU cores while Redis 3.0's single-threaded event loop serialises everything. That gap — ~1.0× at 1 client, ~2.5× at 10+ clients — is the sharding advantage measured directly.

See [`benchmarks/RESULTS.md`](benchmarks/RESULTS.md) for full tables and detailed analysis.

---

## Design Decisions

**Why header-only?**
All source is in `.hpp` files. For a project of this size it avoids a separate compilation model and keeps each layer self-contained and easy to read.

**Why `std::shared_mutex` instead of `std::mutex`?**
`exists()`, `ttl()`, and `size()` are read-only operations that don't touch the LRU list. A shared lock lets multiple clients query these simultaneously without blocking each other. The upgrade to unique lock happens only for mutations.

**Why LRU and not LFU?**
LRU is simpler to implement correctly (O(1) via list + hashmap) and performs better on workloads with temporal locality — which is the common case for a cache. LFU is better under Zipf-distributed access patterns. Adding LFU as an alternative eviction policy using the same `LRUCache` interface is a planned extension.

**Why thread-per-connection and not epoll?**
Thread-per-connection is simple to reason about and sufficient for hundreds of concurrent clients on modern hardware. An `epoll`-based event loop would scale to tens of thousands but adds significant complexity. The architecture is designed so `server.hpp` is the only place networking lives — swapping in an event loop later would not touch any other layer.
