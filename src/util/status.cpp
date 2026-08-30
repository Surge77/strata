#include "strata/status.hpp"

#include <array>
#include <cstddef>
#include <string>

namespace strata {

Status::Status(Code code, Slice message)
    : code_(code), message_(message.empty() ? nullptr : std::make_unique<std::string>(message)) {}

Status::Status(const Status& other)
    : code_(other.code_),
      message_(other.message_ ? std::make_unique<std::string>(*other.message_) : nullptr) {}

Status& Status::operator=(const Status& other) {
  if (this != &other) {
    code_ = other.code_;
    message_ = other.message_ ? std::make_unique<std::string>(*other.message_) : nullptr;
  }
  return *this;
}

Slice Status::CodeName(Code code) noexcept {
  // Indexed by Code, so the order here must track the enum. The static_assert
  // below fails if a code is added without a name.
  static constexpr std::array<Slice, 9> kNames{
      "Ok",           "NotFound",      "Corruption", "InvalidArgument", "IoError",
      "NotSupported", "AlreadyExists", "Busy",       "Internal",
  };
  static_assert(kNames.size() == static_cast<std::size_t>(Code::kInternal) + 1,
                "Status::Code and kNames have diverged");

  const auto index = static_cast<std::size_t>(code);
  return index < kNames.size() ? kNames[index] : Slice("Unknown");
}

std::string Status::ToString() const {
  std::string result(CodeName(code_));
  if (message_ != nullptr && !message_->empty()) {
    result += ": ";
    result += *message_;
  }
  return result;
}

}  // namespace strata
