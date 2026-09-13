#pragma once

#include "spoa/varint.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

namespace spoe::detail {

inline std::span<const uint8_t> as_bytes(std::string_view value) noexcept {
  return {reinterpret_cast<const uint8_t *>(value.data()), value.size()};
}

inline std::string_view as_chars(std::span<const uint8_t> bytes) noexcept {
  return {reinterpret_cast<const char *>(bytes.data()), bytes.size()};
}

[[nodiscard]] constexpr size_t length_prefixed_size(size_t length) noexcept {
  return varint::encoded_size(length) + length;
}

inline void append_length_prefixed(std::vector<uint8_t> &out,
                                   std::span<const uint8_t> bytes) {
  varint::encode_to_vec(out, bytes.size());
  const size_t old_size = out.size();
  out.resize(old_size + bytes.size());
  std::memcpy(out.data() + old_size, bytes.data(), bytes.size());
}

//! Shared bounds checks for frame fields and typed values. Returned views
//! borrow the original receive buffer and must not outlive it.
class Reader {
  std::span<const uint8_t> data_;
  size_t pos_ = 0;

public:
  explicit constexpr Reader(std::span<const uint8_t> data) noexcept
      : data_(data) {}

  [[nodiscard]] constexpr size_t remaining() const noexcept {
    return data_.size() - pos_;
  }
  [[nodiscard]] constexpr bool empty() const noexcept {
    return remaining() == 0;
  }
  [[nodiscard]] constexpr size_t pos() const noexcept { return pos_; }
  [[nodiscard]] constexpr std::span<const uint8_t> rest() const noexcept {
    return data_.subspan(pos_);
  }

  std::span<const uint8_t> read_bytes(size_t count) {
    if (count > remaining()) {
      throw Error("buffer too small");
    }
    const auto bytes = data_.subspan(pos_, count);
    pos_ += count;
    return bytes;
  }

  uint8_t read_byte() { return read_bytes(1)[0]; }

  uint64_t read_varint() {
    const auto decoded = varint::decode(rest());
    if (!decoded) {
      throw Error("truncated or overflowing varint");
    }
    pos_ += decoded->consumed;
    return decoded->value;
  }

  std::span<const uint8_t> read_length_prefixed() {
    const uint64_t length = read_varint();
    if (length > remaining()) {
      throw Error("buffer too small");
    }
    return read_bytes(static_cast<size_t>(length));
  }

  std::string_view read_string() { return as_chars(read_length_prefixed()); }
};

//! Fast, zero-allocation writer into a pre-allocated span.
class BufferWriter {
  std::span<uint8_t> buffer_;
  size_t pos_ = 0;

public:
  explicit constexpr BufferWriter(std::span<uint8_t> buffer) noexcept
      : buffer_(buffer) {}

  [[nodiscard]] constexpr size_t written() const noexcept { return pos_; }
  [[nodiscard]] constexpr size_t remaining() const noexcept {
    return buffer_.size() - pos_;
  }
  [[nodiscard]] constexpr std::span<uint8_t> written_span() const noexcept {
    return buffer_.first(pos_);
  }

  void write_byte(uint8_t b) {
    if (pos_ >= buffer_.size()) {
      throw Error("buffer too small");
    }
    buffer_[pos_++] = b;
  }

  void write_bytes(std::span<const uint8_t> src) {
    if (src.size() > remaining()) {
      throw Error("buffer too small");
    }
    std::memcpy(buffer_.data() + pos_, src.data(), src.size());
    pos_ += src.size();
  }

  void write_varint(uint64_t n) {
    const size_t needed = varint::encoded_size(n);
    if (needed > remaining()) {
      throw Error("buffer too small");
    }
    varint::encode_unsafe(buffer_.data() + pos_, n);
    pos_ += needed;
  }

  void write_length_prefixed(std::span<const uint8_t> bytes) {
    write_varint(bytes.size());
    write_bytes(bytes);
  }

  void write_string(std::string_view str) {
    write_length_prefixed(as_bytes(str));
  }

  void write_u32_be(uint32_t val) {
    if (4 > remaining()) {
      throw Error("buffer too small");
    }
    const uint32_t be = (std::endian::native == std::endian::big)
                            ? val
                            : __builtin_bswap32(val);
    std::memcpy(buffer_.data() + pos_, &be, 4);
    pos_ += 4;
  }
};

} // namespace spoe::detail
