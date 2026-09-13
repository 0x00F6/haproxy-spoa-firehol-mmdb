#pragma once

#include "job_scheduler/job_scheduler.hpp"
#include <seastar/core/metrics.hh>

namespace spoe {
namespace metrics {

// Exposes the scheduler counters; the scheduler must outlive the group.
void register_scheduler_metrics(const job_scheduler::JobScheduler &scheduler,
                                seastar::metrics::metric_groups &groups);

} // namespace metrics
} // namespace spoe
