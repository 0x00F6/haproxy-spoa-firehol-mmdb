#pragma once

#include "spoa/message.h"

#include <cstdint>
#include <initializer_list>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

namespace spoe::frame {

//! SPOE frame types.
enum class FrameType : uint8_t {
  HaproxyHello = 0x01,
  HaproxyDisconnect = 0x02,
  Notify = 0x03,
  AgentHello = 0x65,
  AgentDisconnect = 0x66,
  AgentAck = 0x67,
};

//! Views in inbound frames borrow the payload supplied to parse().
struct HaproxyHello {
  uint64_t stream_id = 0;
  uint64_t frame_id = 0;
  std::string_view engine_id;
  std::string_view supported_versions;
  uint32_t max_frame_size = 0;
  bool healthcheck = false;
};

struct HaproxyDisconnect {
  uint64_t stream_id = 0;
  uint64_t frame_id = 0;
  uint32_t status_code = 0;
  std::string_view message;
};

struct Notify {
  uint64_t stream_id = 0;
  uint64_t frame_id = 0;
  std::vector<Message> messages;
};

using InFrame = std::variant<HaproxyHello, HaproxyDisconnect, Notify>;

//! Parse bytes after the length and type fields, beginning with frame flags.
//! Throws Error for malformed input or a frame type not sent by HAProxy.
InFrame parse(FrameType frame_type, std::span<const uint8_t> payload);

//! Encoded frames start with the four-byte length prefix HAProxy expects.
std::vector<uint8_t> encode_agent_hello(uint64_t stream_id, uint64_t frame_id,
                                        uint32_t max_frame_size);

std::vector<uint8_t> encode_agent_disconnect(uint64_t stream_id,
                                             uint64_t frame_id,
                                             uint32_t status_code,
                                             std::string_view message);

//! Variable scopes used by the SET-VAR action.
enum class VarScope : uint8_t {
  Process = 0,
  Session = 1,
  Transaction = 2,
  Request = 3,
  Response = 4,
};

struct AckVar {
  VarScope scope = VarScope::Session;
  //! Borrowed data is serialized before encode_agent_ack() returns.
  std::string_view name;
  TypedData value;
};

//! Encode SET-VAR actions without requiring a heap-allocated argument list.
std::vector<uint8_t> encode_agent_ack(uint64_t stream_id, uint64_t frame_id,
                                      std::span<const AckVar> vars);

inline std::vector<uint8_t>
encode_agent_ack(uint64_t stream_id, uint64_t frame_id,
                 std::initializer_list<AckVar> vars) {
  return encode_agent_ack(stream_id, frame_id,
                          std::span<const AckVar>(vars.begin(), vars.size()));
}

//! Encode SET-VAR actions directly into the destination buffer without dynamic
//! allocation. Returns bytes written (including 4-byte length prefix), or 0 if
//! `out` is too small.
[[nodiscard]] size_t encode_agent_ack_into(std::span<uint8_t> out,
                                           uint64_t stream_id,
                                           uint64_t frame_id,
                                           std::span<const AckVar> vars);

inline size_t encode_agent_ack_into(std::span<uint8_t> out, uint64_t stream_id,
                                    uint64_t frame_id,
                                    std::initializer_list<AckVar> vars) {
  return encode_agent_ack_into(
      out, stream_id, frame_id,
      std::span<const AckVar>(vars.begin(), vars.size()));
}

} // namespace spoe::frame
