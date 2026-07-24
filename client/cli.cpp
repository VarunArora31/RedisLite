// client/cli.cpp
//
// RedisLite CLI — a simple REPL that connects to a RedisLite server
// over TCP and sends commands interactively.
//
// Usage:
//   redis-lite-cli [host] [port]
//   redis-lite-cli               → connects to 127.0.0.1:6379
//   redis-lite-cli 192.168.1.5 7000
//
// Type any supported command and press Enter.
// Type QUIT or press Ctrl-C to exit.

#include <iostream>
#include <string>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
   using Sock = SOCKET;
   static constexpr Sock BAD = INVALID_SOCKET;
   inline void sockClose(Sock s) { ::closesocket(s); }
   inline bool sockValid(Sock s) { return s != INVALID_SOCKET; }
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
   using Sock = int;
   static constexpr Sock BAD = -1;
   inline void sockClose(Sock s) { ::close(s); }
   inline bool sockValid(Sock s) { return s >= 0; }
#endif

// ── RESP response reader ──────────────────────────────────────────────────────

class Connection {
public:
    Connection(const std::string& host, uint16_t port) {
#ifdef _WIN32
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (!sockValid(fd_)) throw std::runtime_error("socket() failed");

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(port);
        if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1)
            throw std::runtime_error("invalid host: " + host);

        if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
            throw std::runtime_error("connect() failed — is the server running?");
    }

    ~Connection() {
        if (sockValid(fd_)) sockClose(fd_);
#ifdef _WIN32
        WSACleanup();
#endif
    }

    // Send a command line and return the formatted response string.
    std::string send(const std::string& cmd) {
        std::string line = cmd + "\r\n";
#ifdef _WIN32
        ::send(fd_, line.data(), static_cast<int>(line.size()), 0);
#else
        ::send(fd_, line.data(), line.size(), 0);
#endif
        return readResponse();
    }

private:
    std::string readResponse() {
        while (buf_.find('\n') == std::string::npos) readMore();

        char type = buf_[0];

        if (type == '+' || type == '-' || type == ':') {
            std::string line = consumeLine();
            // Strip \r\n for display
            while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
                line.pop_back();
            return line;
        }

        if (type == '$') {
            std::string header = consumeLine();
            long long len = std::stoll(header.substr(1));
            if (len < 0) return "(nil)";

            std::size_t need = static_cast<std::size_t>(len) + 2;
            while (buf_.size() < need) readMore();
            std::string data = buf_.substr(0, static_cast<std::size_t>(len));
            buf_.erase(0, need);
            return '"' + data + '"';
        }

        return consumeLine();
    }

    std::string consumeLine() {
        std::size_t pos = buf_.find('\n');
        if (pos == std::string::npos) return "";
        std::string line = buf_.substr(0, pos + 1);
        buf_.erase(0, pos + 1);
        return line;
    }

    void readMore() {
        char tmp[4096];
#ifdef _WIN32
        int n = ::recv(fd_, tmp, static_cast<int>(sizeof(tmp) - 1), 0);
#else
        ssize_t n = ::recv(fd_, tmp, sizeof(tmp) - 1, 0);
#endif
        if (n <= 0) throw std::runtime_error("connection closed by server");
        tmp[n] = '\0';
        buf_ += tmp;
    }

    Sock        fd_ = BAD;
    std::string buf_;
};

// ── main ──────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    std::string  host = "127.0.0.1";
    uint16_t     port = 6379;

    if (argc >= 2) host = argv[1];
    if (argc >= 3) port = static_cast<uint16_t>(std::atoi(argv[2]));

    std::cout << "RedisLite CLI — connecting to " << host << ":" << port << "\n"
              << "Type commands and press Enter. QUIT to exit.\n\n";

    Connection conn(host, port);

    std::string line;
    while (true) {
        std::cout << host << ":" << port << "> ";
        std::cout.flush();

        if (!std::getline(std::cin, line)) break;   // EOF (Ctrl-D / Ctrl-Z)
        if (line.empty()) continue;

        std::string resp;
        try {
            resp = conn.send(line);
        } catch (const std::exception& e) {
            std::cerr << "(error) " << e.what() << "\n";
            break;
        }

        std::cout << resp << "\n";

        // Local exit on QUIT (server already closed the connection)
        std::string upper = line;
        for (char& c : upper) c = static_cast<char>(std::toupper(
                                      static_cast<unsigned char>(c)));
        if (upper == "QUIT") break;
    }

    return 0;
}
