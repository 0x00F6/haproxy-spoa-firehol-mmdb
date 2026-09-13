#include "spoa/spoa_server.h"
#include "utils/blocking_task.h"
#include <seastar/core/circular_buffer.hh>

#include <seastar/core/byteorder.hh>
#include <seastar/core/do_with.hh>
#include <seastar/core/loop.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/smp.hh>
#include <seastar/util/log.hh>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <span>
#include <system_error>
#include <utility>

namespace spoe {

using namespace seastar;

namespace {

logger server_logger("spoa_server");
constexpr std::size_t kFrameLengthSize = sizeof(uint32_t);

future<stop_iteration> process_one_frame(SpoaAgent &agent,
                                         input_stream<char> &in,
                                         output_stream<char> &out,
                                         circular_buffer<future<>> &pipeline) {
  return in.read_exactly(kFrameLengthSize)
      .then([&agent, &in, &out, &pipeline](temporary_buffer<char> length) {
        if (length.size() < kFrameLengthSize) {
          return make_ready_future<stop_iteration>(stop_iteration::yes);
        }
        const uint32_t size = read_be<uint32_t>(length.get());
        return in.read_exactly(size).then([&agent, &out, &pipeline,
                                           size](temporary_buffer<char> frame) {
          if (frame.size() < size || frame.empty()) {
            return make_ready_future<stop_iteration>(stop_iteration::yes);
          }
          const std::span<const uint8_t> bytes(
              reinterpret_cast<const uint8_t *>(frame.get()), frame.size());

          auto f = agent.handle_frame(static_cast<frame::FrameType>(bytes[0]),
                                      bytes.subspan(1), out);

          // If it's ready, we can check for stop_iteration::yes
          if (f.available() && !f.failed()) {
            if (f.get() == stop_iteration::yes) {
              return make_ready_future<stop_iteration>(stop_iteration::yes);
            }
            pipeline.push_back(make_ready_future<>());
          } else {
            pipeline.push_back(f.then([](auto) {
              // We can't cleanly interrupt the read loop from here,
              // but HaproxyDisconnect always completes synchronously anyway.
            }));
          }

          if (pipeline.size() >= 128) {
            return pipeline.front().then([&pipeline] {
              pipeline.pop_front();
              return stop_iteration::no;
            });
          }
          return make_ready_future<stop_iteration>(stop_iteration::no);
        });
      });
}

future<> handle_connection(SpoaAgent &agent, connected_socket socket) {
  auto in = socket.input();
  auto out = socket.output();
  return do_with(
      std::move(socket), std::move(in), std::move(out),
      seastar::circular_buffer<seastar::future<>>(),
      [&agent](auto &, auto &in, auto &out, auto &pipeline) {
        return repeat([&agent, &in, &out, &pipeline] {
                 return process_one_frame(agent, in, out, pipeline);
               })
            .finally([&out, &pipeline] {
              return do_until([&pipeline] { return pipeline.empty(); },
                              [&pipeline] {
                                return pipeline.front().then(
                                    [&pipeline] { pipeline.pop_front(); });
                              })
                  .finally([&out] { return out.close(); });
            });
      });
}

future<> accept_loop(SpoaAgent &agent, server_socket &listener,
                     gate &connections) {
  return repeat([&agent, &listener, &connections] {
           return listener.accept()
               .then([&agent, &connections](accept_result accepted) {
                 connections.enter();
                 (void)handle_connection(agent, std::move(accepted.connection))
                     .handle_exception([](std::exception_ptr error) {
                       server_logger.debug("connection error: {}", error);
                     })
                     .finally([&connections] { connections.leave(); });
                 return stop_iteration::no;
               })
               .handle_exception([](std::exception_ptr error) {
                 try {
                   std::rethrow_exception(error);
                 } catch (const std::system_error &e) {
                   if (e.code().value() == ECONNABORTED) {
                     server_logger.debug("listener stopped: {}", e.what());
                     return stop_iteration::yes;
                   }
                 } catch (...) {
                 }
                 server_logger.error("accept returned: {}", error);
                 return stop_iteration::yes;
               });
         })
      .discard_result();
}

} // namespace

future<> SpoaServer::start(const AppConfig &config,
                           std::shared_ptr<ReloadableMmdb> mmdb) {
  agent_.load_drop_categories(config.drop_by_category);
  agent_.set_mmdb(std::move(mmdb));
  if (this_shard_id() == 0 && !config.drop_by_category.empty()) {
    server_logger.info("DROP_BY_CATEGORY set, loaded {} category(ies): [{}]",
                       agent_.drop_category_count(),
                       agent_.drop_category_names());
  }
  return start_listening(config);
}

future<> SpoaServer::start_listening(const AppConfig &config) {
  agent_.register_metrics();
  listen_options options;
  options.reuse_address = true;
  listener_ = seastar::listen(ipv4_addr{config.listen_address}, options);
  accepting_ = accept_loop(agent_, listener_, connections_);
  server_logger.info("SPOA shard {} listening on {}", this_shard_id(),
                     config.listen_address);
  return make_ready_future<>();
}

future<> SpoaServer::stop() {
  if (listener_) {
    listener_.abort_accept();
  }
  // Stop accepting before closing the gate, then keep the agent alive until
  // every in-flight connection has finished. Releasing the database snapshot
  // unmaps it and deletes its temporary copy, which would stall the reactor
  // for tens of milliseconds, so it runs on a helper thread.
  return std::move(accepting_)
      .finally([this] { return connections_.close(); })
      .then([this] { return seastar::make_ready_future<>(); });
}

} // namespace spoe
