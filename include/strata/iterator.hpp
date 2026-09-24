#pragma once

#include "strata/slice.hpp"
#include "strata/status.hpp"

namespace strata {

// A cursor over sorted key/value pairs.
//
// The engine uses one interface for every sorted source -- the memtable, an
// SSTable, a merge of several of them -- so each layer composes the ones
// below it without knowing what they are.
//
// key() and value() return Slices that stay valid only until the iterator is
// moved or destroyed; copy them to keep them. Iterators are not thread-safe.
class Iterator {
 public:
  Iterator() = default;
  virtual ~Iterator() = default;
  Iterator(const Iterator&) = delete;
  Iterator& operator=(const Iterator&) = delete;
  Iterator(Iterator&&) = delete;
  Iterator& operator=(Iterator&&) = delete;

  // True when positioned at an entry. Every other accessor requires Valid().
  [[nodiscard]] virtual bool Valid() const = 0;

  virtual void SeekToFirst() = 0;
  virtual void SeekToLast() = 0;

  // Positions at the first entry whose key is >= target.
  virtual void Seek(Slice target) = 0;

  virtual void Next() = 0;
  virtual void Prev() = 0;

  [[nodiscard]] virtual Slice key() const = 0;
  [[nodiscard]] virtual Slice value() const = 0;

  // Ok unless reading the underlying data failed. An iterator that hit an
  // error becomes !Valid(), and status() says why -- so "reached the end" and
  // "stopped because of corruption" are never confused.
  [[nodiscard]] virtual Status status() const = 0;
};

}  // namespace strata
