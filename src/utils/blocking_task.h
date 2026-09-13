#pragma once

#include <seastar/core/alien.hh>
#include <seastar/core/future.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/shared_ptr.hh>

#include <exception>
#include <thread>
#include <utility>

namespace spoe {

// Runs a blocking function on a dedicated OS thread and resolves the returned
// future on the calling shard. Long synchronous work (file copies, Git, MMDB
// generation) must not run on a reactor thread, where it stalls the event
// loop and triggers "Reactor stalled" reports.
template <typename Func> seastar::future<> run_blocking(Func func) {
  auto done = seastar::make_lw_shared<seastar::promise<>>();
  auto result = done->get_future();
  auto &alien = seastar::engine().alien();
  const unsigned shard = seastar::this_shard_id();
  std::thread([done, func = std::move(func), &alien, shard]() mutable {
    std::exception_ptr error;
    try {
      func();
    } catch (...) {
      error = std::current_exception();
    }
    seastar::alien::run_on(alien, shard, [done, error]() noexcept {
      if (error) {
        done->set_exception(error);
      } else {
        done->set_value();
      }
    });
  }).detach();
  return result;
}

} // namespace spoe
