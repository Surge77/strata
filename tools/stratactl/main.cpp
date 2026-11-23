// stratactl -- administration and inspection CLI for a strata database.
//
// Every phase adds the command that makes its feature demonstrable from a
// terminal (see docs/roadmap.md). Phase 3: put, get, delete. Phase 4: dump-sst.
// Phase 12: stats, serve.
//
// Exit codes: 0 success, 1 key not found, 2 usage error, 3 database error.

#include "strata/db.hpp"
#include "strata/env.hpp"
#include "strata/stats.hpp"
#include "strata/stats_server.hpp"
#include "strata/version.hpp"

#include <charconv>
#include <csignal>
#include <iostream>
#include <span>
#include <string>
#include <string_view>

#include "dashboard_files.hpp"
#include "db/dbformat.hpp"
#include "table/table.hpp"

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
      << "  dump-sst <file>                   Print an SSTable's entries\n"
      << "  stats <db>                        Print the database's stats as JSON\n"
      << "  serve <db> [--port N] [--bind A]  Serve /stats, /healthz, and a dashboard at /\n"
      << "                                    over HTTP until interrupted (default\n"
      << "                                    127.0.0.1, a free port)\n"
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

// Printable rendering of arbitrary bytes: printable ASCII as-is, else \xNN.
std::string Escape(strata::Slice bytes) {
  constexpr std::string_view kHex = "0123456789abcdef";
  std::string out;
  for (const char c : bytes) {
    const auto byte = static_cast<unsigned char>(c);
    if (byte >= 0x20 && byte < 0x7F && byte != '\\') {
      out.push_back(c);
    } else {
      out += "\\x";
      out.push_back(kHex[byte >> 4U]);
      out.push_back(kHex[byte & 0xFU]);
    }
  }
  return out;
}

int DumpSst(const std::string& path) {
  strata::Env* env = strata::Env::Default();
  auto size = env->GetFileSize(path);
  if (!size.ok()) {
    return Fail(size.status());
  }
  auto file = env->NewRandomAccessFile(path);
  if (!file.ok()) {
    return Fail(file.status());
  }
  static const strata::InternalKeyComparator kComparator(strata::BytewiseComparator());
  auto table =
      strata::table::Table::Open({.comparator = &kComparator}, std::move(file).value(), *size);
  if (!table.ok()) {
    return Fail(table.status());
  }
  auto it = (*table)->NewIterator();
  std::uint64_t count = 0;
  for (it->SeekToFirst(); it->Valid(); it->Next(), ++count) {
    strata::ParsedInternalKey parsed;
    if (!strata::ParseInternalKey(it->key(), &parsed)) {
      std::cout << "'" << Escape(it->key()) << "' => '" << Escape(it->value()) << "'\n";
    } else if (parsed.type == strata::ValueType::kDeletion) {
      std::cout << "'" << Escape(parsed.user_key) << "' @ " << parsed.sequence << " DEL\n";
    } else {
      std::cout << "'" << Escape(parsed.user_key) << "' @ " << parsed.sequence << " PUT => '"
                << Escape(it->value()) << "'\n";
    }
  }
  if (!it->status().ok()) {
    return Fail(it->status());
  }
  std::cout << count << " entries\n";
  return kExitOk;
}

// Opens an existing database: inspecting a path must not create one there.
bool OpenExistingDb(const std::string& path, strata::DB* db, int* exit_code) {
  const strata::Status status = strata::DB::Open({.path = path, .create_if_missing = false}, db);
  if (!status.ok()) {
    *exit_code = Fail(status);
    return false;
  }
  return true;
}

int PrintStats(const std::string& path) {
  strata::DB db;
  int exit_code = kExitOk;
  if (!OpenExistingDb(path, &db, &exit_code)) {
    return exit_code;
  }
  const auto stats = db.GetStats();
  if (!stats.ok()) {
    return Fail(stats.status());
  }
  std::cout << strata::StatsToJson(*stats) << '\n';
  return kExitOk;
}

// serve <db> [--port N] [--bind ADDRESS]
int Serve(std::span<char*> args) {
  if (args.empty()) {
    return kExitUsage;
  }
  strata::StatsServerOptions options;
  for (std::size_t i = 1; i < args.size(); i += 2) {
    const std::string_view flag = args[i];
    if (i + 1 >= args.size()) {
      return kExitUsage;
    }
    const std::string_view value = args[i + 1];
    if (flag == "--bind") {
      options.bind_address = value;
    } else if (flag == "--port") {
      const auto [end, error] =
          std::from_chars(value.data(), value.data() + value.size(), options.port);
      if (error != std::errc{} || end != value.data() + value.size()) {
        return kExitUsage;
      }
    } else {
      return kExitUsage;
    }
  }

  // Blocked before the server starts its thread, which inherits the mask, so
  // SIGINT and SIGTERM reach only the sigwait below and Ctrl-C shuts down
  // cleanly: the server stops and the database closes, syncing its log.
  sigset_t stop_signals;
  sigemptyset(&stop_signals);
  sigaddset(&stop_signals, SIGINT);
  sigaddset(&stop_signals, SIGTERM);
  pthread_sigmask(SIG_BLOCK, &stop_signals, nullptr);

  strata::DB db;
  int exit_code = kExitOk;
  if (!OpenExistingDb(args[0], &db, &exit_code)) {
    return exit_code;
  }
  if (options.bind_address != "127.0.0.1" && options.bind_address != "::1") {
    std::cerr << "stratactl: warning: serving on " << options.bind_address
              << " with no authentication or TLS (see docs/observability.md)\n";
  }
  options.files = strata::dashboard::Files();
  auto server = strata::StatsServer::Start(&db, options);
  if (!server.ok()) {
    return Fail(server.status());
  }
  const bool v6 = options.bind_address.find(':') != std::string::npos;
  const std::string origin = "http://" + std::string(v6 ? "[" : "") + options.bind_address +
                             (v6 ? "]" : "") + ':' + std::to_string((*server)->port());
  std::cout << "serving " << origin << "/stats\n"
            << "dashboard " << origin << "/" << std::endl;
  int signal = 0;
  sigwait(&stop_signals, &signal);
  // A second Ctrl-C during the shutdown below (up to a few seconds) should
  // still end the process, so the default dispositions come back.
  pthread_sigmask(SIG_UNBLOCK, &stop_signals, nullptr);
  (*server)->Stop();
  const strata::Status closed = db.Close();
  return closed.ok() ? kExitOk : Fail(closed);
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
  if (command == "dump-sst") {
    if (args.size() != 3) {
      PrintUsage(std::cerr);
      return kExitUsage;
    }
    return DumpSst(args[2]);
  }
  if (command == "stats" && args.size() == 3) {
    return PrintStats(args[2]);
  }
  if (command == "serve") {
    const int code = Serve(args.subspan(2));
    if (code == kExitUsage) {
      PrintUsage(std::cerr);
    }
    return code;
  }
  if (command == "put" || command == "get" || command == "delete") {
    return RunDataCommand(command, args.subspan(2));
  }

  std::cerr << "stratactl: unknown command '" << command << "'\n\n";
  PrintUsage(std::cerr);
  return kExitUsage;
}
