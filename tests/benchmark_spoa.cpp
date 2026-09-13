#include "spoa/varint.h"
#include "flamegraph_utils.h"

#include <argparse/argparse.hpp>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utime.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace spoe::bench {

// ---------------------------------------------------------------------------
// Helpers: fast formatting and numbers
// ---------------------------------------------------------------------------

inline std::string format_commas(uint64_t val) {
  std::string s = std::to_string(val);
  for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) {
    s.insert(static_cast<size_t>(i), 1, ',');
  }
  return s;
}

inline std::string format_double(double val, int precision = 3) {
  std::ostringstream oss;
  oss << std::fixed << std::setprecision(precision) << val;
  return oss.str();
}

// Ultra-fast thread-local PRNG (XorShift64*)
class FastRng {
  uint64_t state_;

public:
  explicit FastRng(uint64_t seed = 0x853c49e6748fea9bULL) noexcept
      : state_(seed ? seed : 1) {}

  uint64_t next_u64() noexcept {
    state_ ^= state_ >> 12;
    state_ ^= state_ << 25;
    state_ ^= state_ >> 27;
    return state_ * 0x2545F4914F6CDD1DULL;
  }

  uint32_t next_u32() noexcept { return static_cast<uint32_t>(next_u64()); }
};

// ---------------------------------------------------------------------------
// Benchmark configuration
// ---------------------------------------------------------------------------

struct Config {
  fs::path binary{"build/haproxy-spoa-firehol-mmdb"};
  fs::path seastar_conf{"config/seastar.conf"};
  fs::path io_conf{"config/io.conf"};
  fs::path mmdb{"firehol.mmdb"};
  std::string host{"127.0.0.1"};
  std::string dpdk_interface;
  double duration = 5.0;
  double warmup = 1.0;
  double timeout = 5.0;
  double startup_timeout = 120.0;
  int connections = 4;
  int workers = 0;
  std::optional<int> smp;
  std::vector<std::string> fixed_ips;
  std::string categories{"unroutable,abuse"};
  std::optional<fs::path> flamegraph;
  int available_cpus = 1;
};

// ---------------------------------------------------------------------------
// SPOE Wire Protocol encoding and verification (zero-copy)
// ---------------------------------------------------------------------------

inline std::vector<uint8_t> make_haproxy_hello_frame() {
  std::vector<uint8_t> payload;
  auto append_str_kv = [&](std::string_view key, std::string_view val) {
    spoe::varint::encode_to_vec(payload, key.size());
    payload.insert(payload.end(), key.begin(), key.end());
    payload.push_back(0x08); // string
    spoe::varint::encode_to_vec(payload, val.size());
    payload.insert(payload.end(), val.begin(), val.end());
  };

  append_str_kv("supported-versions", "2.0");

  std::string_view mfs = "max-frame-size";
  spoe::varint::encode_to_vec(payload, mfs.size());
  payload.insert(payload.end(), mfs.begin(), mfs.end());
  payload.push_back(0x03); // uint32
  spoe::varint::encode_to_vec(payload, 16384);

  append_str_kv("capabilities", "");

  size_t body_size = 1 + 4 + 1 + 1 + payload.size();
  std::vector<uint8_t> out(4 + body_size);

  uint32_t be_len = __builtin_bswap32(static_cast<uint32_t>(body_size));
  std::memcpy(out.data(), &be_len, 4);
  out[4] = 1; // HaproxyHello
  out[5] = 0;
  out[6] = 0;
  out[7] = 0;
  out[8] = 1; // Flags FIN
  out[9] = 0; // stream 0
  out[10] = 0; // frame 0
  std::memcpy(out.data() + 11, payload.data(), payload.size());

  return out;
}

inline size_t encode_notify_into(std::span<uint8_t> buf, uint64_t stream_id,
                                 uint64_t frame_id, bool is_ipv6,
                                 const uint8_t *ip_bytes) noexcept {
  constexpr uint8_t msg_header[] = {
      8, 'c', 'h', 'e', 'c', 'k', '-', 'i', 'p', // varint(8) + "check-ip"
      1,                                         // 1 arg
      2, 'i', 'p'                                // varint(2) + "ip"
  };
  const size_t ip_len = is_ipv6 ? 16 : 4;
  const uint8_t ip_type = is_ipv6 ? 0x07 : 0x06;
  const size_t payload_len = sizeof(msg_header) + 1 + ip_len;

  const size_t stream_varint_len = spoe::varint::encoded_size(stream_id);
  const size_t frame_varint_len = spoe::varint::encoded_size(frame_id);

  const size_t body_size =
      1 + 4 + stream_varint_len + frame_varint_len + payload_len;
  const size_t total_size = 4 + body_size;

  if (buf.size() < total_size) {
    return 0;
  }

  uint32_t be_len = __builtin_bswap32(static_cast<uint32_t>(body_size));
  std::memcpy(buf.data(), &be_len, 4);
  size_t pos = 4;

  buf[pos++] = 3; // FrameType::Notify
  buf[pos++] = 0;
  buf[pos++] = 0;
  buf[pos++] = 0;
  buf[pos++] = 1; // Flags FIN

  spoe::varint::encode_unsafe(buf.data() + pos, stream_id);
  pos += stream_varint_len;

  spoe::varint::encode_unsafe(buf.data() + pos, frame_id);
  pos += frame_varint_len;

  std::memcpy(buf.data() + pos, msg_header, sizeof(msg_header));
  pos += sizeof(msg_header);

  buf[pos++] = ip_type;
  std::memcpy(buf.data() + pos, ip_bytes, ip_len);
  pos += ip_len;

  return pos;
}

// ---------------------------------------------------------------------------
// Worker and Connection management
// ---------------------------------------------------------------------------

struct WorkerStats {
  uint64_t count = 0;
  uint64_t blocked = 0;
  uint64_t errors = 0;
  uint64_t tx = 0;
  uint64_t rx = 0;
  double total = 0.0;
  double minimum = std::numeric_limits<double>::infinity();
  double maximum = 0.0;
  std::vector<double> samples;
  double cpu_seconds = 0.0;
  double finished = 0.0;
  std::string first_error;
  FastRng rng;

