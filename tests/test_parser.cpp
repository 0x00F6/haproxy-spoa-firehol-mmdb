#include "spoa/frame.h"
#include "spoa/message.h"
#include "spoa/typed_data.h"
#include "spoa/varint.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace spoe;

namespace {

// These checks remain active when the test is compiled with NDEBUG.
void require(bool condition, std::string_view expression,
             const std::source_location &location = std::source_location::current()) {
  if (!condition) {
    throw std::runtime_error(std::string(location.file_name()) + ":" +
                             std::to_string(location.line()) + ": " +
                             std::string(expression));
  }
}

#define CHECK(...) require((__VA_ARGS__), #__VA_ARGS__)

template <typename Function>
void expect_error(Function function) {
  try {
    function();
  } catch (const Error &) {
    return;
  }
  throw std::runtime_error("expected a SPOE decoding error");
}

void make_payload(std::vector<uint8_t> &buf, uint64_t stream_id,
                  uint64_t frame_id, const std::vector<uint8_t> &data) {
  buf.insert(buf.end(), {0, 0, 0, 1});
  varint::encode_to_vec(buf, stream_id);
  varint::encode_to_vec(buf, frame_id);
  buf.insert(buf.end(), data.begin(), data.end());
}

void name_bytes(std::vector<uint8_t> &buf, std::string_view name) {
  varint::encode_to_vec(buf, name.size());
  buf.insert(buf.end(), name.begin(), name.end());
}

void kv_bytes(std::vector<uint8_t> &buf, std::string_view key,
              const TypedData &value) {
  name_bytes(buf, key);
  value.encode(buf);
}

void test_varint_roundtrip() {
  const std::vector<uint64_t> values{0, 1, 239, 240, 241, 255, 256, 2287, 2288,
                                     264431, 264432, UINT32_MAX,
                                     UINT64_MAX / 2, UINT64_MAX - 1, UINT64_MAX};
  for (uint64_t value : values) {
    std::vector<uint8_t> bytes;
    varint::encode_to_vec(bytes, value);
    const auto decoded = varint::decode(bytes);
    CHECK(decoded && decoded->value == value && decoded->consumed == bytes.size());
    CHECK(bytes.size() == varint::encoded_size(value));
    CHECK(bytes.size() <= varint::max_encoded_size);
    for (size_t length = 0; length < bytes.size(); ++length) {
      CHECK(!varint::decode(std::span<const uint8_t>(bytes).first(length)));
    }
    std::array<uint8_t, varint::max_encoded_size> destination{};
    CHECK(!varint::encode(destination.data(), bytes.size() - 1, value));
    CHECK(varint::encode(destination.data(), bytes.size(), value) == bytes.size());
    CHECK(std::equal(bytes.begin(), bytes.end(), destination.begin()));
  }

  const std::array fixtures{
      std::pair<uint64_t, std::vector<uint8_t>>{239, {0xEF}},
      std::pair<uint64_t, std::vector<uint8_t>>{240, {0xF0, 0x00}},
      std::pair<uint64_t, std::vector<uint8_t>>{255, {0xFF, 0x00}},
      std::pair<uint64_t, std::vector<uint8_t>>{256, {0xF0, 0x01}},
      std::pair<uint64_t, std::vector<uint8_t>>{2287, {0xFF, 0x7F}},
      std::pair<uint64_t, std::vector<uint8_t>>{2288, {0xF0, 0x80, 0x00}},
      std::pair<uint64_t, std::vector<uint8_t>>{264431, {0xFF, 0xFF, 0x7F}},
      std::pair<uint64_t, std::vector<uint8_t>>{264432, {0xF0, 0x80, 0x80, 0x00}}};
  for (const auto &[value, expected] : fixtures) {
    std::vector<uint8_t> bytes;
    varint::encode_to_vec(bytes, value);
    CHECK(bytes == expected);
  }

  // A valid prefix must not consume the next protocol field.
  CHECK(varint::decode(std::vector<uint8_t>{0xF0, 0, 0x42})->consumed == 2);
  CHECK(!varint::decode({}));
  CHECK(!varint::decode(std::vector<uint8_t>{0xF5}));
  CHECK(!varint::decode(std::vector<uint8_t>(32, 0xFF)));
  std::vector<uint8_t> overflow;
  varint::encode_to_vec(overflow, UINT64_MAX);
  ++overflow.back();
  CHECK(!varint::decode(overflow));
}

void check_typed_value(const TypedData &actual, const TypedData &expected) {
  CHECK(actual.type == expected.type);
  switch (expected.type) {
  case TypedType::Null:
    break;
  case TypedType::Boolean:
    CHECK(actual.boolean == expected.boolean);
    break;
  case TypedType::Int32:
    CHECK(actual.i32 == expected.i32);
    break;
  case TypedType::UInt32:
    CHECK(actual.u32 == expected.u32);
    break;
  case TypedType::Int64:
    CHECK(actual.i64 == expected.i64);
    break;
  case TypedType::UInt64:
    CHECK(actual.u64 == expected.u64);
    break;
  case TypedType::IPv4:
    CHECK(actual.ipv4 == expected.ipv4);
    break;
  case TypedType::IPv6:
    CHECK(actual.ipv6 == expected.ipv6);
    break;
  case TypedType::String:
    CHECK(actual.str == expected.str);
    break;
  case TypedType::Binary:
    CHECK(std::equal(actual.bin.begin(), actual.bin.end(),
                     expected.bin.begin(), expected.bin.end()));
    break;
  }
}

void test_typeddata_roundtrip() {
  const std::vector<uint8_t> binary{0xDE, 0xAD, 0xBE, 0xEF};
  const std::string long_string(300, 's');
  const std::vector<TypedData> values{
      TypedData::make_null(),
      TypedData::make_boolean(true),
      TypedData::make_boolean(false),
      TypedData::make_int32(-42),
      TypedData::make_int32(INT32_MIN),
      TypedData::make_int32(INT32_MAX),
      TypedData::make_uint32(UINT32_MAX),
      TypedData::make_int64(INT64_MIN),
      TypedData::make_int64(INT64_MAX),
      TypedData::make_uint64(UINT64_MAX),
      TypedData::make_ipv4(192, 168, 1, 1),
      TypedData::make_ipv6({0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}),
      TypedData::make_string("hello"),
      TypedData::make_string(long_string),
      TypedData::make_string({}),
      TypedData::make_binary(binary),
      TypedData::make_binary({})};
  for (const auto &value : values) {
    std::vector<uint8_t> bytes;
    value.encode(bytes);
    CHECK(bytes.size() == value.encoded_size());
    const size_t encoded_size = bytes.size();
    for (size_t length = 0; length < encoded_size; ++length) {
      expect_error([&] { TypedData::decode(std::span<const uint8_t>(bytes).first(length)); });
    }
    bytes.push_back(0x42); // decoding consumes only the current value
    auto [decoded, consumed] = TypedData::decode(bytes);
    CHECK(consumed == encoded_size);
    check_typed_value(decoded, value);
    if (value.type == TypedType::String) {
      CHECK(decoded.str.data() ==
            reinterpret_cast<const char *>(bytes.data() + consumed - value.str.size()));
    } else if (value.type == TypedType::Binary) {
      CHECK(decoded.bin.data() == bytes.data() + consumed - value.bin.size());
    }
  }
  expect_error([] { TypedData::decode(std::vector<uint8_t>{0x0A}); });
  for (uint8_t type : {0x02, 0x03, 0x04, 0x05, 0x08, 0x09}) {
    std::vector<uint8_t> oversized{type};
    varint::encode_to_vec(oversized, UINT64_MAX);
    if (type == 0x08 || type == 0x09) {
      expect_error([&] { TypedData::decode(oversized); });
    }
    ++oversized.back();
    expect_error([&] { TypedData::decode(oversized); });
  }
}

void test_parse_hello_and_disconnect() {
  std::vector<uint8_t> data;
  kv_bytes(data, "unknown", TypedData::make_string("ignored"));
  kv_bytes(data, "engine-id", TypedData::make_string("my-engine"));
  kv_bytes(data, "supported-versions", TypedData::make_string("1.0,2.0"));
  kv_bytes(data, "max-frame-size", TypedData::make_uint32(16384));
  kv_bytes(data, "max-frame-size", TypedData::make_string("wrong type"));
  kv_bytes(data, "healthcheck", TypedData::make_boolean(true));
  std::vector<uint8_t> payload;
  make_payload(payload, 0, UINT64_MAX, data);
  const auto parsed = frame::parse(frame::FrameType::HaproxyHello, payload);
  const auto *hello = std::get_if<frame::HaproxyHello>(&parsed);
  CHECK(hello);
  CHECK(hello->engine_id == "my-engine");
  CHECK(hello->supported_versions == "1.0,2.0");
  CHECK(hello->max_frame_size == 16384);
  CHECK(hello->healthcheck);
  CHECK(hello->stream_id == 0 && hello->frame_id == UINT64_MAX);
  const auto engine_position = std::search(payload.begin(), payload.end(),
                                          hello->engine_id.begin(), hello->engine_id.end());
  CHECK(hello->engine_id.data() == reinterpret_cast<const char *>(&*engine_position));

  data.clear();
  kv_bytes(data, "status-code", TypedData::make_uint32(7));
  kv_bytes(data, "message", TypedData::make_string("first"));
  kv_bytes(data, "message", TypedData::make_string("bye"));
  payload.clear();
  make_payload(payload, 8, 9, data);
  const auto disconnected = frame::parse(frame::FrameType::HaproxyDisconnect, payload);
  const auto *disconnect = std::get_if<frame::HaproxyDisconnect>(&disconnected);
  CHECK(disconnect);
  CHECK(disconnect->status_code == 7 && disconnect->message == "bye");
  CHECK(disconnect->stream_id == 8 && disconnect->frame_id == 9);
  CHECK(disconnect->message.data() ==
        reinterpret_cast<const char *>(payload.data() + payload.size() - 3));
}

void test_parse_notify() {
  std::vector<uint8_t> data;
  name_bytes(data, "check-ip");
  data.push_back(2);
  kv_bytes(data, "ip", TypedData::make_ipv4(1, 2, 3, 4));
  kv_bytes(data, "label", TypedData::make_string("borrowed"));
  name_bytes(data, "empty");
  data.push_back(0);
  const std::string long_name(300, 'a');
  name_bytes(data, long_name);
  data.push_back(1);
  kv_bytes(data, "", TypedData::make_null());
  std::vector<uint8_t> payload;
  make_payload(payload, 5, 7, data);
  const auto parsed = frame::parse(frame::FrameType::Notify, payload);
  const auto *notify = std::get_if<frame::Notify>(&parsed);
  CHECK(notify);
  CHECK(notify->stream_id == 5 && notify->frame_id == 7);
  CHECK(notify->messages.size() == 3);
  CHECK(notify->messages[0].name == "check-ip");
  CHECK(notify->messages[0].name.data() ==
        reinterpret_cast<const char *>(payload.data() + 7));
  const auto *ip = notify->messages[0].get("ip");
  CHECK(ip && ip->type == TypedType::IPv4 &&
        ip->ipv4 == std::array<uint8_t, 4>{1, 2, 3, 4});
  CHECK(notify->messages[0].get("missing") == nullptr);
  const auto *label = notify->messages[0].get("label");
  CHECK(label && label->str == "borrowed");
  CHECK(notify->messages[1].name == "empty" && notify->messages[1].args.empty());
  CHECK(notify->messages[2].name == long_name);
  CHECK(notify->messages[2].args.size() == 1);
  CHECK(notify->messages[2].args[0].name.empty());
  CHECK(notify->messages[2].args[0].value.type == TypedType::Null);
}

void test_error_paths() {
  const std::vector<std::vector<uint8_t>> invalid_headers{
      {}, {0, 0}, {0, 0, 0, 1}, {0, 0, 0, 1, 0xF5}, {0, 0, 0, 1, 0, 0xF5}};
  for (const auto &payload : invalid_headers) {
    expect_error([&] { frame::parse(frame::FrameType::HaproxyHello, payload); });
  }
  std::vector<uint8_t> payload;
  make_payload(payload, 0, 0, {});
  for (auto type : {frame::FrameType::AgentHello, frame::FrameType::AgentDisconnect,
                    frame::FrameType::AgentAck, static_cast<frame::FrameType>(0xFF)}) {
    expect_error([&] { frame::parse(type, payload); });
  }

  const std::vector<std::vector<uint8_t>> malformed_messages{
      {0xF5},              // truncated name length
      {3, 'x'},            // truncated name
      {0},                 // missing argument count
      {0, 0xFF},           // impossible argument count
      {0, 1, 0, 0x08},     // string argument without a length
      {0, 1, 0, 0x08, 3}}; // truncated string argument
  for (const auto &data : malformed_messages) {
    payload.clear();
    make_payload(payload, 0, 0, data);
    expect_error([&] { frame::parse(frame::FrameType::Notify, payload); });
  }
  std::vector<uint8_t> oversized_name;
  varint::encode_to_vec(oversized_name, UINT64_MAX);
  payload.clear();
  make_payload(payload, 0, 0, oversized_name);
  expect_error([&] { frame::parse(frame::FrameType::Notify, payload); });
  expect_error([&] { frame::parse(frame::FrameType::HaproxyHello, payload); });
}

void check_frame_header(const std::vector<uint8_t> &bytes, frame::FrameType type) {
  CHECK(bytes.size() >= 11);
  const uint32_t length = (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16) |
                          (uint32_t(bytes[2]) << 8) | uint32_t(bytes[3]);
  CHECK(bytes.size() == 4 + uint64_t(length));
  CHECK(bytes[4] == static_cast<uint8_t>(type));
  CHECK(bytes[5] == 0 && bytes[6] == 0 && bytes[7] == 0 && bytes[8] == 1);
}

void test_encode_frames() {
  const auto hello = frame::encode_agent_hello(1, 2, 16384);
  check_frame_header(hello, frame::FrameType::AgentHello);
  const std::vector<uint8_t> expected_hello{
      0, 0, 0, 64, 0x65, 0, 0, 0, 1, 1, 2,
      7, 'v', 'e', 'r', 's', 'i', 'o', 'n', 8, 3, '2', '.', '0',
      14, 'm', 'a', 'x', '-', 'f', 'r', 'a', 'm', 'e', '-', 's', 'i', 'z', 'e',
      3, 0xF0, 0xF1, 6,
      12, 'c', 'a', 'p', 'a', 'b', 'i', 'l', 'i', 't', 'i', 'e', 's',
      8, 10, 'p', 'i', 'p', 'e', 'l', 'i', 'n', 'i', 'n', 'g'};
  CHECK(hello == expected_hello);

  const std::string message(300, 'm');
  const auto disconnected = frame::encode_agent_disconnect(UINT64_MAX, 240, 7, message);
  check_frame_header(disconnected, frame::FrameType::AgentDisconnect);
  const auto parsed = frame::parse(frame::FrameType::HaproxyDisconnect,
                                    std::span<const uint8_t>(disconnected).subspan(5));
  const auto &disconnect = std::get<frame::HaproxyDisconnect>(parsed);
  CHECK(disconnect.stream_id == UINT64_MAX && disconnect.frame_id == 240);
  CHECK(disconnect.status_code == 7 && disconnect.message == message);

  const auto ack = frame::encode_agent_ack(
      5, 7, {{frame::VarScope::Session, "ip_bad", TypedData::make_boolean(true)}});
  check_frame_header(ack, frame::FrameType::AgentAck);
  const std::vector<uint8_t> expected_ack{
      0, 0, 0, 18, 0x67, 0, 0, 0, 1, 5, 7,
      1, 3, 1, 6, 'i', 'p', '_', 'b', 'a', 'd', 0x11};
  CHECK(ack == expected_ack);

  const std::array vars{
      frame::AckVar{frame::VarScope::Session, "ip_bad", TypedData::make_boolean(true)},
      frame::AckVar{frame::VarScope::Transaction, "category", TypedData::make_string("abuse")}};
  const std::vector<frame::AckVar> dynamic_vars(vars.begin(), vars.end());
  const auto multiple = frame::encode_agent_ack(UINT64_MAX, UINT64_MAX, vars);
  check_frame_header(multiple, frame::FrameType::AgentAck);
  CHECK(multiple == frame::encode_agent_ack(UINT64_MAX, UINT64_MAX, dynamic_vars));
  const auto empty = frame::encode_agent_ack(0, 0, {});
  CHECK(empty == std::vector<uint8_t>{0, 0, 0, 7, 0x67, 0, 0, 0, 1, 0, 0});
}

} // namespace

int main() {
  try {
    test_varint_roundtrip();
    test_typeddata_roundtrip();
    test_parse_hello_and_disconnect();
    test_parse_notify();
    test_error_paths();
    test_encode_frames();
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  std::cout << "all spoe parse tests passed\n";
  return 0;
}
