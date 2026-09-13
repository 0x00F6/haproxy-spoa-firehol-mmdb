#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

namespace spoe {

//! Error signalling a malformed or unsupported SPOE frame.
struct Error : std::runtime_error {
  using std::runtime_error::runtime_error;
};

namespace varint {

//! A uint64_t needs at most ten bytes in the SPOE Peers encoding.
inline constexpr size_t max_encoded_size = 10;

[[nodiscard]] constexpr size_t encoded_size(uint64_t n) noexcept {
  if (n < 240) {
    return 1;
  }
  size_t size = 2;
  n = (n - 240) >> 4;
  while (n >= 128) {
    n = (n - 128) >> 7;
    ++size;
  }
  return size;
}

//! A decoded varint plus the number of bytes it occupied.
struct Decoded {
  uint64_t value;
  size_t consumed;
};

//! Decode a SPOE varint (Peers encoding).
//! Returns std::nullopt for truncated encodings or values beyond uint64_t.
[[nodiscard]] constexpr std::optional<Decoded>
decode(std::span<const uint8_t> buf) noexcept {
  if (buf.empty()) {
    return std::nullopt;
  }
  uint64_t n = buf[0];
  if (n < 240) {
    return Decoded{n, 1};
  }
  uint32_t shift = 4;
  size_t i = 1;
  while (true) {
    if (i >= buf.size()) {
      return std::nullopt;
    }
    const uint8_t b = buf[i++];
    // Check before shifting: an untrusted continuation chain can otherwise
    // overflow the value or shift by more than the width of uint64_t.
    if (shift >= 64 ||
        b > ((std::numeric_limits<uint64_t>::max() - n) >> shift)) {
      return std::nullopt;
    }
    n += static_cast<uint64_t>(b) << shift;
    shift += 7;
    if (b < 128) {
      return Decoded{n, i};
    }
  }
}

//! Encode `n` into `buf` (capacity `cap`).
//! Returns bytes written, or std::nullopt if `buf` is too small.
[[nodiscard]] constexpr std::optional<size_t> encode(uint8_t *buf, size_t cap,
                                                     uint64_t n) noexcept {
  if (cap == 0) {
    return std::nullopt;
  }
  if (n < 240) {
    buf[0] = static_cast<uint8_t>(n);
    return 1;
  }
  buf[0] = static_cast<uint8_t>(n) | 0xF0;
  size_t p = 1;
  n = (n - 240) >> 4;
  while (n >= 128) {
    if (p >= cap) {
      return std::nullopt;
    }
    buf[p++] = static_cast<uint8_t>(n) | 0x80;
    n = (n - 128) >> 7;
  }
  if (p >= cap) {
    return std::nullopt;
  }
  buf[p] = static_cast<uint8_t>(n);
  return p + 1;
}

//! Encode `n` assuming `buf` has at least `encoded_size(n)` capacity.
constexpr size_t encode_unsafe(uint8_t *buf, uint64_t n) noexcept {
  if (n < 240) {
    buf[0] = static_cast<uint8_t>(n);
    return 1;
  }
  buf[0] = static_cast<uint8_t>(n) | 0xF0;
  size_t p = 1;
  n = (n - 240) >> 4;
  while (n >= 128) {
    buf[p++] = static_cast<uint8_t>(n) | 0x80;
    n = (n - 128) >> 7;
  }
  buf[p] = static_cast<uint8_t>(n);
  return p + 1;
}

//! Append `n` (varint-encoded) to `out`.
inline void encode_to_vec(std::vector<uint8_t> &out, uint64_t n) {
  const size_t needed = encoded_size(n);
  const size_t old_size = out.size();
  out.resize(old_size + needed);
  encode_unsafe(out.data() + old_size, n);
}

} // namespace varint
} // namespace spoe
