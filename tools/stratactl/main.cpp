// stratactl -- administration and inspection CLI for a strata database.
//
// Phase 0 ships only `version` and `help`; every later phase adds the command
// that makes its feature demonstrable from a terminal (see docs/roadmap.md).

#include "strata/version.hpp"

#include <iostream>
#include <span>
#include <string_view>

namespace {

constexpr int kExitOk = 0;
constexpr int kExitUsage = 2;

void PrintUsage(std::ostream& out) {
  out << "stratactl " << strata::version_string() << "\n\n"
      << "Usage:\n"
      << "  stratactl <command> [args]\n\n"
      << "Commands:\n"
      << "  version    Print the library version\n"
      << "  help       Print this message\n";
}

}  // namespace

int main(int argc, char** argv) {
  const std::span<char*> args(argv, static_cast<std::size_t>(argc));

  if (args.size() < 2) {
    PrintUsage(std::cerr);
    return kExitUsage;
  }

  const std::string_view command = args[1];

  if (command == "version") {
    std::cout << strata::version_string() << '\n';
    return kExitOk;
  }
  if (command == "help" || command == "--help" || command == "-h") {
    PrintUsage(std::cout);
    return kExitOk;
  }

  std::cerr << "stratactl: unknown command '" << command << "'\n\n";
  PrintUsage(std::cerr);
  return kExitUsage;
}
