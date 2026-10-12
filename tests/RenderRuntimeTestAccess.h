#pragma once

#include "RenderRuntime.h"

#include <chrono>
#include <cassert>

// Observation plus narrow executor/fault/publication barriers configured before
// execution. No flag setters, worker entry points or cancellation/gate commands:
// tests drive the same requests, leases and document edits as UI.
class RenderRuntimeTestAccess {
 public:
  // Narrow executor/fault injection only, configured before worker execution.
  // No request/gate/epoch setters are exposed.
  static void evaluation(RenderRuntime &runtime, graph_test::Executor *executor,
                         void (*before)(void *) = nullptr, void *context = nullptr) {
    std::lock_guard<std::mutex> lock(runtime.mutex_);
    assert(!runtime.previewThread_.joinable() && !runtime.previewBusy_ && !runtime.exportBusy_);
    runtime.testExecutor_ = executor;
    runtime.beforeEvaluation_ = before;
    runtime.evaluationContext_ = context;
  }
  static void publication(RenderRuntime &runtime, void (*before)(void *, const OutputRef &), void *context) {
    std::lock_guard<std::mutex> lock(runtime.mutex_);
    assert(!runtime.previewThread_.joinable() && !runtime.previewBusy_ && !runtime.exportBusy_);
    runtime.beforePublication_ = before;
    runtime.publicationContext_ = context;
  }
  using Event = RenderRuntime::Event;
  struct Snapshot {
    bool previewPending, quietPending, displayPending, previewBusy, exportBusy;
    int mutationDepth, epoch;
    bool stopping, previewThreadOwned, exportThreadOwned;
  };
  using Observer = void (*)(void *, Event) noexcept;
  static void observe(RenderRuntime &runtime, void *context, Observer observer) {
    std::lock_guard<std::mutex> lock(runtime.mutex_);
    runtime.observerContext_ = context;
    runtime.observer_ = observer;
  }
  static Snapshot snapshot(const RenderRuntime &runtime) {
    std::lock_guard<std::mutex> lock(runtime.mutex_);
    return lockedSnapshot(runtime);
  }
  // Only from the registered observer: the runtime already holds its mutex.
  static Snapshot duringObservation(const RenderRuntime &runtime) {
    return lockedSnapshot(runtime);
  }
  template<class Predicate>
  static bool wait(RenderRuntime &runtime, Predicate ready) {
    std::unique_lock<std::mutex> lock(runtime.mutex_);
    return runtime.idleCv_.wait_for(lock, std::chrono::seconds(2), [&] {
      return ready(lockedSnapshot(runtime));
    });
  }
 private:
  static Snapshot lockedSnapshot(const RenderRuntime &runtime) {
    return {runtime.previewPending_, runtime.quietPending_, runtime.displayPending_,
            runtime.previewBusy_, runtime.exportBusy_, runtime.mutationDepth_, runtime.epoch_.load(),
            runtime.stopping_, runtime.previewThread_.joinable(), runtime.exportThread_.joinable()};
  }
};
