#pragma once

#include "strata/stats_server.hpp"
#include "strata/status.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace strata {
class DB;
}  // namespace strata

namespace strata::obs {

// What a request is answered from: the database, whether the server listens
// on loopback (which turns on the Host check), and its static files.
struct RouteContext {
  const DB* db = nullptr;
  bool loopback = true;
  const std::vector<StaticFile>* files = nullptr;
};

// The whole HTTP response to one request head: routing, the Host check, and
// every status the endpoint can return. The server thread only moves bytes.
[[nodiscard]] std::string HandleRequest(std::string_view head, const RouteContext& context);

// Whether `files` can be served: absolute paths with no query or fragment,
// no two alike, none hiding /stats or /healthz, and each with a content type.
[[nodiscard]] Status ValidateStaticFiles(const std::vector<StaticFile>& files);

}  // namespace strata::obs
