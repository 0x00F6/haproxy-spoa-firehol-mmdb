#include "spoa/typed_data.h"

#include <cstring>
#include <string>

namespace spoe {
namespace {

template <size_t N> std::array<uint8_t, N> read_array(detail::Reader &reader) {
  std::array<uint8_t, N> octets;
  const auto bytes = reader.read_bytes(N);
  std::memcpy(octets.data(), bytes.data(), N);
  return octets;
}

} // namespace

size_t TypedData::encoded_size() const {
  switch (type) {
  case TypedType::Null:
  case TypedType::Boolean:
    return 1;
  case TypedType::Int32:
    return 1 + varint::encoded_size(static_cast<uint64_t>(i32));
  case TypedType::UInt32:
    return 1 + varint::encoded_size(u32);
  case TypedType::Int64:
    return 1 + varint::encoded_size(static_cast<uint64_t>(i64));
  case TypedType::UInt64:
    return 1 + varint::encoded_size(u64);
  case TypedType::IPv4:
    return 1 + ipv4.size();
  case TypedType::IPv6:
    return 1 + ipv6.size();
  case TypedType::String:
    return 1 + detail::length_prefixed_size(str.size());
  case TypedType::Binary:
    return 1 + detail::length_prefixed_size(bin.size());
  }
  throw Error("unknown typed value type");
}

void TypedData::encode(detail::BufferWriter &writer) const {
  writer.write_byte(static_cast<uint8_t>(type) |
                    (type == TypedType::Boolean && boolean ? 0x10 : 0));
  switch (type) {
  case TypedType::Null:
  case TypedType::Boolean:
    return;
  case TypedType::Int32:
    writer.write_varint(static_cast<uint64_t>(i32));
    return;
  case TypedType::UInt32:
    writer.write_varint(u32);
    return;
  case TypedType::Int64:
    writer.write_varint(static_cast<uint64_t>(i64));
    return;
  case TypedType::UInt64:
    writer.write_varint(u64);
    return;
  case TypedType::IPv4:
    writer.write_bytes(ipv4);
    return;
  case TypedType::IPv6:
    writer.write_bytes(ipv6);
    return;
  case TypedType::String:
    writer.write_string(str);
    return;
  case TypedType::Binary:
    writer.write_length_prefixed(bin);
    return;
  }
  throw Error("unknown typed value type");
}

void TypedData::encode(std::vector<uint8_t> &out) const {
  // Grow once, then reuse the span encoder so both paths emit identical bytes.
  const size_t old_size = out.size();
  out.resize(old_size + encoded_size());
  detail::BufferWriter writer(std::span<uint8_t>(out).subspan(old_size));
  encode(writer);
}

TypedData TypedData::decode(detail::Reader &reader) {
  const uint8_t header = reader.read_byte();
  TypedData value;
  value.type = static_cast<TypedType>(header & 0x0F);
  switch (value.type) {
  case TypedType::Null:
    break;
  case TypedType::Boolean:
    value.boolean = (header & 0x10) != 0;
    break;
  case TypedType::Int32:
    value.i32 = static_cast<int32_t>(reader.read_varint());
    break;
  case TypedType::UInt32:
    value.u32 = static_cast<uint32_t>(reader.read_varint());
    break;
  case TypedType::Int64:
    value.i64 = static_cast<int64_t>(reader.read_varint());
    break;
  case TypedType::UInt64:
    value.u64 = reader.read_varint();
    break;
  case TypedType::IPv4:
    value.ipv4 = read_array<4>(reader);
    break;
  case TypedType::IPv6:
    value.ipv6 = read_array<16>(reader);
    break;
  case TypedType::String:
    value.str = reader.read_string();
    break;
  case TypedType::Binary:
    value.bin = reader.read_length_prefixed();
    break;
  default:
    throw Error("unknown typed value type " + std::to_string(header & 0x0F));
  }
  return value;
}

std::pair<TypedData, size_t> TypedData::decode(std::span<const uint8_t> buf) {
  detail::Reader reader(buf);
  const TypedData value = decode(reader);
  return {value, reader.pos()};
}

const char *typed_type_name(TypedType type) noexcept {
  switch (type) {
  case TypedType::Null:
    return "Null";
  case TypedType::Boolean:
    return "Boolean";
  case TypedType::Int32:
    return "Int32";
  case TypedType::UInt32:
    return "UInt32";
  case TypedType::Int64:
    return "Int64";
  case TypedType::UInt64:
    return "UInt64";
  case TypedType::IPv4:
    return "IPv4";
  case TypedType::IPv6:
    return "IPv6";
  case TypedType::String:
    return "String";
  case TypedType::Binary:
    return "Binary";
  }
  return "Unknown";
}

} // namespace spoe
