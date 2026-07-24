// server.hpp
#pragma once

#include "cache.hpp"
#include "expiry.hpp"
#include "protocol.hpp"

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// ── Platform socket abstraction ───────────────────────────────────────────────
#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
   using SocketFd = SOCKET;
   static constexpr SocketFd INVALID_SOCKET_FD = INVALID_SOCKET;
   inline void closeSocket(SocketFd fd) { ::closesocket(fd); }
   inline bool socketValid(SocketFd fd) { return fd != INVALID_SOCKET; }
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
   using SocketFd = int;
   static constexpr SocketFd INVALID_SOCKET_FD = -1;
   inline void closeSocket(SocketFd fd) { ::close(fd); }
   inline bool socketValid(SocketFd fd) { return fd >= 0; }
#endif

namespace server {

// ─────────────────────────────────────────────────────────────────────────────
// Server — TCP server, one thread per connection.
//
// Architecture
// ────────────
//   acceptLoop thread  — blocks on accept(), spawns a client thread per conn.
//   client thread      — reads lines → parse → dispatch → send response.
//   ExpiryWorker       — background sweep of expired cache keys.
//
// Lifecycle
// ─────────
//   start(port) → binds, listens, starts accept loop + expiry worker.
//   stop()      → closes listen socket (unblocks accept), joins threads.
//   Destructor calls stop() automatically (RAII).
//
// Supported commands
// ──────────────────
//   PING                   → +PONG
//   QUIT                   → +OK  (closes connection)
//   SET    key val         → +OK
//   SETEX  key ms val      → +OK
//   GET    key             → $<n>\r\n<val>\r\n  |  $-1
//   DEL    key             → :1 | :0
//   EXISTS key             → :1 | :0
//   EXPIRE key ms          → :1 | :0
//   PERSIST key            → :1 | :0
//   TTL    key             → :<ms>  (-1 persistent, -2 missing/expired)
// ─────────────────────────────────────────────────────────────────────────────

class Server {
public:
    using Millis = std::chrono::milliseconds;

    explicit Server(std::size_t cacheCapacity  = 100'000,
                    std::size_t numShards      = 16,
                    Millis      expiryInterval = Millis{100})
        : cache_(cacheCapacity, numShards)
        , expiry_(cache_, expiryInterval)
    {
#ifdef _WIN32
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    }

    Server(const Server&)            = delete;
    Server& operator=(const Server&) = delete;
    Server(Server&&)                 = delete;
    Server& operator=(Server&&)      = delete;

    ~Server() {
        stop();
#ifdef _WIN32
        WSACleanup();
#endif
    }

    // ── Control ───────────────────────────────────────────────────────────────

    // Bind to `port` and start accepting connections.
    // Returns true on success, false if already running or bind fails.
    bool start(uint16_t port) {
        if (running_.load()) return false;

        listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (!socketValid(listenFd_)) return false;

        int opt = 1;
#ifdef _WIN32
        ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR,
                     reinterpret_cast<const char*>(&opt), sizeof(opt));
#else
        ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port        = htons(port);

        if (::bind(listenFd_,
                   reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            closeSocket(listenFd_);
            listenFd_ = INVALID_SOCKET_FD;
            return false;
        }
        if (::listen(listenFd_, 128) != 0) {
            closeSocket(listenFd_);
            listenFd_ = INVALID_SOCKET_FD;
            return false;
        }

        running_.store(true);
        expiry_.start();
        acceptThread_ = std::thread(&Server::acceptLoop, this);
        return true;
    }

    // Shut down: close listen socket, join all threads.
    void stop() {
        if (!running_.exchange(false)) return;

        if (socketValid(listenFd_)) {
            closeSocket(listenFd_);
            listenFd_ = INVALID_SOCKET_FD;
        }

        if (acceptThread_.joinable()) acceptThread_.join();

        // Collect and join all client threads
        std::vector<std::thread> toJoin;
        {
            std::lock_guard lock(clientsMutex_);
            toJoin = std::move(clientThreads_);
            clientThreads_.clear();
        }
        for (auto& t : toJoin) {
            if (t.joinable()) t.join();
        }

        expiry_.stop();
    }

