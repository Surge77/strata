// strata::DB: the public handle, forwarding to DBImpl.

#include "strata/db.hpp"

#include <utility>

#include "db/db_impl.hpp"
#include "db/db_iter.hpp"

namespace strata {
namespace {

Status NotOpen() {
  return Status::InvalidArgument("database is not open");
}

}  // namespace

DB::DB() = default;
DB::~DB() {
  static_cast<void>(Close());
}
DB::DB(DB&& other) noexcept = default;

DB& DB::operator=(DB&& other) noexcept {
  if (this != &other) {
    static_cast<void>(Close());
    impl_ = std::move(other.impl_);
  }
  return *this;
}

Status DB::Open(const Options& options, DB* db) {
  if (db->is_open()) {
    return Status::InvalidArgument("DB::Open: the target DB is already open");
  }
  STRATA_ASSIGN_OR_RETURN(db->impl_, DBImpl::Open(options));
  return Status::Ok();
}

Status DB::Put(Slice key, Slice value, const WriteOptions& options) {
  WriteBatch batch;
  batch.Put(key, value);
  return Write(batch, options);
}

Status DB::Delete(Slice key, const WriteOptions& options) {
  WriteBatch batch;
  batch.Delete(key);
  return Write(batch, options);
}

Status DB::Write(const WriteBatch& batch, const WriteOptions& options) {
  return impl_ != nullptr ? impl_->Write(options, batch) : NotOpen();
}

Result<std::string> DB::Get(Slice key, const ReadOptions& options) const {
  if (impl_ == nullptr) {
    return NotOpen();
  }
  return impl_->Get(options, key);
}

std::unique_ptr<Iterator> DB::NewIterator(const ReadOptions& options) const {
  if (impl_ == nullptr) {
    return NewErrorIterator(NotOpen());
  }
  return impl_->NewIterator(options);
}

const Snapshot* DB::GetSnapshot() {
  return impl_ != nullptr ? impl_->GetSnapshot() : nullptr;
}

Status DB::ReleaseSnapshot(const Snapshot* snapshot) {
  return impl_ != nullptr ? impl_->ReleaseSnapshot(snapshot) : NotOpen();
}

Status DB::Close() {
  if (impl_ == nullptr) {
    return Status::Ok();
  }
  Status status = impl_->Close();
  impl_ = nullptr;
  return status;
}

}  // namespace strata
