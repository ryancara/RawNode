#include "RenderRuntime.h"

#include "AppState.h"
#include "RenderPipeline.h"

#include <cassert>
#include <utility>

RenderRuntime::RenderRuntime(App &app) noexcept : app_(app)
#ifndef NDEBUG
    , controlThread_(std::this_thread::get_id())
#endif
{}

RenderRuntime::~RenderRuntime() noexcept { shutdown(); }

void RenderRuntime::start() {
  assertControlThread();
  std::lock_guard<std::mutex> lock(mutex_);
  if (stopping_ || previewThread_.joinable()) return;
  previewThread_ = std::thread([this] { runPreview(); });
}

void RenderRuntime::shutdown() noexcept {
  assertControlThread();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!stopping_) {
      stopping_ = true;
      ++epoch_;
      previewPending_ = quietPending_ = displayPending_ = false;
      observe(Event::Stopping);
    }
  }
  workCv_.notify_one();
  join(previewThread_);
  join(exportThread_);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    assert(!previewBusy_ && !exportBusy_);
    observe(Event::Stopped);
  }
}

void RenderRuntime::requestPreview() {
  assertControlThread();
  requestPreview(false);
}

void RenderRuntime::requestDisplayRefresh() {
  assertControlThread();
  std::lock_guard<std::mutex> lock(mutex_);
  if (stopping_) return;
  // Display demand neither advances cancellation nor creates processor work.
  displayPending_ = true;
  workCv_.notify_one();
}

bool RenderRuntime::canEditParameters() const {
  assertControlThread();
  std::lock_guard<std::mutex> lock(mutex_);
  return !exportBusy_;
}

RenderRuntime::Export RenderRuntime::acquireExport() {
  assertControlThread();
  joinExport();
  std::unique_lock<std::mutex> lock(mutex_);
  if (stopping_) return Export(nullptr);
  // Keep dispatch closed across cancellation and acquisition, just as for a
  // document edit. No restored preview can overtake the export barrier.
  ++mutationDepth_;
  if (previewBusy_) observe(Event::ExportWaiting);
  cancelAndDrain(lock);
  exportBusy_ = true;
  --mutationDepth_;
  observe(Event::ExportAcquired);
  return Export(this);
}

RenderRuntime::Export::~Export() noexcept {
  if (runtime_) runtime_->finishExport();
}

void RenderRuntime::Export::startOwned(std::function<void()> work) {
  assert(runtime_);
  runtime_->assertControlThread();
  std::lock_guard<std::mutex> lock(runtime_->mutex_);
  assert(!runtime_->exportThread_.joinable());
  runtime_->exportThread_ = std::thread([runtime = runtime_, work = std::move(work)] {
    Export ownership(runtime);
    work();                            // Export path publishes final status here.
  });
  runtime_ = nullptr;                   // Successful transfer only.
}

void RenderRuntime::joinExport() {
  assertControlThread();
  join(exportThread_);
}

void RenderRuntime::finishExport() noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  exportBusy_ = false;
  queuePreview(true);                   // Does nothing once shutdown has begun.
  observe(Event::ExportReleased);       // Final export status must already exist.
  idleCv_.notify_all();
  workCv_.notify_one();
}

bool RenderRuntime::beginMutation() {
  assertControlThread();
  std::unique_lock<std::mutex> lock(mutex_);
  ++mutationDepth_;                     // Close the gate before draining.
  if (previewBusy_ || exportBusy_) observe(Event::MutationWaiting);
  return cancelAndDrain(lock);
}

void RenderRuntime::completeMutation(bool changed, bool interrupted) noexcept {
  assertControlThread();
  try {
    if (changed || interrupted) requestPreview(!changed);
  } catch (...) {
    // Source conversion may allocate. Recovery failure must not strand the
    // mutation gate or replace the meaningful operation status.
  }
  std::lock_guard<std::mutex> lock(mutex_);
  // Any final preview request was made above while mutationDepth_ still closes
  // dispatch, so later demand cannot run ahead of the completed edit.
  // Observers see that same boundary.
  observe(Event::MutationCompleted);
  assert(mutationDepth_ > 0);
  --mutationDepth_;
  if (mutationDepth_ == 0 && (previewPending_ || displayPending_)) workCv_.notify_one();
}

