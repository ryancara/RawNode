#pragma once

#include "ofxMultiThread.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

class OfxThreadPoolTestAccess;

// Private to the OFX suite. Owns both the workers and everything they access.
// The owner stops admitting calls before destruction; shutdown drains an
// already admitted job. No application-level shutdown/restart API is needed.
class OfxThreadPool {
 public:
  static OfxThreadPool &host();
  explicit OfxThreadPool(unsigned helpers) : helperCount_(helpers) {}
  ~OfxThreadPool();
  OfxThreadPool(const OfxThreadPool &) = delete;
  OfxThreadPool &operator=(const OfxThreadPool &) = delete;

  // False means busy/stopping: the suite must use its joined ephemeral path.
  bool tryRun(OfxThreadFunctionV1 *function, unsigned slices, void *argument);

 private:
  friend class OfxThreadPoolTestAccess;
  void shutdown();                     // Owner only; terminal and idempotent.
  void runSlices();
  void workerLoop();

  // Tests install a non-throwing observer while idle. Called with mutex_ held;
  // observers must not re-enter the pool. No test scheduling in production.
  enum class Event { WorkerWaiting, WorkerCompleted, CallerWaiting, Stopping, Stopped };
  void observe(Event event) const;
  void (*observer_)(void *, Event) noexcept = nullptr;
  void *observerContext_ = nullptr;

  // All job/lifecycle state is protected by mutex_. Only slice claiming is
  // atomic; published callback fields stay unchanged until every helper has
  // acknowledged the generation.
  const unsigned helperCount_;
  std::mutex mutex_;
  std::condition_variable workCv_, doneCv_;
  std::vector<std::thread> workers_;
  bool started_ = false, active_ = false, stopping_ = false;
  std::uint64_t generation_ = 0;
  unsigned workersRemaining_ = 0;
  OfxThreadFunctionV1 *function_ = nullptr;
  void *argument_ = nullptr;
  unsigned slices_ = 0;
  std::atomic<unsigned> nextSlice_{0};
};
