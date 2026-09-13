#pragma once

#include "spoa/protocol_io.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace spoe {

//! SPOE type identifiers occupy the lower nibble of the encoded type byte.
enum class TypedType : uint8_t {
  Null = 0,
  Boolean = 1,
  Int32 = 2,
  UInt32 = 3,
  Int64 = 4,
  UInt64 = 5,
  IPv4 = 6,
  IPv6 = 7,
  String = 8,
  Binary = 9
};

//! A SPOE typed value. Only the union member selected by `type` is
//! meaningful. String and binary values borrow their storage; decoding never
//! copies them, and the whole value stays trivially copyable.
struct TypedData {
  TypedType type = TypedType::Null;
  union {
    bool boolean;
    int32_t i32;
    uint32_t u32;
    int64_t i64;
    uint64_t u64 = 0;
    std::array<uint8_t, 4> ipv4;
    std::array<uint8_t, 16> ipv6;
    std::string_view str;
    std::span<const uint8_t> bin;
  };

  static constexpr TypedData make_null() noexcept { return {}; }
  static constexpr TypedData make_boolean(bool value) noexcept {
    return {.type = TypedType::Boolean, .boolean = value};
  }
  static constexpr TypedData make_int32(int32_t value) noexcept {
    return {.type = TypedType::Int32, .i32 = value};
  }
  static constexpr TypedData make_uint32(uint32_t value) noexcept {
    return {.type = TypedType::UInt32, .u32 = value};
  }
  static constexpr TypedData make_int64(int64_t value) noexcept {
    return {.type = TypedType::Int64, .i64 = value};
  }
  static constexpr TypedData make_uint64(uint64_t value) noexcept {
    return {.type = TypedType::UInt64, .u64 = value};
  }
  static constexpr TypedData make_ipv4(uint8_t a, uint8_t b, uint8_t c,
                                       uint8_t d) noexcept {
    return make_ipv4({a, b, c, d});
  }
  static constexpr TypedData
  make_ipv4(const std::array<uint8_t, 4> &octets) noexcept {
    return {.type = TypedType::IPv4, .ipv4 = octets};
  }
  static constexpr TypedData
  make_ipv6(const std::array<uint8_t, 16> &octets) noexcept {
    return {.type = TypedType::IPv6, .ipv6 = octets};
  }
  static constexpr TypedData make_string(std::string_view value) noexcept {
    return {.type = TypedType::String, .str = value};
  }
  static constexpr TypedData
  make_binary(std::span<const uint8_t> value) noexcept {
    return {.type = TypedType::Binary, .bin = value};
  }

  //! Size including the type byte, used to allocate each response once.
  [[nodiscard]] size_t encoded_size() const;

  //! Encode this value and append the bytes to `out`.
  void encode(std::vector<uint8_t> &out) const;

  //! Encode directly using BufferWriter without dynamic allocation.
  void encode(detail::BufferWriter &writer) const;

  //! Consume one value from `reader`; throw Error on malformed input.
  static TypedData decode(detail::Reader &reader);

  //! Return the value and consumed byte count; throw Error on malformed input.
  static std::pair<TypedData, size_t> decode(std::span<const uint8_t> buf);
};

//! Human-readable name of a SPOE typed value type.
[[nodiscard]] const char *typed_type_name(TypedType type) noexcept;

} // namespace spoe
