#include "util/crc32c.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

#if defined(__x86_64__) || defined(__i386__)
#define STRATA_CRC32C_X86 1
#include <nmmintrin.h>
#else
#define STRATA_CRC32C_X86 0
#endif

namespace strata::crc32c {
namespace {

// Reflected form of the Castagnoli polynomial. The reflected form is used
// because the algorithm below shifts right, feeding the low-order bit first,
// which matches how the hardware instruction and every other CRC-32C
// implementation define the value.
constexpr std::uint32_t kReflectedPolynomial = 0x82F63B78U;

constexpr std::array<std::uint32_t, 256> MakeTable() {
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t index = 0; index < 256; ++index) {
    std::uint32_t remainder = index;
    for (int bit = 0; bit < 8; ++bit) {
      remainder =
          ((remainder & 1U) != 0U) ? ((remainder >> 1U) ^ kReflectedPolynomial) : (remainder >> 1U);
    }
    table[index] = remainder;
  }
  return table;
}

constexpr std::array<std::uint32_t, 256> kTable = MakeTable();

#if STRATA_CRC32C_X86

// SSE 4.2 provides crc32 against exactly this polynomial, one instruction per
// 8 bytes. The target attribute lets this function be compiled with the
// instruction available even though the translation unit as a whole is not,
// which is what makes runtime dispatch possible from a portable binary.
__attribute__((target("sse4.2"))) std::uint32_t ExtendHardware(std::uint32_t prior_crc,
                                                               Slice data) noexcept {
  std::uint64_t crc = ~prior_crc;
  const auto* cursor = reinterpret_cast<const std::uint8_t*>(data.data());
  std::size_t remaining = data.size();

  // Align to 8 bytes first so the wide loop never straddles a cache line more
  // than it must.
  while (remaining > 0 && (reinterpret_cast<std::uintptr_t>(cursor) & 7U) != 0U) {
    crc = _mm_crc32_u8(static_cast<std::uint32_t>(crc), *cursor);
    ++cursor;
    --remaining;
  }
  while (remaining >= 8) {
    std::uint64_t chunk = 0;
    __builtin_memcpy(&chunk, cursor, sizeof(chunk));
    crc = _mm_crc32_u64(crc, chunk);
    cursor += 8;
    remaining -= 8;
  }
  while (remaining > 0) {
    crc = _mm_crc32_u8(static_cast<std::uint32_t>(crc), *cursor);
    ++cursor;
    --remaining;
  }
  return ~static_cast<std::uint32_t>(crc);
}

bool DetectHardware() noexcept {
  return __builtin_cpu_supports("sse4.2");
}

#else

bool DetectHardware() noexcept {
  return false;
}

#endif  // STRATA_CRC32C_X86

// Queried once. CPU feature support cannot change during a process lifetime, so
// the branch in Extend predicts perfectly after the first call.
const bool kHardware = DetectHardware();

}  // namespace

std::uint32_t ExtendPortable(std::uint32_t prior_crc, Slice data) noexcept {
  std::uint32_t crc = ~prior_crc;
  for (const char byte : data) {
    const auto index = static_cast<std::uint8_t>(crc ^ static_cast<std::uint8_t>(byte));
    crc = kTable[index] ^ (crc >> 8U);
  }
  return ~crc;
}

std::uint32_t Extend(std::uint32_t prior_crc, Slice data) noexcept {
#if STRATA_CRC32C_X86
  if (kHardware) {
    return ExtendHardware(prior_crc, data);
  }
#endif
  return ExtendPortable(prior_crc, data);
}

std::uint32_t Value(Slice data) noexcept {
  return Extend(0, data);
}

bool HasHardwareSupport() noexcept {
  return kHardware;
}

}  // namespace strata::crc32c
