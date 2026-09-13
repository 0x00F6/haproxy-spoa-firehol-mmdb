#pragma once

#include "app_config.h"
#include "spoa/spoa_agent.h"

#include <seastar/core/future.hh>
#include <seastar/core/gate.hh>
#include <seastar/net/api.hh>

#include <string>

namespace spoe {

// Each shard owns its listener, request processor, database and metrics.
class SpoaServer {
public:
  seastar::future<> start(const AppConfig &config,
                          std::shared_ptr<ReloadableMmdb> mmdb);
  seastar::future<> stop();

private:
  seastar::future<> start_listening(const AppConfig &config);

  SpoaAgent agent_;
  seastar::server_socket listener_;
  seastar::gate connections_;
  seastar::future<> accepting_ = seastar::make_ready_future<>();
};

} // namespace spoe
