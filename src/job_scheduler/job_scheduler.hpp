#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>

namespace job_scheduler {

/**
 * @brief JobScheduler
 * Planifie et exécute un job (callback) selon une expression crontab.
 * Exécute le job dans un thread dédié.
 * Garantit qu'un même job n'est jamais exécuté en parallèle par le même
 * scheduler.
 */
class JobScheduler {
public:
  using Job = std::function<void()>;

  /**
   * @brief Compteurs d'exécution, lisibles depuis n'importe quel thread
   * (par exemple un exporteur Prometheus) pendant la vie du scheduler.
   */
  struct Stats {
    std::atomic<uint64_t> runs_total{0}; ///< Jobs démarrés.
    std::atomic<uint64_t> failures_total{
        0};                           ///< Jobs terminés par une exception.
    std::atomic<uint64_t> running{0}; ///< 1 pendant l'exécution d'un job.
    std::atomic<double> last_run_seconds{0}; ///< Durée du dernier job terminé.
    std::atomic<int64_t> last_run_unix{
        0}; ///< Heure Unix de fin du dernier job, 0 si aucun.
    std::atomic<int64_t> next_run_unix{
        0}; ///< Heure Unix de la prochaine exécution planifiée.
  };

  /**
   * @brief Construit un JobScheduler et le démarre immédiatement.
   * @param cron_expr L'expression crontab (5 ou 6 champs supportés).
   * @param job Le callback à exécuter.
   * @throws std::invalid_argument Si l'expression crontab est invalide.
   */
  JobScheduler(std::string_view cron_expr, Job job);

  /**
   * @brief Arrête proprement le scheduler (join automatique sans détachement).
   */
  ~JobScheduler();

  // Interdiction de la copie et du déplacement pour éviter les comportements UB
  // sur les threads.
  JobScheduler(const JobScheduler &) = delete;
  JobScheduler &operator=(const JobScheduler &) = delete;
  JobScheduler(JobScheduler &&) = delete;
  JobScheduler &operator=(JobScheduler &&) = delete;

  const Stats &stats() const noexcept;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace job_scheduler
