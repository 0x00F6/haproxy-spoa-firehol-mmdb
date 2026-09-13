#include "job_scheduler.hpp"
#include "croncpp.h"

#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

namespace job_scheduler {

namespace {

// Counts whitespace-separated fields without streams or allocations.
size_t count_fields(std::string_view expr) {
  size_t count = 0;
  bool in_field = false;
  for (const char c : expr) {
    const bool space = c == ' ' || c == '\t' || c == '\n' || c == '\r';
    if (!space && !in_field)
      count++;
    in_field = !space;
  }
  return count;
}

std::string normalize_cron_expr(std::string_view expr) {
  // Si l'utilisateur fournit une expression crontab standard UNIX (5 champs),
  // on insère implicitement un '0' pour les secondes (exécuté sur la seconde
  // 0). croncpp attend par défaut 6 champs (avec les secondes).
  if (count_fields(expr) == 5) {
    return "0 " + std::string(expr);
  }
  return std::string(expr);
}

} // namespace

class JobScheduler::Impl {
public:
  Impl(std::string_view cron_expr, Job job) : job_(std::move(job)) {

    std::string normalized_expr = normalize_cron_expr(cron_expr);
    try {
      cron_ = cron::make_cron(normalized_expr);
    } catch (const cron::bad_cronexpr &e) {
      throw std::invalid_argument("Invalid cron expression: " +
                                  std::string(e.what()));
    }

    thread_ = std::jthread(
        [this](std::stop_token stoken) { run(std::move(stoken)); });
  }

  ~Impl() {
    thread_.request_stop();
    cv_.notify_all();
    // Attend la fin du job et du thread avant la destruction de cv_.
    thread_.join();
  }

private:
  void run(std::stop_token stoken) {
    std::mutex mtx;
    std::unique_lock<std::mutex> lock(mtx);

    while (!stoken.stop_requested()) {
      auto now = std::chrono::system_clock::now();
      auto next_time_t =
          cron::cron_next(cron_, std::chrono::system_clock::to_time_t(now));

      // Pas de prochaine date de prévue selon l'expression
      if (next_time_t == cron::INVALID_TIME) {
        break;
      }

      auto next_time = std::chrono::system_clock::from_time_t(next_time_t);
      stats_.next_run_unix.store(static_cast<int64_t>(next_time_t),
                                 std::memory_order_relaxed);

      // On s'assure qu'on ne boucle pas sur un temps potentiellement dejà passé
      // très légèrement
      if (next_time <= now) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        continue;
      }

      // On attend jusqu'à la prochaine date ou une demande d'arrêt.
      // L'utilisation du stop_token nous permet d'être réveillé imméditament
      // si le scheduler est détruit même s'il attend pour un job lointain
      // (semaines, mois, etc.).
      cv_.wait_until(lock, stoken, next_time,
                     [&stoken] { return stoken.stop_requested(); });

      if (stoken.stop_requested()) {
        break;
      }

      // Exécution du job.
      // On déverrouille pour le job mais puisqu'il est exécuté dans ce thread
      // unique, il est strictement sérialisé. AUCUNE exécution concurrente du
      // MÊME job possible. La prochaine itération planifiera l'occurance
      // suivante uniquement APRÈS la fin de l'actuelle.
      lock.unlock();
      const auto job_started = std::chrono::steady_clock::now();
      stats_.runs_total.fetch_add(1, std::memory_order_relaxed);
      stats_.running.store(1, std::memory_order_relaxed);
      try {
        job_();
      } catch (const std::exception &e) {
        stats_.failures_total.fetch_add(1, std::memory_order_relaxed);
        std::cerr << "[JobScheduler] Exception levée par le job: " << e.what()
                  << std::endl;
      } catch (...) {
        stats_.failures_total.fetch_add(1, std::memory_order_relaxed);
        std::cerr << "[JobScheduler] Exception inconnue levée par le job."
                  << std::endl;
      }
      stats_.running.store(0, std::memory_order_relaxed);
      stats_.last_run_seconds.store(
          std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                        job_started)
              .count(),
          std::memory_order_relaxed);
      stats_.last_run_unix.store(
          std::chrono::duration_cast<std::chrono::seconds>(
              std::chrono::system_clock::now().time_since_epoch())
              .count(),
          std::memory_order_relaxed);
      lock.lock();
    }
  }

public:
  Stats stats_;

private:
  Job job_;
  cron::cronexpr cron_;
  std::jthread thread_;
  std::condition_variable_any cv_;
};

const JobScheduler::Stats &JobScheduler::stats() const noexcept {
  return impl_->stats_;
}

JobScheduler::JobScheduler(std::string_view cron_expr, Job job)
    : impl_(std::make_unique<Impl>(cron_expr, std::move(job))) {}

JobScheduler::~JobScheduler() = default;

} // namespace job_scheduler