  explicit WorkerStats(uint64_t seed) : rng(seed) { samples.reserve(10000); }

  void record(double latency, bool is_blocked, size_t tx_bytes,
              size_t rx_bytes) noexcept {
    ++count;
    if (is_blocked) {
      ++blocked;
    }
    tx += tx_bytes;
    rx += rx_bytes;
    total += latency;
    if (latency < minimum)
      minimum = latency;
    if (latency > maximum)
      maximum = latency;

    // Uniform reservoir sampling (up to 100,000 samples)
    if (samples.size() < 100000) {
      samples.push_back(latency);
    } else {
      uint64_t idx = rng.next_u64() % count;
      if (idx < samples.size()) {
        samples[idx] = latency;
      }
    }
  }
};

struct FixedIp {
  bool is_ipv6 = false;
  std::array<uint8_t, 16> bytes{};
};

struct Connection {
  int fd = -1;
  uint64_t stream_id = 0;
  uint64_t sequence = 1;
  int conn_index = 0;
  std::chrono::steady_clock::time_point req_start{};
  size_t last_tx_len = 0;

  std::array<uint8_t, 128> rx_buf{};
  size_t rx_received = 0;
  uint32_t expected_frame_size = 0;
};

inline bool configure_socket(int fd, double timeout_sec) {
  int one = 1;
  if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) < 0) {
    return false;
  }
  struct timeval tv {};
  tv.tv_sec = static_cast<time_t>(timeout_sec);
  tv.tv_usec = static_cast<suseconds_t>((timeout_sec - tv.tv_sec) * 1e6);
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  return true;
}

inline bool set_nonblocking(int fd, bool nonblock) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0)
    return false;
  flags = nonblock ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
  return fcntl(fd, F_SETFL, flags) == 0;
}

// ---------------------------------------------------------------------------
// Prometheus HTTP client for shard metrics
// ---------------------------------------------------------------------------

inline std::map<int, uint64_t>
shard_lookups(const std::string &host, uint16_t port, double timeout_sec,
              const std::string &metric = "mmdb_lookups_total") {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    throw std::runtime_error("socket failed");
  }
  struct timeval tv {};
  tv.tv_sec = static_cast<time_t>(timeout_sec);
  tv.tv_usec = static_cast<suseconds_t>((timeout_sec - tv.tv_sec) * 1e6);
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
    close(fd);
    throw std::runtime_error("invalid host address: " + host);
  }

  if (connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
    close(fd);
    throw std::runtime_error("connect failed to " + host + ":" +
                             std::to_string(port));
  }

  std::string req = "GET /metrics?__aggregate__=false HTTP/1.1\r\nHost: " +
                    host + ":" + std::to_string(port) +
                    "\r\nConnection: close\r\n\r\n";
  ssize_t sent = send(fd, req.data(), req.size(), 0);
  (void)sent;

  std::string resp;
  char buf[4096];
  while (true) {
    ssize_t n = recv(fd, buf, sizeof(buf), 0);
    if (n <= 0)
      break;
    resp.append(buf, n);
  }
  close(fd);

  if (resp.find("200 OK") == std::string::npos &&
      resp.find("HTTP/1.1 200") == std::string::npos) {
    throw std::runtime_error("Metrics HTTP request failed");
  }

  std::map<int, uint64_t> counts;
  std::string pattern = "spoa_" + metric + "{";
  size_t pos = 0;
  while ((pos = resp.find(pattern, pos)) != std::string::npos) {
    size_t line_end = resp.find('\n', pos);
    if (line_end == std::string::npos)
      line_end = resp.size();
    std::string_view line(&resp[pos], line_end - pos);
    size_t shard_pos = line.find("shard=\"");
    if (shard_pos != std::string_view::npos) {
      shard_pos += 7;
      size_t quote_end = line.find('"', shard_pos);
      if (quote_end != std::string_view::npos) {
        int shard = std::stoi(
            std::string(line.substr(shard_pos, quote_end - shard_pos)));
        size_t close_brace = line.find('}', quote_end);
        if (close_brace != std::string_view::npos) {
          size_t val_pos = line.find_first_not_of(" \t", close_brace + 1);
          if (val_pos != std::string_view::npos) {
            double val = std::stod(std::string(line.substr(val_pos)));
            counts[shard] = static_cast<uint64_t>(val);
          }
        }
      }
    }
    pos = line_end;
  }

  if (counts.empty()) {
    throw std::runtime_error("No per-shard MMDB metrics found");
  }
  return counts;
}

inline double process_cpu(pid_t pid) {
  std::ifstream stat_file("/proc/" + std::to_string(pid) + "/stat");
  if (!stat_file.is_open())
    return 0.0;
  std::string content((std::istreambuf_iterator<char>(stat_file)),
                      std::istreambuf_iterator<char>());
  auto rparen = content.rfind(')');
  if (rparen == std::string::npos)
    return 0.0;
  std::string_view rest(content.data() + rparen + 1,
                        content.size() - rparen - 1);
  std::istringstream iss(std::string{rest});
  std::string token;
  long utime = 0, stime = 0;
  for (int i = 0; i <= 12; ++i) {
    if (!(iss >> token))
      break;
    if (i == 11)
      utime = std::stol(token);
    if (i == 12)
      stime = std::stol(token);
  }
  long clk_tck = sysconf(_SC_CLK_TCK);
  if (clk_tck <= 0)
    clk_tck = 100;
  return static_cast<double>(utime + stime) / clk_tck;
}

// ---------------------------------------------------------------------------
// FlameGraph Profiler RAII
// ---------------------------------------------------------------------------

