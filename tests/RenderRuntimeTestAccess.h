#pragma once

#include "RenderRuntime.h"

#include <chrono>

// Observation only. No flag setters, worker entry points or cancellation/gate
// operations: tests drive the same requests, leases and document edits as UI.
class RenderRuntimeTestAccess {
 public:
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
