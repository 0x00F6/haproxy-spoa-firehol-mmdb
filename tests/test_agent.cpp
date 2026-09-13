#include "spoa/varint.h"

#include <arpa/inet.h>
#include <cassert>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <tuple>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

static constexpr int TIMEOUT_SEC = 5;

// ---------------------------------------------------------------------------
// SPOE Wire Helpers
// ---------------------------------------------------------------------------

inline std::vector<uint8_t> varint_bytes(uint64_t value) {
  uint8_t buf[16];
  size_t len = spoe::varint::encode_unsafe(buf, value);
  return std::vector<uint8_t>(buf, buf + len);
}

inline std::vector<uint8_t> name_bytes(std::string_view str) {
  auto v = varint_bytes(str.size());
  v.insert(v.end(), str.begin(), str.end());
  return v;
}

inline std::vector<uint8_t> frame_bytes(uint8_t kind, uint64_t stream,
                                        uint64_t sequence,
                                        const std::vector<uint8_t> &payload = {}) {
  std::vector<uint8_t> body;
  body.push_back(kind);

  // Flags: 1 (FIN) as 32-bit big endian uint
  uint32_t flags = htonl(1);
  const uint8_t *flags_ptr = reinterpret_cast<const uint8_t *>(&flags);
  body.insert(body.end(), flags_ptr, flags_ptr + 4);

  auto stream_v = varint_bytes(stream);
  body.insert(body.end(), stream_v.begin(), stream_v.end());

  auto seq_v = varint_bytes(sequence);
  body.insert(body.end(), seq_v.begin(), seq_v.end());

  body.insert(body.end(), payload.begin(), payload.end());

  uint32_t body_len = htonl(static_cast<uint32_t>(body.size()));
  const uint8_t *len_ptr = reinterpret_cast<const uint8_t *>(&body_len);

  std::vector<uint8_t> pkt;
  pkt.reserve(4 + body.size());
  pkt.insert(pkt.end(), len_ptr, len_ptr + 4);
  pkt.insert(pkt.end(), body.begin(), body.end());
  return pkt;
}

// ---------------------------------------------------------------------------
// Network Helpers
// ---------------------------------------------------------------------------

inline bool read_exact(int fd, uint8_t *buf, size_t count, int timeout_sec) {
  size_t total = 0;
  while (total < count) {
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    int ret = poll(&pfd, 1, timeout_sec * 1000);
    if (ret <= 0) {
      return false;
    }
    ssize_t n = read(fd, buf + total, count - total);
    if (n <= 0) {
      return false;
    }
    total += static_cast<size_t>(n);
  }
  return true;
}

inline bool write_all(int fd, const uint8_t *buf, size_t count) {
  size_t total = 0;
  while (total < count) {
    ssize_t n = write(fd, buf + total, count - total);
    if (n <= 0) {
      if (errno == EINTR)
        continue;
      return false;
    }
    total += static_cast<size_t>(n);
  }
  return true;
}

inline std::vector<uint8_t> receive_frame(int fd, int timeout_sec = TIMEOUT_SEC) {
  uint32_t net_len = 0;
  if (!read_exact(fd, reinterpret_cast<uint8_t *>(&net_len), 4, timeout_sec)) {
    throw std::runtime_error("Failed to read frame length from agent");
  }
  uint32_t len = ntohl(net_len);
  if (len < 7 || len > 16384) {
    throw std::runtime_error("Invalid response frame size: " +
                             std::to_string(len));
  }
  std::vector<uint8_t> body(len);
  if (!read_exact(fd, body.data(), len, timeout_sec)) {
    throw std::runtime_error("Failed to read frame body from agent");
  }
  return body;
}

inline std::vector<uint8_t> exchange(int fd, const std::vector<uint8_t> &pkt,
                                     int timeout_sec = TIMEOUT_SEC) {
  if (!write_all(fd, pkt.data(), pkt.size())) {
    throw std::runtime_error("Failed to write packet to agent");
  }
  return receive_frame(fd, timeout_sec);
}