class Profiler {
  fs::path directory_;
  fs::path data_path_;
  pid_t perf_pid_ = -1;
  int log_fd_ = -1;

public:
  Profiler(const fs::path &output_dir, pid_t target_pid) {
    fs::create_directories(output_dir);
    auto t = std::time(nullptr);
    auto tm = *std::localtime(&t);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y%m%d-%H%M%S-bench");
    directory_ = output_dir / oss.str();
    fs::create_directories(directory_);
    data_path_ = directory_ / "perf.data";

    std::string log_file = (directory_ / "perf.log").string();
    log_fd_ = open(log_file.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0666);

    perf_pid_ = fork();
    if (perf_pid_ == 0) {
      if (log_fd_ >= 0) {
        dup2(log_fd_, STDOUT_FILENO);
        dup2(log_fd_, STDERR_FILENO);
        close(log_fd_);
      }
      execlp("perf", "perf", "record", "-e", "cpu-clock:u", "-F", "99",
             "--call-graph", "dwarf,16384", "-p",
             std::to_string(target_pid).c_str(), "-o", data_path_.c_str(),
             nullptr);
      _exit(127);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  void stop() {
    if (perf_pid_ > 0) {
      kill(perf_pid_, SIGINT);
      int status = 0;
      waitpid(perf_pid_, &status, 0);
      perf_pid_ = -1;
    }
    if (log_fd_ >= 0) {
      close(log_fd_);
      log_fd_ = -1;
    }
  }

  ~Profiler() { stop(); }

  void finish(std::string_view bin_name) {
    stop();
    if (!fs::exists(data_path_))
      return;

    fs::path script_path = directory_ / "perf-script.txt";
    fs::path script_log = directory_ / "perf-script.log";
    std::string cmd = "perf script -f -i " + data_path_.string() + " > " +
                      script_path.string() + " 2> " + script_log.string();
    int ret = system(cmd.c_str()); (void)ret;

    std::string script_content;
    {
      std::ifstream sf(script_path);
      std::stringstream ss;
      ss << sf.rdbuf();
      script_content = ss.str();
    }

    uint64_t total_samples = 0;
    fs::path svg_path = directory_ / "flamegraph.svg";
    if (!script_content.empty()) {
      try {
        total_samples = flamegraph::render(script_content, svg_path);
      } catch (const std::exception &e) {
        // Fallback or notice
      }
    }

    auto raw_reactors = flamegraph::reactor_breakdown(script_content);
    std::map<std::string, uint64_t> reactor_counts;
    for (const auto &[k, v] : raw_reactors) {
      std::string comm = k;
      if (comm == bin_name.substr(0, 15)) {
        comm = "reactor-0";
      }
      if (comm.rfind("reactor", 0) == 0) {
        reactor_counts[comm] += v;
      }
    }

    if (total_samples == 0) {
      for (const auto &[_, count] : raw_reactors) {
        total_samples += count;
      }
    }

    fs::path display_dir = directory_;
    const char *host_output = std::getenv("BENCH_HOST_OUTPUT_DIR");
    if (host_output && *host_output) {
      std::string dir_str = directory_.lexically_normal().string();
      if (dir_str.rfind("/results", 0) == 0) {
        fs::path rel = fs::relative(directory_, "/results");
        display_dir = fs::path(host_output) / rel;
      }
    }
    fs::path display_svg_path = display_dir / "flamegraph.svg";
    fs::path display_top_path = display_dir / "flamegraph.top.txt";

    std::cout << "🔥 Flamegraph   : " << display_svg_path.string() << " ("
              << format_commas(total_samples) << " CPU samples)\n";

    size_t active = reactor_counts.size();
    uint64_t total_reactors = 0;
    for (const auto &[_, count] : reactor_counts) {
      total_reactors += count;
    }
    std::string verdict =
        (active > 1) ? "parallel work across cores confirmed"
                     : "work concentrated on a single reactor";
    std::cout << "🧵 Reactors    : " << active
              << " reactor threads with CPU samples ("
              << format_commas(total_reactors) << " samples) - " << verdict
              << "\n";

    std::vector<std::pair<std::string, uint64_t>> sorted_reactors(
        reactor_counts.begin(), reactor_counts.end());
    std::sort(sorted_reactors.begin(), sorted_reactors.end(),
              [](const auto &a, const auto &b) { return a.second > b.second; });

    std::cout << "🔁 By reactor   : ";
    bool f = true;
    for (const auto &[name, count] : sorted_reactors) {
      if (!f)
        std::cout << ", ";
      std::cout << name << "=" << format_commas(count);
      f = false;
    }
    std::cout << "\n";
    std::cout << "🔎 Top functions: " << display_top_path.string() << "\n";

    // Write reactors.txt
    fs::path reactors_path = directory_ / "reactors.txt";
    std::ofstream rf(reactors_path);
    rf << "Reactor threads with CPU samples: " << active
       << " / " << total_reactors << " samples\n";
    for (const auto &[name, count] : sorted_reactors) {
      char buf[64];
      std::snprintf(buf, sizeof(buf), "%8lu ", count);
      rf << buf << name << "\n";
    }
  }
};

// ---------------------------------------------------------------------------
// Worker Thread implementation
// ---------------------------------------------------------------------------

inline void run_worker_thread(
    [[maybe_unused]] int worker_id, int num_conns, int start_conn_idx, const Config &config,
    uint16_t port, const std::vector<FixedIp> &fixed_ips,
    std::barrier<> &sync_barrier, const std::atomic<bool> &stop_requested,
    std::chrono::steady_clock::time_point phase_start, double phase_duration,
    WorkerStats &stats) {
  std::vector<Connection> conns(num_conns);
  const auto hello_packet = make_haproxy_hello_frame();

  sockaddr_in server_addr{};
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(port);
  inet_pton(AF_INET, config.host.c_str(), &server_addr.sin_addr);

  for (int i = 0; i < num_conns; ++i) {
    auto &c = conns[i];
    c.conn_index = start_conn_idx + i;
    c.stream_id = static_cast<uint64_t>(c.conn_index + 1);
    c.sequence = 1;

    c.fd = socket(AF_INET, SOCK_STREAM, 0);
    if (c.fd < 0) {
      stats.errors++;
      stats.first_error = "socket() failed: " + std::string(strerror(errno));
      return;
    }
    if (!configure_socket(c.fd, config.timeout)) {
      stats.errors++;
      stats.first_error = "configure_socket failed";
      return;
    }
    if (connect(c.fd, reinterpret_cast<sockaddr *>(&server_addr),
                sizeof(server_addr)) != 0) {
      stats.errors++;
      stats.first_error = "connect() failed: " + std::string(strerror(errno));
      return;
    }

    ssize_t sent = send(c.fd, hello_packet.data(), hello_packet.size(), 0);
    if (sent != static_cast<ssize_t>(hello_packet.size())) {
      stats.errors++;
      stats.first_error = "Failed to send HAPROXY-HELLO";
      return;
    }

    uint32_t resp_len_be = 0;
    size_t read_bytes = 0;
    while (read_bytes < 4) {
      ssize_t n = recv(c.fd, reinterpret_cast<char *>(&resp_len_be) + read_bytes,
                       4 - read_bytes, 0);
      if (n <= 0) {
        stats.errors++;
        stats.first_error = "Failed to read AGENT-HELLO header";
        return;
      }
      read_bytes += n;
    }
    uint32_t resp_len = __builtin_bswap32(resp_len_be);
    if (resp_len < 7 || resp_len > 16384) {
      stats.errors++;
      stats.first_error = "Invalid AGENT-HELLO frame size: " + std::to_string(resp_len);
      return;
    }
    std::vector<uint8_t> resp_buf(resp_len);
    read_bytes = 0;
    while (read_bytes < resp_len) {
      ssize_t n = recv(c.fd, resp_buf.data() + read_bytes,
                       resp_len - read_bytes, 0);
      if (n <= 0) {
        stats.errors++;
        stats.first_error = "Failed to read AGENT-HELLO payload";
        return;
      }
      read_bytes += n;
    }
    if (resp_buf.size() < 7 || resp_buf[0] != 0x65) {
      stats.errors++;
      stats.first_error = "Invalid AGENT-HELLO response";
      return;
    }

    set_nonblocking(c.fd, true);
  }

  int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
  if (epoll_fd < 0) {
    stats.errors++;
    stats.first_error = "epoll_create1 failed";
    return;
  }

  for (int i = 0; i < num_conns; ++i) {
    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLET;
    ev.data.ptr = &conns[i];
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, conns[i].fd, &ev);
  }

  sync_barrier.arrive_and_wait();

  auto now = std::chrono::steady_clock::now();
  if (now < phase_start) {
    std::this_thread::sleep_until(phase_start);
  }

  struct timespec cpu_start {};
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu_start);

