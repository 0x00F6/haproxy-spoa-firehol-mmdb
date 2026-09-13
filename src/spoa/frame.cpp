#include "spoa/frame.h"

#include "spoa/protocol_io.h"

#include <array>
#include <bit>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

namespace spoe::frame {
namespace {

using detail::BufferWriter;
using detail::Reader;

constexpr size_t kLengthPrefixSize = 4;
//! Only the FIN flag is set: this implementation sends complete frames.
constexpr std::array<uint8_t, 4> kFlagsFin{0, 0, 0, 1};
constexpr uint8_t kActionSetVar = 0x01;
constexpr uint8_t kSetVarArgCount = 3; // scope, name and value

Arg read_arg(Reader &reader) {
  const auto name = reader.read_string();
  return {name, TypedData::decode(reader)};
}

//! Invoke `visit(name, value)` for every key-value field left in the payload.
template <typename Visit> void for_each_field(Reader &reader, Visit &&visit) {
  while (!reader.empty()) {
    const auto [name, value] = read_arg(reader);
    visit(name, value);
  }
}

//! Payload sizes are accumulated before serialization so each response is
//! allocated exactly once; the total must fit the 32-bit frame length.
void add_size(size_t &size, size_t extra) {
  constexpr auto max_size = std::numeric_limits<uint32_t>::max();
  if (extra > max_size || size > max_size - extra) {
    throw Error("frame exceeds the SPOE length limit");
  }
  size += extra;
}

size_t field_size(std::string_view name, const TypedData &value) {
  size_t size = detail::length_prefixed_size(name.size());
  add_size(size, value.encoded_size());
  return size;
}

void append_field(std::vector<uint8_t> &out, std::string_view name,
                  const TypedData &value) {
  detail::append_length_prefixed(out, detail::as_bytes(name));
  value.encode(out);
}

//! Write the length prefix, type, flags and IDs, reserving room for
//! `payload_size` more bytes.
std::vector<uint8_t> begin_frame(FrameType type, uint64_t stream_id,
                                 uint64_t frame_id, size_t payload_size) {
  size_t frame_size = 1 + kFlagsFin.size(); // type and flags
  add_size(frame_size, varint::encoded_size(stream_id));
  add_size(frame_size, varint::encoded_size(frame_id));
  add_size(frame_size, payload_size);

  std::vector<uint8_t> out;
  out.reserve(kLengthPrefixSize + frame_size);

  const uint32_t be_size =
      (std::endian::native == std::endian::big)
          ? static_cast<uint32_t>(frame_size)
          : __builtin_bswap32(static_cast<uint32_t>(frame_size));
  const auto *size_bytes = reinterpret_cast<const uint8_t *>(&be_size);
  out.insert(out.end(), size_bytes, size_bytes + sizeof(be_size));

  out.push_back(static_cast<uint8_t>(type));
  out.insert(out.end(), kFlagsFin.begin(), kFlagsFin.end());
  varint::encode_to_vec(out, stream_id);
  varint::encode_to_vec(out, frame_id);
  return out;
}

std::vector<uint8_t> encode_fields(FrameType type, uint64_t stream_id,
                                   uint64_t frame_id,
                                   std::span<const Arg> fields) {
  size_t payload_size = 0;
  for (const auto &[name, value] : fields) {
    add_size(payload_size, field_size(name, value));
  }
  auto out = begin_frame(type, stream_id, frame_id, payload_size);
  for (const auto &[name, value] : fields) {
    append_field(out, name, value);
  }
  return out;
}

HaproxyHello parse_hello(Reader &reader, uint64_t stream_id,
                         uint64_t frame_id) {
  HaproxyHello frame;
  frame.stream_id = stream_id;
  frame.frame_id = frame_id;
  for_each_field(reader, [&](std::string_view key, const TypedData &value) {
    if (key == "engine-id" && value.type == TypedType::String) {
      frame.engine_id = value.str;
    } else if (key == "supported-versions" && value.type == TypedType::String) {
      frame.supported_versions = value.str;
    } else if (key == "max-frame-size" && value.type == TypedType::UInt32) {
      frame.max_frame_size = value.u32;
    } else if (key == "healthcheck" && value.type == TypedType::Boolean) {
      frame.healthcheck = value.boolean;
    }
  });
  return frame;
}

HaproxyDisconnect parse_disconnect(Reader &reader, uint64_t stream_id,
                                   uint64_t frame_id) {
  HaproxyDisconnect frame;
  frame.stream_id = stream_id;
  frame.frame_id = frame_id;
  for_each_field(reader, [&](std::string_view key, const TypedData &value) {
    if (key == "status-code" && value.type == TypedType::UInt32) {
      frame.status_code = value.u32;
    } else if (key == "message" && value.type == TypedType::String) {
      frame.message = value.str;
    }
  });
  return frame;
}

Notify parse_notify(Reader &reader, uint64_t stream_id, uint64_t frame_id) {
  Notify frame;
  frame.stream_id = stream_id;
  frame.frame_id = frame_id;
  frame.messages.reserve(2);
  while (!reader.empty()) {
    const auto name = reader.read_string();
    const size_t arg_count = reader.read_byte();
    // Even an empty argument name and a null value consume two bytes.
    // Reject impossible counts before allocating their argument storage.
    if (arg_count > reader.remaining() / 2) {
      throw Error("buffer too small for message arguments");
    }
    std::vector<Arg> args;
    args.reserve(arg_count);
    for (size_t i = 0; i < arg_count; ++i) {
      args.push_back(read_arg(reader));
    }
    frame.messages.push_back({name, std::move(args)});
  }
  return frame;
}

} // namespace

InFrame parse(FrameType frame_type, std::span<const uint8_t> payload) {
  Reader reader(payload);
  reader.read_bytes(kFlagsFin.size()); // flags are unused by the receive path
  const uint64_t stream_id = reader.read_varint();
  const uint64_t frame_id = reader.read_varint();

  switch (frame_type) {
  case FrameType::HaproxyHello:
    return parse_hello(reader, stream_id, frame_id);
  case FrameType::HaproxyDisconnect:
    return parse_disconnect(reader, stream_id, frame_id);
  case FrameType::Notify:
    return parse_notify(reader, stream_id, frame_id);
  case FrameType::AgentHello:
  case FrameType::AgentDisconnect:
  case FrameType::AgentAck:
    throw Error("unexpected frame type " +
                std::to_string(static_cast<uint8_t>(frame_type)));
  }
  throw Error("unknown frame type " +
              std::to_string(static_cast<uint8_t>(frame_type)));
}

std::vector<uint8_t> encode_agent_hello(uint64_t stream_id, uint64_t frame_id,
                                        uint32_t max_frame_size) {
  const std::array fields{
      Arg{"version", TypedData::make_string("2.0")},
      Arg{"max-frame-size", TypedData::make_uint32(max_frame_size)},
      Arg{"capabilities", TypedData::make_string("pipelining")}};
  return encode_fields(FrameType::AgentHello, stream_id, frame_id, fields);
}

std::vector<uint8_t> encode_agent_disconnect(uint64_t stream_id,
                                             uint64_t frame_id,
                                             uint32_t status_code,
                                             std::string_view message) {
  const std::array fields{
      Arg{"status-code", TypedData::make_uint32(status_code)},
      Arg{"message", TypedData::make_string(message)}};
  return encode_fields(FrameType::AgentDisconnect, stream_id, frame_id, fields);
}

namespace {

//! Size of an AGENT-ACK frame after its four-byte length prefix.
size_t ack_frame_size(uint64_t stream_id, uint64_t frame_id,
                      std::span<const AckVar> vars) {
  constexpr size_t action_header_size = 3; // action, argument count, scope
  size_t payload_size = 0;
  for (const auto &var : vars) {
    add_size(payload_size, action_header_size);
    add_size(payload_size, field_size(var.name, var.value));
  }
  size_t frame_size = 1 + kFlagsFin.size();
  add_size(frame_size, varint::encoded_size(stream_id));
  add_size(frame_size, varint::encoded_size(frame_id));
  add_size(frame_size, payload_size);
  return frame_size;
}

} // namespace

size_t encode_agent_ack_into(std::span<uint8_t> out, uint64_t stream_id,
                             uint64_t frame_id, std::span<const AckVar> vars) {
  const size_t frame_size = ack_frame_size(stream_id, frame_id, vars);
  if (out.size() < kLengthPrefixSize + frame_size) {
    return 0;
  }

  BufferWriter writer(out);
  writer.write_u32_be(static_cast<uint32_t>(frame_size));
  writer.write_byte(static_cast<uint8_t>(FrameType::AgentAck));
  writer.write_bytes(kFlagsFin);
  writer.write_varint(stream_id);
  writer.write_varint(frame_id);

  for (const auto &var : vars) {
    writer.write_byte(kActionSetVar);
    writer.write_byte(kSetVarArgCount);
    writer.write_byte(static_cast<uint8_t>(var.scope));
    writer.write_string(var.name);
    var.value.encode(writer);
  }

  return writer.written();
}

std::vector<uint8_t> encode_agent_ack(uint64_t stream_id, uint64_t frame_id,
                                      std::span<const AckVar> vars) {
  std::vector<uint8_t> out(kLengthPrefixSize +
                           ack_frame_size(stream_id, frame_id, vars));
  const size_t written = encode_agent_ack_into(out, stream_id, frame_id, vars);
  out.resize(written);
  return out;
}

} // namespace spoe::frame
