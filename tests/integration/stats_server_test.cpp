// The stats endpoint over a real socket.

#include "strata/stats_server.hpp"

#include "strata/db.hpp"
#include "strata/write_batch.hpp"

#include <array>
#include <chrono>
#include <string>

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "obs/http.hpp"
#include "support/fault_injection_env.hpp"
#include "support/test_dir.hpp"

namespace strata {
namespace {

using test::TestDir;

// Connects to the server, sends `request`, and reads until the server closes.
std::string Exchange(std::uint16_t port, const std::string& request) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  EXPECT_GE(fd, 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
    ::close(fd);
    ADD_FAILURE() << "connect failed";
    return {};
  }
  std::size_t sent = 0;
  while (sent < request.size()) {
    const ssize_t n = ::send(fd, request.data() + sent, request.size() - sent, MSG_NOSIGNAL);
    if (n <= 0) {
      break;  // the server may answer early (431) and close
    }
    sent += static_cast<std::size_t>(n);
  }
  std::string response;
  std::array<char, 4096> chunk{};
  ssize_t n = 0;
  while ((n = ::recv(fd, chunk.data(), chunk.size(), 0)) > 0) {
    response.append(chunk.data(), static_cast<std::size_t>(n));
  }
  ::close(fd);
  return response;
}

std::string Get(std::uint16_t port, const std::string& path,
                const std::string& host = "127.0.0.1") {
  return Exchange(port, "GET " + path + " HTTP/1.1\r\nHost: " + host + "\r\n\r\n");
}

std::string Body(const std::string& response) {
  const auto end = http::FindEndOfHead(response);
  return end.has_value() ? response.substr(*end) : std::string();
}

class StatsServerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(DB::Open({.path = dir_.File("db")}, &db_).ok());
    auto server = StatsServer::Start(&db_);
    ASSERT_TRUE(server.ok()) << server.status().ToString();
    server_ = std::move(*server);
    ASSERT_NE(server_->port(), 0);
  }

  TestDir dir_{"stats_server"};
  DB db_;
  std::unique_ptr<StatsServer> server_;
};

TEST_F(StatsServerTest, ServesStatsAsJson) {
  ASSERT_TRUE(db_.Put("k", "v").ok());
  const std::string response = Get(server_->port(), "/stats");
  EXPECT_TRUE(response.starts_with("HTTP/1.1 200 OK\r\n")) << response;
  EXPECT_NE(response.find("Content-Type: application/json\r\n"), std::string::npos);
  const std::string body = Body(response);
  EXPECT_TRUE(body.starts_with("{\"unix_micros\":")) << body;
  EXPECT_NE(body.find(R"("write.calls":1)"), std::string::npos) << body;
  EXPECT_NE(response.find("Content-Length: " + std::to_string(body.size()) + "\r\n"),
            std::string::npos);
}

TEST_F(StatsServerTest, IgnoresTheQueryString) {
  EXPECT_TRUE(Get(server_->port(), "/stats?x=1").starts_with("HTTP/1.1 200"));
}

TEST_F(StatsServerTest, HealthzSaysOk) {
  const std::string response = Get(server_->port(), "/healthz", "localhost:1234");
  EXPECT_TRUE(response.starts_with("HTTP/1.1 200 OK\r\n")) << response;
  EXPECT_EQ(Body(response), "ok\n");
}

TEST_F(StatsServerTest, HeadSendsNoBody) {
  const std::string response =
      Exchange(server_->port(), "HEAD /stats HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
  EXPECT_TRUE(response.starts_with("HTTP/1.1 200 OK\r\n")) << response;
  EXPECT_EQ(Body(response), "");
}

TEST_F(StatsServerTest, UnknownPathsAre404) {
  EXPECT_TRUE(Get(server_->port(), "/nope").starts_with("HTTP/1.1 404"));
}

TEST_F(StatsServerTest, OnlyGetAndHeadAreAllowed) {
  const std::string response =
      Exchange(server_->port(), "POST /stats HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
  EXPECT_TRUE(response.starts_with("HTTP/1.1 405")) << response;
  EXPECT_NE(response.find("Allow: GET, HEAD\r\n"), std::string::npos);
}

TEST_F(StatsServerTest, AForeignHostIsRefused) {
  // What a DNS-rebinding page in a browser would send.
  EXPECT_TRUE(Get(server_->port(), "/stats", "attacker.example:8080").starts_with("HTTP/1.1 403"));
}

TEST_F(StatsServerTest, Http11WithoutHostIs400ButHttp10IsServed) {
  EXPECT_TRUE(
      Exchange(server_->port(), "GET /healthz HTTP/1.1\r\n\r\n").starts_with("HTTP/1.1 400"));
  EXPECT_TRUE(
      Exchange(server_->port(), "GET /healthz HTTP/1.0\r\n\r\n").starts_with("HTTP/1.1 200"));
}

TEST_F(StatsServerTest, AClientThatNeverReadsIsDroppedAtTheDeadline) {
  // Send a request and read nothing: the server must go on serving others
  // within its deadline. The response fits in the socket buffers here, so
  // this checks the server is not stuck on the client; the send deadline for
  // a response that does not fit is tested on SendAll (http_test.cpp).
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  const int small = 1024;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &small, sizeof(small));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(server_->port());
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  ASSERT_EQ(::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)), 0);
  const std::string request = "GET /stats HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
  ASSERT_GT(::send(fd, request.data(), request.size(), MSG_NOSIGNAL), 0);
  const auto start = std::chrono::steady_clock::now();
  EXPECT_TRUE(Get(server_->port(), "/healthz").starts_with("HTTP/1.1 200"));
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(5));
  ::close(fd);
}