  const auto phase_end =
      phase_start +
      std::chrono::duration_cast<std::chrono::steady_clock::duration>(
          std::chrono::duration<double>(phase_duration));

  alignas(64) std::array<uint8_t, 64> send_buf{};
  for (int i = 0; i < num_conns; ++i) {
    auto &c = conns[i];
    const auto &fip = fixed_ips[(c.sequence + c.conn_index) % fixed_ips.size()];
    bool is_v6 = fip.is_ipv6;
    const uint8_t *ip_ptr = fip.bytes.data();

    size_t pkt_len = encode_notify_into(send_buf, c.stream_id, c.sequence,
                                        is_v6, ip_ptr);
    c.last_tx_len = pkt_len;
    c.req_start = std::chrono::steady_clock::now();
    send(c.fd, send_buf.data(), pkt_len, MSG_NOSIGNAL);
  }

  std::array<epoll_event, 64> events{};
  while (!stop_requested.load(std::memory_order_relaxed) &&
         std::chrono::steady_clock::now() < phase_end) {
    int nfds = epoll_wait(epoll_fd, events.data(), static_cast<int>(events.size()), 50);
    if (nfds < 0) {
      if (errno == EINTR)
        continue;
      break;
    }

    auto now_tick = std::chrono::steady_clock::now();
    for (int e = 0; e < nfds; ++e) {
      auto *c = static_cast<Connection *>(events[e].data.ptr);

      while (true) {
        if (c->rx_received < 4) {
          ssize_t n = recv(c->fd, c->rx_buf.data() + c->rx_received,
                           4 - c->rx_received, 0);
          if (n > 0) {
            c->rx_received += n;
            if (c->rx_received == 4) {
              uint32_t len_be = 0;
              std::memcpy(&len_be, c->rx_buf.data(), 4);
              c->expected_frame_size = 4 + __builtin_bswap32(len_be);
            }
          } else {
            break;
          }
        }

        if (c->rx_received >= 4) {
          ssize_t n = recv(c->fd, c->rx_buf.data() + c->rx_received,
                           c->expected_frame_size - c->rx_received, 0);
          if (n > 0) {
            c->rx_received += n;
          } else {
            break;
          }
        }

        if (c->rx_received == c->expected_frame_size) {
          double latency =
              std::chrono::duration<double>(now_tick - c->req_start).count();
          bool is_blocked = (c->rx_buf[c->rx_received - 1] == 0x11);
          stats.record(latency, is_blocked, c->last_tx_len, c->rx_received);

          c->sequence++;
          c->rx_received = 0;
          c->expected_frame_size = 0;

          if (now_tick < phase_end) {
            const auto &fip = fixed_ips[(c->sequence + c->conn_index) % fixed_ips.size()];
            bool is_v6 = fip.is_ipv6;
            const uint8_t *ip_ptr = fip.bytes.data();

            size_t pkt_len = encode_notify_into(send_buf, c->stream_id,
                                                c->sequence, is_v6, ip_ptr);
            c->last_tx_len = pkt_len;
            c->req_start = now_tick;
            send(c->fd, send_buf.data(), pkt_len, MSG_NOSIGNAL);
          }
        }
      }
    }
  }

  struct timespec cpu_end {};
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu_end);
  stats.cpu_seconds = (cpu_end.tv_sec - cpu_start.tv_sec) +
                      (cpu_end.tv_nsec - cpu_start.tv_nsec) * 1e-9;
  stats.finished = std::chrono::duration<double>(
                       std::chrono::steady_clock::now().time_since_epoch())
                       .count();

  close(epoll_fd);
  for (int i = 0; i < num_conns; ++i) {
    if (conns[i].fd >= 0) {
      close(conns[i].fd);
    }
  }
}

