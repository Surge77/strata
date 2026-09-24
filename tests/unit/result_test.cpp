#include "strata/result.hpp"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace {

using strata::Result;
using strata::Status;

Result<int> Parse(bool succeed) {
  if (!succeed) {
    return Status::InvalidArgument("not a number");
  }
  return 42;
}

Status ForwardFailure(bool succeed) {
  STRATA_ASSIGN_OR_RETURN(const int value, Parse(succeed));
  EXPECT_EQ(value, 42);
  return Status::Ok();
}

Result<std::string> ChainFailure(bool succeed) {
  STRATA_ASSIGN_OR_RETURN(const int value, Parse(succeed));
  return std::to_string(value);
}

Status Guarded(const Status& incoming) {
  STRATA_RETURN_IF_ERROR(incoming);
  return Status::Ok();
}

TEST(Result, HoldsAValueOnSuccess) {
  const Result<int> result = Parse(true);
  ASSERT_TRUE(result.ok());
  EXPECT_TRUE(static_cast<bool>(result));
  EXPECT_EQ(result.value(), 42);
  EXPECT_EQ(*result, 42);
  EXPECT_TRUE(result.status().ok());
}

TEST(Result, HoldsAStatusOnFailure) {
  const Result<int> result = Parse(false);
  EXPECT_FALSE(result.ok());
  EXPECT_FALSE(static_cast<bool>(result));
  EXPECT_EQ(result.status().code(), Status::Code::kInvalidArgument);
  EXPECT_EQ(result.status().message(), "not a number");
}

TEST(Result, ValueOrSubstitutesOnFailureOnly) {
  EXPECT_EQ(Parse(true).value_or(-1), 42);
  EXPECT_EQ(Parse(false).value_or(-1), -1);
}

TEST(Result, MovesOutMoveOnlyPayloads) {
  Result<std::unique_ptr<int>> result(std::make_unique<int>(7));
  ASSERT_TRUE(result.ok());
  const std::unique_ptr<int> owned = std::move(result).value();
  ASSERT_NE(owned, nullptr);
  EXPECT_EQ(*owned, 7);
}

TEST(Result, ArrowReachesTheHeldValue) {
  const Result<std::string> result(std::string("abc"));
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result->size(), 3U);
}

TEST(AssignOrReturn, PassesTheValueThroughOnSuccess) {
  EXPECT_TRUE(ForwardFailure(true).ok());
  EXPECT_TRUE(ChainFailure(true).ok());
  EXPECT_EQ(ChainFailure(true).value(), "42");
}

TEST(AssignOrReturn, PropagatesTheStatusOutOfBothReturnShapes) {
  const Status status = ForwardFailure(false);
  EXPECT_EQ(status.code(), Status::Code::kInvalidArgument);
  EXPECT_EQ(status.message(), "not a number");

  const Result<std::string> chained = ChainFailure(false);
  EXPECT_FALSE(chained.ok());
  EXPECT_EQ(chained.status().code(), Status::Code::kInvalidArgument);
}

TEST(AssignOrReturn, EvaluatesItsExpressionExactlyOnce) {
  int calls = 0;
  const auto counted = [&calls]() -> Result<int> {
    ++calls;
    return 1;
  };
  const auto body = [&counted]() -> Status {
    STRATA_ASSIGN_OR_RETURN(const int value, counted());
    EXPECT_EQ(value, 1);
    return Status::Ok();
  };
  EXPECT_TRUE(body().ok());
  EXPECT_EQ(calls, 1);
}

TEST(ReturnIfError, ReturnsOnlyOnFailure) {
  EXPECT_TRUE(Guarded(Status::Ok()).ok());
  EXPECT_TRUE(Guarded(Status::Busy("locked")).IsBusy());
}

TEST(ReturnIfError, AllowsTwoUsesInOneScope) {
  const auto body = [](const Status& first, const Status& second) -> Status {
    STRATA_RETURN_IF_ERROR(first);
    STRATA_RETURN_IF_ERROR(second);
    return Status::Ok();
  };
  EXPECT_TRUE(body(Status::Ok(), Status::Ok()).ok());
  EXPECT_TRUE(body(Status::Ok(), Status::NotFound()).IsNotFound());
}

}  // namespace