// ---------------------------------------------------------------------------
// Prometheus Metrics Client
// ---------------------------------------------------------------------------

struct Counters {
  uint64_t lookups = 0;
  uint64_t errors = 0;
  uint64_t blocked = 0;

  bool operator==(const Counters &o) const {
    return lookups == o.lookups && errors == o.errors && blocked == o.blocked;
  }
};

inline Counters fetch_counters(uint16_t metrics_port) {
  int sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0) {
    throw std::runtime_error("socket() failed");
  }

  struct timeval tv {
    TIMEOUT_SEC, 0
  };
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  sockaddr_in sin{};
  sin.sin_family = AF_INET;
  sin.sin_port = htons(metrics_port);
  sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  if (connect(sock, reinterpret_cast<sockaddr *>(&sin), sizeof(sin)) != 0) {
    close(sock);
    throw std::runtime_error("Failed to connect to metrics port");
  }

  std::string req = "GET /metrics HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
  write_all(sock, reinterpret_cast<const uint8_t *>(req.data()), req.size());

  std::string body;
  char buf[4096];
  while (true) {
    ssize_t n = read(sock, buf, sizeof(buf));
    if (n <= 0)
      break;
    body.append(buf, n);
  }
  close(sock);

  if (body.find("HTTP/1.1 200 OK") == std::string::npos &&
      body.find("HTTP/1.0 200 OK") == std::string::npos) {
    throw std::runtime_error("Metrics endpoint returned non-200");
  }

  auto parse_metric = [&](std::string_view name) -> uint64_t {
    std::regex re("^" + std::string(name) + R"((?:\{[^}]*\})?\s+(\S+))",
                  std::regex::multiline);
    auto begin = std::sregex_iterator(body.begin(), body.end(), re);
    auto end = std::sregex_iterator();
    if (begin == end) {
      throw std::runtime_error("Missing metric in Prometheus body: " +
                               std::string(name));
    }
    uint64_t total = 0;
    for (auto it = begin; it != end; ++it) {
      std::smatch m = *it;
      total += static_cast<uint64_t>(std::stod(m[1].str()));
    }
    return total;
  };

  Counters c;
  c.lookups = parse_metric("spoa_mmdb_lookups_total");
  c.errors = parse_metric("spoa_mmdb_lookup_errors_total");
  c.blocked = parse_metric("spoa_firehol_blocked_total");
  return c;
}

// ---------------------------------------------------------------------------
// Server RAII Manager
// ---------------------------------------------------------------------------

class ScopedServer {
  pid_t pid_ = -1;
  uint16_t spoe_port_ = 0;
  uint16_t metrics_port_ = 0;
  std::string log_file_;

  static uint16_t reserve_port() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
      throw std::runtime_error("socket failed");
    }
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in sin{};
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sin.sin_port = 0;
    if (bind(fd, reinterpret_cast<sockaddr *>(&sin), sizeof(sin)) != 0) {
      close(fd);
      throw std::runtime_error("bind failed");
    }
    socklen_t len = sizeof(sin);
    getsockname(fd, reinterpret_cast<sockaddr *>(&sin), &len);
    uint16_t p = ntohs(sin.sin_port);
    close(fd);
    return p;
  }