// ---------------------------------------------------------------------------
// Main Benchmark Engine
// ---------------------------------------------------------------------------

struct AggregateResult {
  uint64_t count = 0;
  uint64_t blocked = 0;
  uint64_t errors = 0;
  uint64_t tx = 0;
  uint64_t rx = 0;
  double total = 0.0;
  double minimum = std::numeric_limits<double>::infinity();
  double maximum = 0.0;
  std::string first_error;
  double client_cpu = 0.0;
  double server_cpu = 0.0;
  int touches = 0;
  std::map<int, uint64_t> shard_counts;
  std::map<int, uint64_t> reload_counts;
  std::vector<std::pair<double, double>> weighted_samples;

  void report(double elapsed, const Config &args) {
    auto samples = weighted_samples;
    std::sort(samples.begin(), samples.end(),
              [](const auto &a, const auto &b) { return a.first < b.first; });

    double total_weight = 0.0;
    for (const auto &[_, weight] : samples) {
      total_weight += weight;
    }

    auto percentile = [&](double p) -> double {
      if (samples.empty())
        return 0.0;
      double target = p * total_weight;
      double cumulative = 0.0;
      for (const auto &[sample, weight] : samples) {
        cumulative += weight;
        if (cumulative >= target) {
          return sample * 1000.0;
        }
      }
      return samples.back().first * 1000.0;
    };

    std::cout
        << "\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";
    std::cout << "🏁 SPOE benchmark results\n";
    std::cout << "📂 Binary       : " << args.binary.string() << "\n";
    std::cout << "🗃️  Database     : " << args.mmdb.string() << "\n";
    std::cout << "🛡️  Categories   : " << args.categories << "\n";
    std::string network =
        args.dpdk_interface.empty()
            ? "local"
            : ("DPDK AF_PACKET virtual (" + args.dpdk_interface + ")");
    std::cout << "🌐 Network      : " << network << ", server " << args.host
              << "\n";
    std::cout << "🔌 Connections  : " << args.connections
              << " (one outstanding request each)\n";
    std::cout << "⚡ Client workers: " << args.workers << " processes\n";
    std::cout << "💻 CPU usage    : client " << format_double(client_cpu, 1)
              << "% / server " << format_double(server_cpu, 1)
              << "% (100% = one CPU)\n";
    int active_smp = args.smp.value_or(static_cast<int>(shard_counts.size()));
    std::cout << "🧠 CPU shards   : " << active_smp << " / "
              << args.available_cpus << " available CPUs\n";

    size_t active_lookups = 0;
    for (const auto &[_, v] : shard_counts) {
      if (v > 0)
        ++active_lookups;
    }
    std::cout << "⚙️  Active shards: " << active_lookups << " / " << active_smp
              << " performed lookups\n";

    std::cout << "🔎 Shard lookups: ";
    bool first = true;
    for (const auto &[shard, count] : shard_counts) {
      if (!first)
        std::cout << ", ";
      std::cout << shard << "=" << format_commas(count);
      first = false;
    }
    std::cout << "\n";

    std::string workload;
    if (!args.fixed_ips.empty()) {
      for (size_t i = 0; i < args.fixed_ips.size(); ++i) {
        if (i)
          workload += ", ";
        workload += args.fixed_ips[i];
      }
    } else {
      workload = "Random IPv4/IPv6 (50/50 probability, full address ranges)";
    }
    std::cout << "🌐 IP workload  : " << workload << "\n";

    std::cout << "⏱️  Measured time: " << format_double(elapsed, 3)
              << " s (warmup excluded: " << args.warmup << " s)\n";
    std::cout << "✅ Requests     : " << format_commas(count)
              << " successful / " << format_commas(errors) << " errors\n";
    std::cout << "🔄 MMDB touches : " << touches
              << " (one per second during measurement)\n";

    uint64_t min_reloads = 0;
    uint64_t sum_reloads = 0;
    if (!reload_counts.empty()) {
      min_reloads = std::numeric_limits<uint64_t>::max();
      for (const auto &[_, count] : reload_counts) {
        min_reloads = std::min(min_reloads, count);
        sum_reloads += count;
      }
    }
    std::cout << "🗃️  DB rotations : " << min_reloads
              << " confirmed on every shard / " << sum_reloads
              << " shard reloads in total\n";

    std::cout << "🔎 Shard reloads: ";
    first = true;
    for (const auto &[shard, count] : reload_counts) {
      if (!first)
        std::cout << ", ";
      std::cout << shard << "=" << count;
      first = false;
    }
    std::cout << "\n";

    std::cout << "✅ ip_bad = 0   : " << format_commas(count - blocked)
              << "\n";
    std::cout << "🚫 ip_bad = 1   : " << format_commas(blocked) << "\n";
    std::cout << "🚀 Throughput   : "
              << format_commas(static_cast<uint64_t>(std::round(count / elapsed)))
              << " requests/s\n";
    std::cout << "📡 SPOE traffic : TX "
              << format_double(tx / elapsed / 1048576.0, 2) << " MiB/s / RX "
              << format_double(rx / elapsed / 1048576.0, 2) << " MiB/s\n";

    if (!samples.empty() && count > 0) {
      double min_disp = (minimum < 1e-6) ? 0.001 : (minimum * 1000.0);
      std::cout << "📊 Latency (ms) : min " << format_double(min_disp, 3)
                << " / mean " << format_double((total / count) * 1000.0, 3)
                << " / max " << format_double(maximum * 1000.0, 3) << "\n";
      std::cout << "📈 Percentiles  : p50 " << format_double(percentile(0.50), 3)
                << " / p95 " << format_double(percentile(0.95), 3) << " / p99 "
                << format_double(percentile(0.99), 3) << "\n";
      std::cout << "🔬 Samples      : " << format_commas(samples.size())
                << " (weighted reservoirs, at most 100,000 samples per client "
                   "process)\n";
    }

    if (!first_error.empty()) {
      std::cout << "❌ First error  : " << first_error << "\n";
    }

    std::cout << "ℹ️  Latency includes TCP and C++ client scheduling.\n";
    std::cout
        << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
        << std::flush;
  }
};

