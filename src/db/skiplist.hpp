#pragma once

#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>

#include "util/arena.hpp"
#include "util/random.hpp"

namespace strata {

// An ordered set built for the memtable: one writer, any number of concurrent
// readers that never take a lock.
//
// Concurrency contract:
//   * Insert must be externally serialised (the DB holds its write mutex).
//   * Readers need no synchronisation at all. A reader sees every key inserted
//     before it started, and possibly some inserted during its scan.
//   * Nodes are never removed while the list lives; memory is owned by the
//     Arena and freed all at once when the memtable is dropped. That is what
//     lets readers traverse without reference counts or hazard pointers.
//
// How readers stay safe: a node is fully constructed -- key set, every next
// pointer initialised -- before a release store links it into level 0, and
// readers follow links with acquire loads. So a reader that can reach a node
// also sees everything written to it before it was linked.
//
// Keys are copied by value and compared with `Comparator`, which must provide
// `int operator()(const Key&, const Key&) const`. The memtable uses a pointer
// into arena memory as its Key, so copying one is cheap.
template <typename Key, class Comparator>
class SkipList {
  // Nodes live in Arena memory, which is freed wholesale without running
  // destructors, and is aligned only to max_align_t.
  static_assert(std::is_trivially_destructible_v<Key>,
                "SkipList never destroys keys; Key must be trivially destructible");
  static_assert(alignof(Key) <= alignof(std::max_align_t),
                "Arena memory is only aligned to max_align_t");

 public:
  // `arena` must outlive the list.
  SkipList(Comparator compare, Arena* arena)
      : compare_(compare), arena_(arena), head_(NewNode(Key{}, kMaxHeight)), random_(0xdeadbeef) {}

  SkipList(const SkipList&) = delete;
  SkipList& operator=(const SkipList&) = delete;
  SkipList(SkipList&&) = delete;
  SkipList& operator=(SkipList&&) = delete;
  ~SkipList() = default;

  // Precondition: nothing equal to `key` is in the list. The memtable
  // guarantees this because every entry carries a unique sequence number.
  void Insert(const Key& key) {
    std::array<Node*, kMaxHeight> previous{};
    Node* const successor = FindGreaterOrEqual(key, previous.data());
    assert(successor == nullptr || !Equal(key, successor->key));
    static_cast<void>(successor);

    const std::size_t height = RandomHeight();
    const std::size_t current_max = MaxHeight();
    if (height > current_max) {
      for (std::size_t level = current_max; level < height; ++level) {
        previous[level] = head_;
      }
      // Relaxed is enough: a reader that sees the new height before the node
      // is linked finds null pointers at the new levels in head_ and simply
      // drops to a lower level.
      max_height_.store(height, std::memory_order_relaxed);
    }

    Node* const node = NewNode(key, height);
    for (std::size_t level = 0; level < height; ++level) {
      // The new node's own pointers need no barrier: nobody can reach the node
      // until the release store on the next line publishes it.
      node->SetNextRelaxed(level, previous[level]->NextRelaxed(level));
      previous[level]->SetNext(level, node);
    }
  }

  [[nodiscard]] bool Contains(const Key& key) const {
    Node* const node = FindGreaterOrEqual(key, nullptr);
    return node != nullptr && Equal(key, node->key);
  }

  // Iteration over a (possibly concurrently growing) list. Not thread-safe
  // itself, but any number of Iterators may run alongside Insert.
  class Iterator {
   public:
    explicit Iterator(const SkipList* list) : list_(list) {}

    [[nodiscard]] bool Valid() const noexcept { return node_ != nullptr; }

    // Precondition: Valid().
    [[nodiscard]] const Key& key() const {
      assert(Valid());
      return node_->key;
    }

    void Next() {
      assert(Valid());
      node_ = node_->Next(0);
    }

    // No back pointers are kept (they would double the write cost for a rare
    // operation), so Prev re-searches from the head: O(log n).
    void Prev() {
      assert(Valid());
      node_ = list_->FindLessThan(node_->key);
      if (node_ == list_->head_) {
        node_ = nullptr;
      }
    }

    // Positions at the first entry >= target.
    void Seek(const Key& target) { node_ = list_->FindGreaterOrEqual(target, nullptr); }

    void SeekToFirst() { node_ = list_->head_->Next(0); }

    void SeekToLast() {
      node_ = list_->FindLast();
      if (node_ == list_->head_) {
        node_ = nullptr;
      }
    }

