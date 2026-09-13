#include "git_repository.hpp"
#include "metrics/firehol_metrics.hpp"
#include "utils/environment.h"
#include <filesystem>
#include <git2.h>

#include <chrono>
#include <seastar/util/log.hh>
#include <string>
#include <string_view>

namespace firehol_ipsets {

static seastar::logger gitlog("git");

namespace {

using spoe::utils::environment_non_empty;

// libgit2 keeps the last error per thread; prefer its message when present.
std::string git_error_message(int error_code, const std::string &msg) {
  const git_error *e = git_error_last();
  if (e && e->message)
    return msg + ": " + e->message;
  return msg + " (" + std::to_string(error_code) + ")";
}

// OpenSSL is linked statically with a build-time prefix, so its default
// certificate directory does not exist at runtime. Point libgit2 at the
// system CA store (or SSL_CERT_FILE / SSL_CERT_DIR) so HTTPS clones verify.
void configure_ssl_certificates() {
  namespace fs = std::filesystem;
  const char *cert_file = environment_non_empty("SSL_CERT_FILE");
  const char *cert_dir = environment_non_empty("SSL_CERT_DIR");
  if (!cert_file && !cert_dir) {
    for (const char *candidate :
         {"/etc/ssl/certs/ca-certificates.crt",
          "/etc/pki/tls/certs/ca-bundle.crt", "/etc/ssl/ca-bundle.pem",
          "/etc/pki/tls/cacert.pem", "/etc/ssl/cert.pem"}) {
      if (fs::is_regular_file(candidate)) {
        cert_file = candidate;
        break;
      }
    }
    for (const char *candidate : {"/etc/ssl/certs", "/etc/pki/tls/certs"}) {
      if (fs::is_directory(candidate)) {
        cert_dir = candidate;
        break;
      }
    }
  }
  if (!cert_file && !cert_dir) {
    gitlog.warn("No CA certificate store found; HTTPS repositories will fail "
                "certificate validation (set SSL_CERT_FILE or SSL_CERT_DIR)");
    return;
  }
  const int err =
      git_libgit2_opts(GIT_OPT_SET_SSL_CERT_LOCATIONS, cert_file, cert_dir);
  if (err < 0)
    throw GitError(err, "Failed to configure SSL certificate locations");
  gitlog.info("Using CA certificates: file={} dir={}",
              cert_file ? cert_file : "(none)", cert_dir ? cert_dir : "(none)");
}

} // namespace

GitError::GitError(int error_code, const std::string &msg)
    : std::runtime_error(git_error_message(error_code, msg)) {}

GitGlobalInit::GitGlobalInit() {
  gitlog.debug("Initializing libgit2");
  const int err = git_libgit2_init();
  if (err < 0)
    throw GitError(err, "Failed to initialize libgit2");
  try {
    configure_ssl_certificates();
  } catch (...) {
    git_libgit2_shutdown();
    throw;
  }
}

GitGlobalInit::~GitGlobalInit() {
  gitlog.debug("Shutting down libgit2");
  git_libgit2_shutdown();
}

struct GitRepository::Impl {
  mutable int64_t last_commit_time = 0;

  std::unique_ptr<git_repository, decltype(&git_repository_free)> repo{
      nullptr, git_repository_free};

  // Returns true when a fresh clone was made, false when an existing
  // checkout was opened.
  bool open_or_clone(const std::string &url,
                     const std::filesystem::path &local_path) {
    git_repository *opened_repo = nullptr;
    const auto local_path_string = local_path.string();
    int err;
    const bool cloned = !(std::filesystem::exists(local_path) &&
                          std::filesystem::exists(local_path / ".git"));
    if (!cloned) {
      gitlog.info("Opening existing repository at {}", local_path_string);
      err = git_repository_open(&opened_repo, local_path_string.c_str());
    } else {
      gitlog.info("Cloning repository from {} to {}", url, local_path_string);
      git_clone_options clone_opts = GIT_CLONE_OPTIONS_INIT;
      git_fetch_options fetch_opts = GIT_FETCH_OPTIONS_INIT;
      if (url.find("http") == 0)
        fetch_opts.depth = 1;
      fetch_opts.download_tags = GIT_REMOTE_DOWNLOAD_TAGS_NONE;
      clone_opts.fetch_opts = fetch_opts;

      err = git_clone(&opened_repo, url.c_str(), local_path_string.c_str(),
                      &clone_opts);
    }
    std::unique_ptr<git_repository, decltype(&git_repository_free)> candidate{
        opened_repo, git_repository_free};
    if (err < 0)
      throw GitError(err, "Failed to open or clone repository");
    repo = std::move(candidate);
    return cloned;
  }

