// tests/test_server.cpp
//
// Integration tests for server::Server — starts a real Server on localhost
// and drives it with real TCP connections via TestClient.

#include "server.hpp"
#include "test_client_helper.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

// Windows uses _getpid() in <process.h>; POSIX uses getpid() in <unistd.h>.
#ifdef _WIN32
#  include <process.h>
   inline int pid() { return ::_getpid(); }
#else
   inline int pid() { return ::getpid(); }
#endif

using namespace std::chrono_literals;

// Each test gets a unique port so parallel runs and TIME_WAIT ports don't clash.
static uint16_t nextTestPort() {
    static std::atomic<uint16_t> counter{
        static_cast<uint16_t>(20000 + (pid() % 5000))};
    return counter.fetch_add(1);
}

// Small helper: start server and wait briefly for the accept loop to be ready.
static bool startServer(server::Server& srv, uint16_t port) {
    if (!srv.start(port)) return false;
    std::this_thread::sleep_for(20ms);   // let acceptLoop reach accept()
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// 1. Lifecycle
// ─────────────────────────────────────────────────────────────────────────────

TEST(ServerLifecycle, StartAndStopSucceeds) {
    server::Server srv;
    ASSERT_TRUE(startServer(srv, nextTestPort()));
    EXPECT_TRUE(srv.isRunning());
    srv.stop();
    EXPECT_FALSE(srv.isRunning());
}

TEST(ServerLifecycle, DoubleStartReturnsFalse) {
    server::Server srv;
    uint16_t port = nextTestPort();
    ASSERT_TRUE(startServer(srv, port));
    EXPECT_FALSE(srv.start(port));   // already running
    srv.stop();
}

TEST(ServerLifecycle, StopIsIdempotent) {
    server::Server srv;
    ASSERT_TRUE(startServer(srv, nextTestPort()));
    srv.stop();
    EXPECT_NO_THROW(srv.stop());     // second stop must be a no-op
}

TEST(ServerLifecycle, DestructorStopsServer) {
    uint16_t port = nextTestPort();
    {
        server::Server srv;
        ASSERT_TRUE(startServer(srv, port));
    }   // destructor runs here — must join all threads cleanly
    SUCCEED();
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. Basic command round-trips
//    A fixture starts/stops the server around each test.
// ─────────────────────────────────────────────────────────────────────────────

class ServerCommandTest : public ::testing::Test {
protected:
    void SetUp() override {
        port_ = nextTestPort();
        ASSERT_TRUE(startServer(srv_, port_));
    }
    void TearDown() override { srv_.stop(); }

    server::Server srv_;
    uint16_t       port_ = 0;
};

TEST_F(ServerCommandTest, Ping) {
    TestClient c("127.0.0.1", port_);
    EXPECT_EQ(c.sendCommand("PING"), "+PONG\r\n");
}

TEST_F(ServerCommandTest, SetThenGet) {
    TestClient c("127.0.0.1", port_);
    EXPECT_EQ(c.sendCommand("SET foo bar"), "+OK\r\n");
    EXPECT_EQ(c.sendCommand("GET foo"),     "$3\r\nbar\r\n");
}

TEST_F(ServerCommandTest, GetMissingReturnsNullBulk) {
    TestClient c("127.0.0.1", port_);
    EXPECT_EQ(c.sendCommand("GET missing"), "$-1\r\n");
}

TEST_F(ServerCommandTest, DelExistingReturnsOne) {
    TestClient c("127.0.0.1", port_);
    c.sendCommand("SET k v");
    EXPECT_EQ(c.sendCommand("DEL k"),  ":1\r\n");
    EXPECT_EQ(c.sendCommand("GET k"),  "$-1\r\n");
}

TEST_F(ServerCommandTest, DelMissingReturnsZero) {
    TestClient c("127.0.0.1", port_);
    EXPECT_EQ(c.sendCommand("DEL ghost"), ":0\r\n");
}

TEST_F(ServerCommandTest, ExistsReflectsState) {
    TestClient c("127.0.0.1", port_);
    EXPECT_EQ(c.sendCommand("EXISTS k"), ":0\r\n");
    c.sendCommand("SET k v");
    EXPECT_EQ(c.sendCommand("EXISTS k"), ":1\r\n");
}

TEST_F(ServerCommandTest, SetexAndTtl) {
    TestClient c("127.0.0.1", port_);
    EXPECT_EQ(c.sendCommand("SETEX k 5000 v"), "+OK\r\n");
    EXPECT_EQ(c.sendCommand("GET k"),          "$1\r\nv\r\n");

    std::string ttlResp = c.sendCommand("TTL k");
    ASSERT_FALSE(ttlResp.empty());
    EXPECT_EQ(ttlResp[0], ':');   // integer reply; exact ms value varies
}

TEST_F(ServerCommandTest, ExpireAndPersist) {
    TestClient c("127.0.0.1", port_);
    c.sendCommand("SET k v");
    EXPECT_EQ(c.sendCommand("EXPIRE k 5000"),  ":1\r\n");
    EXPECT_EQ(c.sendCommand("PERSIST k"),      ":1\r\n");
    EXPECT_EQ(c.sendCommand("TTL k"),          ":-1\r\n");
}

TEST_F(ServerCommandTest, ExpireOnMissingKeyReturnsZero) {
    TestClient c("127.0.0.1", port_);
    EXPECT_EQ(c.sendCommand("EXPIRE ghost 1000"), ":0\r\n");
}

TEST_F(ServerCommandTest, TtlOnPersistentKeyReturnsMinusOne) {
    TestClient c("127.0.0.1", port_);
    c.sendCommand("SET k v");
    EXPECT_EQ(c.sendCommand("TTL k"), ":-1\r\n");
}

TEST_F(ServerCommandTest, TtlOnMissingKeyReturnsMinusTwo) {
    TestClient c("127.0.0.1", port_);
    EXPECT_EQ(c.sendCommand("TTL ghost"), ":-2\r\n");
}

TEST_F(ServerCommandTest, OverwriteClearsTTL) {
    TestClient c("127.0.0.1", port_);
    c.sendCommand("SETEX k 5000 v1");
    c.sendCommand("SET k v2");           // overwrite — TTL must be gone
    EXPECT_EQ(c.sendCommand("TTL k"), ":-1\r\n");
    EXPECT_EQ(c.sendCommand("GET k"), "$2\r\nv2\r\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. Error handling
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(ServerCommandTest, WrongArityReturnsError) {
    TestClient c("127.0.0.1", port_);
    std::string resp = c.sendCommand("SET onlykey");
    EXPECT_EQ(resp[0], '-');
}

TEST_F(ServerCommandTest, UnknownCommandReturnsError) {
    TestClient c("127.0.0.1", port_);
    std::string resp = c.sendCommand("FROBNICATE x");
    EXPECT_EQ(resp[0], '-');
}

TEST_F(ServerCommandTest, InvalidExpireValueReturnsError) {
    TestClient c("127.0.0.1", port_);
    c.sendCommand("SET k v");
    std::string resp = c.sendCommand("EXPIRE k notanumber");
    EXPECT_EQ(resp[0], '-');
}

TEST_F(ServerCommandTest, SetexZeroMsReturnsError) {
    TestClient c("127.0.0.1", port_);
    std::string resp = c.sendCommand("SETEX k 0 v");
    EXPECT_EQ(resp[0], '-');
}

TEST_F(ServerCommandTest, SetexNonIntegerMsReturnsError) {
    TestClient c("127.0.0.1", port_);
    std::string resp = c.sendCommand("SETEX k notanumber v");
    EXPECT_EQ(resp[0], '-');
}

// ─────────────────────────────────────────────────────────────────────────────
// 4. QUIT closes the connection
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(ServerCommandTest, QuitReceivesOk) {
    TestClient c("127.0.0.1", port_);
    EXPECT_EQ(c.sendCommand("QUIT"), "+OK\r\n");
}

TEST_F(ServerCommandTest, QuitClosesConnection) {
    TestClient c("127.0.0.1", port_);
    c.sendCommand("QUIT");
    std::this_thread::sleep_for(30ms);   // give server time to close its end
    EXPECT_TRUE(c.isClosedByPeer());
}

// ─────────────────────────────────────────────────────────────────────────────
// 5. Multiple simultaneous clients
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(ServerCommandTest, MultipleClientsShareState) {
    TestClient a("127.0.0.1", port_);
    TestClient b("127.0.0.1", port_);

    a.sendCommand("SET shared_key from_a");
    EXPECT_EQ(b.sendCommand("GET shared_key"), "$6\r\nfrom_a\r\n");
}

TEST_F(ServerCommandTest, ManyConcurrentClientsNoRace) {
    constexpr int kClients = 20;
    std::vector<std::thread> threads;
    std::atomic<int> failures{0};

    for (int i = 0; i < kClients; ++i) {
        threads.emplace_back([this, i, &failures]() {
            try {
                TestClient c("127.0.0.1", port_);
                std::string key = "client" + std::to_string(i);
                std::string val = "val"    + std::to_string(i);

                c.sendCommand("SET " + key + " " + val);

                std::string got      = c.sendCommand("GET " + key);
                std::string expected = "$" + std::to_string(val.size())
                                     + "\r\n" + val + "\r\n";
                if (got != expected) failures.fetch_add(1);
            } catch (...) {
                failures.fetch_add(1);
            }
        });
    }
    for (auto& t : threads) t.join();
    EXPECT_EQ(failures.load(), 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// 6. Cache accessor
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(ServerCommandTest, CacheAccessorReflectsClientWrites) {
    TestClient c("127.0.0.1", port_);
    c.sendCommand("SET k v");
    std::this_thread::sleep_for(10ms);   // let the server thread complete the write
    EXPECT_TRUE(srv_.cache().exists("k"));
}

TEST_F(ServerCommandTest, CacheAccessorReflectsClientDel) {
    TestClient c("127.0.0.1", port_);
    c.sendCommand("SET k v");
    std::this_thread::sleep_for(10ms);
    c.sendCommand("DEL k");
    std::this_thread::sleep_for(10ms);
    EXPECT_FALSE(srv_.cache().exists("k"));
}

// ─────────────────────────────────────────────────────────────────────────────
// 7. TTL expiry end-to-end through the server
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(ServerCommandTest, SetexKeyExpiresAndGetReturnsNull) {
    TestClient c("127.0.0.1", port_);
    c.sendCommand("SETEX expiring 80 value");

    // Key should be alive now
    EXPECT_EQ(c.sendCommand("GET expiring"), "$5\r\nvalue\r\n");

    // Wait for TTL + expiry sweep (server sweeps every 100ms)
    std::this_thread::sleep_for(300ms);

    EXPECT_EQ(c.sendCommand("GET expiring"), "$-1\r\n");
}
