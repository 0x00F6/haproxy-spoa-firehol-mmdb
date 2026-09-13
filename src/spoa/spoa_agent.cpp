#include "spoa/spoa_agent.h"
#include "utils/net_utils.h"

#include <seastar/core/deleter.hh>
#include <seastar/core/temporary_buffer.hh>
#include <seastar/util/log.hh>

#include <fmt/ranges.h>

#include <array>
#include <string>
#include <utility>
#include <variant>

namespace spoe {
namespace {

seastar::logger logger("spoa_agent");

template <typename... Handlers> struct Overloaded : Handlers... {
  using Handlers::operator()...;
};

} // namespace

void SpoaAgent::load_drop_categories(std::string_view raw) {
  drop_categories_.load(raw);
}

void SpoaAgent::evaluate_lookup(MMDB_lookup_result_s &result, int mmdb_error,
                                std::string_view kind, bool &ip_bad) {
  metrics_.inc_lookups();
  if (mmdb_error != MMDB_SUCCESS) {
    metrics_.inc_lookup_errors();
    logger.warn("      MMDB lookup error: {}", MMDB_strerror(mmdb_error));
    return;
  }
  if (!result.found_entry) {
    logger.debug("      no MMDB entry for {}", kind);
    return;
  }

  // Only the first configured category in database order is counted. Without
  // logging, iteration stops there; the category list is collected only when
  // the log line that prints it is enabled.
  const bool log_categories = logger.is_enabled(seastar::log_level::info);
  std::vector<std::string_view> categories;
  std::string_view matched;
  Mmdb::for_each_category(result, [&](std::string_view category) {
    if (matched.empty()) {
      matched = drop_categories_.match_and_increment(category);
    }
    if (log_categories) {
      categories.push_back(category);
    }
    return log_categories || matched.empty();
  });
  if (!matched.empty()) {
    ip_bad = true;
  }
  if (log_categories) {
    logger.debug("      MMDB match matched={} categories=[{}] ip_bad={}",
                 matched, fmt::join(categories, ", "), ip_bad);
  }
}

bool SpoaAgent::evaluate_argument(const Arg &arg, const Mmdb *db,
                                  bool &ip_bad) {
  const TypedData &value = arg.value;
  const bool supported = value.type == TypedType::IPv4 ||
                         value.type == TypedType::IPv6 ||
                         value.type == TypedType::String;
  if (!supported) {
    logger.error("not implemented: arg '{}' has unsupported type {}", arg.name,
                 typed_type_name(value.type));
    return false;
  }
  if (!db) {
    logger.debug("    MMDB is disabled; skipping arg '{}'", arg.name);
    return true;
  }

  const bool log_values = logger.is_enabled(seastar::log_level::info);
  int error = MMDB_SUCCESS;
  MMDB_lookup_result_s result{};
  std::string_view kind = "address";
  switch (value.type) {
  case TypedType::IPv4:
    if (log_values) {
      logger.debug("    arg '{}' value {} (ipv4)", arg.name,
                   net::format_ip(value.ipv4).text);
    }
    result = db->lookup_ipv4(value.ipv4, &error);
    break;
  case TypedType::IPv6:
    if (log_values) {
      logger.debug("    arg '{}' value {} (ipv6)", arg.name,
                   net::format_ip(value.ipv6).text);
    }
    result = db->lookup_ipv6(value.ipv6, &error);
    break;
  default:
    if (log_values) {
      logger.debug("    arg '{}' value '{}' (string)", arg.name, value.str);
    }
    result = db->lookup_string(value.str, &error);
    kind = "string";
    break;
  }
  evaluate_lookup(result, error, kind, ip_bad);
  return true;
}

seastar::future<seastar::stop_iteration>
SpoaAgent::send_buffer(seastar::output_stream<char> &out, const uint8_t *data,
                       size_t size, seastar::stop_iteration stop) {
  return out.write(reinterpret_cast<const char *>(data), size)
      .then([&out] { return out.flush(); })
      .then([stop] { return stop; });
}

seastar::future<seastar::stop_iteration>
SpoaAgent::send_frame(seastar::output_stream<char> &out,
                      std::vector<uint8_t> frame,
                      seastar::stop_iteration stop) {
  // Transfer the encoded storage to Seastar without copying its bytes. The
  // deleter owns the vector until the output sink releases the buffer.
  auto *data = reinterpret_cast<char *>(frame.data());
  const auto size = frame.size();
  auto owner = seastar::make_deleter([frame = std::move(frame)] {});
  return out
      .write(seastar::temporary_buffer<char>(data, size, std::move(owner)))
      .then([&out] { return out.flush(); })
      .then([stop] { return stop; });
}

seastar::future<seastar::stop_iteration>
SpoaAgent::handle_notify(const frame::Notify &notify,
                         seastar::output_stream<char> &out) {
  bool not_implemented = false;
  bool ip_bad = false;
  // One retained snapshot keeps all arguments in this frame on the same
  // database version and protects category views until evaluation finishes.
  const auto db = mmdb_ ? mmdb_->snapshot() : nullptr;
  for (const auto &message : notify.messages) {
    logger.debug("  message '{}' with {} args", message.name,
                 message.args.size());
    for (const auto &arg : message.args) {
      if (!evaluate_argument(arg, db.get(), ip_bad)) {
        not_implemented = true;
      }
    }
  }
  if (not_implemented) {
    auto response = frame::encode_agent_disconnect(
        0, 0, kStatusNotImplemented, "agent does not implement argument type");
    return send_frame(out, std::move(response), seastar::stop_iteration::yes);
  }

  // Fast zero-allocation path for typical ACK responses: encode into a stack
  // buffer.
  alignas(uint32_t) std::array<uint8_t, 64> stack_buf;
  const auto ack_var = frame::AckVar{frame::VarScope::Session, "ip_bad",
                                     TypedData::make_boolean(ip_bad)};
  const size_t written = frame::encode_agent_ack_into(
      stack_buf, notify.stream_id, notify.frame_id, std::span(&ack_var, 1));
  if (__builtin_expect(written > 0, 1)) {
    return send_buffer(out, stack_buf.data(), written,
                       seastar::stop_iteration::no);
  }

  // Fallback for unexpectedly large responses.
  auto response = frame::encode_agent_ack(
      notify.stream_id, notify.frame_id,
      {{frame::VarScope::Session, "ip_bad", TypedData::make_boolean(ip_bad)}});
  return send_frame(out, std::move(response), seastar::stop_iteration::no);
}

seastar::future<seastar::stop_iteration>
SpoaAgent::handle_frame(frame::FrameType type, std::span<const uint8_t> payload,
                        seastar::output_stream<char> &out) {
  using seastar::stop_iteration;
  try {
    return std::visit(
        Overloaded{
            [&](const frame::HaproxyHello &hello) {
              logger.debug("HAPROXY-HELLO stream={} frame={} engine={} "
                           "versions={} max_frame={} healthcheck={}",
                           hello.stream_id, hello.frame_id, hello.engine_id,
                           hello.supported_versions, hello.max_frame_size,
                           hello.healthcheck);
              auto response = frame::encode_agent_hello(
                  hello.stream_id, hello.frame_id, hello.max_frame_size);
              return send_frame(out, std::move(response), stop_iteration::no);
            },
            [&](const frame::HaproxyDisconnect &disconnect) {
              logger.debug("HAPROXY-DISCONNECT stream={} frame={} status={} "
                           "msg={}",
                           disconnect.stream_id, disconnect.frame_id,
                           disconnect.status_code, disconnect.message);
              return seastar::make_ready_future<stop_iteration>(
                  stop_iteration::yes);
            },
            [&](const frame::Notify &notify) {
              return handle_notify(notify, out);
            }},
        frame::parse(type, payload));
  } catch (const std::exception &error) {
    logger.warn("parse error: {}", error.what());
    return seastar::make_ready_future<stop_iteration>(stop_iteration::yes);
  }
}

} // namespace spoe
