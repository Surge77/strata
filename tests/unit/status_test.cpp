#include "strata/status.hpp"

#include <string>
#include <utility>

#include <gtest/gtest.h>

namespace {

using strata::Status;

TEST(Status, DefaultConstructedIsOk) {
  const Status status;
  EXPECT_TRUE(status.ok());
  EXPECT_EQ(status.code(), Status::Code::kOk);
  EXPECT_TRUE(status.message().empty());
  EXPECT_EQ(status.ToString(), "Ok");
}

TEST(Status, CarriesCodeAndMessage) {
  const Status status = Status::Corruption("bad block checksum");
  EXPECT_FALSE(status.ok());
  EXPECT_TRUE(status.IsCorruption());
  EXPECT_EQ(status.message(), "bad block checksum");
  EXPECT_EQ(status.ToString(), "Corruption: bad block checksum");
}

TEST(Status, OmitsSeparatorWhenThereIsNoMessage) {
  EXPECT_EQ(Status::NotFound().ToString(), "NotFound");
}

TEST(Status, PredicatesAreMutuallyExclusive) {
  const Status status = Status::IoError("write failed");
  EXPECT_TRUE(status.IsIoError());
  EXPECT_FALSE(status.IsNotFound());
  EXPECT_FALSE(status.IsCorruption());
  EXPECT_FALSE(status.IsBusy());
}

TEST(Status, CopyDoesNotShareTheMessageBuffer) {
  const Status original = Status::Busy("database is locked");
  Status copy = original;
  EXPECT_EQ(copy.message(), original.message());
  EXPECT_NE(copy.message().data(), original.message().data());
}

TEST(Status, MoveLeavesTheSourceUsable) {
  Status source = Status::Internal("invariant violated");
  const Status moved = std::move(source);
  EXPECT_EQ(moved.message(), "invariant violated");
  EXPECT_EQ(moved.code(), Status::Code::kInternal);
}

TEST(Status, SelfAssignmentKeepsTheMessage) {
  Status status = Status::NotFound("missing");
  const Status& alias = status;
  status = alias;
  EXPECT_EQ(status.message(), "missing");
}

TEST(Status, EqualityComparesCodesNotMessages) {
  EXPECT_EQ(Status::NotFound("a"), Status::NotFound("b"));
  EXPECT_NE(Status::NotFound(), Status::Ok());
}

TEST(Status, EveryCodeHasAName) {
  // Guards the parallel name table in status.cpp against a code being added
  // without one, which would otherwise surface as "Unknown" in a log.
  for (auto code = static_cast<int>(Status::Code::kOk);
       code <= static_cast<int>(Status::Code::kInternal); ++code) {
    const auto name = Status::CodeName(static_cast<Status::Code>(code));
    EXPECT_FALSE(name.empty());
    EXPECT_NE(name, "Unknown") << "code " << code;
  }
}

}  // namespace
