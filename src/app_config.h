#pragma once

#include <filesystem>
#include <string>

namespace spoe {

struct AppConfig {
  std::string listen_address;
  std::string metrics_listen_address;
  std::string drop_by_category;
  std::filesystem::path mmdb_path;
  std::filesystem::path firehol_git_path;
  std::string firehol_git_repo_url;
};

AppConfig read_app_config();

} // namespace spoe
