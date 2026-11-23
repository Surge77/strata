// The dashboard compiled into strata_dashboard is the one in tools/dashboard.

#include "dashboard_files.hpp"

#include <fstream>
#include <map>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

#include "obs/routes.hpp"

namespace strata {
namespace {

std::string ReadSource(const std::string& name) {
  std::ifstream file(std::string(STRATA_DASHBOARD_DIR) + "/" + name, std::ios::binary);
  std::ostringstream contents;
  contents << file.rdbuf();
  return contents.str();
}

TEST(DashboardFilesTest, EmbedsEveryFileWithItsType) {
  std::map<std::string, std::string> types;
  for (const StaticFile& file : dashboard::Files()) {
    types[file.path] = file.content_type;
  }
  const std::map<std::string, std::string> expected = {
      {"/", "text/html; charset=utf-8"},
      {"/dashboard.css", "text/css; charset=utf-8"},
      {"/dashboard.js", "text/javascript; charset=utf-8"},
      {"/model.js", "text/javascript; charset=utf-8"},
      {"/charts.js", "text/javascript; charset=utf-8"},
      {"/favicon.svg", "image/svg+xml"},
  };
  EXPECT_EQ(types, expected);
}

TEST(DashboardFilesTest, EmbeddedBodiesMatchTheSourceFiles) {
  const std::map<std::string, std::string> sources = {
      {"/", "index.html"},
      {"/dashboard.css", "dashboard.css"},
      {"/dashboard.js", "dashboard.js"},
      {"/model.js", "model.js"},
      {"/charts.js", "charts.js"},
      {"/favicon.svg", "favicon.svg"},
  };
  for (const StaticFile& file : dashboard::Files()) {
    const std::string source = ReadSource(sources.at(file.path));
    ASSERT_FALSE(source.empty()) << file.path;
    EXPECT_EQ(file.body, source) << file.path << " is stale: re-run CMake";
  }
}

TEST(DashboardFilesTest, TheServerAcceptsThem) {
  EXPECT_TRUE(obs::ValidateStaticFiles(dashboard::Files()).ok());
}

}  // namespace
}  // namespace strata