   private:
    const SkipList* list_;
    typename SkipList::Node* node_ = nullptr;
  };

 private:
  // Height distribution: each level is 1/kBranching as likely as the one
  // below, so a search visits about kBranching nodes per level. 12 levels with
  // branching 4 keeps searches logarithmic up to 4^12 (16.7 million) entries.
  static constexpr std::size_t kMaxHeight = 12;
  static constexpr std::uint64_t kBranching = 4;

  struct Node {
    explicit Node(const Key& k) : key(k) {}

    const Key key;

    Node* Next(std::size_t level) const noexcept {
      return Links()[level].load(std::memory_order_acquire);
    }
    void SetNext(std::size_t level, Node* node) noexcept {
      Links()[level].store(node, std::memory_order_release);
    }
    Node* NextRelaxed(std::size_t level) const noexcept {
      return Links()[level].load(std::memory_order_relaxed);
    }
    void SetNextRelaxed(std::size_t level, Node* node) noexcept {
      Links()[level].store(node, std::memory_order_relaxed);
    }

    // The node's `height` atomic pointers live directly after it in the same
    // arena allocation; NewNode constructs them there with placement new. (A
    // trailing `next_[1]` array indexed past its end, as LevelDB does, is
    // undefined behaviour in C++; this layout is not.) The const_cast is sound:
    // the links are never const objects -- they live in writable arena memory.
    std::atomic<Node*>* Links() const noexcept {
      return std::launder(reinterpret_cast<std::atomic<Node*>*>(const_cast<Node*>(this) + 1));
    }
  };

  Node* NewNode(const Key& key, std::size_t height) {
    static_assert(sizeof(Node) % alignof(std::atomic<Node*>) == 0,
                  "links must be correctly aligned after the node");
    const std::size_t bytes = sizeof(Node) + sizeof(std::atomic<Node*>) * height;
    char* const memory = arena_->AllocateAligned(bytes);
    Node* const node = new (memory) Node(key);
    auto* const links = reinterpret_cast<std::atomic<Node*>*>(node + 1);
    for (std::size_t level = 0; level < height; ++level) {
      new (links + level) std::atomic<Node*>(nullptr);
    }
    return node;
  }

  std::size_t RandomHeight() {
    std::size_t height = 1;
    while (height < kMaxHeight && random_.OneIn(kBranching)) {
      ++height;
    }
    return height;
  }

  [[nodiscard]] std::size_t MaxHeight() const noexcept {
    return max_height_.load(std::memory_order_relaxed);
  }

  [[nodiscard]] bool Equal(const Key& a, const Key& b) const { return compare_(a, b) == 0; }

  // True if `key` sorts after the key stored in `node` (null is "infinitely
  // large", so nothing sorts after it).
  [[nodiscard]] bool KeyIsAfterNode(const Key& key, Node* node) const {
    return node != nullptr && compare_(node->key, key) < 0;
  }

  // The first node >= key, or null. If `previous` is non-null, fills
  // previous[level] with the last node < key at every level -- the splice
  // points Insert needs.
  Node* FindGreaterOrEqual(const Key& key, Node** previous) const {
    Node* node = head_;
    std::size_t level = MaxHeight() - 1;
    while (true) {
      Node* const next = node->Next(level);
      if (KeyIsAfterNode(key, next)) {
        node = next;
        continue;
      }
      if (previous != nullptr) {
        previous[level] = node;
      }
      if (level == 0) {
        return next;
      }
      --level;
    }
  }

  // The last node < key, or head_ if there is none.
  Node* FindLessThan(const Key& key) const {
    Node* node = head_;
    std::size_t level = MaxHeight() - 1;
    while (true) {
      Node* const next = node->Next(level);
      if (next != nullptr && compare_(next->key, key) < 0) {
        node = next;
        continue;
      }
      if (level == 0) {
        return node;
      }
      --level;
    }
  }

  // The last node, or head_ if the list is empty.
  Node* FindLast() const {
    Node* node = head_;
    std::size_t level = MaxHeight() - 1;
    while (true) {
      Node* const next = node->Next(level);
      if (next != nullptr) {
        node = next;
        continue;
      }
      if (level == 0) {
        return node;
      }
      --level;
    }
  }

  Comparator const compare_;
  Arena* const arena_;
  Node* const head_;
  std::atomic<std::size_t> max_height_{1};
  Random random_;  // only touched by the single writer
};

}  // namespace strata
