#include "mmdb/mmdb_reload.h"

#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <poll.h>
#include <seastar/util/log.hh>
#include <stdexcept>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>

namespace spoe {

static seastar::logger mmdblog("mmdb");

namespace {

constexpr uint32_t database_events = IN_ATTRIB | IN_CLOSE_WRITE | IN_MOVED_TO;
constexpr uint32_t directory_events = IN_DELETE_SELF | IN_MOVE_SELF;

std::system_error posix_error(const std::string &what) {
  return {errno, std::generic_category(), what};
}

// Copies `source` to `copy` inside the kernel, without a user-space buffer.
// copy_file_range may reflink on supporting file systems; sendfile handles
// the kernels and file systems that reject it.
void copy_contents(int source, int copy, const std::string &path) {
  constexpr size_t chunk = size_t{1} << 30;
  bool use_sendfile = false;
  for (;;) {
    const ssize_t copied =
        use_sendfile
            ? ::sendfile(copy, source, nullptr, chunk)
            : ::copy_file_range(source, nullptr, copy, nullptr, chunk, 0);
    if (copied == 0) {
      return;
    }
    if (copied > 0 || errno == EINTR) {
      continue;
    }
    if (!use_sendfile && (errno == EXDEV || errno == EINVAL ||
                          errno == ENOSYS || errno == EOPNOTSUPP)) {
      use_sendfile = true;
      continue;
    }
    throw posix_error("MMDB snapshot copy of " + path);
  }
}

std::shared_ptr<const Mmdb> load_snapshot(const std::string &path) {
  // A separate inode keeps mmap readers safe even if the source is truncated.
  UniqueFd source(::open(path.c_str(), O_RDONLY | O_CLOEXEC));
  if (!source) {
    throw posix_error(path);
  }
  const std::unique_ptr<FILE, int (*)(FILE *)> copy(std::tmpfile(),
                                                    &std::fclose);
  if (!copy) {
    throw posix_error("MMDB temporary file");
  }
  copy_contents(source.get(), fileno(copy.get()), path);
  return std::make_shared<const Mmdb>("/proc/self/fd/" +
                                      std::to_string(fileno(copy.get())));
}

} // namespace

ReloadableMmdb::~ReloadableMmdb() { close(); }

void ReloadableMmdb::close() noexcept {
  stop_watcher();
  // Readers still holding a snapshot keep it alive; this drops ours.
  current_.store(nullptr, std::memory_order_release);
  inotify_.reset();
  stop_event_.reset();
}

void ReloadableMmdb::open(const std::filesystem::path &path, bool log_enabled) {
  log_enabled_ = log_enabled;
  if (inotify_) {
    throw std::logic_error("MMDB watcher already started");
  }
  const auto absolute = std::filesystem::absolute(path);
  path_ = absolute.string();
  basename_ = absolute.filename().string();
  UniqueFd inotify(inotify_init1(IN_NONBLOCK | IN_CLOEXEC));
  if (!inotify) {
    throw posix_error("inotify_init1");
  }
  // Install before loading, so updates during the initial copy are queued.
  if (inotify_add_watch(inotify.get(), absolute.parent_path().c_str(),
                        database_events | directory_events) == -1) {
    throw posix_error("MMDB inotify_add_watch");
  }
  UniqueFd stop_event(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
  if (!stop_event) {
    throw posix_error("MMDB eventfd");
  }
  auto snapshot = load_snapshot(path_);
  const auto metadata = snapshot->metadata_string();
  current_.store(std::move(snapshot), std::memory_order_release);
  if (log_enabled_)
    mmdblog.info("loaded in memory {}: {}", path_, metadata);
  inotify_ = std::move(inotify);
  stop_event_ = std::move(stop_event);
  watcher_ = std::jthread([this](std::stop_token stop) { watch(stop); });
}

void ReloadableMmdb::stop_watcher() noexcept {
  if (!watcher_.joinable()) {
    return;
  }
  watcher_.request_stop();
  const uint64_t one = 1;
  if (::write(stop_event_.get(), &one, sizeof(one)) < 0) {
    // EAGAIN means the counter is already non-zero: the watcher will wake.
  }
  watcher_.join();
}

void ReloadableMmdb::reload() {
  try {
    auto next = load_snapshot(path_);
    const auto metadata = next->metadata_string();
    current_.store(std::move(next), std::memory_order_release);
    reload_count_.fetch_add(1);
    if (log_enabled_)
      mmdblog.info("reloaded {}: {}", path_, metadata);
  } catch (const std::exception &error) {
    if (log_enabled_)
      mmdblog.error("reload failed; keeping previous snapshot: {}",
                    error.what());
  }
}

void ReloadableMmdb::watch(std::stop_token stop) {
  alignas(inotify_event) char buffer[16384];
  const int fd = inotify_.get();
  while (!stop.stop_requested()) {
    pollfd descriptors[] = {{fd, POLLIN, 0}, {stop_event_.get(), POLLIN, 0}};
    const int ready = ::poll(descriptors, 2, -1);
    if (ready < 0 && errno == EINTR) {
      continue;
    }
    if (ready < 0 ||
        (descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL))) {
      if (log_enabled_)
        mmdblog.error("inotify poll failed");
      return;
    }
    if (descriptors[1].revents != 0 || stop.stop_requested()) {
      return;
    }
    bool changed = false;
    for (;;) {
      const auto count = ::read(fd, buffer, sizeof(buffer));
      if (count < 0 && errno == EINTR) {
        continue;
      }
      if (count < 0 && errno == EAGAIN) {
        break;
      }
      if (count <= 0) {
        if (log_enabled_)
          mmdblog.error("inotify read failed");
        return;
      }
      for (size_t offset = 0; offset < static_cast<size_t>(count);) {
        const auto *event =
            reinterpret_cast<const inotify_event *>(buffer + offset);
        offset += sizeof(inotify_event) + event->len;
        if (event->mask & (IN_IGNORED | directory_events)) {
          if (log_enabled_)
            mmdblog.warn(
                "watched directory disappeared or moved; hot reload stopped");
          return;
        }
        if (event->mask & IN_Q_OVERFLOW) {
          changed = true;
        }
        if (event->len && (event->mask & database_events) &&
            basename_ == event->name) {
          changed = true;
        }
      }
      if (stop.stop_requested()) {
        return;
      }
    }
    if (changed && !stop.stop_requested()) {
      reload();
    }
  }
}

} // namespace spoe