void RenderRuntime::requestPreview(bool quiet) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (stopping_) return;
  if (app_.nodes.empty() || app_.preview.px.empty()) {
    display_detail::showSourcePreview(app_);
    return;
  }
  ++epoch_;
  queuePreview(quiet);
  if (mutationDepth_ == 0) workCv_.notify_one();
}

void RenderRuntime::queuePreview(bool quiet) {
  if (stopping_) return;
  // Both request kinds share one domain; recovery cannot downgrade a normal
  // request already queued by a later edit (including a nested edit).
  quietPending_ = quiet && (!previewPending_ || quietPending_);
  previewPending_ = true;
}

bool RenderRuntime::cancelAndDrain(std::unique_lock<std::mutex> &lock) {
  bool interrupted = previewPending_ || previewBusy_ || displayPending_ || exportBusy_;
  ++epoch_;
  previewPending_ = quietPending_ = displayPending_ = false;
  idleCv_.wait(lock, [&] { return !previewBusy_ && !exportBusy_; });
  // The caller closes dispatch before draining, so export restoration cannot
  // race this barrier into another preview evaluation.
  interrupted = interrupted || previewPending_;
  previewPending_ = quietPending_ = false;
  return interrupted;
}

void RenderRuntime::runPreview() {
  struct BusyGuard {
    RenderRuntime &runtime;
    ~BusyGuard() { runtime.finishPreview(); }
  };
  for (;;) {
    bool recolorOnly, quiet;
    int gen = 0, width = 0, height = 0;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      workCv_.wait(lock, [&] {
        const bool ready = stopping_ ||
            ((previewPending_ || displayPending_) && !exportBusy_ && mutationDepth_ == 0);
        if (!ready) observe(Event::WorkerIdle);
        return ready;
      });
      if (stopping_) break;
      recolorOnly = !previewPending_ && displayPending_; // Processor work wins.
      quiet = !recolorOnly && quietPending_;
      previewPending_ = quietPending_ = displayPending_ = false;
      previewBusy_ = true;
      if (!recolorOnly) {
        gen = ++epoch_;                // Cannot overtake a locked cancellation.
        width = app_.preview.w;
        height = app_.preview.h;
      }
    }
    BusyGuard busy{*this};
    if (recolorOnly) {
      display_detail::recolorDisplay(app_);
      continue;
    }
    if (app_.nodes.empty() || app_.preview.px.empty()) continue;
    for (auto &node : app_.nodes)
      if (node.processor) node.processor->setRenderSize(width, height);
    if (!quiet) app_.setStatus("Rendering...");
    const RenderCancellation cancellation(epoch_, gen);
    Image out;
    const ProcessorResult result = renderChain(app_, app_.preview, out, cancellation);
    if (cancellation.cancelled()) continue;
    if (result.ok) {
      const int ow = out.w, oh = out.h;
      display_detail::publishPreview(app_, std::move(out), gen);
      if (!quiet) app_.setStatus(std::to_string(ow) + "×" + std::to_string(oh) + " preview");
    } else {
      // Retain/recolour the last good result, without resurrecting demand.
      display_detail::recolorDisplay(app_);
      app_.setStatus("Render failed" + (result.message.empty() ? std::string() : ": " + result.message));
    }
  }
}

void RenderRuntime::finishPreview() noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  previewBusy_ = false;
  idleCv_.notify_all();
}

void RenderRuntime::join(std::thread &worker) {
  // Move under the state lock so even self-test snapshots never race a thread
  // handle being assigned/joined. Joining itself cannot hold a worker's mutex.
  std::thread owned;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    owned = std::move(worker);
  }
  if (owned.joinable()) owned.join();
}

void RenderRuntime::observe(Event event) const noexcept {
  if (observer_) observer_(observerContext_, event);
}

void RenderRuntime::assertControlThread() const {
#ifndef NDEBUG
  assert(controlThread_ == std::this_thread::get_id() && "renderer operation requires the control thread");
#endif
}