TEST_F(StatsServerTest, MalformedRequestsAre400) {
  EXPECT_TRUE(Exchange(server_->port(), "nonsense\r\n\r\n").starts_with("HTTP/1.1 400"));
}

TEST_F(StatsServerTest, AnOversizedHeadIs431) {
  const std::string request =
      "GET /stats HTTP/1.1\r\nX-Pad: " + std::string(http::kMaxRequestHeadBytes, 'a') + "\r\n\r\n";
  EXPECT_TRUE(Exchange(server_->port(), request).starts_with("HTTP/1.1 431"));
}

TEST_F(StatsServerTest, AnIncompleteRequestTimesOutAndTheServerMovesOn) {
  const auto start = std::chrono::steady_clock::now();
  const std::string response = Exchange(server_->port(), "GET /stats HTTP/1.1\r\n");
  const auto elapsed = std::chrono::steady_clock::now() - start;
  EXPECT_TRUE(response.starts_with("HTTP/1.1 408")) << response;
  EXPECT_LT(elapsed, std::chrono::seconds(5));
  EXPECT_TRUE(Get(server_->port(), "/healthz").starts_with("HTTP/1.1 200"));
}

// Closing a DB while its server runs is a race (stats_server.hpp), so the
// server is stopped first and a new one started on the closed handle.
TEST_F(StatsServerTest, AClosedDatabaseIs503) {
  server_->Stop();
  ASSERT_TRUE(db_.Close().ok());
  auto server = StatsServer::Start(&db_);
  ASSERT_TRUE(server.ok()) << server.status().ToString();
  EXPECT_TRUE(Get((*server)->port(), "/stats").starts_with("HTTP/1.1 503"));
  EXPECT_TRUE(Get((*server)->port(), "/healthz").starts_with("HTTP/1.1 503"));
}

TEST(StatsServerHealthTest, HealthzIs503WithTheReasonOnceWritesAreRefused) {
  TestDir dir("stats_server_health");
  test::FaultInjectionEnv env(Env::Default());
  DB db;
  ASSERT_TRUE(DB::Open({.path = dir.File("db"), .env = &env}, &db).ok());
  env.SetSyncFailure(true, ".log");
  WriteBatch batch;
  batch.Put("a", "1");
  EXPECT_FALSE(db.Write(batch, WriteOptions{.sync = true}).ok());
  {
    auto server = StatsServer::Start(&db);
    ASSERT_TRUE(server.ok()) << server.status().ToString();
    const std::string response = Get((*server)->port(), "/healthz");
    EXPECT_TRUE(response.starts_with("HTTP/1.1 503")) << response;
    EXPECT_NE(Body(response).find("IoError"), std::string::npos) << response;
  }
  env.SetSyncFailure(false);
  static_cast<void>(db.Close());
}

TEST_F(StatsServerTest, StopIsIdempotentAndClosesThePort) {
  const std::uint16_t port = server_->port();
  server_->Stop();
  server_->Stop();
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  EXPECT_NE(::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)), 0);
  ::close(fd);
}

TEST(StatsServerStartTest, RejectsBadArguments) {
  TestDir dir("stats_server_start");
  DB db;
  ASSERT_TRUE(DB::Open({.path = dir.File("db")}, &db).ok());
  EXPECT_TRUE(StatsServer::Start(nullptr).status().IsInvalidArgument());
  EXPECT_TRUE(StatsServer::Start(&db, {.bind_address = "not-an-ip"}).status().IsInvalidArgument());
  EXPECT_TRUE(StatsServer::Start(&db, {.bind_address = "192.0.2.1"}).status().IsIoError())
      << "binding an address this machine does not have fails";
}

TEST(StatsServerStartTest, ServesOverIpv6Loopback) {
  TestDir dir("stats_server_v6");
  DB db;
  ASSERT_TRUE(DB::Open({.path = dir.File("db")}, &db).ok());
  auto server = StatsServer::Start(&db, {.bind_address = "::1"});
  if (!server.ok()) {
    GTEST_SKIP() << "no IPv6 loopback here: " << server.status().ToString();
  }
  const std::uint16_t port = (*server)->port();
  const auto exchange_v6 = [port](const std::string& host) {
    const int fd = ::socket(AF_INET6, SOCK_STREAM, 0);
    sockaddr_in6 address{};
    address.sin6_family = AF_INET6;
    address.sin6_port = htons(port);
    address.sin6_addr = in6addr_loopback;
    std::string response;
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0) {
      const std::string request = "GET /healthz HTTP/1.1\r\nHost: " + host + "\r\n\r\n";
      static_cast<void>(::send(fd, request.data(), request.size(), MSG_NOSIGNAL));
      std::array<char, 4096> chunk{};
      ssize_t n = 0;
      while ((n = ::recv(fd, chunk.data(), chunk.size(), 0)) > 0) {
        response.append(chunk.data(), static_cast<std::size_t>(n));
      }
    }
    ::close(fd);
    return response;
  };
  EXPECT_TRUE(exchange_v6("[::1]:" + std::to_string(port)).starts_with("HTTP/1.1 200"));
  // The DNS-rebinding check applies on the IPv6 loopback too.
  EXPECT_TRUE(exchange_v6("attacker.example").starts_with("HTTP/1.1 403"));
  (*server)->Stop();
}

}  // namespace
}  // namespace strata
