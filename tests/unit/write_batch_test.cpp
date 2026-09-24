#include "strata/write_batch.hpp"

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "db/write_batch_internal.hpp"
#include "util/coding.hpp"

namespace {

using strata::Slice;
using strata::Status;
using strata::WriteBatch;
using strata::WriteBatchInternal;

// Renders a batch as "Put(k, v)Delete(k)..." so tests compare whole sequences.
class Recorder final : public WriteBatch::Handler {
 public:
  void Put(Slice key, Slice value) override {
    log += "Put(" + std::string(key) + ", " + std::string(value) + ")";
  }
  void Delete(Slice key) override { log += "Delete(" + std::string(key) + ")"; }
  std::string log;
};

std::string Render(const WriteBatch& batch, Status* status = nullptr) {
  Recorder recorder;
  const Status result = batch.Iterate(&recorder);
  if (status != nullptr) {
    *status = result;
  } else {
    EXPECT_TRUE(result.ok()) << result.ToString();
  }
  return recorder.log;
}

WriteBatch FromBytes(Slice bytes) {
  WriteBatch batch;
  EXPECT_TRUE(WriteBatchInternal::SetContents(&batch, bytes).ok());
  return batch;
}

TEST(WriteBatch, StartsEmptyWithAHeaderOnly) {
  const WriteBatch batch;
  EXPECT_EQ(batch.Count(), 0U);
  EXPECT_EQ(batch.ApproximateSize(), WriteBatchInternal::kHeaderSize);
  EXPECT_EQ(Render(batch), "");
  EXPECT_EQ(WriteBatchInternal::Sequence(batch), 0U);
}

TEST(WriteBatch, DeliversOperationsInOrder) {
  WriteBatch batch;
  batch.Put("foo", "bar");
  batch.Delete("box");
  batch.Put("baz", "boo");
  EXPECT_EQ(batch.Count(), 3U);
  EXPECT_EQ(Render(batch), "Put(foo, bar)Delete(box)Put(baz, boo)");
}

TEST(WriteBatch, KeysAndValuesAreArbitraryBytes) {
  WriteBatch batch;
  const std::string key("k\0ey", 4);
  const std::string value(100000, '\xff');
  batch.Put(key, value);
  batch.Put("", "");
  struct Check final : WriteBatch::Handler {
    std::vector<std::string> keys;
    std::vector<std::size_t> sizes;
    void Put(Slice k, Slice v) override {
      keys.emplace_back(k);
      sizes.push_back(v.size());
    }
    void Delete(Slice /*key*/) override {}
  } check;
  ASSERT_TRUE(batch.Iterate(&check).ok());
  EXPECT_EQ(check.keys, (std::vector<std::string>{key, ""}));
  EXPECT_EQ(check.sizes, (std::vector<std::size_t>{100000, 0}));
}

// A Slice may claim any size; the batch must refuse to encode one past the
// format's limits rather than truncate its length. The bytes are never read,
// so a tiny buffer can stand in for a 5 GiB key.
TEST(WriteBatch, OversizedOperationsPoisonTheBatchInsteadOfTruncating) {
  const char byte = 'x';
  const Slice huge(&byte, WriteBatch::kMaxValueSize + 1ULL);
  const Slice huge_key(&byte, WriteBatch::kMaxKeySize + 1ULL);

  WriteBatch batch;
  batch.Put("ok", "fine");
  batch.Put("k", huge);
  EXPECT_TRUE(batch.oversized());
  EXPECT_EQ(batch.Count(), 1U);  // the oversized Put was not recorded
  EXPECT_EQ(Render(batch), "Put(ok, fine)");

  WriteBatch deletes;
  deletes.Delete(huge_key);
  EXPECT_TRUE(deletes.oversized());
  WriteBatch puts;
  puts.Put(huge_key, "v");
  EXPECT_TRUE(puts.oversized());

  WriteBatch clean;
  clean.Put("a", "b");
  EXPECT_FALSE(clean.oversized());
  clean.Append(deletes);
  EXPECT_TRUE(clean.oversized()) << "Append must carry the mark";
  clean.Clear();
  EXPECT_FALSE(clean.oversized());
}

// The count is a fixed32. Appending batches whose counts would sum past
// 2^32 - 1 cannot be encoded, so the result is refused as oversized.
TEST(WriteBatch, AppendRefusesACountOverflow) {
  WriteBatch huge;
  huge.Put("a", "1");
  WriteBatchInternal::SetCount(&huge, 0xFFFFFFFFU);  // pretend: a maximal batch
  WriteBatch one;
  one.Put("b", "2");
  huge.Append(one);
  EXPECT_TRUE(huge.oversized());
  EXPECT_EQ(huge.Count(), 0xFFFFFFFFU) << "the overflowing append was not applied";
}

TEST(WriteBatch, SequenceNumberRoundTrips) {
  WriteBatch batch;
  batch.Put("a", "1");
  WriteBatchInternal::SetSequence(&batch, 0x00FF'1234'5678'9ABCULL);
  EXPECT_EQ(WriteBatchInternal::Sequence(batch), 0x00FF'1234'5678'9ABCULL);
  EXPECT_EQ(Render(batch), "Put(a, 1)");  // setting it did not disturb the operations
}

TEST(WriteBatch, ClearRemovesEverything) {
  WriteBatch batch;
  batch.Put("a", "1");
  WriteBatchInternal::SetSequence(&batch, 7);
  batch.Clear();
  EXPECT_EQ(batch.Count(), 0U);
  EXPECT_EQ(WriteBatchInternal::Sequence(batch), 0U);
  EXPECT_EQ(Render(batch), "");
}

TEST(WriteBatch, AppendConcatenatesOperationsAndCounts) {
  WriteBatch first;
  first.Put("a", "1");
  WriteBatchInternal::SetSequence(&first, 100);
  WriteBatch second;
  second.Delete("b");
  second.Put("c", "3");
  WriteBatchInternal::SetSequence(&second, 999);  // ignored: the result keeps first's
  first.Append(second);
  EXPECT_EQ(first.Count(), 3U);
  EXPECT_EQ(WriteBatchInternal::Sequence(first), 100U);
  EXPECT_EQ(Render(first), "Put(a, 1)Delete(b)Put(c, 3)");

  WriteBatch empty;
  first.Append(empty);
  EXPECT_EQ(first.Count(), 3U);
}

// The encoding is the one in docs/format/write_batch.md, byte for byte.
TEST(WriteBatch, EncodingMatchesTheSpec) {
  WriteBatch batch;
  batch.Put("k", "vv");
  batch.Delete("d");
  WriteBatchInternal::SetSequence(&batch, 5);
  std::string expected;
  strata::PutFixed64(&expected, 5);
  strata::PutFixed32(&expected, 2);
  // Adjacent literals keep "\x01" from swallowing the following hex-looking
  // letter ("\x01d" would be one escape, 0x1d).
  expected += std::string(
      "\x01\x01"
      "k"
      "\x02"
      "vv",
      6);
  expected += std::string(
      "\x00\x01"
      "d",
      3);
  EXPECT_EQ(WriteBatchInternal::Contents(batch), expected);
}

TEST(WriteBatch, ContentsRoundTripThroughSetContents) {
  WriteBatch batch;
  batch.Put("x", "y");
  batch.Delete("z");
  WriteBatchInternal::SetSequence(&batch, 42);
  const WriteBatch copy = FromBytes(WriteBatchInternal::Contents(batch));
  EXPECT_EQ(copy.Count(), 2U);
  EXPECT_EQ(WriteBatchInternal::Sequence(copy), 42U);
  EXPECT_EQ(Render(copy), "Put(x, y)Delete(z)");
}

TEST(WriteBatch, SetContentsRejectsBytesShorterThanTheHeader) {
  WriteBatch batch;
  EXPECT_TRUE(WriteBatchInternal::SetContents(&batch, "short").IsCorruption());
}

// Every way a WAL record could be damaged must come back as Corruption, with
// only the operations before the damage delivered.
TEST(WriteBatch, RejectsTruncatedOperations) {
  WriteBatch batch;
  batch.Put("key", "value");
  batch.Put("second", "op");
  const std::string bytes(WriteBatchInternal::Contents(batch));
  for (std::size_t cut = WriteBatchInternal::kHeaderSize; cut < bytes.size(); ++cut) {
    Status status;
    const std::string delivered = Render(FromBytes(Slice(bytes).substr(0, cut)), &status);
    EXPECT_TRUE(status.IsCorruption()) << "cut at " << cut;
    EXPECT_TRUE(delivered.empty() || delivered == "Put(key, value)") << "cut at " << cut;
  }
}

TEST(WriteBatch, RejectsAnUnknownTag) {
  WriteBatch batch;
  batch.Put("a", "1");
  std::string bytes(WriteBatchInternal::Contents(batch));
  bytes[WriteBatchInternal::kHeaderSize] = 0x7;
  Status status;
  EXPECT_EQ(Render(FromBytes(bytes), &status), "");
  EXPECT_TRUE(status.IsCorruption());
  EXPECT_NE(status.ToString().find("unknown operation tag"), std::string::npos);
}

TEST(WriteBatch, RejectsACountThatDisagreesWithTheOperations) {
  WriteBatch batch;
  batch.Put("a", "1");
  batch.Put("b", "2");

  WriteBatch too_few = FromBytes(WriteBatchInternal::Contents(batch));
  WriteBatchInternal::SetCount(&too_few, 1);
  Status status;
  EXPECT_EQ(Render(too_few, &status), "Put(a, 1)");
  EXPECT_TRUE(status.IsCorruption());

  WriteBatch too_many = FromBytes(WriteBatchInternal::Contents(batch));
  WriteBatchInternal::SetCount(&too_many, 3);
  EXPECT_EQ(Render(too_many, &status), "Put(a, 1)Put(b, 2)");
  EXPECT_TRUE(status.IsCorruption());
}

TEST(WriteBatch, RejectsAMalformedDelete) {
  std::string bytes(WriteBatchInternal::kHeaderSize, '\0');
  strata::EncodeFixed32(bytes.data() + 8, 1);
  bytes += std::string(
      "\x00\x05"
      "ab",
      4);  // claims 5 key bytes, has 2
  Status status;
  EXPECT_EQ(Render(FromBytes(bytes), &status), "");
  EXPECT_TRUE(status.IsCorruption());
}

}  // namespace
