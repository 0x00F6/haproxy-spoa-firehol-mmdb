#pragma once

#include <unistd.h>

#include <utility>

namespace spoe {

//! Owns a POSIX file descriptor and closes it on destruction.
class UniqueFd {
public:
  UniqueFd() = default;
  explicit UniqueFd(int fd) noexcept : fd_(fd) {}
  ~UniqueFd() { reset(); }

  UniqueFd(const UniqueFd &) = delete;
  UniqueFd &operator=(const UniqueFd &) = delete;
  UniqueFd(UniqueFd &&other) noexcept : fd_(other.release()) {}
  UniqueFd &operator=(UniqueFd &&other) noexcept {
    if (this != &other) {
      reset();
      fd_ = other.release();
    }
    return *this;
  }

  [[nodiscard]] int get() const noexcept { return fd_; }
  explicit operator bool() const noexcept { return fd_ != -1; }

  int release() noexcept { return std::exchange(fd_, -1); }
  void reset(int new_fd = -1) noexcept {
    if (fd_ != -1) {
      ::close(fd_);
    }
    fd_ = new_fd;
  }

private:
  int fd_ = -1;
};

} // namespace spoe
