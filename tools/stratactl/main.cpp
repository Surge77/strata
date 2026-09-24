// stratactl -- administration and inspection CLI for a strata database.
//
// Every phase adds the command that makes its feature demonstrable from a
// terminal (see docs/roadmap.md). Phase 3: put, get, delete.
//
// Exit codes: 0 success, 1 key not found, 2 usage error, 3 database error.

#include "strata/db.hpp"
#include "strata/version.hpp"

#include <iostream>
#include <span>
#include <string>
#include <string_view>

namespace {

constexpr int kExitOk = 0;
constexpr int kExitNotFound = 1;
constexpr int kExitUsage = 2;
constexpr int kExitError = 3;

void PrintUsage(std::ostream& out) {
  out << "stratactl " << strata::version_string() << "\n\n"
      << "Usage:\n"
      << "  stratactl <command> [args]\n\n"
      << "Commands:\n"
      << "  put <db> <key> <value> [--sync]   Set a key (--sync: durable against power loss)\n"
      << "  get <db> <key>                    Print a key's value (exit 1 if absent)\n"
      << "  delete <db> <key> [--sync]        Remove a key\n"
      << "  version                           Print the library version\n"
      << "  help                              Print this message\n";
}

int Fail(const strata::Status& status) {
  std::cerr << "stratactl: " << status.ToString() << '\n';
  return kExitError;
}

// Opens the database at `path`, creating it if needed.
bool OpenDb(const std::string& path, strata::DB* db, int* exit_code) {
  const strata::Status status = strata::DB::Open({.path = path}, db);
  if (!status.ok()) {
    *exit_code = Fail(status);
    return false;
  }
  return true;
}

int RunDataCommand(std::string_view command, std::span<char*> args) {
  const bool sync = args.size() > 0 && std::string_view(args.back()) == "--sync";
  const std::size_t positional = args.size() - (sync ? 1 : 0);
  const strata::WriteOptions write_options{.sync = sync};

  strata::DB db;
  int exit_code = kExitOk;

  if (command == "put" && positional == 3) {
    if (!OpenDb(args[0], &db, &exit_code)) {
      return exit_code;
    }
    const strata::Status status = db.Put(args[1], args[2], write_options);
    return status.ok() ? kExitOk : Fail(status);
  }
  if (command == "get" && positional == 2 && !sync) {
    if (!OpenDb(args[0], &db, &exit_code)) {
      return exit_code;
    }
    const strata::Result<std::string> value = db.Get(args[1]);
    if (value.status().IsNotFound()) {
      std::cerr << "stratactl: not found\n";
      return kExitNotFound;
    }
    if (!value.ok()) {
      return Fail(value.status());
    }
    std::cout << *value << '\n';
    return kExitOk;
  }
  if (command == "delete" && positional == 2) {
    if (!OpenDb(args[0], &db, &exit_code)) {
      return exit_code;
    }
    const strata::Status status = db.Delete(args[1], write_options);
    return status.ok() ? kExitOk : Fail(status);
  }

  std::cerr << "stratactl: wrong arguments for '" << command << "'\n\n";
  PrintUsage(std::cerr);
  return kExitUsage;
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
  if (command == "put" || command == "get" || command == "delete") {
    return RunDataCommand(command, args.subspan(2));
  }

  std::cerr << "stratactl: unknown command '" << command << "'\n\n";
  PrintUsage(std::cerr);
  return kExitUsage;
}
