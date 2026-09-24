// A small stand-in for libFuzzer, used when the harness is built with GCC.
//
// Every fuzz target in tests/fuzz implements the standard libFuzzer entry point
// LLVMFuzzerTestOneInput, plus StrataFuzzSeeds(), a handful of well-formed
// inputs. Under Clang the target links libFuzzer and gets coverage-guided
// search. Under GCC -- the only compiler on the development machine -- it
// links this driver instead, which:
//
//   1. runs every seed and every file named on the command line, then
//   2. applies random mutations (bit flips, byte overwrites with interesting
//      values, truncation, insertion, duplication, splicing) to seeds for a
//      fixed number of iterations or seconds.
//
// No coverage guidance, so it finds less than libFuzzer. It still catches the
// crash class that matters most for decoders -- reads past a buffer on a
// malformed length -- especially when built with AddressSanitizer.
//
// Usage: <target> [--iterations N] [--seconds S] [--seed X] [corpus files...]

#include "strata/env.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);
std::vector<std::string> StrataFuzzSeeds();

namespace {

std::string ReadFile(const std::string& path) {
  std::string contents;
  auto file = strata::Env::Default()->NewSequentialFile(path);
  if (!file.ok()) {
    std::cerr << "fuzz driver: " << file.status().ToString() << '\n';
    std::exit(2);
  }
  std::string scratch(64 * 1024, '\0');
  while (true) {
    auto chunk = (*file)->Read(scratch.size(), scratch.data());
    if (!chunk.ok() || chunk->empty()) {
      break;
    }
    contents.append(*chunk);
  }
  return contents;
}

void RunOne(const std::string& input) {
  static_cast<void>(
      LLVMFuzzerTestOneInput(reinterpret_cast<const std::uint8_t*>(input.data()), input.size()));
}

std::string Mutate(std::string input, const std::vector<std::string>& pool, std::mt19937_64& rng) {
  constexpr std::uint8_t kInteresting[] = {0x00, 0x01, 0x7F, 0x80, 0xFE, 0xFF};
  const int rounds = 1 + static_cast<int>(rng() % 8);
  for (int round = 0; round < rounds; ++round) {
    const auto position = [&] { return input.empty() ? 0 : rng() % input.size(); };
    switch (rng() % 7) {
      case 0:  // flip one bit
        if (!input.empty()) {
          const std::size_t at = position();
          input[at] = static_cast<char>(static_cast<unsigned>(input[at]) ^ (1U << (rng() % 8)));
        }
        break;
      case 1:  // overwrite with an interesting byte
        if (!input.empty()) {
          input[position()] = static_cast<char>(kInteresting[rng() % std::size(kInteresting)]);
        }
        break;
      case 2:  // truncate
        input.resize(position());
        break;
      case 3:  // insert random bytes
        input.insert(position(), std::string(1 + rng() % 16, static_cast<char>(rng())));
        break;
      case 4:  // duplicate a chunk
        if (!input.empty()) {
          const std::size_t start = position();
          const std::size_t length = 1 + rng() % std::min<std::size_t>(64, input.size() - start);
          input.insert(position(), input.substr(start, length));
        }
        break;
      case 5:  // splice in part of another seed
        if (!pool.empty()) {
          const std::string& other = pool[rng() % pool.size()];
          if (!other.empty()) {
            const std::size_t from = rng() % other.size();
            input.replace(position(), rng() % 64, other.substr(from, rng() % 256));
          }
        }
        break;
      default:  // random byte
        if (!input.empty()) {
          input[position()] = static_cast<char>(rng());
        }
        break;
    }
  }
  return input;
}

}  // namespace

int main(int argc, char** argv) {
  const std::span<char*> args(argv, static_cast<std::size_t>(argc));
  std::uint64_t iterations = 10000;
  double seconds = 0;  // 0 = bounded by iterations only
  std::uint64_t seed = 1;
  std::vector<std::string> pool = StrataFuzzSeeds();

  for (std::size_t index = 1; index < args.size(); ++index) {
    const std::string_view arg = args[index];
    const bool has_value = index + 1 < args.size();
    if (arg == "--iterations" && has_value) {
      iterations = std::strtoull(args[++index], nullptr, 10);
    } else if (arg == "--seconds" && has_value) {
      seconds = std::strtod(args[++index], nullptr);
      iterations = UINT64_MAX;
    } else if (arg == "--seed" && has_value) {
      seed = std::strtoull(args[++index], nullptr, 10);
    } else {
      pool.push_back(ReadFile(std::string(arg)));
    }
  }

  for (const auto& input : pool) {
    RunOne(input);
  }

  std::mt19937_64 rng(seed);
  const auto start = std::chrono::steady_clock::now();
  std::uint64_t runs = 0;
  for (; runs < iterations; ++runs) {
    if (seconds > 0 && (runs & 0xFFU) == 0 &&
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() > seconds) {
      break;
    }
    const std::string& base = pool.empty() ? std::string() : pool[rng() % pool.size()];
    RunOne(Mutate(base, pool, rng));
  }
  std::cout << "fuzz driver: " << pool.size() << " seeds, " << runs << " mutated inputs, seed "
            << seed << ", no crashes\n";
  return 0;
}