public:
  ScopedServer(const fs::path &binary, const std::string &database) {
    // Reserve two distinct ports
    while (true) {
      spoe_port_ = reserve_port();
      metrics_port_ = reserve_port();
      if (spoe_port_ != metrics_port_) {
        break;
      }
    }

    char templ[] = "/tmp/spoa_test_log_XXXXXX";
    int log_fd = mkstemp(templ);
    if (log_fd < 0) {
      throw std::runtime_error("mkstemp failed");
    }
    log_file_ = templ;

    pid_ = fork();
    if (pid_ == 0) {
      dup2(log_fd, STDOUT_FILENO);
      dup2(log_fd, STDERR_FILENO);
      close(log_fd);

      setenv("DROP_BY_CATEGORY", "unroutable", 1);
      setenv("SERVER_LISTEN_ADDRESS",
             ("127.0.0.1:" + std::to_string(spoe_port_)).c_str(), 1);
      setenv("METRICS_LISTEN_ADDRESS",
             ("127.0.0.1:" + std::to_string(metrics_port_)).c_str(), 1);
      // Exercise startup with the supplied MMDB when a FireHOL update fails.
      // A regular local file cannot be a Git repository; no network is used.
      setenv("FIREHOL_GIT_PATH", log_file_.c_str(), 1);
      setenv("FIREHOL_GIT_REPO_URL", log_file_.c_str(), 1);

      // An empty MMDB_PATH disables generation and lookups; unset would
      // default to firehol.mmdb in the working directory.
      setenv("MMDB_PATH", database.c_str(), 1);

      // First available CPU for cpuset
      cpu_set_t cpuset;
      CPU_ZERO(&cpuset);
      sched_getaffinity(0, sizeof(cpuset), &cpuset);
      int first_cpu = 0;
      for (int i = 0; i < CPU_SETSIZE; ++i) {
        if (CPU_ISSET(i, &cpuset)) {
          first_cpu = i;
          break;
        }
      }

      std::string cpu_str = std::to_string(first_cpu);
      std::vector<std::string> args = {
          binary.string(),  "--smp",
          "1",              "--cpuset",
          cpu_str,          "--memory",
          "256M",           "--overprovisioned",
          "--default-log-level", "error"};

      std::vector<char *> c_args;
      for (const auto &a : args) {
        c_args.push_back(const_cast<char *>(a.c_str()));
      }
      c_args.push_back(nullptr);

      execv(c_args[0], c_args.data());
      _exit(127);
    }
    close(log_fd);

    // Wait up to 30 seconds for metrics readiness
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    bool ready = false;
    while (std::chrono::steady_clock::now() < deadline) {
      int status = 0;
      pid_t ret = waitpid(pid_, &status, WNOHANG);
      if (ret == pid_) {
        std::ifstream lf(log_file_);
        std::stringstream ss;
        ss << lf.rdbuf();
        std::string logs = ss.str();
        if (logs.size() > 8000) {
          logs = logs.substr(logs.size() - 8000);
        }
        std::cerr << "Agent exited during startup: " << status << "\n"
                  << logs << "\n";
        throw std::runtime_error("Agent exited during startup");
      }

      try {
        fetch_counters(metrics_port_);
        ready = true;
        break;
      } catch (...) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
    }

    if (!ready) {
      stop();
      throw std::runtime_error("Agent did not become ready within 30 seconds");
    }
  }

  void stop() {
    if (pid_ > 0) {
      kill(pid_, SIGTERM);
      int status = 0;
      bool stopped = false;
      for (int i = 0; i < 50; ++i) {
        pid_t ret = waitpid(pid_, &status, WNOHANG);
        if (ret == pid_) {
          stopped = true;
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      if (!stopped) {
        kill(pid_, SIGKILL);
        waitpid(pid_, &status, 0);
      }
      pid_ = -1;
    }
    if (!log_file_.empty()) {
      unlink(log_file_.c_str());
      log_file_.clear();
    }
  }

  ~ScopedServer() { stop(); }

  uint16_t spoe_port() const { return spoe_port_; }
  uint16_t metrics_port() const { return metrics_port_; }
};

// ---------------------------------------------------------------------------
// Client Connection RAII
// ---------------------------------------------------------------------------

class ScopedConnection {
  int fd_ = -1;

public:
  explicit ScopedConnection(uint16_t port) {
    fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) {
      throw std::runtime_error("socket failed");
    }

    int flag = 1;
    setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    struct timeval tv {
      TIMEOUT_SEC, 0
    };
    setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    sockaddr_in sin{};
    sin.sin_family = AF_INET;
    sin.sin_port = htons(port);
    sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (connect(fd_, reinterpret_cast<sockaddr *>(&sin), sizeof(sin)) != 0) {
      close(fd_);
      throw std::runtime_error("Failed to connect to agent SPOE port");
    }

    // Handshake: HAPROXY-HELLO -> AGENT-HELLO
    std::vector<uint8_t> hello_payload;
    auto sv_name = name_bytes("supported-versions");
    hello_payload.insert(hello_payload.end(), sv_name.begin(), sv_name.end());
    hello_payload.push_back(8); // string
    auto sv_val = name_bytes("2.0");
    hello_payload.insert(hello_payload.end(), sv_val.begin(), sv_val.end());

    auto mfs_name = name_bytes("max-frame-size");
    hello_payload.insert(hello_payload.end(), mfs_name.begin(), mfs_name.end());
    hello_payload.push_back(3); // uint32
    auto mfs_val = varint_bytes(16384);
    hello_payload.insert(hello_payload.end(), mfs_val.begin(), mfs_val.end());

    auto cap_name = name_bytes("capabilities");
    hello_payload.insert(hello_payload.end(), cap_name.begin(), cap_name.end());
    hello_payload.push_back(8); // string
    auto cap_val = name_bytes("");
    hello_payload.insert(hello_payload.end(), cap_val.begin(), cap_val.end());

    auto hello_frame = frame_bytes(1, 0, 0, hello_payload);
    auto response = exchange(fd_, hello_frame);

    // Expected AGENT-HELLO body (type 101, flags 1, stream 0, seq 0, payload)
    std::vector<uint8_t> exp_payload;
    auto v_name = name_bytes("version");
    exp_payload.insert(exp_payload.end(), v_name.begin(), v_name.end());
    exp_payload.push_back(8);
    auto v_val = name_bytes("2.0");
    exp_payload.insert(exp_payload.end(), v_val.begin(), v_val.end());

    exp_payload.insert(exp_payload.end(), mfs_name.begin(), mfs_name.end());
    exp_payload.push_back(3);
    exp_payload.insert(exp_payload.end(), mfs_val.begin(), mfs_val.end());

    exp_payload.insert(exp_payload.end(), cap_name.begin(), cap_name.end());
    exp_payload.push_back(8);
    auto cap_pipe = name_bytes("pipelining");
    exp_payload.insert(exp_payload.end(), cap_pipe.begin(), cap_pipe.end());

    auto exp_frame = frame_bytes(101, 0, 0, exp_payload);
    // Compare without the 4-byte frame length
    std::vector<uint8_t> exp_body(exp_frame.begin() + 4, exp_frame.end());
    if (response != exp_body) {
      throw std::runtime_error("Unexpected AGENT-HELLO response from server");
    }
  }

  int fd() const { return fd_; }

  void close_socket() {
    if (fd_ >= 0) {
      close(fd_);
      fd_ = -1;
    }
  }

  ~ScopedConnection() { close_socket(); }
};