    bool isRunning() const noexcept { return running_.load(); }

    // Direct cache access — useful for integration tests.
    cache::ShardedCache& cache() noexcept { return cache_; }

    // Returns the actual bound port. Call after start().
    // Useful when port 0 was passed (OS assigns an ephemeral port).
    uint16_t boundPort() const noexcept { return boundPort_; }

private:
    // ── Accept loop (runs in acceptThread_) ───────────────────────────────────

    void acceptLoop() {
        while (running_.load()) {
            sockaddr_in clientAddr{};
#ifdef _WIN32
            int addrLen = sizeof(clientAddr);
#else
            socklen_t addrLen = sizeof(clientAddr);
#endif
            SocketFd cfd = ::accept(listenFd_,
                                    reinterpret_cast<sockaddr*>(&clientAddr),
                                    &addrLen);
            if (!socketValid(cfd)) break; // listenFd_ was closed by stop()

            // Reap finished threads before adding new one (bounded growth)
            {
                std::lock_guard lock(clientsMutex_);
                // Move finished threads out, keep only joinable ones
                std::vector<std::thread> live;
                for (auto& t : clientThreads_) {
                    if (t.joinable()) live.push_back(std::move(t));
                }
                clientThreads_ = std::move(live);
                clientThreads_.emplace_back(&Server::handleClient, this, cfd);
            }
        }
    }

    // ── RESP array parser ─────────────────────────────────────────────────────
    // Converts a RESP array (*<n>\r\n$<l>\r\n<arg>\r\n...) into an inline
    // command string ("CMD arg1 arg2\r\n") that protocol::parse() understands.
    // Returns empty string if the buffer doesn't yet have a full array.
    // Advances `buf` past the consumed bytes on success.
    static std::string tryParseRESPArray(std::string& buf) {
        if (buf.empty() || buf[0] != '*') return "";

        std::size_t pos = buf.find("\r\n");
        if (pos == std::string::npos) return "";

        int argc = std::stoi(buf.substr(1, pos - 1));
        if (argc <= 0) return "";

        std::size_t cur = pos + 2;
        std::vector<std::string> args;
        args.reserve(static_cast<std::size_t>(argc));

        for (int i = 0; i < argc; ++i) {
            // Expect $<len>\r\n
            if (cur >= buf.size() || buf[cur] != '$') return "";
            std::size_t eol = buf.find("\r\n", cur);
            if (eol == std::string::npos) return "";
            int len = std::stoi(buf.substr(cur + 1, eol - cur - 1));
            cur = eol + 2;

            // Expect <len> bytes + \r\n
            if (cur + static_cast<std::size_t>(len) + 2 > buf.size()) return "";
            args.push_back(buf.substr(cur, static_cast<std::size_t>(len)));
            cur += static_cast<std::size_t>(len) + 2;
        }

        // Reassemble as inline command
        std::string inline_cmd;
        for (std::size_t i = 0; i < args.size(); ++i) {
            if (i > 0) inline_cmd += ' ';
            inline_cmd += args[i];
        }
        inline_cmd += "\r\n";

        buf.erase(0, cur);   // consume the RESP array from buffer
        return inline_cmd;
    }

    // ── Client handler (one per connected client) ─────────────────────────────

