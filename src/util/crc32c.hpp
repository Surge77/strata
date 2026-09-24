#pragma once

#include "strata/slice.hpp"

#include <cstdint>

namespace strata::crc32c {

// CRC-32C (Castagnoli, polynomial 0x1EDC6F41 / reflected 0x82F63B78).
//
// Every durable structure in this engine carries one: WAL record headers,
// SSTable blocks, and the manifest. Castagnoli rather than the more familiar
// CRC-32 because x86-64 implements it as a single instruction, so checksumming
// does not become the bottleneck of the write path, and because its error
// detection is better for the short records the WAL writes.

// Checksum of a single buffer.
[[nodiscard]] std::uint32_t Value(Slice data) noexcept;

// Checksum of data logically appended to whatever produced prior_crc. Lets a
// checksum be computed across a buffer that arrives in pieces.
[[nodiscard]] std::uint32_t Extend(std::uint32_t prior_crc, Slice data) noexcept;

// True when this process is using the hardware CRC instruction. Exposed only so
// tests can assert both implementations agree on the same machine.
[[nodiscard]] bool HasHardwareSupport() noexcept;

// Software implementation, always available. Public so tests can compare it
// against the hardware path rather than trusting one of them.
[[nodiscard]] std::uint32_t ExtendPortable(std::uint32_t prior_crc, Slice data) noexcept;

// A checksum is often stored immediately after the bytes it covers. Storing it
// raw makes it possible for a later checksum to be computed over a buffer that
// already contains one, where a systematic corruption can cancel out. Rotating
// and offsetting the value before it is written removes that coincidence.
//
// Unmask(Mask(c)) == c for every c.
inline constexpr std::uint32_t kMaskDelta = 0xA282EAD8U;

[[nodiscard]] inline constexpr std::uint32_t Mask(std::uint32_t crc) noexcept {
  return ((crc >> 15) | (crc << 17)) + kMaskDelta;
}

[[nodiscard]] inline constexpr std::uint32_t Unmask(std::uint32_t masked_crc) noexcept {
  const std::uint32_t rotated = masked_crc - kMaskDelta;
  return (rotated >> 17) | (rotated << 15);
}

}  // namespace strata::crc32c
