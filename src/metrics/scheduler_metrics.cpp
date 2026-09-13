#include "metrics/scheduler_metrics.hpp"

namespace spoe {
namespace metrics {

void register_scheduler_metrics(const job_scheduler::JobScheduler &scheduler,
                                seastar::metrics::metric_groups &groups) {
  namespace sm = seastar::metrics;
  const auto &stats = scheduler.stats();
  static const sm::label job_label("job");
  const std::vector<sm::label_instance> labels{job_label("firehol_mmdb")};
  groups.add_group(
      "job_scheduler",
      {
          sm::make_counter("runs_total",
                           sm::description("Scheduled jobs started"), labels,
                           [&stats] { return stats.runs_total.load(); })
              .aggregate({sm::shard_label}),
          sm::make_counter(
              "failures_total",
              sm::description("Scheduled jobs that raised an exception"),
              labels, [&stats] { return stats.failures_total.load(); })
              .aggregate({sm::shard_label}),
          sm::make_gauge(
              "running",
              sm::description("1 while a scheduled job is executing"), labels,
              [&stats] { return stats.running.load(); })
              .aggregate({sm::shard_label}),
          sm::make_gauge("last_run_seconds",
                         sm::description("Duration of the last finished job"),
                         labels,
                         [&stats] { return stats.last_run_seconds.load(); })
              .aggregate({sm::shard_label}),
          sm::make_gauge("last_run_timestamp_seconds",
                         sm::description(
                             "Unix time when the last job finished, 0 if none"),
                         labels,
                         [&stats] { return stats.last_run_unix.load(); })
              .aggregate({sm::shard_label}),
          sm::make_gauge("next_run_timestamp_seconds",
                         sm::description("Unix time of the next scheduled job"),
                         labels,
                         [&stats] { return stats.next_run_unix.load(); })
              .aggregate({sm::shard_label}),
      });
}

} // namespace metrics
} // namespace spoe
