#include "metrics/spoa_metrics.hpp"

namespace spoe {
namespace metrics {

void SpoaMetrics::register_metrics(std::shared_ptr<ReloadableMmdb> mmdb,
                                   const DropCategories &drop_categories) {
  namespace sm = seastar::metrics;
  metrics_.add_group(
      "mmdb",
      {
          sm::make_counter(
              "reloads_total",
              sm::description(
                  "Successful MMDB snapshot reloads, excluding initial load"),
              [mmdb] { return mmdb ? mmdb->reload_count() : 0; })
              .aggregate({sm::shard_label}),
          sm::make_counter("lookups_total",
                           sm::description("MaxMind lookups performed"),
                           [this] { return lookups_total_; })
              .aggregate({sm::shard_label}),
          sm::make_counter("lookup_errors_total",
                           sm::description("MaxMind lookup errors"),
                           [this] { return lookup_errors_total_; })
              .aggregate({sm::shard_label}),
      });

  static const sm::label category_label("categorie"); // Keeping typo for compat
  for (const auto key : drop_categories.names()) {
    metrics_.add_group(
        "firehol",
        {
            sm::make_counter("blocked_total",
                             sm::description("IPs flagged bad and blocked"),
                             {category_label(key)},
                             [&drop_categories, key] {
                               return drop_categories.blocked(key);
                             })
                .aggregate({sm::shard_label}),
        });
  }
}

} // namespace metrics
} // namespace spoe
