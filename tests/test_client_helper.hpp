// test_client_helper.hpp
//
// A minimal synchronous TCP client used exclusively in server integration tests.
// Sends one command line (appends \r\n), reads until it has a complete response,
// and returns it as a string.
//
// Response framing rules (RESP subset):
//   +...\r\n          simple string   — read one line
//   -...\r\n          error           — read one line
//   :...\r\n          integer         — read one line
//   $-1\r\n           null bulk       — read one line
//   $<n>\r\n<data>\r\n bulk string    — read header line, then n+2 bytes
#pragma once

#include <string>
#include <stdexcept>
#include <cstring>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
   using TcpSocket = SOCKET;
   static constexpr TcpSocket BAD_SOCKET = INVALID_SOCKET;
   inline void tcpClose(TcpSocket s) { ::closesocket(s); }
   inline bool tcpValid(TcpSocket s) { return s != INVALID_SOCKET; }
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
   using TcpSocket = int;
   static constexpr TcpSocket BAD_SOCKET = -1;
   inline void tcpClose(TcpSocket s) { ::close(s); }
   inline bool tcpValid(TcpSocket s) { return s >= 0; }
#endif

class TestClient {
public:
    TestClient(const std::string& host, uint16_t port) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (!tcpValid(fd_)) throw std::runtime_error("socket() failed");

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(port);
        if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
            tcpClose(fd_); fd_ = BAD_SOCKET;
            throw std::runtime_error("inet_pton() failed");
        }
        if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            tcpClose(fd_); fd_ = BAD_SOCKET;
            throw std::runtime_error("connect() failed");
        }
    }

    ~TestClient() {
        if (tcpValid(fd_)) tcpClose(fd_);
    }

    // Non-copyable
    TestClient(const TestClient&)            = delete;
    TestClient& operator=(const TestClient&) = delete;

    // Send command (appends \r\n) and return the full response.
    std::string sendCommand(const std::string& cmd) {
        std::string line = cmd + "\r\n";
#ifdef _WIN32
        ::send(fd_, line.data(), static_cast<int>(line.size()), 0);
#else
        ::send(fd_, line.data(), line.size(), 0);
#endif
        return readResponse();
    }

    // Returns true if the peer closed the connection
    // (recv returns 0 on an otherwise empty socket).
    bool isClosedByPeer() {
        char buf[1];
#ifdef _WIN32
        int n = ::recv(fd_, buf, 1, 0);
#else
        ssize_t n = ::recv(fd_, buf, 1, 0);
#endif
        return n == 0;
    }

private:
    // Read bytes into buf_ until we have a complete RESP response.
    std::string readResponse() {
        // Read until we have at least one complete line
        while (buf_.find('\n') == std::string::npos) {
            if (!readMore()) return "";
        }

        // Peek at the first byte to determine response type
        char type = buf_[0];

        if (type == '+' || type == '-' || type == ':') {
            // Single-line response
            return consumeLine();
        }

        if (type == '$') {
            // Bulk string: $<n>\r\n  or $-1\r\n
            std::string header = consumeLine();
            // Parse length from header: skip '$'
            long long len = std::stoll(header.substr(1));
            if (len < 0) return header;   // null bulk: "$-1\r\n"

            // Read <len> bytes + \r\n
            std::size_t need = static_cast<std::size_t>(len) + 2;
            while (buf_.size() < need) {
                if (!readMore()) return header;
            }
            std::string data = buf_.substr(0, need);
            buf_.erase(0, need);
            return header + data;
        }

        // Fallback: return whatever one line we have
        return consumeLine();
    }

    std::string consumeLine() {
        std::size_t pos = buf_.find('\n');
        if (pos == std::string::npos) return "";
        std::string line = buf_.substr(0, pos + 1);
        buf_.erase(0, pos + 1);
        return line;
    }

    bool readMore() {
        char tmp[4096];
#ifdef _WIN32
        int n = ::recv(fd_, tmp, static_cast<int>(sizeof(tmp) - 1), 0);
#else
        ssize_t n = ::recv(fd_, tmp, sizeof(tmp) - 1, 0);
#endif
        if (n <= 0) return false;
        tmp[n] = '\0';
        buf_ += tmp;
        return true;
    }

    TcpSocket   fd_ = BAD_SOCKET;
    std::string buf_;
};
