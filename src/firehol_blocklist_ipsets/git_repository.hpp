#pragma once
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>

namespace firehol_ipsets {

class GitError : public std::runtime_error {
public:
  explicit GitError(int error_code, const std::string &msg);
};

struct GitGlobalInit {
  GitGlobalInit();
  ~GitGlobalInit();
  GitGlobalInit(const GitGlobalInit &) = delete;
  GitGlobalInit &operator=(const GitGlobalInit &) = delete;
};

class GitRepository {
public:
  GitRepository();
  ~GitRepository();

  // Prepares the repository at the given local path
  // If it does not exist, it clones it.
  // If it exists, its origin must match repo_url before fetching and
  // hard resetting to the latest commit of default_branch.
  // An origin mismatch is rejected without fetching or modifying the worktree.
  // Returns the path to the working tree.
  std::filesystem::path
  prepare_repository(const std::string &repo_url,
                     const std::filesystem::path &local_path,
                     const std::string &default_branch = "master");

  // Returns the timestamp of the last commit from the default branch in the
  // working tree
  int64_t last_commit_unix() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace firehol_ipsets