    void handleClient(SocketFd fd) {
        std::string buf;
        char tmp[4096];

        while (running_.load()) {
#ifdef _WIN32
            int n = ::recv(fd, tmp, static_cast<int>(sizeof(tmp) - 1), 0);
#else
            ssize_t n = ::recv(fd, tmp, sizeof(tmp) - 1, 0);
#endif
            if (n <= 0) break;

            tmp[n] = '\0';
            buf += tmp;

            // Process all complete commands in buffer.
            // Supports two framing modes:
            //   RESP array  (*<n>\r\n...)  — used by redis-cli / redis-benchmark
            //   Inline text (CMD arg\r\n)  — used by our CLI / netcat
            bool progress = true;
            while (progress && !buf.empty()) {
                progress = false;
                std::string line;

                if (buf[0] == '*') {
                    // RESP array framing
                    line = tryParseRESPArray(buf);
                    if (line.empty()) break;   // incomplete — wait for more data
                } else {
                    // Inline framing — find complete line
                    std::size_t pos = buf.find('\n');
                    if (pos == std::string::npos) break;
                    line = buf.substr(0, pos + 1);
                    buf.erase(0, pos + 1);
                }

                progress = true;
                std::string resp = dispatch(line);
#ifdef _WIN32
                ::send(fd, resp.data(), static_cast<int>(resp.size()), 0);
#else
                ::send(fd, resp.data(), resp.size(), 0);
#endif
                auto cmd = protocol::parse(line);
                if (cmd.type == protocol::CommandType::QUIT) {
                    closeSocket(fd);
                    return;
                }
            }
        }
        closeSocket(fd);
    }

    // ── Command dispatch ──────────────────────────────────────────────────────

    std::string dispatch(const std::string& line) {
        using namespace protocol;
        Command cmd = parse(line);

        switch (cmd.type) {

        case CommandType::PING:
            return pong();

        case CommandType::QUIT:
            return ok();

        case CommandType::SET: {
            if (auto e = checkArity(cmd, 3); !e.empty()) return e;
            cache_.set(cmd.args[1], cmd.args[2]);
            return ok();
        }

        case CommandType::SETEX: {
            if (auto e = checkArity(cmd, 4); !e.empty()) return e;
            long long ms = 0;
            try { ms = std::stoll(cmd.args[2]); }
            catch (...) { return error("value is not an integer"); }
            if (ms <= 0) return error("invalid expire time");
            cache_.setWithTTL(cmd.args[1], cmd.args[3], Millis{ms});
            return ok();
        }

        case CommandType::GET: {
            if (auto e = checkArity(cmd, 2); !e.empty()) return e;
            auto val = cache_.get(cmd.args[1]);
            return val ? bulkString(*val) : nullBulk();
        }

        case CommandType::DEL: {
            if (auto e = checkArity(cmd, 2); !e.empty()) return e;
            bool had = cache_.exists(cmd.args[1]);
            cache_.del(cmd.args[1]);
            return integer(had ? 1 : 0);
        }

        case CommandType::EXISTS: {
            if (auto e = checkArity(cmd, 2); !e.empty()) return e;
            return integer(cache_.exists(cmd.args[1]) ? 1 : 0);
        }

        case CommandType::EXPIRE: {
            if (auto e = checkArity(cmd, 3); !e.empty()) return e;
            long long ms = 0;
            try { ms = std::stoll(cmd.args[2]); }
            catch (...) { return error("value is not an integer"); }
            if (ms <= 0) return error("invalid expire time");
            return integer(cache_.expire(cmd.args[1], Millis{ms}) ? 1 : 0);
        }

        case CommandType::PERSIST: {
            if (auto e = checkArity(cmd, 2); !e.empty()) return e;
            return integer(cache_.persist(cmd.args[1]) ? 1 : 0);
        }

        case CommandType::TTL: {
            if (auto e = checkArity(cmd, 2); !e.empty()) return e;
            return integer(cache_.ttl(cmd.args[1]));
        }

        case CommandType::UNKNOWN:
        default:
            if (cmd.args.empty()) return error("empty command");
            return error("unknown command '" + cmd.args[0] + "'");
        }
    }

    // ── State ─────────────────────────────────────────────────────────────────
    cache::ShardedCache      cache_;
    cache::ExpiryWorker      expiry_;
    SocketFd                 listenFd_  = INVALID_SOCKET_FD;
    std::atomic<bool>        running_{false};
    uint16_t                 boundPort_ = 0;
    std::thread              acceptThread_;
    std::vector<std::thread> clientThreads_;
    std::mutex               clientsMutex_;
};

}  // namespace server
