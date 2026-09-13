#include "app_config.h"

#include <maxminddb.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <sched.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

void check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

class TestDirectory {
public:
    TestDirectory() {
        auto pattern = (fs::temp_directory_path() / "firehol_job_XXXXXX").string();
        const char* created = ::mkdtemp(pattern.data());
        if (!created) {
            throw std::system_error(errno, std::generic_category(), "Create test directory");
        }
        path_ = created;
    }

    ~TestDirectory() {
        std::error_code error;
        fs::remove_all(path_, error);
    }

    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

class WorkingDirectory {
public:
    explicit WorkingDirectory(const fs::path& path) : original_(fs::current_path()) {
        fs::current_path(path);
    }

    ~WorkingDirectory() {
        std::error_code error;
        fs::current_path(original_, error);
    }

private:
    fs::path original_;
};

class Environment {
public:
    Environment() {
        for (const char* name : names_) {
            const char* value = std::getenv(name);
            original_.emplace_back(name, value ? std::optional<std::string>(value)
                                               : std::nullopt);
        }
    }

    ~Environment() {
        for (const auto& [name, value] : original_) {
            if (value) ::setenv(name, value->c_str(), 1);
            else ::unsetenv(name);
        }
    }

    void clear() const {
        for (const char* name : names_) unset(name);
    }

    static void set(const char* name, const std::string& value) {
        if (::setenv(name, value.c_str(), 1) != 0) {
            throw std::system_error(errno, std::generic_category(), "Set test environment");
        }
    }

    static void unset(const char* name) {
        if (::unsetenv(name) != 0) {
            throw std::system_error(errno, std::generic_category(), "Clear test environment");
        }
    }

private:
    static constexpr std::array names_{
        "SERVER_LISTEN_ADDRESS", "METRICS_LISTEN_ADDRESS", "DROP_BY_CATEGORY",
        "MMDB_PATH", "FIREHOL_GIT_PATH", "FIREHOL_GIT_REPO_URL"};
    std::vector<std::pair<const char*, std::optional<std::string>>> original_;
};

std::string read_file(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    check(static_cast<bool>(input), "Cannot read test file: " + path.string());
    return {std::istreambuf_iterator<char>(input), {}};
}

void write_file(const fs::path& path, std::string_view content) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << content;
    check(static_cast<bool>(output), "Cannot write test file: " + path.string());
}

