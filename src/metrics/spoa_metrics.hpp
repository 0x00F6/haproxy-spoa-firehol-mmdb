#pragma once

#include "mmdb/mmdb_reload.h"
#include "spoa/drop_categories.h"
#include <cstdint>
#include <seastar/core/metrics.hh>
#include <string_view>

namespace spoe {
namespace metrics {

class SpoaMetrics {
public:
  void register_metrics(std::shared_ptr<ReloadableMmdb> mmdb,
                        const DropCategories &categories);

  void inc_lookups() noexcept { lookups_total_++; }
  void inc_lookup_errors() noexcept { lookup_errors_total_++; }

private:
  uint64_t lookups_total_ = 0;
  uint64_t lookup_errors_total_ = 0;
  seastar::metrics::metric_groups metrics_;
};

} // namespace metrics
} // namespace spoe
