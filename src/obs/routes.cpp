#include "obs/routes.hpp"

#include "strata/db.hpp"
#include "strata/stats.hpp"

#include <set>
#include <string>

#include "obs/http.hpp"

namespace strata::obs {
namespace {

constexpr std::string_view kJson = "application/json";
constexpr std::string_view kText = "text/plain; charset=utf-8";

// For static files, which may be a page: it may load only this server's own
// scripts and styles and talk only to this server, and no other page may
// frame it. Everything else is refused, including inline script, so text
// that reaches the page through the stats (a file name in an event) can
// never run as code.
constexpr std::string_view kStaticHeaders =
    "Content-Security-Policy: default-src 'none'; script-src 'self'; style-src 'self'; "
    "connect-src 'self'; img-src 'self'; base-uri 'none'; form-action 'none'; "
    "frame-ancestors 'none'\r\n"
    "X-Frame-Options: DENY\r\n"
    "Referrer-Policy: no-referrer\r\n";

bool IsReservedPath(std::string_view path) {
  return path == "/stats" || path == "/healthz";
}

std::string StatsResponse(const DB* db, bool head_only) {
  const Result<Stats> stats = db->GetStats();
  if (!stats.ok()) {
    return http::Response(503, kText, stats.status().ToString() + "\n", head_only);
  }
  return http::Response(200, kJson, StatsToJson(*stats), head_only);
}

std::string HealthResponse(const DB* db, bool head_only) {
  const Result<Stats> stats = db->GetStats();
  if (!stats.ok()) {
    return http::Response(503, kText, stats.status().ToString() + "\n", head_only);
  }
  if (!stats->background_error.empty()) {
    return http::Response(503, kText, stats->background_error + "\n", head_only);
  }
  return http::Response(200, kText, "ok\n", head_only);
}

}  // namespace

std::string HandleRequest(std::string_view head, const RouteContext& context) {
  const auto request = http::ParseRequestHead(head);
  if (!request.has_value()) {
    return http::Response(400, kText, "malformed request\n");
  }
  // HTTP/1.1 requires Host; HTTP/1.0 clients (curl -0, scripts) may omit it.
  if (!request->host.has_value() && request->version == "HTTP/1.1") {
    return http::Response(400, kText, "HTTP/1.1 requires a Host header\n");
  }
  if (context.loopback && request->host.has_value() && !http::IsLoopbackHost(*request->host)) {
    return http::Response(403, kText, "this server answers only to a loopback Host\n");
  }
  const bool head_only = request->method == "HEAD";
  if (request->method != "GET" && !head_only) {
    return http::Response(405, kText, "only GET and HEAD are supported\n", false,
                          "Allow: GET, HEAD\r\n");
  }
  if (request->path == "/stats") {
    return StatsResponse(context.db, head_only);
  }
  if (request->path == "/healthz") {
    return HealthResponse(context.db, head_only);
  }
  if (context.files != nullptr) {
    for (const StaticFile& file : *context.files) {
      if (file.path == request->path) {
        return http::Response(200, file.content_type, file.body, head_only, kStaticHeaders);
      }
    }
  }
  return http::Response(404, kText, "not found\n", head_only);
}

Status ValidateStaticFiles(const std::vector<StaticFile>& files) {
  std::set<std::string_view> seen;
  for (const StaticFile& file : files) {
    if (!file.path.starts_with('/') || file.path.find_first_of("?#") != std::string::npos) {
      return Status::InvalidArgument("static file path must be absolute, without ? or #: " +
                                     file.path);
    }
    if (IsReservedPath(file.path)) {
      return Status::InvalidArgument("static file would hide " + file.path);
    }
    if (!seen.insert(file.path).second) {
      return Status::InvalidArgument("two static files at " + file.path);
    }
    if (file.content_type.empty() || file.content_type.find_first_of("\r\n") != std::string::npos) {
      return Status::InvalidArgument("static file needs a one-line content type: " + file.path);
    }
  }
  return Status::Ok();
}

}  // namespace strata::obs
