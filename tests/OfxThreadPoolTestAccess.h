#pragma once

#include "ofx/OfxThreadPool.h"

// The production owner is also used as a local fixture, so teardown is testable
// without stopping/restarting the process-wide host or exposing an app API.
class OfxThreadPoolTestAccess {
 public:
  using Event = OfxThreadPool::Event;
  using Observer = void (*)(void *, Event) noexcept;
  struct Snapshot {
    std::uint64_t generation;
    unsigned remaining, workers;
    bool active, stopping;
  };
  static void observe(OfxThreadPool &pool, void *context, Observer observer) {
    std::lock_guard<std::mutex> lock(pool.mutex_);
    pool.observerContext_ = context;
    pool.observer_ = observer;
  }
  static Snapshot duringObservation(const OfxThreadPool &pool) {
    return {pool.generation_, pool.workersRemaining_, (unsigned)pool.workers_.size(),
            pool.active_, pool.stopping_};
  }
  static Snapshot snapshot(OfxThreadPool &pool) {
    std::lock_guard<std::mutex> lock(pool.mutex_);
    return duringObservation(pool);
  }
  static void shutdown(OfxThreadPool &pool) { pool.shutdown(); }
  // Only after shutdown returned, or in Stopped (never while join is running).
  static bool joined(const OfxThreadPool &pool) {
    for (const auto &worker : pool.workers_)
      if (worker.joinable()) return false;
    return true;
  }
};
