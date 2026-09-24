#include "util/coding.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace strata {
namespace {

constexpr std::uint8_t kContinuationBit = 0x80;
constexpr std::uint8_t kPayloadMask = 0x7F;
constexpr int kPayloadBits = 7;

}  // namespace

void PutFixed32(std::string* destination, std::uint32_t value) {
  std::array<char, sizeof(value)> buffer{};
  EncodeFixed32(buffer.data(), value);
  destination->append(buffer.data(), buffer.size());
}

void PutFixed64(std::string* destination, std::uint64_t value) {
  std::array<char, sizeof(value)> buffer{};
  EncodeFixed64(buffer.data(), value);
  destination->append(buffer.data(), buffer.size());
}

int VarintLength(std::uint64_t value) noexcept {
  int length = 1;
  while (value >= kContinuationBit) {
    value >>= kPayloadBits;
    ++length;
  }
  return length;
}

namespace {

// One encoder for both widths: the loop is identical and only the argument type
// differs, so writing it twice would be duplication rather than clarity.
template <typename T>
char* EncodeVarint(char* destination, T value) noexcept {
  auto* out = reinterpret_cast<std::uint8_t*>(destination);
  while (value >= kContinuationBit) {
    *out = static_cast<std::uint8_t>(static_cast<std::uint8_t>(value & kPayloadMask) |
                                     kContinuationBit);
    ++out;
    value >>= kPayloadBits;
  }
  *out = static_cast<std::uint8_t>(value);
  ++out;
  return reinterpret_cast<char*>(out);
}

}  // namespace

char* EncodeVarint32(char* destination, std::uint32_t value) noexcept {
  return EncodeVarint(destination, value);
}

char* EncodeVarint64(char* destination, std::uint64_t value) noexcept {
  return EncodeVarint(destination, value);
}

void PutVarint32(std::string* destination, std::uint32_t value) {
  std::array<char, kMaxVarint32Length> buffer{};
  char* const end = EncodeVarint32(buffer.data(), value);
  destination->append(buffer.data(), static_cast<std::size_t>(end - buffer.data()));
}

void PutVarint64(std::string* destination, std::uint64_t value) {
  std::array<char, kMaxVarint64Length> buffer{};
  char* const end = EncodeVarint64(buffer.data(), value);
  destination->append(buffer.data(), static_cast<std::size_t>(end - buffer.data()));
}

namespace {

// max_bytes bounds the encoding length so that a run of bytes with the
// continuation bit set, which is what a corrupt file looks like, can neither
// walk past the buffer nor shift past the width of the result.
template <typename T>
bool GetVarint(Slice* input, T* value, std::size_t max_bytes) noexcept {
  T result = 0;
  unsigned shift = 0;

  for (std::size_t index = 0; index < input->size(); ++index) {
    if (index >= max_bytes) {
      return false;  // over-long encoding
    }
    const auto byte = static_cast<std::uint8_t>((*input)[index]);
    result |= static_cast<T>(static_cast<T>(byte & kPayloadMask) << shift);
    if ((byte & kContinuationBit) == 0) {
      *value = result;
      input->remove_prefix(index + 1);
      return true;
    }
    shift += static_cast<unsigned>(kPayloadBits);
  }
  return false;  // truncated: input ended with the continuation bit still set
}

}  // namespace

bool GetVarint32(Slice* input, std::uint32_t* value) noexcept {
  return GetVarint(input, value, static_cast<std::size_t>(kMaxVarint32Length));
}

bool GetVarint64(Slice* input, std::uint64_t* value) noexcept {
  return GetVarint(input, value, static_cast<std::size_t>(kMaxVarint64Length));
}

void PutLengthPrefixedSlice(std::string* destination, Slice value) {
  PutVarint64(destination, value.size());
  destination->append(value.data(), value.size());
}

bool GetLengthPrefixedSlice(Slice* input, Slice* value) noexcept {
  Slice cursor = *input;
  std::uint64_t length = 0;
  if (!GetVarint64(&cursor, &length)) {
    return false;
  }
  // Compared as uint64 before any narrowing, so a length near 2^64 cannot wrap
  // into something that looks satisfiable.
  if (length > cursor.size()) {
    return false;
  }
  const auto size = static_cast<std::size_t>(length);
  *value = cursor.substr(0, size);
  cursor.remove_prefix(size);
  *input = cursor;
  return true;
}

}  // namespace strata
