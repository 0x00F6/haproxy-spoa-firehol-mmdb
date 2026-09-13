#pragma once

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <poll.h>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace flamegraph {

namespace fs = std::filesystem;

struct ProcessResult {
  int exit_code = -1;
  std::string stdout_str;
  std::string stderr_str;
};

inline ProcessResult run_command_with_input(const std::vector<std::string> &args,
                                            const std::string &input) {
  int in_pipe[2];
  int out_pipe[2];
  int err_pipe[2];

  if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0 || pipe(err_pipe) != 0) {
    throw std::runtime_error("pipe() failed");
  }

  pid_t pid = fork();
  if (pid < 0) {
    throw std::runtime_error("fork() failed");
  }

  if (pid == 0) {
    dup2(in_pipe[0], STDIN_FILENO);
    dup2(out_pipe[1], STDOUT_FILENO);
    dup2(err_pipe[1], STDERR_FILENO);

    close(in_pipe[0]);
    close(in_pipe[1]);
    close(out_pipe[0]);
    close(out_pipe[1]);
    close(err_pipe[0]);
    close(err_pipe[1]);

    std::vector<char *> c_args;
    c_args.reserve(args.size() + 1);
    for (const auto &a : args) {
      c_args.push_back(const_cast<char *>(a.c_str()));
    }
    c_args.push_back(nullptr);

    execvp(c_args[0], c_args.data());
    _exit(127);
  }

  close(in_pipe[0]);
  close(out_pipe[1]);
  close(err_pipe[1]);

  fcntl(in_pipe[1], F_SETFL, O_NONBLOCK);
  fcntl(out_pipe[0], F_SETFL, O_NONBLOCK);
  fcntl(err_pipe[0], F_SETFL, O_NONBLOCK);

  ProcessResult res;
  size_t in_offset = 0;
  bool in_closed = input.empty();
  if (in_closed) {
    close(in_pipe[1]);
  }

  struct pollfd fds[3];
  while (true) {
    int nfds = 0;
    int in_idx = -1, out_idx = -1, err_idx = -1;

    if (!in_closed) {
      in_idx = nfds++;
      fds[in_idx].fd = in_pipe[1];
      fds[in_idx].events = POLLOUT;
    }
    if (out_pipe[0] >= 0) {
      out_idx = nfds++;
      fds[out_idx].fd = out_pipe[0];
      fds[out_idx].events = POLLIN | POLLHUP;
    }
    if (err_pipe[0] >= 0) {
      err_idx = nfds++;
      fds[err_idx].fd = err_pipe[0];
      fds[err_idx].events = POLLIN | POLLHUP;
    }

    if (nfds == 0)
      break;

    int pret = poll(fds, nfds, 60000);
    if (pret <= 0)
      break;

    if (in_idx >= 0 && (fds[in_idx].revents & POLLOUT)) {
      ssize_t n = write(in_pipe[1], input.data() + in_offset,
                        input.size() - in_offset);
      if (n > 0) {
        in_offset += n;
        if (in_offset >= input.size()) {
          close(in_pipe[1]);
          in_closed = true;
        }
      } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        close(in_pipe[1]);
        in_closed = true;
      }
    }
    if (in_idx >= 0 && (fds[in_idx].revents & (POLLERR | POLLHUP))) {
      close(in_pipe[1]);
      in_closed = true;
    }

    if (out_idx >= 0 && (fds[out_idx].revents & (POLLIN | POLLHUP))) {
      char buf[4096];
      ssize_t n = read(out_pipe[0], buf, sizeof(buf));
      if (n > 0) {
        res.stdout_str.append(buf, n);
      } else if (n == 0 ||
                 (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
        close(out_pipe[0]);
        out_pipe[0] = -1;
      }
    }

    if (err_idx >= 0 && (fds[err_idx].revents & (POLLIN | POLLHUP))) {
      char buf[4096];
      ssize_t n = read(err_pipe[0], buf, sizeof(buf));
      if (n > 0) {
        res.stderr_str.append(buf, n);
      } else if (n == 0 ||
                 (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
        close(err_pipe[0]);
        err_pipe[0] = -1;
      }
    }
  }

  if (!in_closed)
    close(in_pipe[1]);
  if (out_pipe[0] >= 0)
    close(out_pipe[0]);
  if (err_pipe[0] >= 0)
    close(err_pipe[0]);

  int status = 0;
  waitpid(pid, &status, 0);
  if (WIFEXITED(status))
    res.exit_code = WEXITSTATUS(status);
  else if (WIFSIGNALED(status))
    res.exit_code = 128 + WTERMSIG(status);

  return res;
}

struct FlamegraphTools {
  fs::path collapse;
  fs::path generator;
};

inline FlamegraphTools flamegraph_tools() {
  const char *env_dir = std::getenv("FLAMEGRAPH_DIR");
  std::vector<fs::path> candidates;
  if (env_dir && *env_dir) {
    candidates.emplace_back(env_dir);
  }
  candidates.emplace_back("/opt/FlameGraph");
  candidates.emplace_back(".cache/FlameGraph");
  candidates.emplace_back("../.cache/FlameGraph");
  candidates.emplace_back("../../.cache/FlameGraph");

  for (const auto &dir : candidates) {
    auto sc = dir / "stackcollapse-perf.pl";
    auto fg = dir / "flamegraph.pl";
    if (fs::exists(sc) && fs::exists(fg)) {
      int ret = system("which perl > /dev/null 2>&1");
      if (ret == 0) {
        return {sc, fg};
      }
    }
  }
  throw std::runtime_error(
      "Brendan Gregg FlameGraph and Perl are required; use make bench or set "
      "FLAMEGRAPH_DIR");
}

inline std::string normalize_perf_script(const std::string &script) {
  std::regex re(R"((:\s+)\d+(\s+cpu-clock(?::u)?:\s*$))");
  std::istringstream iss(script);
  std::string line;
  std::string result;
  while (std::getline(iss, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    result += std::regex_replace(line, re, "$1$2");
    result += '\n';
  }
  return result;
}

inline std::map<std::string, uint64_t>
reactor_breakdown(const std::string &perf_script) {
  std::map<std::string, uint64_t> counts;
  std::istringstream iss(perf_script);
  std::string line;
  while (std::getline(iss, line)) {
    if (line.find("cpu-clock") != std::string::npos) {
      std::istringstream liss(line);
      std::string comm;
      if (liss >> comm) {
        counts[comm]++;
      }
    }
  }
  return counts;
}

inline uint64_t render(const std::string &script, const fs::path &output) {
  auto tools = flamegraph_tools();
  std::string normalized = normalize_perf_script(script);

  while (!normalized.empty() &&
         (normalized.back() == '\n' || normalized.back() == '\r' ||
          normalized.back() == ' ')) {
    normalized.pop_back();
  }
  normalized += "\n\n";

  auto folded_res = run_command_with_input(
      {"perl", tools.collapse.string()}, normalized);

  auto collapse_log = output;
  collapse_log.replace_extension(".collapse.log");
  {
    std::ofstream lf(collapse_log);
    lf << folded_res.stderr_str;
  }

  if (folded_res.exit_code != 0) {
    throw std::runtime_error("stackcollapse-perf.pl failed: " +
                             folded_res.stderr_str);
  }

  std::map<std::string, uint64_t> leaves;
  uint64_t total = 0;

  std::istringstream folded_iss(folded_res.stdout_str);
  std::string line;
  while (std::getline(folded_iss, line)) {
    if (line.empty())
      continue;
    auto sp = line.rfind(' ');
    if (sp == std::string::npos)
      continue;
    std::string stack = line.substr(0, sp);
    uint64_t count = std::stoull(line.substr(sp + 1));
    total += count;
    auto semi = stack.rfind(';');
    std::string leaf = (semi != std::string::npos) ? stack.substr(semi + 1) : stack;
    leaves[leaf] += count;
  }

  if (total == 0) {
    throw std::runtime_error(
        "perf recorded no usable call stacks; no flamegraph generated");
  }

  auto folded_path = output;
  folded_path.replace_extension(".folded");
  {
    std::ofstream ff(folded_path);
    ff << folded_res.stdout_str;
  }

  auto svg_res = run_command_with_input(
      {"perl", tools.generator.string(), "--title",
       "Seastar server CPU flamegraph", "--countname", "samples", "--width",
       "2400"},
      folded_res.stdout_str);

  auto render_log = output;
  render_log.replace_extension(".render.log");
  {
    std::ofstream rf(render_log);
    rf << svg_res.stderr_str;
  }

  if (svg_res.exit_code != 0) {
    throw std::runtime_error("flamegraph.pl failed: " + svg_res.stderr_str);
  }

  {
    std::ofstream of(output);
    of << svg_res.stdout_str;
  }

  std::set<std::string> titles;
  std::regex title_re(R"(<title>(.*?)</title>)");
  auto words_begin = std::sregex_iterator(svg_res.stdout_str.begin(),
                                          svg_res.stdout_str.end(), title_re);
  auto words_end = std::sregex_iterator();
  for (std::sregex_iterator i = words_begin; i != words_end; ++i) {
    std::smatch match = *i;
    std::string t = match[1].str();
    if (!t.empty()) {
      titles.insert(t);
    }
  }

  auto functions_path = output;
  functions_path.replace_extension(".functions.txt");
  {
    std::ofstream func_f(functions_path);
    for (const auto &title : titles) {
      func_f << title << "\n";
    }
  }

  std::vector<std::pair<std::string, uint64_t>> sorted_leaves(leaves.begin(),
                                                               leaves.end());
  std::sort(sorted_leaves.begin(), sorted_leaves.end(),
            [](const auto &a, const auto &b) {
              if (a.second != b.second)
                return a.second > b.second;
              return a.first < b.first;
            });

  auto top_path = output;
  top_path.replace_extension(".top.txt");
  {
    std::ofstream top_f(top_path);
    top_f << "Self CPU samples (leaf functions; not inclusive time)\n";
    size_t limit = std::min<size_t>(sorted_leaves.size(), 50);
    for (size_t i = 0; i < limit; ++i) {
      const auto &[symbol, count] = sorted_leaves[i];
      double pct = (double)count / (double)total * 100.0;
      char buf[256];
      std::snprintf(buf, sizeof(buf), "%6.2f%% %8lu %s\n", pct, count,
                    symbol.c_str());
      top_f << buf;
    }
  }

  return total;
}

} // namespace flamegraph
