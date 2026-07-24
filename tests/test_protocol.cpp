// tests/test_protocol.cpp
//
// Unit tests for protocol::parse() and response formatters.
//
// Test groups:
//   1.  Parsing — empty / whitespace
//   2.  Parsing — command type recognition (case-insensitive)
//   3.  Parsing — argument tokenisation
//   4.  Parsing — line ending variants (\r\n, \n, none)
//   5.  Parsing — unknown commands
//   6.  Response — ok / simpleString / error
//   7.  Response — integer
//   8.  Response — bulkString / nullBulk
//   9.  Response — pong
//  10.  Arity validation — checkArity / checkMinArity

#include "protocol.hpp"
#include <gtest/gtest.h>

using namespace protocol;

// ─────────────────────────────────────────────────────────────────────────────
// 1. Empty / whitespace input
// ─────────────────────────────────────────────────────────────────────────────

TEST(ProtocolParse, EmptyLineIsUnknown) {
    auto cmd = parse("");
    EXPECT_EQ(cmd.type, CommandType::UNKNOWN);
    EXPECT_TRUE(cmd.args.empty());
}

TEST(ProtocolParse, WhitespaceOnlyIsUnknown) {
    auto cmd = parse("   ");
    EXPECT_EQ(cmd.type, CommandType::UNKNOWN);
    EXPECT_TRUE(cmd.args.empty());
}

