#pragma once

#include "strata/stats_server.hpp"

#include <vector>

namespace strata::dashboard {

// The dashboard in tools/dashboard, ready for StatsServerOptions::files: a
// page at "/" and the scripts and styles it loads. It reads nothing but
// /stats, so it shows what any other client of the JSON API could.
[[nodiscard]] std::vector<StaticFile> Files();

}  // namespace strata::dashboard