  void fetch_and_reset(const std::string &repo_url,
                       const std::string &default_branch) {
    git_remote *remote_ptr = nullptr;
    int err = git_remote_lookup(&remote_ptr, repo.get(), "origin");
    const std::unique_ptr<git_remote, decltype(&git_remote_free)> remote{
        remote_ptr, git_remote_free};
    if (err < 0)
      throw GitError(err, "Failed to lookup remote 'origin'");

    const char *url = git_remote_url(remote.get());
    if (!url || std::string_view(url) != repo_url) {
      throw std::runtime_error(
          "Repository origin URL does not match FIREHOL_GIT_REPO_URL; "
          "use FIREHOL_GIT_PATH to select the intended FireHOL repository");
    }

    // Get local HEAD oid and time
    git_oid local_oid = {0};
    int64_t local_time = 0;
    bool has_local = false;
    git_reference *head_ref = nullptr;
    if (git_repository_head(&head_ref, repo.get()) == 0) {
      const git_oid *oid = git_reference_target(head_ref);
      if (oid) {
        git_oid_cpy(&local_oid, oid);
        has_local = true;
      }
      git_object *head_commit = nullptr;
      if (git_reference_peel(&head_commit, head_ref, GIT_OBJECT_COMMIT) == 0) {
        local_time =
            git_commit_time(reinterpret_cast<git_commit *>(head_commit));
        git_object_free(head_commit);
      }
      git_reference_free(head_ref);
    }

    // Check if remote hash matches local hash to avoid fetch if nothing changed
    bool needs_fetch = true;
    git_remote_callbacks callbacks = GIT_REMOTE_CALLBACKS_INIT;
    if (has_local && git_remote_connect(remote.get(), GIT_DIRECTION_FETCH,
                                        &callbacks, nullptr, nullptr) == 0) {
      const git_remote_head **refs;
      size_t refs_len;
      if (git_remote_ls(&refs, &refs_len, remote.get()) == 0) {
        std::string target_ref = "refs/heads/" + default_branch;
        for (size_t i = 0; i < refs_len; ++i) {
          if (target_ref == refs[i]->name) {
            if (git_oid_cmp(&local_oid, &refs[i]->oid) == 0) {
              needs_fetch = false;
            }
            break;
          }
        }
      }
      git_remote_disconnect(remote.get());
    }

    if (!needs_fetch) {
      last_commit_time = local_time;
      return;
    }

    gitlog.info("Fetching remote 'origin'");
    git_fetch_options fetch_opts = GIT_FETCH_OPTIONS_INIT;
    if (repo_url.find("http") == 0)
      fetch_opts.depth = 1;
    fetch_opts.download_tags = GIT_REMOTE_DOWNLOAD_TAGS_NONE;

    err = git_remote_fetch(remote.get(), nullptr, &fetch_opts, nullptr);
    if (err < 0)
      throw GitError(err, "Failed to fetch from remote");

    std::string branch_ref_name = "refs/remotes/origin/" + default_branch;
    git_reference *remote_ref_ptr = nullptr;
    err = git_reference_lookup(&remote_ref_ptr, repo.get(),
                               branch_ref_name.c_str());
    const std::unique_ptr<git_reference, decltype(&git_reference_free)>
        remote_ref{remote_ref_ptr, git_reference_free};
    if (err < 0)
      throw GitError(err, "Failed to lookup remote branch " + branch_ref_name);

    git_object *target_commit_ptr = nullptr;
    err = git_reference_peel(&target_commit_ptr, remote_ref.get(),
                             GIT_OBJECT_COMMIT);
    const std::unique_ptr<git_object, decltype(&git_object_free)> target_commit{
        target_commit_ptr, git_object_free};
    if (err < 0)
      throw GitError(err, "Failed to peel reference to commit");

    int64_t remote_time =
        git_commit_time(reinterpret_cast<git_commit *>(target_commit.get()));

    // Only reset if remote commit date is strictly greater
    if (has_local && remote_time <= local_time) {
      last_commit_time = local_time;
      return;
    }

    gitlog.info("Hard resetting to {}", branch_ref_name);
    git_checkout_options checkout_opts = GIT_CHECKOUT_OPTIONS_INIT;
    checkout_opts.checkout_strategy = GIT_CHECKOUT_FORCE;

    err = git_reset(repo.get(), target_commit.get(), GIT_RESET_HARD,
                    &checkout_opts);

    if (err < 0)
      throw GitError(err, "Failed to hard reset repository");

    last_commit_time = remote_time;
    gitlog.info("Repository updated and reset successfully");
  }
};

GitRepository::GitRepository() : impl_(std::make_unique<Impl>()) {}

GitRepository::~GitRepository() = default;

std::filesystem::path
GitRepository::prepare_repository(const std::string &repo_url,
                                  const std::filesystem::path &local_path,
                                  const std::string &default_branch) {
  const auto started = std::chrono::steady_clock::now();
  auto &m = metrics();
  try {
    const bool cloned = impl_->open_or_clone(repo_url, local_path);
    impl_->fetch_and_reset(repo_url, default_branch);
    (cloned ? m.git_clones_total : m.git_fetches_total)
        .fetch_add(1, std::memory_order_relaxed);
  } catch (...) {
    m.git_failures_total.fetch_add(1, std::memory_order_relaxed);
    throw;
  }
  m.git_sync_seconds.store(
      std::chrono::duration<double>(std::chrono::steady_clock::now() - started)
          .count(),
      std::memory_order_relaxed);
  m.git_last_success_unix.store(unix_now(), std::memory_order_relaxed);
  return local_path;
}

int64_t GitRepository::last_commit_unix() const {
  return impl_->last_commit_time;
}

} // namespace firehol_ipsets
