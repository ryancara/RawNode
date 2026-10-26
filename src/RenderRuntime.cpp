#include "RenderRuntime.h"

#include "AppState.h"
#include "RenderPipeline.h"

#include <cassert>
#include <utility>
#include <exception>

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

bool RenderRuntime::canCaptureSnapshot() const {
  assertControlThread();
  std::lock_guard<std::mutex> lock(mutex_);
  return !stopping_ && !exportBusy_ && mutationDepth_ == 0 && !app_.isPhotoProtected();
}

bool RenderRuntime::canEditParameters() const {
  assertControlThread();
  std::lock_guard<std::mutex> lock(mutex_);
  return !exportBusy_ && !app_.isPhotoProtected();
}

RenderRuntime::Export RenderRuntime::acquireExport() {
  assertControlThread();
  joinExport();
  std::unique_lock<std::mutex> lock(mutex_);
  if (stopping_) return Export(nullptr);
  try {
    pendingEndpoint_ = inspection_ ? inspection_ : std::make_shared<OutputRef>(app_.documentOutput);
  } catch (...) {
    // Acquisition has not closed dispatch or taken ownership yet.
    try { app_.setStatus("Export failed: Could not allocate preview request"); } catch (...) {}
    return Export(nullptr);
  }
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
    if (changed) {
      std::lock_guard<std::mutex> lock(mutex_);
      if (inspection_ && !findOutput(GraphView{app_.nodes, app_.primarySource, app_.documentOutput}, *inspection_))
        inspection_.reset();
    }
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
  pendingEndpoint_ = inspection_ ? inspection_ : std::make_shared<OutputRef>(app_.documentOutput);
  ++epoch_;
  queuePreview(quiet);
  if (mutationDepth_ == 0) workCv_.notify_one();
}

void RenderRuntime::requestInspection(const OutputRef &endpoint) {
  assertControlThread();
  auto request = std::make_shared<OutputRef>(endpoint);
  std::lock_guard<std::mutex> lock(mutex_);
  if (stopping_) return;
  inspection_ = pendingEndpoint_ = std::move(request);
  ++epoch_;
  queuePreview(false);
  if (mutationDepth_ == 0) workCv_.notify_one();
}

void RenderRuntime::requestDocumentPreview() {
  assertControlThread();
  auto request = std::make_shared<OutputRef>(app_.documentOutput);
  std::lock_guard<std::mutex> lock(mutex_);
  if (stopping_) return;
  inspection_.reset();
  pendingEndpoint_ = std::move(request);
  ++epoch_;
  queuePreview(false);
  if (mutationDepth_ == 0) workCv_.notify_one();
}

void RenderRuntime::resetInspection() {
  assertControlThread();
  std::lock_guard<std::mutex> lock(mutex_);
  inspection_.reset();
}