// ---------------------------------------------------------------------------
// SPOE NOTIFY Builder and Decision Checker
// ---------------------------------------------------------------------------

inline std::vector<uint8_t>
build_notify_frame(const std::vector<std::vector<std::vector<uint8_t>>> &groups,
                   uint64_t sequence) {
  std::vector<uint8_t> payload;
  for (size_t index = 0; index < groups.size(); ++index) {
    auto chk_name = name_bytes("check-" + std::to_string(index));
    payload.insert(payload.end(), chk_name.begin(), chk_name.end());
    payload.push_back(static_cast<uint8_t>(groups[index].size()));
    for (size_t i = 0; i < groups[index].size(); ++i) {
      auto ip_name = name_bytes("ip-" + std::to_string(i));
      payload.insert(payload.end(), ip_name.begin(), ip_name.end());
      payload.insert(payload.end(), groups[index][i].begin(),
                     groups[index][i].end());
    }
  }
  return frame_bytes(3, 240, sequence, payload);
}

inline bool query_decision(int client_fd,
                           const std::vector<std::vector<std::vector<uint8_t>>> &groups,
                           uint64_t sequence) {
  auto req = build_notify_frame(groups, sequence);
  auto response = exchange(client_fd, req);

  // Expected frame: 103, 240, sequence, payload: \x01\x03\x01 + name("ip_bad")
  std::vector<uint8_t> exp_p = {0x01, 0x03, 0x01};
  auto ip_bad_n = name_bytes("ip_bad");
  exp_p.insert(exp_p.end(), ip_bad_n.begin(), ip_bad_n.end());

  auto exp_frame = frame_bytes(103, 240, sequence, exp_p);
  std::vector<uint8_t> exp_body(exp_frame.begin() + 4, exp_frame.end());

  if (response.size() != exp_body.size() + 1) {
    throw std::runtime_error("Unexpected ACK response size: " +
                             std::to_string(response.size()));
  }
  if (!std::equal(exp_body.begin(), exp_body.end(), response.begin())) {
    throw std::runtime_error("ACK frame header/action prefix mismatch");
  }

  uint8_t last = response.back();
  if (last != 0x01 && last != 0x11) {
    throw std::runtime_error("ACK invalid boolean value: " + std::to_string(last));
  }
  return last == 0x11;
}

