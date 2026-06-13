#pragma once
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

namespace vmosue {

template <typename T>
class Result {
 public:
  static Result Ok(T value) { return Result(std::move(value), std::string{}); }
  static Result Err(std::string msg) { return Result(T{}, std::move(msg)); }

  bool isOk() const { return !error_.has_value(); }
  const T& value() const {
    if (!isOk()) failAbort();
    return value_;
  }
  T& value() {
    if (!isOk()) failAbort();
    return value_;
  }
  const std::string& error() const { return *error_; }

 private:
  Result(T v, std::string e) : value_(std::move(v)) {
    if (!e.empty()) error_ = std::move(e);
  }
  [[noreturn]] void failAbort() const {
    std::fprintf(stderr, "Result: value() on error: %s\n", error_->c_str());
    std::abort();
  }
  T value_;
  std::optional<std::string> error_;
};

// Specialization for void: no payload, just ok/err state.
// Required by CameraCapture::Init() and similar APIs.
template <>
class Result<void> {
 public:
  // Unused dummy parameter lets callers write Result<void>::Ok({})
  // mirroring the Result<T>::Ok(T) idiom.
  static Result Ok(int = 0) { return Result(std::string{}); }
  static Result Err(std::string msg) { return Result(std::move(msg)); }

  bool isOk() const { return !error_.has_value(); }
  void value() const {
    if (!isOk()) failAbort();
  }
  const std::string& error() const { return *error_; }

 private:
  explicit Result(std::string e) {
    if (!e.empty()) error_ = std::move(e);
  }
  [[noreturn]] void failAbort() const {
    std::fprintf(stderr, "Result: value() on error: %s\n", error_->c_str());
    std::abort();
  }
  std::optional<std::string> error_;
};

}  // namespace vmosue