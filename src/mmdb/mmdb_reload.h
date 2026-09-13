#pragma once

#include "mmdb/mmdb.h"
#include "utils/unique_fd.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

namespace spoe {

// Publishes a private copy of a MaxMind database and replaces it when the
// file changes. Readers retain their snapshot until all entry data and
// string_views are consumed.
class ReloadableMmdb {
public:
  ReloadableMmdb() = default;
  ~ReloadableMmdb();
  ReloadableMmdb(const ReloadableMmdb &) = delete;
  ReloadableMmdb &operator=(const ReloadableMmdb &) = delete;

  //! Loads the database and starts watching its directory. Call once.
  void open(const std::filesystem::path &path, bool log_enabled = true);

public:
  //! Stops the watcher and releases this holder's snapshot. Unmapping a
  //! large database and deleting its temporary copy take tens of
  //! milliseconds: call from a non-reactor thread when latency matters.
  void close() noexcept;
  uint64_t reload_count() const { return reload_count_.load(); }
  std::shared_ptr<const Mmdb> snapshot() const {
    return current_.load(std::memory_order_acquire);
  }

private:
  void watch(std::stop_token stop);
  void reload();
  void stop_watcher() noexcept;

  std::atomic<std::shared_ptr<const Mmdb>> current_;
  std::atomic<uint64_t> reload_count_{0};
  bool log_enabled_ = true;
  std::string path_;
  std::string basename_;
  UniqueFd inotify_;
  // Woken on shutdown so the watcher never polls on a timer.
  UniqueFd stop_event_;
  // Declared last so it is joined before the members the thread uses go away.
  std::jthread watcher_;
};

} // namespace spoe