inline int execute_benchmark(const Config &args) {
  uint16_t port = 0;
  uint16_t metrics_port = 0;
  {
    int spoe_fd = socket(AF_INET, SOCK_STREAM, 0);
    int metrics_fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    bind(spoe_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
    bind(metrics_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
    socklen_t len = sizeof(addr);
    getsockname(spoe_fd, reinterpret_cast<sockaddr *>(&addr), &len);
    port = ntohs(addr.sin_port);
    len = sizeof(addr);
    getsockname(metrics_fd, reinterpret_cast<sockaddr *>(&addr), &len);
    metrics_port = ntohs(addr.sin_port);
    close(spoe_fd);
    close(metrics_fd);
  }

  std::vector<std::string> cmd;
  cmd.push_back(args.binary.string());
  cmd.push_back("--seastar-conf");
  cmd.push_back(args.seastar_conf.string());
  cmd.push_back("--io-conf");
  cmd.push_back(args.io_conf.string());
  if (args.smp) {
    cmd.push_back("--smp");
    cmd.push_back(std::to_string(*args.smp));
  }
  if (!args.dpdk_interface.empty()) {
    cmd.push_back("--dpdk-interface");
    cmd.push_back(args.dpdk_interface);
    cmd.push_back("--host-ipv4-addr");
    cmd.push_back(args.host);
    cmd.push_back("--gw-ipv4-addr");
    cmd.push_back("198.18.0.1");
    cmd.push_back("--netmask-ipv4-addr");
    cmd.push_back("255.255.255.252");
    cmd.push_back("--dhcp");
    cmd.push_back("0");
    cmd.push_back("--lro");
    cmd.push_back("off");
  }

  std::cout << "Server command:";
  for (const auto &c : cmd) {
    std::cout << " " << c;
  }
  std::cout << std::endl;

  char log_path[] = "/tmp/spoa_bench_server_XXXXXX";
  int log_fd = mkstemp(log_path);
  if (log_fd < 0) {
    std::cerr << "Failed to create temp log file\n";
    return 1;
  }

  pid_t server_pid = fork();
  if (server_pid == 0) {
    dup2(log_fd, STDOUT_FILENO);
    dup2(log_fd, STDERR_FILENO);
    close(log_fd);

    setenv("MMDB_PATH", args.mmdb.c_str(), 1);
    setenv("DROP_BY_CATEGORY", args.categories.c_str(), 1);
    std::string s_addr = args.host + ":" + std::to_string(port);
    std::string m_addr = args.host + ":" + std::to_string(metrics_port);
    setenv("SERVER_LISTEN_ADDRESS", s_addr.c_str(), 1);
    setenv("METRICS_LISTEN_ADDRESS", m_addr.c_str(), 1);

    std::vector<char *> argv_ptrs;
    for (const auto &s : cmd) {
      argv_ptrs.push_back(const_cast<char *>(s.c_str()));
    }
    argv_ptrs.push_back(nullptr);

    execvp(argv_ptrs[0], argv_ptrs.data());
    _exit(127);
  }

  auto cleanup_server = [&]() {
    if (server_pid > 0) {
      kill(server_pid, SIGTERM);
      int status = 0;
      for (int i = 0; i < 50; ++i) {
        pid_t p = waitpid(server_pid, &status, WNOHANG);
        if (p == server_pid)
          break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      kill(server_pid, SIGKILL);
      waitpid(server_pid, &status, 0);
      server_pid = -1;
    }
    if (log_fd >= 0) {
      close(log_fd);
      log_fd = -1;
    }
    unlink(log_path);
  };

  auto startup_deadline =
      std::chrono::steady_clock::now() +
      std::chrono::duration<double>(args.startup_timeout);
  int detected_smp = 0;

  while (true) {
    int status = 0;
    if (waitpid(server_pid, &status, WNOHANG) != 0) {
      std::cerr << "❌ Benchmark failed: Server exited with code " << status
                << "\n";
      std::ifstream f(log_path);
      std::string l;
      while (std::getline(f, l)) {
        std::cerr << l << "\n";
      }
      cleanup_server();
      return 1;
    }

    try {
      auto shards = shard_lookups(args.host, metrics_port, args.timeout);
      if (!args.smp || static_cast<int>(shards.size()) == *args.smp) {
        detected_smp = static_cast<int>(shards.size());
        break;
      }
    } catch (...) {
      // Retry
    }

    if (std::chrono::steady_clock::now() >= startup_deadline) {
      std::cerr << "❌ Benchmark failed: Server startup timed out\n";
      cleanup_server();
      return 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  Config run_args = args;
  if (!run_args.smp) {
    run_args.smp = detected_smp;
  }

  std::vector<FixedIp> parsed_fixed_ips;
  for (const auto &ip_str : args.fixed_ips) {
    FixedIp fip;
    if (inet_pton(AF_INET, ip_str.c_str(), fip.bytes.data()) == 1) {
      fip.is_ipv6 = false;
      parsed_fixed_ips.push_back(fip);
    } else if (inet_pton(AF_INET6, ip_str.c_str(), fip.bytes.data()) == 1) {
      fip.is_ipv6 = true;
      parsed_fixed_ips.push_back(fip);
    }
  }
  
  if (parsed_fixed_ips.empty()) {
    FastRng rng(42);
    parsed_fixed_ips.resize(65536);
    for (auto &fip : parsed_fixed_ips) {
      fip.is_ipv6 = (rng.next_u32() & 1) != 0;
      if (fip.is_ipv6) {
        uint64_t h = rng.next_u64();
        uint64_t l = rng.next_u64();
        std::memcpy(fip.bytes.data(), &h, 8);
        std::memcpy(fip.bytes.data() + 8, &l, 8);
      } else {
        uint32_t v4 = rng.next_u32();
        std::memcpy(fip.bytes.data(), &v4, 4);
      }
    }
  }

  const int workers = run_args.workers;
  const int total_conns = run_args.connections;

  auto run_phase = [&](double duration, bool is_warmup,
                       std::vector<WorkerStats> &worker_stats) {
    std::barrier sync_barrier(workers + 1);
    std::atomic<bool> stop_requested(false);

    auto start_time =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(100);

    std::vector<std::jthread> threads;
    threads.reserve(workers);

    for (int w = 0; w < workers; ++w) {
      int conns_for_worker =
          total_conns / workers + (w < (total_conns % workers) ? 1 : 0);
      int start_idx = w * (total_conns / workers) +
                      std::min(w, total_conns % workers);

      threads.emplace_back([&, w, conns_for_worker, start_idx]() {
        run_worker_thread(w, conns_for_worker, start_idx, run_args, port,
                          parsed_fixed_ips, sync_barrier, stop_requested,
                          start_time, duration, worker_stats[w]);
      });
    }

    sync_barrier.arrive_and_wait();

    if (is_warmup) {
      std::cout << "🔥 Warming up for " << run_args.warmup << " s with "
                << workers << " client processes…" << std::endl;
    }

    for (auto &th : threads) {
      if (th.joinable())
        th.join();
    }
  };

  // 1. Warmup Phase
  std::vector<WorkerStats> warmup_stats;
  warmup_stats.reserve(workers);
  for (int i = 0; i < workers; ++i) {
    warmup_stats.emplace_back(1000 + i);
  }
  run_phase(run_args.warmup, true, warmup_stats);

  for (const auto &ws : warmup_stats) {
    if (ws.errors > 0) {
      std::cerr << "❌ Warmup failed: " << ws.first_error << "\n";
      cleanup_server();
      return 1;
    }
  }

  // 2. Initial counters before benchmark measurement
  auto before_counts =
      shard_lookups(args.host, metrics_port, args.timeout, "mmdb_lookups_total");
  auto before_reloads = shard_lookups(args.host, metrics_port, args.timeout,
                                      "mmdb_reloads_total");

  // FlameGraph profiler optional
  std::unique_ptr<Profiler> profiler;
  if (run_args.flamegraph) {
    profiler = std::make_unique<Profiler>(*run_args.flamegraph, server_pid);
  }

  // 3. Measurement Phase
  std::cout << "⏱️  Benchmarking for " << run_args.duration << " s…"
            << std::endl;

  std::vector<WorkerStats> bench_stats;
  bench_stats.reserve(workers);
  for (int i = 0; i < workers; ++i) {
    bench_stats.emplace_back(2000 + i);
  }

  std::barrier sync_barrier(workers + 1);
  std::atomic<bool> stop_requested(false);

  auto bench_start_time =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(100);

  std::vector<std::jthread> threads;
  threads.reserve(workers);

  for (int w = 0; w < workers; ++w) {
    int conns_for_worker =
        total_conns / workers + (w < (total_conns % workers) ? 1 : 0);
    int start_idx =
        w * (total_conns / workers) + std::min(w, total_conns % workers);

    threads.emplace_back([&, w, conns_for_worker, start_idx]() {
      run_worker_thread(w, conns_for_worker, start_idx, run_args, port,
                        parsed_fixed_ips, sync_barrier, stop_requested,
                        bench_start_time, run_args.duration, bench_stats[w]);
    });
  }

  sync_barrier.arrive_and_wait();

  if (std::chrono::steady_clock::now() < bench_start_time) {
    std::this_thread::sleep_until(bench_start_time);
  }

  auto cpu_start_wall = std::chrono::steady_clock::now();
  double server_cpu_before = process_cpu(server_pid);

  int touches = 0;
  auto next_touch = bench_start_time + std::chrono::seconds(1);
  auto bench_end_time =
      bench_start_time +
      std::chrono::duration_cast<std::chrono::steady_clock::duration>(
          std::chrono::duration<double>(run_args.duration));

  while (std::chrono::steady_clock::now() < bench_end_time) {
    auto now_pt = std::chrono::steady_clock::now();
    if (now_pt >= next_touch) {
      utime(args.mmdb.c_str(), nullptr);
      touches++;
      next_touch += std::chrono::seconds(1);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  for (auto &th : threads) {
    if (th.joinable())
      th.join();
  }

  double server_cpu_after = process_cpu(server_pid);
  auto cpu_end_wall = std::chrono::steady_clock::now();
  double cpu_wall_sec =
      std::chrono::duration<double>(cpu_end_wall - cpu_start_wall).count();
  double server_cpu_percent =
      (server_cpu_after - server_cpu_before) / cpu_wall_sec * 100.0;

  if (profiler) {
    profiler->stop();
  }

  double max_finished = 0.0;
  for (const auto &ws : bench_stats) {
    if (ws.finished > max_finished)
      max_finished = ws.finished;
  }
  double bench_start_epoch =
      std::chrono::duration<double>(bench_start_time.time_since_epoch()).count();
  double elapsed = max_finished > bench_start_epoch
                       ? (max_finished - bench_start_epoch)
                       : run_args.duration;

  auto after_counts =
      shard_lookups(args.host, metrics_port, args.timeout, "mmdb_lookups_total");

  auto reload_deadline =
      std::chrono::steady_clock::now() +
      std::chrono::duration<double>(args.timeout);
  std::map<int, uint64_t> reload_counts;

  while (true) {
    auto after_reloads = shard_lookups(args.host, metrics_port, args.timeout,
                                       "mmdb_reloads_total");
    reload_counts.clear();
    uint64_t min_rel = std::numeric_limits<uint64_t>::max();
    for (const auto &[shard, count] : after_reloads) {
      uint64_t diff = count - before_reloads[shard];
      reload_counts[shard] = diff;
      min_rel = std::min(min_rel, diff);
    }
    if (min_rel >= static_cast<uint64_t>(touches) ||
        std::chrono::steady_clock::now() >= reload_deadline) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  AggregateResult res;
  res.touches = touches;
  res.reload_counts = reload_counts;
  res.server_cpu = server_cpu_percent;

  double total_client_cpu = 0.0;
  for (const auto &ws : bench_stats) {
    res.count += ws.count;
    res.blocked += ws.blocked;
    res.errors += ws.errors;
    res.tx += ws.tx;
    res.rx += ws.rx;
    res.total += ws.total;
    if (ws.minimum < res.minimum)
      res.minimum = ws.minimum;
    if (ws.maximum > res.maximum)
      res.maximum = ws.maximum;
    if (res.first_error.empty() && !ws.first_error.empty()) {
      res.first_error = ws.first_error;
    }
    total_client_cpu += ws.cpu_seconds;

    if (!ws.samples.empty()) {
      double weight = static_cast<double>(ws.count) / ws.samples.size();
      for (double lat : ws.samples) {
        res.weighted_samples.emplace_back(lat, weight);
      }
    }
  }

  res.client_cpu = (total_client_cpu / elapsed) * 100.0;

  for (const auto &[shard, val] : after_counts) {
    res.shard_counts[shard] = val - before_counts[shard];
  }

  res.report(elapsed, run_args);

  if (profiler) {
    profiler->finish(run_args.binary.filename().string());
  }

  cleanup_server();

  return (res.errors > 0 || res.count == 0) ? 1 : 0;
}

} // namespace spoe::bench

int main(int argc, char **argv) {
  argparse::ArgumentParser program("benchmark_spoa", "2.0");

  program.add_argument("--binary")
      .default_value(std::string("build/haproxy-spoa-firehol-mmdb"))
      .help("path to haproxy-spoa-firehol-mmdb binary");

  program.add_argument("--seastar-conf")
      .default_value(std::string("config/seastar.conf"))
      .help("server configuration file");

  program.add_argument("--io-conf")
      .default_value(std::string("config/io.conf"))
      .help("server I/O configuration file");

  const char *env_mmdb = std::getenv("MMDB_PATH");
  program.add_argument("--mmdb")
      .default_value(std::string(env_mmdb ? env_mmdb : "firehol.mmdb"))
      .help("path to MMDB file");

  const char *env_dpdk = std::getenv("SPOA_BENCH_DPDK_INTERFACE");
  std::string default_dpdk = env_dpdk ? env_dpdk : "";

  program.add_argument("--host")
      .default_value(std::string(""))
      .help("server address (default: 127.0.0.1, or 198.18.0.2 with DPDK)");

  program.add_argument("--dpdk-interface")
      .default_value(default_dpdk)
      .help("interface used by DPDK AF_PACKET virtual driver");

  program.add_argument("--duration")
      .default_value(5.0)
      .scan<'g', double>()
      .help("duration of benchmark measurement in seconds");

  program.add_argument("--warmup")
      .default_value(1.0)
      .scan<'g', double>()
      .help("duration of warmup phase in seconds");

  program.add_argument("--timeout")
      .default_value(5.0)
      .scan<'g', double>()
      .help("network socket timeout in seconds");

  program.add_argument("--startup-timeout")
      .default_value(120.0)
      .scan<'g', double>()
      .help("seconds to wait for the server to be ready");

  program.add_argument("--connections")
      .default_value(4)
      .scan<'i', int>()
      .help("number of concurrent SPOE client connections");

  program.add_argument("--workers")
      .default_value(0)
      .scan<'i', int>()
      .help("number of client worker threads");

  program.add_argument("--smp")
      .default_value(0)
      .scan<'i', int>()
      .help("override Seastar shard count");

  const char *env_cat = std::getenv("DROP_BY_CATEGORY");
  program.add_argument("--categories")
      .default_value(std::string(env_cat ? env_cat : "unroutable,abuse"))
      .help("drop categories");

  program.add_argument("--ip")
      .append()
      .help("repeat to use fixed IPs instead of random IPv4/IPv6 addresses");

  program.add_argument("--flamegraph")
      .default_value(std::string(""))
      .help("record server CPU with perf and save SVG under this directory");

  try {
    program.parse_args(argc, argv);
  } catch (const std::exception &err) {
    std::cerr << err.what() << std::endl;
    std::cerr << program;
    return 1;
  }

  spoe::bench::Config config;
  config.binary = fs::canonical(program.get<std::string>("--binary"));
  config.seastar_conf = fs::canonical(program.get<std::string>("--seastar-conf"));
  config.io_conf = fs::canonical(program.get<std::string>("--io-conf"));
  config.mmdb = fs::canonical(program.get<std::string>("--mmdb"));
  config.dpdk_interface = program.get<std::string>("--dpdk-interface");

  std::string host_arg = program.get<std::string>("--host");
  if (!host_arg.empty()) {
    config.host = host_arg;
  } else {
    config.host = config.dpdk_interface.empty() ? "127.0.0.1" : "198.18.0.2";
  }

  config.duration = program.get<double>("--duration");
  config.warmup = program.get<double>("--warmup");
  config.timeout = program.get<double>("--timeout");
  config.startup_timeout = program.get<double>("--startup-timeout");
  config.connections = program.get<int>("--connections");
  config.categories = program.get<std::string>("--categories");

  int smp_val = program.get<int>("--smp");
  if (smp_val > 0) {
    config.smp = smp_val;
  }

  int available_cpus = static_cast<int>(std::thread::hardware_concurrency());
  if (available_cpus < 1)
    available_cpus = 1;
  config.available_cpus = available_cpus;

  int workers_val = program.get<int>("--workers");
  if (workers_val > 0) {
    config.workers = workers_val;
  } else {
    config.workers = std::min(available_cpus, config.connections);
  }

  if (program.is_used("--ip")) {
    config.fixed_ips = program.get<std::vector<std::string>>("--ip");
  }

  std::string fg = program.get<std::string>("--flamegraph");
  if (!fg.empty()) {
    config.flamegraph = fs::path(fg);
  }

  try {
    return spoe::bench::execute_benchmark(config);
  } catch (const std::exception &e) {
    std::cerr << "❌ Benchmark failed: " << e.what() << std::endl;
    return 1;
  }
}