TEST(ProtocolParse, CRLFOnlyIsUnknown) {
    auto cmd = parse("\r\n");
    EXPECT_EQ(cmd.type, CommandType::UNKNOWN);
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. Command type recognition (case-insensitive)
// ─────────────────────────────────────────────────────────────────────────────

TEST(ProtocolParse, SetUpperCase) {
    EXPECT_EQ(parse("SET k v\r\n").type, CommandType::SET);
}

TEST(ProtocolParse, SetLowerCase) {
    EXPECT_EQ(parse("set k v\r\n").type, CommandType::SET);
}

TEST(ProtocolParse, SetMixedCase) {
    EXPECT_EQ(parse("SeT k v\r\n").type, CommandType::SET);
}

TEST(ProtocolParse, SetexRecognised) {
    EXPECT_EQ(parse("SETEX k 5000 v\r\n").type, CommandType::SETEX);
}

TEST(ProtocolParse, GetRecognised) {
    EXPECT_EQ(parse("GET k\r\n").type, CommandType::GET);
}

TEST(ProtocolParse, DelRecognised) {
    EXPECT_EQ(parse("DEL k\r\n").type, CommandType::DEL);
}

TEST(ProtocolParse, ExistsRecognised) {
    EXPECT_EQ(parse("EXISTS k\r\n").type, CommandType::EXISTS);
}

TEST(ProtocolParse, ExpireRecognised) {
    EXPECT_EQ(parse("EXPIRE k 1000\r\n").type, CommandType::EXPIRE);
}

TEST(ProtocolParse, PersistRecognised) {
    EXPECT_EQ(parse("PERSIST k\r\n").type, CommandType::PERSIST);
}

TEST(ProtocolParse, TtlRecognised) {
    EXPECT_EQ(parse("TTL k\r\n").type, CommandType::TTL);
}

TEST(ProtocolParse, PingRecognised) {
    EXPECT_EQ(parse("PING\r\n").type, CommandType::PING);
}

TEST(ProtocolParse, QuitRecognised) {
    EXPECT_EQ(parse("QUIT\r\n").type, CommandType::QUIT);
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. Argument tokenisation
// ─────────────────────────────────────────────────────────────────────────────

TEST(ProtocolParse, SetHasThreeArgs) {
    auto cmd = parse("SET mykey myvalue\r\n");
    ASSERT_EQ(cmd.args.size(), 3u);
    EXPECT_EQ(cmd.args[0], "SET");
    EXPECT_EQ(cmd.args[1], "mykey");
    EXPECT_EQ(cmd.args[2], "myvalue");
}

TEST(ProtocolParse, GetHasTwoArgs) {
    auto cmd = parse("GET mykey\r\n");
    ASSERT_EQ(cmd.args.size(), 2u);
    EXPECT_EQ(cmd.args[1], "mykey");
}

TEST(ProtocolParse, PingHasOneArg) {
    auto cmd = parse("PING\r\n");
    ASSERT_EQ(cmd.args.size(), 1u);
    EXPECT_EQ(cmd.args[0], "PING");
}

TEST(ProtocolParse, SetexHasFourArgs) {
    auto cmd = parse("SETEX key 3000 value\r\n");
    ASSERT_EQ(cmd.args.size(), 4u);
    EXPECT_EQ(cmd.args[1], "key");
    EXPECT_EQ(cmd.args[2], "3000");
    EXPECT_EQ(cmd.args[3], "value");
}

TEST(ProtocolParse, ExpireHasThreeArgs) {
    auto cmd = parse("EXPIRE mykey 5000\r\n");
    ASSERT_EQ(cmd.args.size(), 3u);
    EXPECT_EQ(cmd.args[1], "mykey");
    EXPECT_EQ(cmd.args[2], "5000");
}

TEST(ProtocolParse, ExtraWhitespaceBetweenTokensIgnored) {
    auto cmd = parse("SET   key   value\r\n");
    ASSERT_EQ(cmd.args.size(), 3u);
    EXPECT_EQ(cmd.args[1], "key");
    EXPECT_EQ(cmd.args[2], "value");
}

TEST(ProtocolParse, CommandNameNormalisedToUpper) {
    // args[0] should be the original token, not uppercased
    // (the type is determined from an uppercased copy, args keep original)
    auto cmd = parse("set key val\r\n");
    EXPECT_EQ(cmd.args[0], "set");      // original preserved
    EXPECT_EQ(cmd.type, CommandType::SET); // type still correct
}

// ─────────────────────────────────────────────────────────────────────────────
// 4. Line ending variants
// ─────────────────────────────────────────────────────────────────────────────

TEST(ProtocolParse, CRLFTerminated) {
    EXPECT_EQ(parse("GET key\r\n").type, CommandType::GET);
}

TEST(ProtocolParse, LFOnlyTerminated) {
    EXPECT_EQ(parse("GET key\n").type, CommandType::GET);
}

TEST(ProtocolParse, NoLineEnding) {
    EXPECT_EQ(parse("GET key").type, CommandType::GET);
}

TEST(ProtocolParse, MultipleCRLF) {
    // Should still parse correctly even with extra trailing CRs
    auto cmd = parse("SET k v\r\n");
    EXPECT_EQ(cmd.type, CommandType::SET);
    ASSERT_EQ(cmd.args.size(), 3u);
}

// ─────────────────────────────────────────────────────────────────────────────
// 5. Unknown commands
// ─────────────────────────────────────────────────────────────────────────────

TEST(ProtocolParse, UnknownCommandType) {
    auto cmd = parse("FLUSHALL\r\n");
    EXPECT_EQ(cmd.type, CommandType::UNKNOWN);
    EXPECT_EQ(cmd.args[0], "FLUSHALL");
}

TEST(ProtocolParse, GibberishIsUnknown) {
    EXPECT_EQ(parse("XYZ 123\r\n").type, CommandType::UNKNOWN);
}

// ─────────────────────────────────────────────────────────────────────────────
// 6. Response — ok / simpleString / error
// ─────────────────────────────────────────────────────────────────────────────

TEST(ProtocolResponse, OkFormat) {
    EXPECT_EQ(ok(), "+OK\r\n");
}

TEST(ProtocolResponse, SimpleStringFormat) {
    EXPECT_EQ(simpleString("hello"), "+hello\r\n");
}

TEST(ProtocolResponse, SimpleStringEmpty) {
    EXPECT_EQ(simpleString(""), "+\r\n");
}

TEST(ProtocolResponse, ErrorFormat) {
    EXPECT_EQ(error("something went wrong"), "-ERR something went wrong\r\n");
}

TEST(ProtocolResponse, ErrorEmpty) {
    EXPECT_EQ(error(""), "-ERR \r\n");
}

TEST(ProtocolResponse, WrongTypeFormat) {
    EXPECT_EQ(wrongType("op not permitted"), "-WRONGTYPE op not permitted\r\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// 7. Response — integer
// ─────────────────────────────────────────────────────────────────────────────

TEST(ProtocolResponse, IntegerPositive) {
    EXPECT_EQ(integer(42), ":42\r\n");
}

TEST(ProtocolResponse, IntegerZero) {
    EXPECT_EQ(integer(0), ":0\r\n");
}

TEST(ProtocolResponse, IntegerNegative) {
    EXPECT_EQ(integer(-1), ":-1\r\n");
    EXPECT_EQ(integer(-2), ":-2\r\n");
}

TEST(ProtocolResponse, IntegerLarge) {
    EXPECT_EQ(integer(1000000LL), ":1000000\r\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// 8. Response — bulkString / nullBulk
// ─────────────────────────────────────────────────────────────────────────────

TEST(ProtocolResponse, BulkStringFormat) {
    EXPECT_EQ(bulkString("hello"), "$5\r\nhello\r\n");
}

TEST(ProtocolResponse, BulkStringEmpty) {
    EXPECT_EQ(bulkString(""), "$0\r\n\r\n");
}

TEST(ProtocolResponse, BulkStringWithSpaces) {
    EXPECT_EQ(bulkString("hello world"), "$11\r\nhello world\r\n");
}

TEST(ProtocolResponse, BulkStringLengthMatchesData) {
    std::string data = "abcdefghij";
    std::string resp = bulkString(data);
    // Format: $10\r\nabcdefghij\r\n
    EXPECT_EQ(resp, "$10\r\nabcdefghij\r\n");
}

TEST(ProtocolResponse, NullBulkFormat) {
    EXPECT_EQ(nullBulk(), "$-1\r\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// 9. Response — pong
// ─────────────────────────────────────────────────────────────────────────────

TEST(ProtocolResponse, PongFormat) {
    EXPECT_EQ(pong(), "+PONG\r\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// 10. Arity validation
// ─────────────────────────────────────────────────────────────────────────────

TEST(ProtocolArity, CorrectArityReturnsEmpty) {
    auto cmd = parse("SET k v\r\n");     // 3 args
    EXPECT_TRUE(checkArity(cmd, 3).empty());
}

TEST(ProtocolArity, WrongArityReturnsError) {
    auto cmd = parse("SET k\r\n");       // 2 args, expected 3
    auto resp = checkArity(cmd, 3);
    EXPECT_FALSE(resp.empty());
    EXPECT_EQ(resp[0], '-');             // must be an error response
}

TEST(ProtocolArity, MinArityPassedWhenEnough) {
    auto cmd = parse("SET k v\r\n");
    EXPECT_TRUE(checkMinArity(cmd, 3).empty());
}

TEST(ProtocolArity, MinArityFailsWhenTooFew) {
    auto cmd = parse("SET\r\n");         // only 1 arg
    auto resp = checkMinArity(cmd, 3);
    EXPECT_FALSE(resp.empty());
    EXPECT_EQ(resp[0], '-');
}

TEST(ProtocolArity, MinArityPassesWithMoreThanMin) {
    auto cmd = parse("DEL k1 k2 k3\r\n");  // 4 args, min 2
    EXPECT_TRUE(checkMinArity(cmd, 2).empty());
}
