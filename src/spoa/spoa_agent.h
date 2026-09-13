#pragma once

#include "metrics/spoa_metrics.hpp"
#include "mmdb/mmdb_reload.h"
#include "spoa/drop_categories.h"
#include "spoa/frame.h"

#include <seastar/core/future.hh>
#include <seastar/core/iostream.hh>
#include <seastar/core/loop.hh>

#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

namespace spoe {

//! Stateful SPOE request processor confined to its owning Seastar shard.
//!
//! Frame parsing exposes views into Seastar's input buffer. This class only
//! consumes those views while handle_frame() is active; it never stores them.
//! Database categories are views into the MMDB mmap and configured categories
//! own their strings. The request path copies no category bytes and uses
//! zero-allocation buffer encoding for the response frame.
class SpoaAgent {
public:
  void set_mmdb(std::shared_ptr<ReloadableMmdb> mmdb) {
    mmdb_ = std::move(mmdb);
  }
  void load_drop_categories(std::string_view raw);
  [[nodiscard]] size_t drop_category_count() const noexcept {
    return drop_categories_.size();
  }
  [[nodiscard]] std::string drop_category_names() const {
    return drop_categories_.names_string();
  }
  [[nodiscard]] std::string metadata_string() const {
    if (!mmdb_)
      return "(database not open)";
    const auto db = mmdb_->snapshot();
    return db ? db->metadata_string() : "(database not open)";
  }

  //! Register Prometheus callbacks after loading the category configuration.
  void register_metrics() {
    metrics_.register_metrics(mmdb_, drop_categories_);
  }

  seastar::future<seastar::stop_iteration>
  handle_frame(frame::FrameType type, std::span<const uint8_t> payload,
               seastar::output_stream<char> &out);

private:
  static constexpr uint32_t kStatusNotImplemented = 14;

  seastar::future<seastar::stop_iteration>
  handle_notify(const frame::Notify &notify, seastar::output_stream<char> &out);
  static seastar::future<seastar::stop_iteration>
  send_frame(seastar::output_stream<char> &out, std::vector<uint8_t> frame,
             seastar::stop_iteration stop);
  static seastar::future<seastar::stop_iteration>
  send_buffer(seastar::output_stream<char> &out, const uint8_t *data,
              size_t size, seastar::stop_iteration stop);

  void evaluate_lookup(MMDB_lookup_result_s &result, int mmdb_error,
                       std::string_view kind, bool &ip_bad);
  bool evaluate_argument(const Arg &arg, const Mmdb *db, bool &ip_bad);

  std::shared_ptr<ReloadableMmdb> mmdb_;
  DropCategories drop_categories_;
  // Requests and metric callbacks run on the owning Seastar shard.
  metrics::SpoaMetrics metrics_;
};

} // namespace spoe
