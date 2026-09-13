#include "app_config.h"
#include "utils/environment.h"

namespace spoe {

AppConfig read_app_config() {
  namespace fs = std::filesystem;
  using utils::environment_or;
  AppConfig config;
  config.listen_address =
      environment_or("SERVER_LISTEN_ADDRESS", "0.0.0.0:9000");
  config.metrics_listen_address =
      environment_or("METRICS_LISTEN_ADDRESS", "0.0.0.0:9100");
  config.drop_by_category = environment_or("DROP_BY_CATEGORY", "");
  // Unset: firehol.mmdb in the working directory. Empty: generation and
  // lookups disabled.
  const auto mmdb_path = environment_or("MMDB_PATH", "firehol.mmdb");
  if (!mmdb_path.empty()) {
    config.mmdb_path = fs::absolute(fs::path(mmdb_path));
  }
  config.firehol_git_path = fs::absolute(
      fs::path(environment_or("FIREHOL_GIT_PATH", "firehol-blocklist-ipsets")));
  config.firehol_git_repo_url = environment_or(
      "FIREHOL_GIT_REPO_URL", "https://github.com/firehol/blocklist-ipsets");
  return config;
}

} // namespace spoe
