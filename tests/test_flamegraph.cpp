#include "flamegraph_utils.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

class TempDir {
  fs::path path_;

public:
  TempDir() {
    char templ[] = "/tmp/flamegraph_test_XXXXXX";
    char *dir = mkdtemp(templ);
    if (!dir) {
      throw std::runtime_error("mkdtemp failed");
    }
    path_ = dir;
  }

  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path_, ec);
  }

  const fs::path &path() const { return path_; }
};

static std::string read_file_content(const fs::path &p) {
  std::ifstream f(p);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

static void test_perf_stacks_and_self_time() {
  std::string script =
      "server 123 [001] 1.000: 10101010 cpu-clock:u:\n"
      "        abcd lookup<int>+0x1 (/tmp/server)\n"
      "        bcde dispatch+0x2 (/tmp/server)\n"
      "\n"
      "server 124 [002] 1.001: cpu-clock:u:\n"
      "        abcd lookup<int>+0x1 (/tmp/server)\n"
      "        bcde dispatch+0x2 (/tmp/server)\n"
      "\n"
      "server 123 [001] 1.002: cpu-clock:u:\n"
      "        cdef reload&copy+0x1 (/tmp/server)\n"
      "        bcde dispatch+0x2 (/tmp/server)\n";

  TempDir temp_dir;
  fs::path svg_path = temp_dir.path() / "flamegraph.svg";

  uint64_t total = flamegraph::render(script, svg_path);
  assert(total == 3);

  fs::path folded_path = svg_path;
  folded_path.replace_extension(".folded");
  std::string folded_content = read_file_content(folded_path);
  std::string expected_folded =
      "server;dispatch;lookup<int> 2\n"
      "server;dispatch;reload&copy 1\n";
  assert(folded_content == expected_folded);

  std::string svg_content = read_file_content(svg_path);
  assert(svg_content.find("dispatch (3 samples, 100.00%)") != std::string::npos);
  assert(svg_content.find("lookup&lt;int&gt; (2 samples, 66.67%)") !=
             std::string::npos ||
         svg_content.find("lookup<int> (2 samples, 66.67%)") !=
             std::string::npos);
  assert(svg_content.find("width=\"2400\"") != std::string::npos);
  assert(svg_content.find("function zoom(") != std::string::npos);

  fs::path functions_path = svg_path;
  functions_path.replace_extension(".functions.txt");
  std::string functions_content = read_file_content(functions_path);
  assert(functions_content.find("lookup<int> (2 samples, 66.67%)") !=
             std::string::npos ||
         functions_content.find("lookup&lt;int&gt; (2 samples, 66.67%)") !=
             std::string::npos);

  fs::path top_path = svg_path;
  top_path.replace_extension(".top.txt");
  std::string top_content = read_file_content(top_path);
  assert(top_content.find("dispatch") == std::string::npos);

  std::cout << "✅ test_perf_stacks_and_self_time passed\n";
}

static void test_no_samples_is_an_error() {
  TempDir temp_dir;
  fs::path svg_path = temp_dir.path() / "flamegraph.svg";

  bool exception_caught = false;
  try {
    flamegraph::render("", svg_path);
  } catch (const std::runtime_error &e) {
    std::string msg = e.what();
    if (msg.find("no usable call stacks") != std::string::npos) {
      exception_caught = true;
    } else {
      std::cerr << "Unexpected error message: " << msg << "\n";
    }
  }
  assert(exception_caught);
  assert(!fs::exists(svg_path));

  std::cout << "✅ test_no_samples_is_an_error passed\n";
}

static void test_unresolved_samples_are_not_discarded() {
  TempDir temp_dir;
  fs::path svg_path = temp_dir.path() / "flamegraph.svg";

  uint64_t total =
      flamegraph::render("server 123 1.000: 10101010 cpu-clock:u:\n\n", svg_path);
  assert(total == 1);

  fs::path folded_path = svg_path;
  folded_path.replace_extension(".folded");
  std::string folded_content = read_file_content(folded_path);
  assert(folded_content == "server 1\n");

  std::cout << "✅ test_unresolved_samples_are_not_discarded passed\n";
}

int main() {
  try {
    flamegraph::flamegraph_tools();
  } catch(const std::exception& e) {
    std::cout << "⏩ Skipping flamegraph test: " << e.what() << "\n";
    return 0;
  }
  try {
    test_perf_stacks_and_self_time();
    test_no_samples_is_an_error();
    test_unresolved_samples_are_not_discarded();
    std::cout << "🎉 All flamegraph tests passed successfully!\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "❌ Flamegraph test failed: " << e.what() << "\n";
    return 1;
  }
}
