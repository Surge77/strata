#pragma once

#include "strata/result.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace strata {

class DB;

// A file the server serves as it is, such as the pages of a dashboard that
// reads /stats (stratactl serve adds the one in tools/dashboard).
struct StaticFile {
  std::string path;          // absolute, e.g. "/" or "/app.js"; not /stats or /healthz
  std::string content_type;  // e.g. "text/html; charset=utf-8"
  std::string body;
};

struct StatsServerOptions {
  // An IPv4 or IPv6 address to listen on. The default serves this machine
  // only. On a loopback address, requests must also name a loopback Host
  // (see docs/observability.md); any other address serves anyone who can
  // reach it, so put it behind something that authenticates.
  std::string bind_address = "127.0.0.1";
  // 0 picks a free port; port() reports which.
  std::uint16_t port = 0;
  // Served at their paths, with a Content-Security-Policy that lets a page
  // load only this server's files and connect only to this server.
  std::vector<StaticFile> files;
};

// A small HTTP server for a database's stats, read-only:
//
//   GET /stats     DB::GetStats as JSON (StatsToJson)
//   GET /healthz   "ok", or 503 once the database refuses writes
//   GET <path>     one of StatsServerOptions::files
//
// HEAD works on both. Requests are served one at a time on the server's own
// thread, with a 2-second limit on each whole exchange, so a slow client
// delays others by at most about that. It is meant for an operator or a
// dashboard, not for traffic.
class StatsServer {
 public:
  // Starts serving `db`. The server calls `db` from its own thread, so the
  // rule in db.hpp applies: `db` must not be closed, destroyed, reopened, or
  // moved while the server runs. Stop() (or destroy) the server first. A
  // `db` that was already closed when Start was called answers 503.
  [[nodiscard]] static Result<std::unique_ptr<StatsServer>> Start(
      const DB* db, const StatsServerOptions& options = {});

  ~StatsServer();
  StatsServer(const StatsServer&) = delete;
  StatsServer& operator=(const StatsServer&) = delete;
  StatsServer(StatsServer&&) = delete;
  StatsServer& operator=(StatsServer&&) = delete;

  [[nodiscard]] std::uint16_t port() const noexcept;

  // Stops accepting, finishes the request in progress, and joins the thread.
  // The destructor does this too.
  void Stop();

  class Impl;

 private:
  explicit StatsServer(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace strata
