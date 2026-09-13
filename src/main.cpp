#include "app_config.h"
#include "firehol_blocklist_ipsets/compiler.hpp"
#include "firehol_blocklist_ipsets/git_repository.hpp"
#include "job_scheduler/job_scheduler.hpp"
#include "metrics/firehol_metrics.hpp"
#include "metrics/scheduler_metrics.hpp"
#include "spoa/spoa_server.h"
#include "utils/blocking_task.h"
#include <seastar/core/thread.hh>

#include <seastar/core/app-template.hh>
#include <seastar/core/do_with.hh>
#include <seastar/core/dpdk_rte.hh>
#include <seastar/core/future.hh>
#include <seastar/core/metrics.hh>
#include <seastar/core/prometheus.hh>
#include <seastar/core/sharded.hh>
#include <seastar/core/signal.hh>
#include <seastar/core/smp.hh>
#include <seastar/core/sstring.hh>
#include <seastar/http/httpd.hh>
#include <seastar/net/api.hh>
#include <seastar/util/log.hh>

#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <net/if.h>
#include <stdexcept>
#include <string>
#include <utility>

using namespace seastar;

namespace {

logger spoa_logger("spoa");

//! What the process does once the MMDB has been prepared.
enum class RunMode { Serve, FetchOnly };

struct FireholJobState {
  spoe::AppConfig settings;
  // Destruction joins the worker before its configuration is released.
  std::unique_ptr<job_scheduler::JobScheduler> scheduler;
  seastar::metrics::metric_groups scheduler_metrics;
  std::shared_ptr<spoe::ReloadableMmdb> shared_mmdb;
};

void job_fetch_and_create_mmdb(const spoe::AppConfig &config) {
  if (config.mmdb_path.empty()) {
    throw std::invalid_argument("MMDB_PATH must be set to generate the MMDB");
  }
  firehol_ipsets::GitGlobalInit git_init;
  firehol_ipsets::GitRepository repository;
  const auto local_path = repository.prepare_repository(
      config.firehol_git_repo_url, config.firehol_git_path);

  const uint64_t last_commit_unix = repository.last_commit_unix();

  if (std::filesystem::exists(config.mmdb_path)) {
    try {
      spoe::Mmdb existing_db(config.mmdb_path.string());
      const uint64_t built_at = existing_db.build_epoch();

      if (built_at >= last_commit_unix) {
        std::time_t built_at_t = built_at;
        std::time_t last_commit_t = last_commit_unix;
        char built_at_buf[32];
        char last_commit_buf[32];
        std::strftime(built_at_buf, sizeof(built_at_buf), "%Y-%m-%d %H:%M:%S",
                      std::localtime(&built_at_t));
        std::strftime(last_commit_buf, sizeof(last_commit_buf),
                      "%Y-%m-%d %H:%M:%S", std::localtime(&last_commit_t));
        spoa_logger.info("MMDB is up to date (built_at: {}, last_commit: {}). "
                         "Skipping compilation, it will be loaded in memory.",
                         built_at_buf, last_commit_buf);
        return;
      }
    } catch (const std::exception &e) {
      spoa_logger.warn(
          "Could not read existing MMDB built_at timestamp: {}. Recompiling...",
          e.what());
    }
  }

  firehol_ipsets::compile_to_mmdb(config.mmdb_path, local_path,
                                  last_commit_unix);
}

// Fetches the FireHOL repository and (re)generates the MMDB. On failure it
// keeps running against an existing file unless an explicit exit is required.
// Returns false when the application must stop.
bool ensure_mmdb_available(const spoe::AppConfig &config, RunMode mode) {
  if (config.mmdb_path.empty() && mode == RunMode::Serve) {
    spoa_logger.warn(
        "MMDB_PATH is empty; MMDB generation and lookups disabled");
    return true;
  }
  try {
    job_fetch_and_create_mmdb(config);
    return true;
  } catch (const std::exception &error) {
    spoa_logger.error("FireHOL MMDB generation failed: {}", error.what());
    if (mode == RunMode::FetchOnly ||
        !std::filesystem::is_regular_file(config.mmdb_path)) {
      return false;
    }
    spoa_logger.warn("Using the existing MMDB at {}",
                     config.mmdb_path.string());
    return true;
  }
}

// Runs the SPOA agent: starts the FireHOL refresh scheduler, the SPOA servers
// and the Prometheus endpoint, then waits for a shutdown signal.
future<int> run_spoa_agent(spoe::AppConfig settings) {
  return do_with(
             FireholJobState{std::move(settings), nullptr, {}, nullptr},
             promise<>(), false, std::make_unique<sharded<spoe::SpoaServer>>(),
             httpd::http_server_control(),
             [](FireholJobState &job, promise<> &shutdown, bool &signaled,
                std::unique_ptr<sharded<spoe::SpoaServer>> &server_storage,
                httpd::http_server_control &http) {
               const auto &settings = job.settings;
               auto &scheduler = job.scheduler;
               auto &servers = *server_storage;

               // Refresh the FireHOL MMDB hourly.
               if (!settings.mmdb_path.empty()) {
                 scheduler = std::make_unique<job_scheduler::JobScheduler>(
                     "0 * * * *",
                     [&settings] { job_fetch_and_create_mmdb(settings); });
                 spoe::metrics::register_scheduler_metrics(
                     *scheduler, job.scheduler_metrics);
                 spoa_logger.info("FireHOL scheduler started: 0 * * * *");
               }
               // Publishes the counters of the startup compilation too.
               firehol_ipsets::metrics().register_metrics();

               // Install signal handlers to trigger a clean shutdown.
               for (int signal : {SIGTERM, SIGQUIT, SIGINT})
                 handle_signal(signal, [&shutdown, &signaled] {
                   if (!signaled) {
                     signaled = true;
                     shutdown.set_value();
                   }
                 });

               prometheus::config config;
               config.metric_help =
                   "haproxy-spoa-firehol-mmdb SPOA agent metrics";
               config.prefix = "spoa";

               job.shared_mmdb = std::make_shared<spoe::ReloadableMmdb>();

               auto open_mmdb_future = seastar::make_ready_future<>();
               if (!settings.mmdb_path.empty()) {
                 open_mmdb_future = spoe::run_blocking([&settings, &job] {
                   job.shared_mmdb->open(settings.mmdb_path, true);
                 });
               }

               return open_mmdb_future
                   .then([&servers] { return servers.start(); })
                   .then([&servers, &settings, &job] {
                     return servers.invoke_on_all(
                         [&settings, &job](spoe::SpoaServer &server) {
                           return server.start(settings, job.shared_mmdb);
                         });
                   })
                   .then([&http, config, &settings]() mutable {
                     spoa_logger.info("MMDB: inotify hot reload enabled for {}",
                                      settings.mmdb_path.string());
                     return http.start()
                         .then([&http, config]() mutable {
                           return prometheus::add_prometheus_routes(
                               http.server(), config);
                         })
                         .then([&http, &settings] {
                           return http
                               .listen(
                                   ipv4_addr{settings.metrics_listen_address})
                               .then([&settings] {
                                 spoa_logger.info(
                                     "Metrics server listening at "
                                     "http://{}/metrics",
                                     settings.metrics_listen_address);
                               });
                         });
                   })
                   .then([&shutdown, &settings] {
                     spoa_logger.info(
                         "SPOA server listening on {} across {} CPU shards",
                         settings.listen_address, smp::count);
                     return shutdown.get_future();
                   })
                   .finally([&http] { return http.stop(); })
                   .finally([&servers] { return servers.stop(); })
                   .finally([&job] {
                     job.scheduler_metrics.clear();
                     job.scheduler.reset();
                     return spoe::run_blocking([&job] {
                       if (job.shared_mmdb)
                         job.shared_mmdb->close();
                     });
                   });
             })
      .then([] { return EXIT_SUCCESS; });
}

// Loads Seastar configuration files and applies DPDK-related overrides.
void setup_seastar_configuration_reader(seastar::app_template &app) {
  namespace bpo = boost::program_options;
  app.set_configuration_reader([&app](bpo::variables_map &config) {
    // Command-line values take precedence over Seastar's configuration files.
    const char *config_home = std::getenv("HOME");
    for (const auto &[argument, name] :
         {std::pair{"seastar-conf", "seastar.conf"},
          std::pair{"io-conf", "io.conf"}}) {
      const auto selected = config.find(argument);
      const bool explicit_path = selected != config.end();
      if (!explicit_path && !config_home) {
        continue;
      }
      const std::string path =
          explicit_path ? selected->second.as<std::string>()
                        : std::string(config_home) + "/.config/seastar/" + name;
      std::ifstream input(path);
      if (!input) {
        if (explicit_path) {
          throw bpo::error("Cannot open configuration file '" + path + "' (--" +
                           argument + ")");
        }
        continue;
      }
      try {
        bpo::store(bpo::parse_config_file(
                       input, app.get_conf_file_options_description()),
                   config);
      } catch (const bpo::error &error) {
        throw bpo::error("Invalid configuration file '" + path +
                         "': " + error.what());
      }
      if (input.bad()) {
        throw bpo::error("Cannot read configuration file '" + path + "'");
      }
    }
    if (config.count("dpdk-interface")) {
      if (config.count("hugepages")) {
        throw bpo::error(
            "--dpdk-interface uses ordinary memory; remove --hugepages");
      }
      // AF_PACKET explicitly opts into DPDK; ordinary startup keeps the
      // configured network stack (Seastar defaults to POSIX).
      config.insert_or_assign(
          "network-stack", bpo::variable_value(std::string("native"), false));
      config.insert_or_assign("dpdk-pmd", bpo::variable_value{});
    }
  });
}

// Registers the application-specific command-line options.
void register_command_line_options(seastar::app_template &app,
                                   bool &fetch_only) {
  namespace bpo = boost::program_options;
  app.add_options()(
      "seastar-conf", bpo::value<std::string>()->value_name("PATH"),
      "Seastar configuration file (default: ~/.config/seastar/seastar.conf)")(
      "io-conf", bpo::value<std::string>()->value_name("PATH"),
      "I/O configuration file (default: ~/.config/seastar/io.conf)");

  app.add_options()(
      "dpdk-interface",
      bpo::value<std::string>()->notifier([](const std::string &interface) {
        if (interface.empty() || interface.size() >= IFNAMSIZ ||
            interface.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKL"
                                        "MNOPQRSTUVWXYZ0123456789_.-") !=
                std::string::npos) {
          throw bpo::error(
              "--dpdk-interface must be a valid Linux interface name");
        }
        dpdk::eal::packet_interface = interface;
      }),
      "Use a Linux interface through DPDK AF_PACKET without hugepages");

