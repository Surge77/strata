#include "obs/http.hpp"

#include <array>
#include <chrono>
#include <string>

#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/socket.h>
#include <unistd.h>

namespace strata::http {
namespace {

TEST(FindEndOfHeadTest, FindsTheBlankLine) {
  EXPECT_FALSE(FindEndOfHead("GET / HTTP/1.1\r\nHost: x\r\n").has_value());
  EXPECT_EQ(FindEndOfHead("GET / HTTP/1.1\r\n\r\nbody"), 18U);
  EXPECT_EQ(FindEndOfHead("GET / HTTP/1.0\n\n"), 16U);
  // The earlier terminator wins whichever style it is.
  EXPECT_EQ(FindEndOfHead("A\n\nB\r\n\r\n"), 3U);
}

TEST(ParseRequestHeadTest, ParsesMethodPathAndHost) {
  const auto request = ParseRequestHead(
      "GET /stats?pretty=1 HTTP/1.1\r\nUser-Agent: t\r\nHoSt:  localhost:80 \r\n\r\n");
  ASSERT_TRUE(request.has_value());
  EXPECT_EQ(request->method, "GET");
  EXPECT_EQ(request->path, "/stats");
  EXPECT_EQ(request->version, "HTTP/1.1");
  ASSERT_TRUE(request->host.has_value());
  EXPECT_EQ(*request->host, "localhost:80");
}

TEST(ParseRequestHeadTest, HostIsOptional) {
  const auto request = ParseRequestHead("HEAD /healthz HTTP/1.0\n\n");
  ASSERT_TRUE(request.has_value());
  EXPECT_EQ(request->method, "HEAD");
  EXPECT_FALSE(request->host.has_value());
}

TEST(ParseRequestHeadTest, RejectsMalformedHeads) {
  for (const char* head : {
           "",
           "GET\r\n\r\n",
           "GET /\r\n\r\n",
           "GET / HTTP/2\r\n\r\n",
           "get / HTTP/1.1\r\n\r\n",
           "GET http://evil/ HTTP/1.1\r\n\r\n",
           "GET * HTTP/1.1\r\n\r\n",
           "GET / x HTTP/1.1\r\n\r\n",
           "GET / HTTP/1.1\r\nno colon here\r\n\r\n",
           "GET / HTTP/1.1\r\n: empty name\r\n\r\n",
           "GET / HTTP/1.1\r\nHost: a\r\nhost: b\r\n\r\n",
           "GET / HTTP/1.1\r\nHost : evil\r\n\r\n",
           "GET / HTTP/1.1\r\n folded: x\r\n\r\n",
           "GET / HTTP/1.2\r\n\r\n",
       }) {
    EXPECT_FALSE(ParseRequestHead(head).has_value()) << head;
  }
}

TEST(IsLoopbackHostTest, AcceptsLoopbackNamesWithOrWithoutPort) {
  for (const char* host : {"localhost", "LOCALHOST", "localhost:8080", "127.0.0.1", "127.0.0.1:1",
                           "[::1]", "[::1]:9000"}) {
    EXPECT_TRUE(IsLoopbackHost(host)) << host;
  }
}

TEST(IsLoopbackHostTest, RejectsEverythingElse) {
  for (const char* host : {"", "example.com", "evil.localhost", "localhost.evil.com", "127.0.0.2",
                           "127.0.0.1:", "127.0.0.1:80x", "localhost:80:80", "[::1", "[::1]x",
                           "[::2]", "0.0.0.0", "[::]"}) {
    EXPECT_FALSE(IsLoopbackHost(host)) << host;
  }
}

TEST(ResponseTest, WritesStatusHeadersAndBody) {
  EXPECT_EQ(Response(200, "text/plain", "ok\n"),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: 3\r\n"
            "Cache-Control: no-store\r\n"
            "X-Content-Type-Options: nosniff\r\n"
            "Connection: close\r\n"
            "\r\n"
            "ok\n");
}

TEST(ResponseTest, HeadKeepsTheLengthButDropsTheBody) {
  const std::string response =
      Response(405, "text/plain", "no", /*head_only=*/true, "Allow: GET, HEAD\r\n");
  EXPECT_TRUE(response.starts_with("HTTP/1.1 405 Method Not Allowed\r\n"));
  EXPECT_NE(response.find("Content-Length: 2\r\n"), std::string::npos);
  EXPECT_NE(response.find("Allow: GET, HEAD\r\n\r\n"), std::string::npos);
  EXPECT_TRUE(response.ends_with("\r\n\r\n"));
}

// A connected pair whose first end is non-blocking with a small send buffer.
struct SocketPair {
  SocketPair() {
    EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds.data()), 0);
    const int small = 4096;
    ::setsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof(small));
    ::fcntl(fds[0], F_SETFL, ::fcntl(fds[0], F_GETFL) | O_NONBLOCK);
  }
  ~SocketPair() {
    ::close(fds[0]);
    ::close(fds[1]);
  }
  SocketPair(const SocketPair&) = delete;
  SocketPair& operator=(const SocketPair&) = delete;
  std::array<int, 2> fds{-1, -1};
};

TEST(SendAllTest, SendsEverythingTheReaderTakes) {
  SocketPair pair;
  const std::string data(1000, 'x');
  EXPECT_TRUE(
      SendAll(pair.fds[0], data, std::chrono::steady_clock::now() + std::chrono::seconds(2)));
  std::array<char, 2000> received{};
  EXPECT_EQ(::recv(pair.fds[1], received.data(), received.size(), 0), 1000);
}

// The guarantee behind the server's "a stuck client delays others by at most
// the deadline": a peer that never reads cannot hold SendAll past it.
TEST(SendAllTest, GivesUpAtTheDeadlineWhenThePeerNeverReads) {
  SocketPair pair;
  const std::string data(8U << 20U, 'x');  // far more than both buffers hold
  constexpr auto kDeadline = std::chrono::milliseconds(200);
  const auto start = std::chrono::steady_clock::now();
  EXPECT_FALSE(SendAll(pair.fds[0], data, start + kDeadline));
  const auto elapsed = std::chrono::steady_clock::now() - start;
  EXPECT_GE(elapsed, kDeadline - std::chrono::milliseconds(5));
  EXPECT_LT(elapsed, std::chrono::seconds(2));
}

TEST(SendAllTest, FailsWithoutRaisingSigpipeWhenThePeerHasGone) {
  SocketPair pair;
  ::close(pair.fds[1]);
  pair.fds[1] = ::open("/dev/null", O_RDONLY);  // keep the destructor's close valid
  EXPECT_FALSE(
      SendAll(pair.fds[0], "data", std::chrono::steady_clock::now() + std::chrono::seconds(1)));
}

}  // namespace
}  // namespace strata::http
