#pragma once

#include "RenderCancellation.h"

#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>

struct App;
namespace document_detail { class DocumentMutation; }
class RenderRuntimeTestAccess;

// One control thread owns requests, document edits and lifecycle operations.
// Workers borrow App only while this owner is alive. App explicitly shuts us
// down in its destructor body, before any borrowed members are destroyed.
class RenderRuntime {
 public:
  explicit RenderRuntime(App &app) noexcept;
  ~RenderRuntime() noexcept;
  RenderRuntime(const RenderRuntime &) = delete;
  RenderRuntime &operator=(const RenderRuntime &) = delete;

  void start();                         // Optional; tests may evaluate without a worker.
  void shutdown() noexcept;            // Terminal and idempotent; drains both workers.
  void requestPreview();
  void requestDisplayRefresh();
  bool canEditParameters() const;      // Same control thread as export acquisition.

  // Export-specific execution ownership, also used by synchronous export.
  // The export path captures its request and publishes final status while the
  // lease is held. start() transfers it to our owned thread only on success;
  // capture/thread-start failures leave the caller's lease intact for reporting.
  class Export {
   public:
    ~Export() noexcept;
    Export(const Export &) = delete;
    Export &operator=(const Export &) = delete;
    explicit operator bool() const { return runtime_ != nullptr; }
    // Export keeps its exception/status policy inside the closure. Reject an
    // unguarded throwing entry point rather than inventing runtime status policy.
    template<class Work> void start(Work work) {
      static_assert(std::is_nothrow_invocable_v<Work &>, "export work must handle its exceptions");
      startOwned(std::move(work));
    }

   private:
    friend class RenderRuntime;
    explicit Export(RenderRuntime *runtime) : runtime_(runtime) {}
    void startOwned(std::function<void()> work);
    RenderRuntime *runtime_;
  };
  Export acquireExport();              // Joins previous execution before reuse.
  void joinExport();

 private:
  friend class document_detail::DocumentMutation;
  friend class RenderRuntimeTestAccess;

  // DocumentMutation is the sole caller of the structural edit boundary.
  bool beginMutation();
  void completeMutation(bool changed, bool interrupted) noexcept;

  void requestPreview(bool quiet);
  void queuePreview(bool quiet);        // mutex_ held; normal demand wins.
  bool cancelAndDrain(std::unique_lock<std::mutex> &lock);

  void runPreview();
  void finishPreview() noexcept;
  void finishExport() noexcept;
  void join(std::thread &worker);
  void assertControlThread() const;

  // Configured only through the self-test friend, before execution. Observers
  // run with mutex_ held, must not re-enter the runtime, and cannot throw.
  enum class Event { WorkerIdle, MutationWaiting, ExportWaiting, ExportAcquired,
                     ExportReleased, MutationCompleted, Stopping, Stopped };
  void observe(Event event) const noexcept;
  void (*observer_)(void *, Event) noexcept = nullptr;
  void *observerContext_ = nullptr;

  App &app_;
  mutable std::mutex mutex_;
  std::condition_variable workCv_, idleCv_;

  // Demand and execution gates are protected by mutex_. A display refresh
  // cannot consume required processor work; mutation/export close dispatch.
  bool stopping_ = false;
  bool previewPending_ = false, quietPending_ = false;
  bool displayPending_ = false;
  bool previewBusy_ = false, exportBusy_ = false;
  int mutationDepth_ = 0;

  // Evaluation reads cancellation without taking mutex_. Thread handles are
  // moved under mutex_ and joined outside it so workers can finish.
  std::atomic<int> epoch_{0};
  std::thread previewThread_, exportThread_;
#ifndef NDEBUG
  const std::thread::id controlThread_;
#endif
};
