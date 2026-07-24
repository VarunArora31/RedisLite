// protocol.hpp
#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// RedisLite wire protocol
//
// Format (intentionally simple, subset of RESP inline commands):
//
//   Request  (client → server):
//     <COMMAND> [arg1] [arg2] ...\r\n
//     e.g.  SET foo bar\r\n
//           GET foo\r\n
//           EXPIRE foo 5000\r\n
//
//   Response (server → client):
//     +<message>\r\n          simple string  (OK, value, etc.)
//     -<error message>\r\n    error
//     :<integer>\r\n          integer (TTL, exists 0/1, etc.)
//     $-1\r\n                 null bulk string (key not found)
//     $<len>\r\n<data>\r\n    bulk string
//
// Supported commands:
//   SET    key value
//   SETEX  key milliseconds value
//   GET    key
//   DEL    key
//   EXISTS key
//   EXPIRE key milliseconds
//   PERSIST key
//   TTL    key
//   PING
//   QUIT
// ─────────────────────────────────────────────────────────────────────────────

#include <string>
#include <vector>
#include <string_view>

namespace protocol {

// ── Command ──────────────────────────────────────────────────────────────────

enum class CommandType {
    SET,
    SETEX,
    GET,
    DEL,
    EXISTS,
    EXPIRE,
    PERSIST,
    TTL,
    PING,
    QUIT,
    UNKNOWN
};

struct Command {
    CommandType          type = CommandType::UNKNOWN;
    std::vector<std::string> args;   // raw tokens including the command name
};

// ── Parsing ───────────────────────────────────────────────────────────────────

// Parse a single null-terminated or \r\n-terminated line into a Command.
// Accepts both "\r\n" and "\n" line endings.
// Tokens are split on whitespace. Quoted strings are NOT supported (keep it
// simple — Redis itself uses RESP framing for binary-safe values).
//
// Returns Command{UNKNOWN} if the line is empty or unrecognised.
inline Command parse(std::string_view line) {
    // Strip trailing \r\n or \n
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
        line.remove_suffix(1);
    }

    Command cmd;

    // Tokenise on whitespace
    std::size_t i = 0;
    while (i < line.size()) {
        // Skip leading whitespace
        while (i < line.size() && line[i] == ' ') ++i;
        if (i >= line.size()) break;

        // Find end of token
        std::size_t start = i;
        while (i < line.size() && line[i] != ' ') ++i;

        cmd.args.emplace_back(line.substr(start, i - start));
    }

    if (cmd.args.empty()) return cmd;   // UNKNOWN

    // Normalise command name to uppercase for case-insensitive matching
    std::string name = cmd.args[0];
    for (char& c : name) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

    if      (name == "SET")     cmd.type = CommandType::SET;
    else if (name == "SETEX")   cmd.type = CommandType::SETEX;
    else if (name == "GET")     cmd.type = CommandType::GET;
    else if (name == "DEL")     cmd.type = CommandType::DEL;
    else if (name == "EXISTS")  cmd.type = CommandType::EXISTS;
    else if (name == "EXPIRE")  cmd.type = CommandType::EXPIRE;
    else if (name == "PERSIST") cmd.type = CommandType::PERSIST;
    else if (name == "TTL")     cmd.type = CommandType::TTL;
    else if (name == "PING")    cmd.type = CommandType::PING;
    else if (name == "QUIT")    cmd.type = CommandType::QUIT;
    else                        cmd.type = CommandType::UNKNOWN;

    return cmd;
}

// ── Response formatters ───────────────────────────────────────────────────────

// +OK\r\n
inline std::string ok() {
    return "+OK\r\n";
}

// +<msg>\r\n
inline std::string simpleString(std::string_view msg) {
    return "+" + std::string(msg) + "\r\n";
}

// -ERR <msg>\r\n
inline std::string error(std::string_view msg) {
    return "-ERR " + std::string(msg) + "\r\n";
}

// -WRONGTYPE <msg>\r\n  (mirrors Redis convention)
inline std::string wrongType(std::string_view msg) {
    return "-WRONGTYPE " + std::string(msg) + "\r\n";
}

// :<n>\r\n
inline std::string integer(long long n) {
    return ":" + std::to_string(n) + "\r\n";
}

// $-1\r\n  (null bulk — key not found)
inline std::string nullBulk() {
    return "$-1\r\n";
}

// $<len>\r\n<data>\r\n
inline std::string bulkString(std::string_view data) {
    return "$" + std::to_string(data.size()) + "\r\n"
         + std::string(data) + "\r\n";
}

// +PONG\r\n
inline std::string pong() {
    return "+PONG\r\n";
}

// ── Argument count validation ─────────────────────────────────────────────────

// Returns an error response string if arg count is wrong, empty string if OK.
// args includes the command name itself (args[0]).
inline std::string checkArity(const Command& cmd, std::size_t expected) {
    // expected = total tokens including command name
    if (cmd.args.size() != expected) {
        return error("wrong number of arguments for '" + cmd.args[0] + "' command");
    }
    return "";
}

inline std::string checkMinArity(const Command& cmd, std::size_t minExpected) {
    if (cmd.args.size() < minExpected) {
        return error("wrong number of arguments for '" + cmd.args[0] + "' command");
    }
    return "";
}

}  // namespace protocol