EvaluationResult RenderRuntime::evaluateOwned(const OutputRef &endpoint, const EvaluationContext &context,
                                             ImageValue source, const RenderCancellation &cancellation) {
  if (beforeEvaluation_) beforeEvaluation_(evaluationContext_);
  return evaluateGraph({app_.nodes, app_.primarySource, app_.documentOutput}, endpoint, context,
                        std::move(source), cancellation, testExecutor_);
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
    int gen;
    std::shared_ptr<const OutputRef> endpoint;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      workCv_.wait(lock, [&] {
        const bool ready = stopping_ ||
            ((previewPending_ || displayPending_) && !exportBusy_ && mutationDepth_ == 0);
        if (!ready) observe(Event::WorkerIdle);
        return ready;
      });
      if (stopping_) break;
      recolorOnly = !previewPending_ && displayPending_;
      quiet = !recolorOnly && quietPending_;
      previewPending_ = quietPending_ = displayPending_ = false;
      previewBusy_ = true;
      gen = recolorOnly ? epoch_.load() : ++epoch_;
      endpoint = pendingEndpoint_;
    }
    BusyGuard busy{*this};
    // Includes evaluator bookkeeping, display allocations and unexpected
    // implementation exceptions, not just Processor::render exceptions.
    try {
      if (recolorOnly) {
        const auto snapshot = [&] {
          std::lock_guard<std::mutex> displayLock(app_.displayMutex);
          return app_.processed; // Immutable shared value and publication identity.
        }();
        endpoint = snapshot.request;
        auto presentation = display_detail::preparePresentation(app_, snapshot.result);
        if (beforePublication_) beforePublication_(publicationContext_, snapshot.result.requested);
        std::lock_guard<std::mutex> lock(mutex_);
        if (gen == epoch_.load() && !stopping_)
          display_detail::commitDisplayRefresh(app_, snapshot, std::move(presentation));
        continue;
      }
      const ImageValue source = app_.preview;
      // Startup and processor edits are valid before a photo is opened. Keep
      // the initial guidance; the evaluator still validates every actual Source.
      if (!endpoint || !source || source->px.empty() || source->w <= 0 || source->h <= 0 ||
          app_.full.px.empty() || app_.full.w <= 0 || app_.full.h <= 0) continue;
      if (!quiet) app_.setStatus("Rendering...");
      const RenderCancellation cancellation(epoch_, gen);
      // previewBusy_ owns this document until finishPreview; every Source
      // replacement drains that ownership through DocumentMutation.
      const EvaluationContext context{EvaluationPurpose::Preview, app_.full.w, app_.full.h,
                                       source->w, source->h};
      auto result = evaluateOwned(*endpoint, context, source, cancellation);
      if (result.state == EvaluationState::Cancelled || cancellation.cancelled()) continue;
      std::string status;
      if (result.state == EvaluationState::Available) {
        int w = 0, h = 0;
        if (const auto *image = std::get_if<ImageValue>(&result.value)) { w = (*image)->w; h = (*image)->h; }
        if (const auto *mask = std::get_if<MaskValue>(&result.value)) { w = (*mask)->w; h = (*mask)->h; }
        status = std::to_string(w) + "×" + std::to_string(h) + " preview";
      } else if (result.state == EvaluationState::Inactive) status = "Mask inactive (bypassed)";
      else status = result.state == EvaluationState::Unavailable ? "Preview unavailable" : "Render failed";
      for (const auto &diagnostic : result.diagnostics) status += " — " + diagnostic.message;
      auto presentation = display_detail::preparePresentation(app_, result);
      if (beforePublication_) beforePublication_(publicationContext_, *endpoint);
      // This same gate records endpoint demand/generation and protects typed
      // publication. An older Image or Mask cannot publish after a new request.
      std::lock_guard<std::mutex> lock(mutex_);
      if (gen != epoch_.load() || endpoint != pendingEndpoint_ || stopping_) continue;
      const bool reportStatus = !quiet || (result.state != EvaluationState::Available &&
                                           result.state != EvaluationState::Inactive);
      display_detail::publishPreview(app_, std::move(result), gen, endpoint, std::move(presentation));
      if (reportStatus) app_.setStatus(status);
    } catch (...) {
      reportPreviewException(endpoint, gen);
    }
  }
}

void RenderRuntime::reportPreviewException(const std::shared_ptr<const OutputRef> &endpoint, int gen) noexcept {
  const auto error = std::current_exception();
  try {
    if (!endpoint) return;
    std::string message = "Unexpected preview exception";
    try { std::rethrow_exception(error); }
    catch (const std::exception &e) { message = e.what(); }
    catch (...) {}
    EvaluationResult result{*endpoint, std::nullopt, EvaluationState::Error, {}, {}};
    const auto *port = findOutput(GraphView{app_.nodes, app_.primarySource, app_.documentOutput}, *endpoint);
    if (port) result.valueType = port->type;
    result.diagnostics.push_back({DiagnosticLevel::Error, *endpoint, message});
    std::lock_guard<std::mutex> lock(mutex_);
    if (gen != epoch_.load() || stopping_) return;
    display_detail::publishPreview(app_, std::move(result), gen, endpoint, {});
    app_.setStatus("Render failed: " + message);
  } catch (...) {
    // Even sustained allocation failure must not escape the worker or retain
    // a successful raster as though this request succeeded. The shared request
    // identity can still be retained without allocating strings/diagnostics.
    try {
      std::lock_guard<std::mutex> lock(mutex_);
      if (gen != epoch_.load() || stopping_) return;
      {
        std::lock_guard<std::mutex> displayLock(app_.displayMutex);
        app_.processed.generation = gen;
        app_.processed.request = endpoint;
        app_.processed.result.state = EvaluationState::Error;
        app_.processed.result.value = std::monostate{};
        app_.processed.result.valueType.reset();
        app_.processed.result.requested.nodeId.value.clear();
        app_.processed.result.requested.outputPortId.value.clear();
        app_.processed.result.diagnostics.clear();
        app_.display = {};
        app_.displayRGBA.clear();
        app_.displayDirty = true;
      }
      try { app_.setStatus("Render failed"); } catch (...) {}
    } catch (...) {}
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
