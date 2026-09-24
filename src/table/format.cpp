#include "table/format.hpp"

#include <cstring>

#include "util/coding.hpp"
#include "util/crc32c.hpp"

namespace strata::table {

void BlockHandle::EncodeTo(std::string* destination) const {
  PutVarint64(destination, offset_);
  PutVarint64(destination, size_);
}

Status BlockHandle::DecodeFrom(Slice* input) {
  if (!GetVarint64(input, &offset_) || !GetVarint64(input, &size_)) {
    return Status::Corruption("bad block handle");
  }
  return Status::Ok();
}

void Footer::EncodeTo(std::string* destination) const {
  const std::size_t start = destination->size();
  metaindex_handle.EncodeTo(destination);
  index_handle.EncodeTo(destination);
  destination->resize(start + 2 * BlockHandle::kMaxEncodedLength);  // zero padding
  PutFixed64(destination, kTableMagicNumber);
}

Status Footer::DecodeFrom(Slice input) {
  if (input.size() != kEncodedLength) {
    return Status::Corruption("footer has the wrong size");
  }
  if (DecodeFixed64(input.data() + kEncodedLength - 8) != kTableMagicNumber) {
    return Status::Corruption("not an SSTable (bad magic number)");
  }
  Slice handles = input.substr(0, 2 * BlockHandle::kMaxEncodedLength);
  STRATA_RETURN_IF_ERROR(metaindex_handle.DecodeFrom(&handles));
  return index_handle.DecodeFrom(&handles);
}

Result<std::string> ReadBlock(const RandomAccessFile& file, std::uint64_t file_size,
                              const BlockHandle& handle) {
  // Written to avoid overflow: offset and size are untrusted 64-bit values.
  if (handle.offset() > file_size || handle.size() > file_size - handle.offset() ||
      kBlockTrailerSize > file_size - handle.offset() - handle.size()) {
    return Status::Corruption("block handle points outside the file");
  }
  const std::size_t length = handle.size();
  std::string buffer(length + kBlockTrailerSize, '\0');
  STRATA_ASSIGN_OR_RETURN(const Slice read,
                          file.Read(handle.offset(), buffer.size(), buffer.data()));
  if (read.size() != buffer.size()) {
    return Status::Corruption("block is shorter than its handle claims");
  }

  const char* const data = read.data();
  const auto type = static_cast<std::uint8_t>(data[length]);
  const std::uint32_t expected = crc32c::Unmask(DecodeFixed32(data + length + 1));
  const std::uint32_t actual = crc32c::Value(Slice(data, length + 1));
  if (actual != expected) {
    return Status::Corruption("block checksum mismatch");
  }
  if (type != static_cast<std::uint8_t>(CompressionType::kNone)) {
    return Status::Corruption("unknown block compression type");
  }

  // Read() may return a Slice pointing somewhere other than the scratch buffer
  // (e.g. an mmap); copy only in that case.
  if (data != buffer.data()) {
    std::memcpy(buffer.data(), data, length);
  }
  buffer.resize(length);
  return buffer;
}

}  // namespace strata::table
