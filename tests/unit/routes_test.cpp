// Request routing for the stats endpoint, without sockets.

#include "obs/routes.hpp"

#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace strata::obs {
namespace {

std::vector<StaticFile> Files() {
  return {
      {.path = "/", .content_type = "text/html; charset=utf-8", .body = "<p>page</p>"},
      {.path = "/app.js", .content_type = "text/javascript; charset=utf-8", .body = "run()"},
  };
}

std::string Request(const std::vector<StaticFile>& files, const std::string& method,
                    const std::string& path, const std::string& host = "127.0.0.1:8080") {
  // Static files never reach the database, so none is needed here.
  const RouteContext context{.db = nullptr, .loopback = true, .files = &files};
  return HandleRequest(method + " " + path + " HTTP/1.1\r\nHost: " + host + "\r\n\r\n", context);
}

bool HasHeader(const std::string& response, const std::string& header) {
  const std::size_t end = response.find("\r\n\r\n");
  return response.substr(0, end).find("\r\n" + header) != std::string::npos;
}

TEST(RoutesTest, ServesAStaticFileWithItsTypeAndBody) {
  const std::string response = Request(Files(), "GET", "/app.js");
  EXPECT_TRUE(response.starts_with("HTTP/1.1 200 OK\r\n")) << response;
  EXPECT_TRUE(HasHeader(response, "Content-Type: text/javascript; charset=utf-8\r\n"));
  EXPECT_TRUE(response.ends_with("\r\n\r\nrun()"));
}

TEST(RoutesTest, AStaticFileMayOnlyLoadAndCallItsOwnServer) {
  const std::string response = Request(Files(), "GET", "/");
  EXPECT_TRUE(HasHeader(response,
                        "Content-Security-Policy: default-src 'none'; script-src 'self'; "
                        "style-src 'self'; connect-src 'self';"))
      << response;
  EXPECT_TRUE(HasHeader(response, "X-Frame-Options: DENY\r\n"));
  EXPECT_TRUE(HasHeader(response, "Referrer-Policy: no-referrer\r\n"));
  EXPECT_TRUE(HasHeader(response, "X-Content-Type-Options: nosniff\r\n"));
  EXPECT_TRUE(HasHeader(response, "Cache-Control: no-store\r\n"));
}

TEST(RoutesTest, HeadOfAStaticFileSendsItsLengthButNoBody) {
  const std::string response = Request(Files(), "HEAD", "/app.js");
  EXPECT_TRUE(response.starts_with("HTTP/1.1 200 OK\r\n"));
  EXPECT_TRUE(HasHeader(response, "Content-Length: 5\r\n"));
  EXPECT_TRUE(response.ends_with("\r\n\r\n"));
}

TEST(RoutesTest, TheQueryStringDoesNotChangeWhichFileIsServed) {
  EXPECT_TRUE(Request(Files(), "GET", "/app.js?v=2").ends_with("run()"));
}

TEST(RoutesTest, PathsMatchExactly) {
  EXPECT_TRUE(Request(Files(), "GET", "/app.js/").starts_with("HTTP/1.1 404"));
  EXPECT_TRUE(Request(Files(), "GET", "/APP.js").starts_with("HTTP/1.1 404"));
  EXPECT_TRUE(Request(Files(), "GET", "/index.html").starts_with("HTTP/1.1 404"));
  EXPECT_TRUE(Request({}, "GET", "/").starts_with("HTTP/1.1 404"));
}

TEST(RoutesTest, StaticFilesGetTheSameHostCheckAsTheStats) {
  // Without it, a page could rebind its DNS name to 127.0.0.1 and read the
  // dashboard -- and, through it, nothing more than /stats already shows.
  EXPECT_TRUE(Request(Files(), "GET", "/", "attacker.example").starts_with("HTTP/1.1 403"));
  EXPECT_TRUE(Request(Files(), "GET", "/", "localhost:9").starts_with("HTTP/1.1 200"));
}

TEST(RoutesTest, OnlyGetAndHeadReachAStaticFile) {
  EXPECT_TRUE(Request(Files(), "POST", "/").starts_with("HTTP/1.1 405"));
}

TEST(RoutesTest, ValidFilesPassValidation) {
  EXPECT_TRUE(ValidateStaticFiles(Files()).ok());
  EXPECT_TRUE(ValidateStaticFiles({}).ok());
}

TEST(RoutesTest, ValidationRefusesFilesThatCouldNotBeServedOrWouldHideAnEndpoint) {
  const auto with = [](std::string path, std::string type = "text/plain") {
    return std::vector<StaticFile>{{.path = std::move(path), .content_type = std::move(type)}};
  };
  EXPECT_TRUE(ValidateStaticFiles(with("app.js")).IsInvalidArgument());
  EXPECT_TRUE(ValidateStaticFiles(with("")).IsInvalidArgument());
  EXPECT_TRUE(ValidateStaticFiles(with("/a?b")).IsInvalidArgument());
  EXPECT_TRUE(ValidateStaticFiles(with("/a#b")).IsInvalidArgument());
  EXPECT_TRUE(ValidateStaticFiles(with("/stats")).IsInvalidArgument());
  EXPECT_TRUE(ValidateStaticFiles(with("/healthz")).IsInvalidArgument());
  EXPECT_TRUE(ValidateStaticFiles(with("/a", "")).IsInvalidArgument());
  EXPECT_TRUE(ValidateStaticFiles(with("/a", "text/plain\r\nSet-Cookie: x=1")).IsInvalidArgument())
      << "a content type must not inject a header";
  std::vector<StaticFile> twice = Files();
  twice.push_back(twice.front());
  EXPECT_TRUE(ValidateStaticFiles(twice).IsInvalidArgument());
}

}  // namespace
}  // namespace strata::obs