inline void assert_closed(int fd) {
  uint8_t b = 0;
  struct pollfd pfd;
  pfd.fd = fd;
  pfd.events = POLLIN;
  int ret = poll(&pfd, 1, TIMEOUT_SEC * 1000);
  assert(ret >= 0);
  ssize_t n = read(fd, &b, 1);
  assert(n == 0 && "Connection remained open");
}

// ---------------------------------------------------------------------------
// Integration Test Routine
// ---------------------------------------------------------------------------

inline void exercise(uint16_t port, uint16_t metrics_port, bool enabled) {
  // Build test fixture values for 4 IP addresses
  std::vector<std::pair<std::string, bool>> test_ips = {
      {"127.0.0.1", false},
      {"::1", true},
      {"1.1.1.1", false},
      {"2606:4700:4700::1111", true}};

  std::vector<std::vector<uint8_t>> values;
  for (const auto &[addr_str, is_v6] : test_ips) {
    // 1. Binary representation
    std::vector<uint8_t> bin_val;
    if (!is_v6) {
      bin_val.push_back(6); // type ipv4
      in_addr in;
      inet_pton(AF_INET, addr_str.c_str(), &in);
      const uint8_t *p = reinterpret_cast<const uint8_t *>(&in.s_addr);
      bin_val.insert(bin_val.end(), p, p + 4);
    } else {
      bin_val.push_back(7); // type ipv6
      in6_addr in6;
      inet_pton(AF_INET6, addr_str.c_str(), &in6);
      const uint8_t *p = reinterpret_cast<const uint8_t *>(&in6.s6_addr);
      bin_val.insert(bin_val.end(), p, p + 16);
    }
    values.push_back(bin_val);

    // 2. String representation
    std::vector<uint8_t> str_val;
    str_val.push_back(8); // type string
    auto n = name_bytes(addr_str);
    str_val.insert(str_val.end(), n.begin(), n.end());
    values.push_back(str_val);
  }

  std::vector<bool> decisions;
  std::vector<uint64_t> errors;

  {
    ScopedConnection client(port);

    for (size_t i = 0; i < values.size(); ++i) {
      auto before = fetch_counters(metrics_port);
      bool dec = query_decision(client.fd(), {{values[i]}}, 65536 + i);
      decisions.push_back(dec);
      auto after = fetch_counters(metrics_port);

      uint64_t d_lookups = after.lookups - before.lookups;
      uint64_t d_errors = after.errors - before.errors;
      uint64_t d_blocked = after.blocked - before.blocked;

      assert(d_lookups == (enabled ? 1 : 0));
      assert(d_blocked == (dec ? 1 : 0));
      assert(enabled ? (d_errors == 0 || d_errors == 1) : (d_errors == 0));
      errors.push_back(d_errors);
    }

    // Binary and String forms must agree for decision and errors
    for (size_t i = 0; i < values.size(); i += 2) {
      assert(decisions[i] == decisions[i + 1]);
      assert(errors[i] == errors[i + 1]);
    }

    auto initial = fetch_counters(metrics_port);
    uint64_t sum_err = 0, sum_dec = 0;
    for (auto e : errors)
      sum_err += e;
    for (auto d : decisions)
      if (d)
        sum_dec++;

    assert(initial.lookups == (enabled ? values.size() : 0));
    assert(initial.errors == sum_err);
    assert(initial.blocked == sum_dec);

    std::vector<std::vector<std::vector<uint8_t>>> groups;
    std::vector<uint8_t> blocked;
    std::vector<uint8_t> allowed;

    if (enabled) {
      bool any_dec = false, all_dec = true;
      int blocked_idx = -1, allowed_idx = -1;
      for (size_t i = 0; i < decisions.size(); ++i) {
        if (decisions[i]) {
          any_dec = true;
          if (blocked_idx < 0)
            blocked_idx = static_cast<int>(i);
        } else {
          all_dec = false;
          if (allowed_idx < 0 && errors[i] == 0)
            allowed_idx = static_cast<int>(i);
        }
      }
      assert(any_dec && !all_dec &&
             "Database must contain blocked and allowed fixture IPs");
      assert(blocked_idx >= 0 && allowed_idx >= 0);

      blocked = values[blocked_idx];
      allowed = values[allowed_idx];
      groups = {{blocked, allowed},
                {values[blocked_idx + 1], blocked, values[allowed_idx + 1]}};
    } else {
      for (auto d : decisions)
        assert(!d);
      groups = {
          {values[0], values[1], values[2]},
          {values[3], values[4], values[5], values[6], values[7]}};
    }

    auto before = fetch_counters(metrics_port);
    assert(query_decision(client.fd(), groups, 99999) == enabled);
    auto after = fetch_counters(metrics_port);

    uint64_t exp_lookups = enabled ? 5 : 0;
    uint64_t exp_errors = 0;
    uint64_t exp_blocked = enabled ? 3 : 0;
    assert(after.lookups - before.lookups == exp_lookups);
    assert(after.errors - before.errors == exp_errors);
    assert(after.blocked - before.blocked == exp_blocked);

    assert(!query_decision(client.fd(), {{}}, 100000) &&
           "Decision leaked from a previous NOTIFY");

    if (enabled) {
      before = fetch_counters(metrics_port);
      assert(query_decision(client.fd(), {{blocked}, {allowed}}, 100001) &&
             "Later message cleared ip_bad");
      after = fetch_counters(metrics_port);
      assert(after.lookups - before.lookups == 2);
      assert(after.errors - before.errors == 0);
      assert(after.blocked - before.blocked == 1);
    }
  }

  // Unsupported argument types
  std::vector<std::vector<uint8_t>> unsupported = {
      {0x00},
      {0x11},
      {0x03, 0x01},
      {0x09, 0x01, 'x'}};

  for (const auto &unsup : unsupported) {
    ScopedConnection client(port);
    auto before = fetch_counters(metrics_port);

    std::vector<std::vector<std::vector<uint8_t>>> bad_groups = {
        {values[0], unsup, values[0]}};
    auto req = build_notify_frame(bad_groups, 70000);
    auto resp = exchange(client.fd(), req);

    // Expected: frame(102, 0, 0, name("status-code") + \x03 + varint(14)
    // + name("message") + \x08 + name("agent does not implement argument type"))
    std::vector<uint8_t> exp_p;
    auto sc_n = name_bytes("status-code");
    exp_p.insert(exp_p.end(), sc_n.begin(), sc_n.end());
    exp_p.push_back(0x03);
    auto sc_v = varint_bytes(14);
    exp_p.insert(exp_p.end(), sc_v.begin(), sc_v.end());

    auto msg_n = name_bytes("message");
    exp_p.insert(exp_p.end(), msg_n.begin(), msg_n.end());
    exp_p.push_back(0x08);
    auto msg_v = name_bytes("agent does not implement argument type");
    exp_p.insert(exp_p.end(), msg_v.begin(), msg_v.end());

    auto exp_frame = frame_bytes(102, 0, 0, exp_p);
    std::vector<uint8_t> exp_body(exp_frame.begin() + 4, exp_frame.end());
    assert(resp == exp_body);

    assert_closed(client.fd());

    auto after = fetch_counters(metrics_port);
    uint64_t exp_lookups = enabled ? 2 : 0;
    uint64_t exp_errors = enabled ? (2 * errors[0]) : 0;
    uint64_t exp_blocked = enabled ? (2 * (decisions[0] ? 1 : 0)) : 0;
    assert(after.lookups - before.lookups == exp_lookups);
    assert(after.errors - before.errors == exp_errors);
    assert(after.blocked - before.blocked == exp_blocked);
  }

  // Malformed packets
  std::vector<std::vector<uint8_t>> malformed;
  malformed.push_back({0, 0});

  uint32_t net_20 = htonl(20);
  const uint8_t *p20 = reinterpret_cast<const uint8_t *>(&net_20);
  malformed.push_back({p20[0], p20[1], p20[2], p20[3], 0x03, 0x00});

  uint32_t net_0 = 0;
  const uint8_t *p0 = reinterpret_cast<const uint8_t *>(&net_0);
  malformed.push_back({p0[0], p0[1], p0[2], p0[3]});

  std::vector<uint8_t> bad_payload = name_bytes("bad");
  bad_payload.push_back(1);
  auto ip_n = name_bytes("ip");
  bad_payload.insert(bad_payload.end(), ip_n.begin(), ip_n.end());
  bad_payload.push_back(0x0f);
  malformed.push_back(frame_bytes(3, 1, 1, bad_payload));

  for (const auto &pkt : malformed) {
    ScopedConnection client(port);
    write_all(client.fd(), pkt.data(), pkt.size());
    shutdown(client.fd(), SHUT_WR);
    assert_closed(client.fd());
  }

  // Confirm agent still accepts new requests unharmed
  {
    ScopedConnection client(port);
    assert(!query_decision(client.fd(), {{}}, 100002) &&
           "Malformed connection affected later requests");
  }
}

// ---------------------------------------------------------------------------
// Main Entrypoint
// ---------------------------------------------------------------------------

int main(int argc, char **argv) {
  fs::path binary_path;
  std::string mmdb_path;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--binary" && i + 1 < argc) {
      binary_path = argv[++i];
    } else if (arg == "--mmdb" && i + 1 < argc) {
      mmdb_path = argv[++i];
    }
  }

  if (binary_path.empty() || !fs::exists(binary_path)) {
    std::cerr << "Usage: " << argv[0] << " --binary <path> [--mmdb <path>]\n";
    return 1;
  }

  std::vector<std::string> databases = {""};
  if (!mmdb_path.empty() && fs::exists(mmdb_path)) {
    databases.push_back(fs::canonical(mmdb_path).string());
  }

  try {
    for (const auto &db : databases) {
      ScopedServer srv(binary_path, db);
      exercise(srv.spoe_port(), srv.metrics_port(), !db.empty());
      std::cout << "SPOE integration passed (MMDB "
                << (!db.empty() ? "enabled" : "disabled") << ")\n";
    }
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "❌ Integration test failed: " << e.what() << "\n";
    return 1;
  }
}