int run_process(std::vector<std::string> arguments, const fs::path& cwd,
                const fs::path& log_path, std::string_view ready_message = {}) {
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (auto& argument : arguments) argv.push_back(argument.data());
    argv.push_back(nullptr);

    const int log_fd = ::open(log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (log_fd < 0) {
        throw std::system_error(errno, std::generic_category(), "Open child log");
    }
    const pid_t pid = ::fork();
    if (pid == 0) {
        if (::dup2(log_fd, STDOUT_FILENO) < 0 ||
            ::dup2(log_fd, STDERR_FILENO) < 0 || ::chdir(cwd.c_str()) != 0) {
            ::_exit(126);
        }
        ::close(log_fd);
        ::execvp(argv.front(), argv.data());
        ::_exit(127);
    }
    const int fork_error = errno;
    ::close(log_fd);
    if (pid < 0) {
        throw std::system_error(fork_error, std::generic_category(), "Fork test process");
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(25);
    int status = 0;
    bool shutdown_requested = false;
    while (std::chrono::steady_clock::now() < deadline) {
        const pid_t result = ::waitpid(pid, &status, WNOHANG);
        if (result == pid) {
            if (WIFEXITED(status)) return WEXITSTATUS(status);
            if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
        }
        if (result < 0 && errno != EINTR) {
            throw std::system_error(errno, std::generic_category(), "Wait for test process");
        }
        if (!ready_message.empty() && !shutdown_requested &&
            read_file(log_path).find(ready_message) != std::string::npos) {
            ::kill(pid, SIGTERM);
            shutdown_requested = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ::kill(pid, SIGKILL);
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    throw std::runtime_error("Process timed out: " + arguments.front() + "\n" +
                             read_file(log_path));
}

void run_git(const fs::path& repo, const fs::path& log,
             std::initializer_list<std::string> arguments) {
    std::vector<std::string> command{"git", "-C", repo.string(),
                                     "-c", "commit.gpgsign=false",
                                     "-c", "core.hooksPath=/dev/null"};
    command.insert(command.end(), arguments.begin(), arguments.end());
    const int status = run_process(std::move(command), repo, log);
    check(status == 0, "Git fixture command failed:\n" + read_file(log));
}

void setup_repository(const fs::path& repo, const fs::path& log) {
    fs::create_directories(repo);
    run_git(repo, log, {"init", "-b", "master"});
    run_git(repo, log, {"config", "user.name", "FireHOL Test"});
    run_git(repo, log, {"config", "user.email", "firehol-test@example.invalid"});
    write_file(repo / "initial.netset", "192.0.2.0/24\n");
    run_git(repo, log, {"add", "initial.netset"});
    run_git(repo, log, {"commit", "-m", "Initial blocklist"});
}

void check_network(const fs::path& output, const char* address,
                   std::optional<std::string_view> expected_list) {
    MMDB_s mmdb{};
    check(MMDB_open(output.c_str(), MMDB_MODE_MMAP, &mmdb) == MMDB_SUCCESS,
          "Generated MMDB should open: " + output.string());
    std::unique_ptr<MMDB_s, decltype(&MMDB_close)> guard(&mmdb, &MMDB_close);
    int gai_error = 0;
    int mmdb_error = 0;
    auto result = MMDB_lookup_string(&mmdb, address, &gai_error, &mmdb_error);
    check(gai_error == 0 && mmdb_error == MMDB_SUCCESS, "MMDB lookup failed");
    if (!expected_list) {
        check(!result.found_entry, "MMDB retained a network removed by the latest commit");
        return;
    }
    check(result.found_entry, "MMDB is missing expected network: " + std::string(address));
    MMDB_entry_data_s entry{};
    check(MMDB_get_value(&result.entry, &entry, "file_name", "0", nullptr) == MMDB_SUCCESS &&
              entry.has_data && entry.type == MMDB_DATA_TYPE_UTF8_STRING &&
              std::string_view(entry.utf8_string, entry.data_size) ==
                  std::string(*expected_list) + ".netset",
          "MMDB network should identify its local source blocklist");
}

void check_configuration(const fs::path& root, const Environment& environment) {
    std::cout << "Test 1: Environment defaults and overrides\n";
    environment.clear();
    WorkingDirectory cwd(root);
    const auto defaults = spoe::read_app_config();
    check(defaults.firehol_git_repo_url == "https://github.com/firehol/blocklist-ipsets",
          "Default FIREHOL_GIT_REPO_URL is incorrect");
    check(defaults.firehol_git_path == fs::current_path() / "firehol-blocklist-ipsets",
          "Default FIREHOL_GIT_PATH must be firehol-blocklist-ipsets in the working directory");
    check(defaults.mmdb_path == fs::current_path() / "firehol.mmdb",
          "Default MMDB_PATH must be firehol.mmdb in the working directory");
    Environment::set("MMDB_PATH", "");
    check(spoe::read_app_config().mmdb_path.empty(), "Empty MMDB_PATH should disable the MMDB");

    Environment::set("FIREHOL_GIT_REPO_URL", "https://example.invalid/custom-lists.git");
    Environment::set("FIREHOL_GIT_PATH", "local repository");
    Environment::set("MMDB_PATH", "output databases/custom.mmdb");
    Environment::set("SERVER_LISTEN_ADDRESS", "127.0.0.1:9123");
    Environment::set("METRICS_LISTEN_ADDRESS", "127.0.0.1:9456");
    Environment::set("DROP_BY_CATEGORY", "abuse,malware");
    const auto custom = spoe::read_app_config();
    check(custom.firehol_git_repo_url == "https://example.invalid/custom-lists.git",
          "Custom FIREHOL_GIT_REPO_URL must be used");
    check(custom.firehol_git_path == root / "local repository",
          "Relative FIREHOL_GIT_PATH must resolve against the execution directory");
    check(custom.mmdb_path == root / "output databases" / "custom.mmdb",
          "MMDB_PATH must retain the full configured filename");
    check(custom.listen_address == "127.0.0.1:9123" &&
              custom.metrics_listen_address == "127.0.0.1:9456" &&
              custom.drop_by_category == "abuse,malware",
          "Centralized configuration must retain existing environment settings");

    Environment::set("FIREHOL_GIT_PATH", (root / "absolute checkout").string());
    Environment::set("MMDB_PATH", (root / "absolute.mmdb").string());
    const auto absolute = spoe::read_app_config();
    check(absolute.firehol_git_path == root / "absolute checkout" &&
              absolute.mmdb_path == root / "absolute.mmdb",
          "Absolute configured paths must be retained");
    environment.clear();
}

void check_job(const fs::path& binary, const fs::path& root) {
    const fs::path log = root / "child.log";
    const fs::path remote = root / "remote repository";
    setup_repository(remote, log);
    Environment::set("FIREHOL_GIT_REPO_URL", remote.string());

    cpu_set_t available;
    CPU_ZERO(&available);
    check(::sched_getaffinity(0, sizeof(available), &available) == 0,
          "Cannot determine available test CPU");
    int first_cpu = 0;
    while (first_cpu < CPU_SETSIZE && !CPU_ISSET(first_cpu, &available)) ++first_cpu;
    check(first_cpu < CPU_SETSIZE, "No CPU available for test process");

    const std::vector<std::string> runtime_arguments{
        binary.string(), "--smp", "1", "--cpuset", std::to_string(first_cpu),
        "--memory", "512M", "--overprovisioned", "--network-stack", "posix",
        "--seastar-conf", "/dev/null", "--io-conf", "/dev/null",
        "--default-log-level", "error"};
    auto run_job = [&](const fs::path& cwd, bool expect_success) {
        auto arguments = runtime_arguments;
        arguments.emplace_back("--fetch-and-create-mmdb");
        const int status = run_process(std::move(arguments), cwd, log);
        check(status == (expect_success ? 0 : 1),
              "Unexpected fetch-and-create exit status " + std::to_string(status) +
                  "\n" + read_file(log));
    };

    std::cout << "Test 2: Default checkout is firehol-blocklist-ipsets in the execution directory\n";
    const fs::path default_cwd = root / "default execution directory";
    const fs::path default_checkout = default_cwd / "firehol-blocklist-ipsets";
    const fs::path default_output = root / "default output.mmdb";
    fs::create_directories(default_cwd);
    Environment::unset("FIREHOL_GIT_PATH");
    Environment::set("MMDB_PATH", default_output.string());
    run_job(default_cwd, true);
    check(fs::exists(default_checkout / ".git") &&
              fs::exists(default_checkout / "initial.netset") && !fs::exists(default_cwd / ".git"),
          "prepare_repository must clone into firehol-blocklist-ipsets under the working directory");
    check_network(default_output, "192.0.2.3", "initial");

    std::cout << "Test 3: Custom repository and exact MMDB output paths\n";
    const fs::path custom_cwd = root / "execution directory";
    const fs::path checkout = custom_cwd / "custom checkout";
    const fs::path output = custom_cwd / "databases" / "chosen file.mmdb";
    fs::create_directories(output.parent_path());
    Environment::set("FIREHOL_GIT_PATH", "custom checkout");
    Environment::set("MMDB_PATH", (fs::path("databases") / "chosen file.mmdb").string());
    run_job(custom_cwd, true);
    check(fs::exists(checkout / ".git") && !fs::exists(custom_cwd / ".git"),
          "The checkout must use FIREHOL_GIT_PATH");
    check_network(output, "192.0.2.3", "initial");
    check(!fs::exists(checkout / "firehol.mmdb"),
          "MMDB output must use the configured filename and directory");

    std::cout << "Test 4: Compilation reads the latest remote commit after preparation\n";
    std::this_thread::sleep_for(std::chrono::seconds(2));
    write_file(remote / "initial.netset", "198.51.100.0/24\n");
    write_file(remote / "latest.netset", "203.0.113.0/24\n");
    run_git(remote, log, {"add", "initial.netset", "latest.netset"});
    run_git(remote, log, {"commit", "-m", "Update blocklists"});
    run_job(custom_cwd, true);
    check_network(output, "198.51.100.7", "initial");
    check_network(output, "203.0.113.8", "latest");
    check_network(output, "192.0.2.3", std::nullopt);

    std::cout << "Test 5: Normal startup generates and opens the configured MMDB\n";
    fs::remove(output);
    Environment::set("SERVER_LISTEN_ADDRESS", "127.0.0.1:0");
    Environment::set("METRICS_LISTEN_ADDRESS", "127.0.0.1:0");
    auto startup_arguments = runtime_arguments;
    startup_arguments.back() = "info";
    const std::string_view ready_message = "SPOA server listening on";
    const int startup_status = run_process(std::move(startup_arguments), custom_cwd, log,
                                           ready_message);
    const std::string startup_log = read_file(log);
    check(startup_status == 0 && startup_log.find(ready_message) != std::string::npos &&
              startup_log.find("loaded in memory") != std::string::npos &&
              startup_log.find("FireHOL scheduler started: 0 * * * *") != std::string::npos,
          "Normal startup should generate and open its MMDB, then stop cleanly:\n" +
              startup_log);
    check_network(output, "203.0.113.8", "latest");

    std::cout << "Test 6: Failed preparation never compiles valid local blocklists\n";
    const std::string original_mmdb = read_file(output);
    const fs::path unavailable_remote = root / "missing remote";
    run_git(checkout, log, {"remote", "set-url", "origin", unavailable_remote.string()});
    Environment::set("FIREHOL_GIT_REPO_URL", unavailable_remote.string());
    // This input is valid and changes the MMDB if compile_to_mmdb is called.
    write_file(checkout / "initial.netset", "10.0.0.0/8\n");
    run_job(custom_cwd, false);
    check(read_file(output) == original_mmdb,
          "Failed preparation must leave the existing MMDB unchanged");
    fs::remove(output);
    run_job(custom_cwd, false);
    check(!fs::exists(output), "Failed preparation must not generate an MMDB");

    std::cout << "Test 7: A different repository origin preserves the checkout\n";
    Environment::set("FIREHOL_GIT_REPO_URL", remote.string());
    const std::string local_content = read_file(checkout / "initial.netset");
    run_job(custom_cwd, false);
    check(read_file(checkout / "initial.netset") == local_content && !fs::exists(output),
          "Origin mismatch must preserve local data and skip compilation");
}

} // namespace

int main(int argc, char** argv) {
    try {
        check(argc == 3 && std::string_view(argv[1]) == "--binary",
              "Usage: firehol_job_test --binary /path/to/haproxy-spoa-firehol-mmdb");
        const fs::path binary = fs::absolute(argv[2]);
        TestDirectory directory;
        Environment environment;
        check_configuration(directory.path(), environment);
        check_job(binary, directory.path());
        std::cout << "All FireHOL job and configuration tests passed!\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