  app.add_options()("version,V", bpo::bool_switch()->notifier([](bool version) {
    if (version) {
      std::cout << SPOA_SERVER_VERSION << '\n';
      std::exit(EXIT_SUCCESS);
    }
  }),
                    "Print version");
  app.add_options()(
      "fetch-and-create-mmdb", bpo::bool_switch(&fetch_only),
      "Update the FireHOL repository, generate MMDB_PATH and exit");
}

} // namespace

int main(int argc, char **argv) {
  bool fetch_only = false;

  app_template::seastar_options options;
  options.log_opts.log_with_color.set_default_value(true);
  seastar::app_template app(std::move(options));

  setup_seastar_configuration_reader(app);
  register_command_line_options(app, fetch_only);

  return app.run(argc, argv, [&fetch_only]() -> future<int> {
    const RunMode mode = fetch_only ? RunMode::FetchOnly : RunMode::Serve;
    auto settings = spoe::read_app_config();
    if (settings.drop_by_category.empty())
      spoa_logger.warn(
          "DROP_BY_CATEGORY is not set; no IP will be flagged bad");

    // Ensure the MMDB can be generated before serving traffic. Git and MMDB
    // generation block for seconds, so they run off the reactor thread.
    return do_with(std::move(settings), false,
                   [mode](spoe::AppConfig &settings, bool &available) {
                     return spoe::run_blocking([&settings, &available, mode] {
                              available = ensure_mmdb_available(settings, mode);
                            })
                         .then([&settings, &available, mode]() -> future<int> {
                           if (!available) {
                             return make_ready_future<int>(EXIT_FAILURE);
                           }
                           if (mode == RunMode::FetchOnly) {
                             return make_ready_future<int>(EXIT_SUCCESS);
                           }
                           return run_spoa_agent(std::move(settings));
                         });
                   });
  });
}
