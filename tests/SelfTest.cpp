#include "SelfTest.h"
#include "OfxThreadPoolTests.h"

#include "imgio/ImageIO.h"
#include "imgio/ImageIOPriv.h"
#include "color/LinearColorTransform.h"
#include "NodeGraph.h"
#include "RenderPipeline.h"
#include "DocumentMutation.h"
#include "RenderRuntimeTestAccess.h"
#include "ofx/OfxHost.h"
#include "ofx/OfxHostPriv.h"
#include "ofxMultiThread.h"
#include "processors/CtlProcessor.h"
#include "processors/NativeCstProcessor.h"
#include "processors/NativeExposureProcessor.h"
#include "processors/OfxProcessor.h"
#include "persist/ProjectPersist.h"
#include "persist/DocumentActions.h"

#include <tiffio.h>
#include <lcms2.h>

#include <algorithm>
#include <array>
#include <optional>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>

#if !defined(NDEBUG) && !defined(_WIN32)
#include <csignal>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

using document_detail::DocumentMutation;
using RuntimeAccess = RenderRuntimeTestAccess;
using RuntimeEvent = RuntimeAccess::Event;

static auto renderState(const App &app) { return RuntimeAccess::snapshot(app.renderer); }
static bool previewIdle(App &app) {
  return RuntimeAccess::wait(app.renderer, [](auto s) {
    return !s.previewBusy && !s.previewPending && !s.displayPending;
  });
}

// Lifetime-scoped, non-throwing observations. Obtaining a snapshot after an
// event also proves the runtime released its lock into the corresponding wait.
struct RuntimeEvents {
  App &app;
  std::mutex mutex;
  std::condition_variable cv;
  std::array<int, 8> counts{};
  std::array<RuntimeAccess::Snapshot, 8> states{};
  std::string releasedStatus;
  bool failed = false;
  explicit RuntimeEvents(App &a) : app(a) {
    RuntimeAccess::observe(app.renderer, this, [](void *context, RuntimeEvent event) noexcept {
      auto &self = *static_cast<RuntimeEvents *>(context);
      std::lock_guard<std::mutex> lock(self.mutex);
      const auto index = static_cast<size_t>(event);
      ++self.counts[index];
      self.states[index] = RuntimeAccess::duringObservation(self.app.renderer);
      if (event == RuntimeEvent::ExportReleased) {
        try { self.releasedStatus = self.app.getStatus(); }
        catch (...) { self.failed = true; }
      }
      self.cv.notify_all();
    });
  }
  ~RuntimeEvents() { RuntimeAccess::observe(app.renderer, nullptr, nullptr); }
  bool wait(RuntimeEvent event, int count = 1) {
    std::unique_lock<std::mutex> lock(mutex);
    return cv.wait_for(lock, std::chrono::seconds(2), [&] {
      return counts[static_cast<size_t>(event)] >= count;
    });
  }
};

enum class ExportFailure { None, Result, RenderException, UnknownException,
                           SizeException, RestoreException, CaptureException };

// A barrier makes queue ordering deterministic without timing a slow render.
class SchedulingSelfTestProcessor : public Processor {
 public:
  std::mutex mutex;
  std::condition_variable cv;
  int calls = 0;
  bool release = false;
  int releasedCalls = 0;
  ExportFailure exportFailure = ExportFailure::None;
  bool failRender = false;
  int failFromCall = 1;
  bool preserveInput = false;
  int width = 0, height = 0;
  struct Call { int width, height; bool interactive; };
  std::vector<Call> history;
  RenderCancellation lastCancellation;
  std::vector<bool> cancelledCalls;

  ProcessorBackend backend() const override { return ProcessorBackend::Native; }
  std::string identifier() const override { return "org.rawnode.selftest.scheduling"; }
  std::string displayName() const override { return "Self-test Scheduling"; }
  std::vector<ProcessorParameter> parameters() const override {
    if (exportFailure == ExportFailure::CaptureException)
      throw std::runtime_error("Self-test capture failure");
    return {};
  }
  bool setParameterValue(const std::string &, const ParameterValue &, bool) override { return false; }
  bool resetParameter(const std::string &, bool) override { return false; }
  bool activateParameter(const std::string &) override { return false; }
  void setRenderSize(int w, int h) override {
    std::lock_guard<std::mutex> lock(mutex);
    const bool restoring = width == 2 && w == 1;
    width = w;
    height = h;
    if (exportFailure == ExportFailure::SizeException && w == 2)
      throw std::runtime_error("Self-test full sizing failure");
    if (exportFailure == ExportFailure::RestoreException && restoring)
      throw std::runtime_error("Self-test preview sizing failure");
  }

  ProcessorResult render(const Image &input, Image &output, const RenderCancellation &cancellation) override {
    std::unique_lock<std::mutex> lock(mutex);
    ++calls;
    lastCancellation = cancellation;
    history.push_back({width, height, cancellation.interactive()});
    cv.notify_all();
    cv.wait(lock, [&] { return release || calls <= releasedCalls; });
    cancelledCalls.push_back(cancellation.cancelled());
    if (!cancellation.interactive()) {
      if (exportFailure == ExportFailure::Result)
        return ProcessorResult::failure(-2, "Self-test export failure");
      if (exportFailure == ExportFailure::RenderException)
        throw std::runtime_error("Self-test export exception");
      if (exportFailure == ExportFailure::UnknownException) throw 42;
    }
    if (failRender && calls >= failFromCall)
      return ProcessorResult::failure(-2, "Self-test render failure");
    output = input;
    // Different results let the test distinguish the latest required render
    // from both the stale display and a superseded in-flight render.
    for (size_t i = 0; i < output.px.size(); ++i)
      if (!preserveInput && i % 4 != 3) output.px[i] = 0.25f * calls;
    return ProcessorResult::success();
  }
};

// Always release the barrier and join before destroying the graph, including
// on a failed assertion or timeout.
struct SchedulingWorkerGuard {
  App &app;
  SchedulingSelfTestProcessor &probe;
  ~SchedulingWorkerGuard() {
    {
      std::lock_guard<std::mutex> lock(probe.mutex);
      probe.release = true;
    }
    probe.cv.notify_all();
    app.renderer.shutdown();
  }
};

struct ExportTestFiles {
  fs::path dir = fs::temp_directory_path() /
      ("rawnode-selftest-export-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  ExportTestFiles() { fs::create_directories(dir); }
  ~ExportTestFiles() {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
  std::string output() const { return (dir / "test.png").string(); }
};

static bool testRuntimeIdleLifecycle(bool started, bool pending) {
  App app;
  app.preview = {{0.18f, 0.18f, 0.18f, 1.0f}, 1, 1};
  Node node;
  node.processor = std::make_unique<NativeExposureProcessor>();
  app.nodes.push_back(std::move(node));
  RuntimeEvents events(app);
  std::optional<DocumentMutation> gate;
  gate.emplace(app);
  if (pending) {
    app.renderer.requestPreview();
    app.renderer.requestDisplayRefresh();
  }
  if (started) {
    app.renderer.start();
    app.renderer.start();
    if (!events.wait(RuntimeEvent::WorkerIdle)) return false;
  }
  const auto before = renderState(app);
  if (before.previewPending != pending || before.displayPending != pending ||
      before.previewThreadOwned != started || before.previewBusy) return false;
  app.renderer.shutdown();
  gate.reset();
  const auto stopped = renderState(app);
  if (!stopped.stopping || stopped.previewThreadOwned || stopped.exportThreadOwned ||
      stopped.previewPending || stopped.quietPending || stopped.displayPending ||
      stopped.previewBusy || stopped.exportBusy || stopped.mutationDepth || app.quit) return false;
  app.renderer.requestPreview();
  app.renderer.requestDisplayRefresh();
  app.renderer.start();
  app.renderer.shutdown();
  auto rejected = app.renderer.acquireExport();
  const auto after = renderState(app);
  return !rejected && after.epoch == stopped.epoch && !after.previewPending &&
         !after.displayPending && !after.previewThreadOwned && !app.displayDirty;
}

// The barrier's lifetime is external to App, so destruction tests can verify
// that the processor was destroyed only after render returned, without touching
// its freed instance.
struct RenderLifetimeFacts {
  std::atomic<bool> active{false}, destroyed{false}, destroyedActive{false};
};
class LifetimeSelfTestProcessor final : public SchedulingSelfTestProcessor {
 public:
  explicit LifetimeSelfTestProcessor(RenderLifetimeFacts &facts) : facts_(facts) {}
  ~LifetimeSelfTestProcessor() override {
    facts_.destroyedActive = facts_.active.load();
    facts_.destroyed = true;
  }
  ProcessorResult render(const Image &input, Image &output,
                         const RenderCancellation &cancellation) override {
    facts_.active = true;
    struct Finish {
      RenderLifetimeFacts &facts;
      ~Finish() { facts.active = false; }
    } finish{facts_};
    return SchedulingSelfTestProcessor::render(input, output, cancellation);
  }
 private:
  RenderLifetimeFacts &facts_;
};

static bool testRuntimeActiveShutdown(bool exporting, bool destroy,
                                      ExportFailure failure = ExportFailure::None) {
  ExportTestFiles files;
  RenderLifetimeFacts facts;
  auto owner = std::make_unique<App>();
  App &app = *owner;
  app.preview = {{0.125f, 0.125f, 0.125f, 1.0f}, 1, 1};
  app.full = {{0.125f, 0.125f, 0.125f, 1.0f, 0.125f, 0.125f, 0.125f, 1.0f}, 2, 1};
  app.display = app.preview;
  auto processor = std::make_unique<LifetimeSelfTestProcessor>(facts);
  auto *probe = processor.get();
  probe->exportFailure = failure;
  Node node;
  node.processor = std::move(processor);
  app.nodes.push_back(std::move(node));
  // This callback owns no App data and remains alive through App destruction.
  struct StopObservation {
    std::mutex mutex;
    std::condition_variable cv;
    bool stopping = false;
  } stop;
  RuntimeAccess::observe(app.renderer, &stop, [](void *context, RuntimeEvent event) noexcept {
    if (event != RuntimeEvent::Stopping) return;
    auto &stop = *static_cast<StopObservation *>(context);
    std::lock_guard<std::mutex> lock(stop.mutex);
    stop.stopping = true;
    stop.cv.notify_all();
  });
  struct ObserverGuard {
    std::unique_ptr<App> &owner;
    ~ObserverGuard() { if (owner) RuntimeAccess::observe(owner->renderer, nullptr, nullptr); }
  } observerGuard{owner};
  if (exporting && !startExport(app, files.output())) return false;
  app.renderer.start();
  if (!exporting) app.renderer.requestPreview();
  bool entered;
  {
    std::unique_lock<std::mutex> lock(probe->mutex);
    entered = probe->cv.wait_for(lock, std::chrono::seconds(2), [&] { return probe->calls == 1; });
  }
  bool ownedAtStop = false;
  std::thread release([&] {
    bool stopping;
    {
      std::unique_lock<std::mutex> lock(stop.mutex);
      stopping = stop.cv.wait_for(lock, std::chrono::seconds(2), [&] { return stop.stopping; });
    }
    const auto state = renderState(app);
    ownedAtStop = stopping && facts.active && !facts.destroyed && state.stopping &&
                 (exporting ? state.exportBusy && !state.previewBusy : state.previewBusy && !state.exportBusy);
    {
      std::lock_guard<std::mutex> lock(probe->mutex);
      probe->release = true;
      // App destruction may follow as soon as the worker leaves this barrier.
      // Finish notifying while the processor's mutex still pins its lifetime.
      probe->cv.notify_all();
    }
    // Do not touch App or the processor after releasing execution.
  });
  if (destroy) owner.reset();
  else app.renderer.shutdown();
  release.join();
  if (!entered || !ownedAtStop || facts.active || facts.destroyedActive || facts.destroyed != destroy) return false;
  if (!destroy) {
    const auto state = renderState(app);
    if (state.previewThreadOwned || state.exportThreadOwned || state.previewBusy || state.exportBusy ||
        state.previewPending || state.displayPending || app.displayDirty) return false;
    // Runtime shutdown is independent of application/thumbnail shutdown intent.
    if (app.quit || probe->calls != 1) return false;
    RuntimeAccess::observe(app.renderer, nullptr, nullptr);
    owner.reset();
  }
  return facts.destroyed && !facts.destroyedActive;
}

static bool testNormalPreviewDominatesQuiet(bool observe) {
  App app;
  app.preview = {{0.125f, 0.125f, 0.125f, 1.0f}, 1, 1};
  Node node;
  node.processor = std::make_unique<NativeExposureProcessor>();
  app.nodes.push_back(std::move(node));
  RuntimeEvents events(app);
  if (!observe) RuntimeAccess::observe(app.renderer, nullptr, nullptr);
  app.renderer.requestPreview();
  {
    DocumentMutation outer(app);       // Interrupted; will request quiet recovery.
    {
      DocumentMutation inner(app);
      inner.changed();                 // A normal request is already queued.
    }
  }
  const auto state = renderState(app);
  if (!state.previewPending || state.quietPending || state.mutationDepth || state.epoch != 5) return false;
  if (observe) {
    const auto completing = events.states[static_cast<size_t>(RuntimeEvent::MutationCompleted)];
    if (!completing.previewPending || completing.quietPending || completing.mutationDepth != 1) return false;
  }
  // Observation must not change demand, cancellation, dispatch, or quiet status.
  for (int i = 0; i < 100; ++i) {
    const auto observed = renderState(app);
    if (observed.epoch != state.epoch || !observed.previewPending || observed.quietPending ||
        observed.previewBusy || observed.previewThreadOwned) return false;
  }
  app.renderer.start();
  if (!previewIdle(app) || app.getStatus() != "1×1 preview") return false;
  const auto completed = renderState(app);
  return !completed.previewPending && !completed.quietPending && completed.epoch == state.epoch + 1 &&
         app.displayGen == completed.epoch && app.displayDirty && app.display.px == app.preview.px;
}

static bool testRuntimeCancellation() {
  App app, independent;
  app.preview = {{0.125f, 0.125f, 0.125f, 1.0f}, 1, 1};
  app.display = app.preview;
  auto processor = std::make_unique<SchedulingSelfTestProcessor>();
  auto *probe = processor.get();
  Node node;
  node.processor = std::move(processor);
  app.nodes.push_back(std::move(node));
  auto following = std::make_unique<SchedulingSelfTestProcessor>();
  auto *next = following.get();
  next->release = true;
  Node nextNode;
  nextNode.processor = std::move(following);
  app.nodes.push_back(std::move(nextNode));
  SchedulingWorkerGuard guard{app, *probe};
  app.renderer.start();
  app.renderer.requestPreview();
  {
    std::unique_lock<std::mutex> lock(probe->mutex);
    if (!probe->cv.wait_for(lock, std::chrono::seconds(2), [&] { return probe->calls == 1; })) return false;
  }
  independent.preview = app.preview;
  if (!addNativeExposureNode(independent)) return false;
  independent.renderer.requestPreview();
  {
    std::lock_guard<std::mutex> lock(probe->mutex);
    if (!probe->lastCancellation.interactive() || probe->lastCancellation.cancelled()) return false;
  }
  const int epoch = renderState(app).epoch;
  app.renderer.requestPreview();
  if (renderState(app).epoch != epoch + 1) return false;
  {
    std::lock_guard<std::mutex> lock(probe->mutex);
    if (!probe->lastCancellation.cancelled()) return false;
    probe->releasedCalls = 1;
  }
  probe->cv.notify_all();
  {
    std::unique_lock<std::mutex> lock(probe->mutex);
    if (!probe->cv.wait_for(lock, std::chrono::seconds(2), [&] { return probe->calls == 2; }) ||
        probe->cancelledCalls.size() != 1 || !probe->cancelledCalls[0]) return false;
  }
  // Cancellation must stop the obsolete chain before its next processor and
  // leave the last good display intact while replacement is still blocked.
  {
    std::lock_guard<std::mutex> lock(next->mutex);
    if (next->calls != 0) return false;
  }
  if (app.displayDirty || app.display.px != app.preview.px) return false;
  {
    std::lock_guard<std::mutex> lock(probe->mutex);
    probe->release = true;
  }
  probe->cv.notify_all();
  if (!previewIdle(app)) return false;
  return next->calls == 1 && probe->cancelledCalls.size() == 2 && !probe->cancelledCalls[1] &&
         app.displayDirty && app.displayGen > epoch;
}

#if !defined(NDEBUG) && !defined(_WIN32)
static bool testControlThreadAssertions() {
  // Run before any workers/plugin loading. Each child deliberately violates one
  // contract; no process-wide assertion handler or production escape hatch.
  for (int operation = 0; operation < 8; ++operation) {
    const pid_t child = fork();
    if (child < 0) return false;
    if (child == 0) {
      const rlimit noCore{0, 0};
      setrlimit(RLIMIT_CORE, &noCore);
      App app;
      std::thread wrongThread([&] {
        switch (operation) {
          case 0: { DocumentMutation mutation(app); break; }
          case 1: { auto lease = app.renderer.acquireExport(); break; }
          case 2: app.renderer.requestPreview(); break;
          case 3: app.renderer.requestDisplayRefresh(); break;
          case 4: (void)app.renderer.canEditParameters(); break;
          case 5: app.renderer.start(); break;
          case 6: app.renderer.shutdown(); break;
          case 7: app.renderer.joinExport(); break;
        }
      });
      wrongThread.join();
      _exit(0);
    }
    int status = 0;
    if (waitpid(child, &status, 0) != child || !WIFSIGNALED(status) || WTERMSIG(status) != SIGABRT) return false;
  }
  return true;
}
#endif

static bool testRenderScheduling(bool busy, bool recolorFirst, bool queueFull = true, bool failRender = false) {
  App app;
  app.preview.w = app.preview.h = 1;
  app.preview.px = {0.18f, 0.18f, 0.18f, 1.0f};
  app.display = app.preview;
  app.display.px = {0.05f, 0.05f, 0.05f, 1.0f};
  toDisplayRGBA8(app.display, app.outputEncoding, app.displayRGBA);
  auto processor = std::make_unique<SchedulingSelfTestProcessor>();
  auto *probe = processor.get();
  probe->failRender = failRender;
  Node node;
  node.processor = std::move(processor);
  app.nodes.push_back(std::move(node));

  SchedulingWorkerGuard guard{app, *probe};

  std::optional<DocumentMutation> gate;
  if (busy) {
    app.renderer.start();
    app.renderer.requestPreview();
    std::unique_lock<std::mutex> lock(probe->mutex);
    if (!probe->cv.wait_for(lock, std::chrono::seconds(2), [&] { return probe->calls == 1; }))
      return false;
  } else {
    // Exercise waking the worker after PR #32's mutation gate opens as well.
    gate.emplace(app);
    app.renderer.start();
  }

  {
    std::lock_guard<std::mutex> lock(app.colorMutex);
    app.outputEncoding = {RgbGamut::Rec709, TransferFunction::Linear};
  }
  bool generationUnchanged = true;
  auto recolor = [&] {
    const int before = renderState(app).epoch;
    app.renderer.requestDisplayRefresh();
    generationUnchanged = generationUnchanged && renderState(app).epoch == before;
  };
  if (recolorFirst) recolor();
  if (queueFull) app.renderer.requestPreview();
  if (!recolorFirst) recolor();
  const int requiredGen = renderState(app).epoch;

  {
    std::lock_guard<std::mutex> lock(probe->mutex);
    probe->release = true;
  }
  probe->cv.notify_all();
  gate.reset();

  if (!previewIdle(app)) return false;
  const int expectedCalls = busy && queueFull ? 2 : 1;
  {
    std::lock_guard<std::mutex> lock(probe->mutex);
    if (probe->calls != expectedCalls) return false;
  }
  Image expected = app.preview;
  expected.px = failRender ? std::vector<float>{0.05f, 0.05f, 0.05f, 1.0f}
                           : std::vector<float>{0.25f * expectedCalls, 0.25f * expectedCalls,
                                                0.25f * expectedCalls, 1.0f};
  std::vector<unsigned char> expectedRGBA;
  toDisplayRGBA8(expected, app.outputEncoding, expectedRGBA);
  std::lock_guard<std::mutex> lock(app.displayMutex);
  return generationUnchanged && app.display.px == expected.px &&
         app.displayRGBA == expectedRGBA && app.displayDirty &&
         (failRender ? app.displayGen == 0 && app.getStatus() == "Render failed: Self-test render failure"
                     : app.displayGen >= requiredGen);
}

// Display-only demand stays behind real nested document scopes and never
// creates processor work. Shutdown cancels it without a restoration request.
static bool testRecolorMutationGate(bool cancel) {
  App app;
  app.preview = {{0.18f, 0.18f, 0.18f, 1.0f}, 1, 1};
  app.display = app.preview;
  toDisplayRGBA8(app.display, app.outputEncoding, app.displayRGBA);
  const auto originalRGBA = app.displayRGBA;
  auto processor = std::make_unique<SchedulingSelfTestProcessor>();
  auto *probe = processor.get();
  probe->release = true;
  Node node;
  node.processor = std::move(processor);
  app.nodes.push_back(std::move(node));
  RuntimeEvents events(app);
  SchedulingWorkerGuard guard{app, *probe};
  std::optional<DocumentMutation> outer, inner;
  outer.emplace(app);
  inner.emplace(app);
  app.outputEncoding = {RgbGamut::Rec709, TransferFunction::Linear};
  const int generation = renderState(app).epoch;
  app.renderer.requestDisplayRefresh();
  app.renderer.start();
  if (!events.wait(RuntimeEvent::WorkerIdle)) return false;
  auto state = renderState(app);
  if (state.previewBusy || state.previewPending || !state.displayPending ||
      state.mutationDepth != 2 || state.epoch != generation || app.displayDirty) return false;
  if (cancel) app.renderer.shutdown();
  inner.reset();
  state = renderState(app);
  if (state.mutationDepth != 1 || state.previewBusy || state.displayPending == cancel) return false;
  outer.reset();
  if (!previewIdle(app)) return false;
  state = renderState(app);
  std::vector<unsigned char> expectedRGBA;
  toDisplayRGBA8(app.display, app.outputEncoding, expectedRGBA);
  std::lock_guard<std::mutex> lock(app.displayMutex);
  return probe->calls == 0 && app.display.px == app.preview.px && app.displayGen == 0 &&
         app.displayDirty == !cancel && app.displayRGBA == (cancel ? originalRGBA : expectedRGBA) &&
         state.epoch == generation + (cancel ? 1 : 0);
}

// F2: the render consuming a recolour is superseded, then its replacement fails.
// F5: a document barrier cancels a recolour, then the expected full render fails.
static bool testFailedRenderWithoutPendingRecolor(bool cancelRecolor) {
  App app;
  app.preview.w = app.preview.h = 1;
  app.preview.px = {0.18f, 0.18f, 0.18f, 1.0f};
  app.display = app.preview;
  app.display.px = {0.05f, 0.05f, 0.05f, 1.0f};
  app.displayGen = 17;
  const Image lastGood = app.display;
  toDisplayRGBA8(app.display, app.outputEncoding, app.displayRGBA);
  const auto originalRGBA = app.displayRGBA;
  auto processor = std::make_unique<SchedulingSelfTestProcessor>();
  auto *probe = processor.get();
  probe->failRender = true;
  probe->failFromCall = cancelRecolor ? 1 : 2;
  Node node;
  node.processor = std::move(processor);
  app.nodes.push_back(std::move(node));
  SchedulingWorkerGuard guard{app, *probe};

  std::optional<DocumentMutation> gate;
  gate.emplace(app);
  app.renderer.start();
  {
    std::lock_guard<std::mutex> lock(app.colorMutex);
    app.outputEncoding = {RgbGamut::Rec709, cancelRecolor ? TransferFunction::Linear
                                                       : TransferFunction::Rec709};
  }
  app.renderer.requestDisplayRefresh();
  if (cancelRecolor) {
    // Keep the worker behind the mutation gate so cancellation is guaranteed
    // to clear queued work before the expected full render is scheduled.
    { DocumentMutation cancellation(app); }
    if (renderState(app).displayPending || renderState(app).previewBusy) return false;
  } else {
    app.renderer.requestPreview();
    gate.reset();
    {
      std::unique_lock<std::mutex> lock(probe->mutex);
      if (!probe->cv.wait_for(lock, std::chrono::seconds(2), [&] { return probe->calls == 1; }))
        return false;
    }
    const auto state = renderState(app);
    if (!state.previewBusy || state.previewPending || state.displayPending) return false;
    // The first render has consumed the recolour but is held at the processor
    // barrier. Supersede it with processor work alone and a newer output tag.
    std::lock_guard<std::mutex> lock(app.colorMutex);
    app.outputEncoding = {RgbGamut::Rec709, TransferFunction::Linear};
  }
  {
    std::lock_guard<std::mutex> lock(app.displayMutex);
    if (app.displayDirty || app.displayRGBA != originalRGBA) return false;
  }
  app.renderer.requestPreview();
  {
    std::lock_guard<std::mutex> lock(probe->mutex);
    probe->release = true;
  }
  probe->cv.notify_all();
  if (cancelRecolor) gate.reset();
  if (!previewIdle(app)) return false;
  app.renderer.shutdown();
  std::vector<unsigned char> expectedRGBA;
  toDisplayRGBA8(lastGood, app.outputEncoding, expectedRGBA);
  std::lock_guard<std::mutex> lock(app.displayMutex);
  return probe->calls == (cancelRecolor ? 1 : 2) && app.display.px == lastGood.px &&
         app.displayGen == 17 && app.displayDirty && expectedRGBA != originalRGBA &&
         app.displayRGBA == expectedRGBA && app.getStatus() == "Render failed: Self-test render failure";
}

// Use the production export job and a processor barrier, rather than
// sleeps or a large image, to force cancellation before export acquires the graph.
enum class ExportPreviewWork { None, Pending, Active };
enum class ExportPreviewEnd { Resume, Mutate, MutateDuring, StopDuring, StopAfter };

static bool testPreviewAfterExport(ExportPreviewWork work,
                                   ExportPreviewEnd ending = ExportPreviewEnd::Resume,
                                   int ofxPluginIndex = -1) {
  ExportTestFiles files;
  App app;
  if (!app.renderer.canEditParameters()) return false;
  app.preview = {{0.125f, 0.125f, 0.125f, 1.0f}, 1, 1};
  app.full = {{0.125f, 0.125f, 0.125f, 1.0f, 0.125f, 0.125f, 0.125f, 1.0f}, 2, 1};
  app.display = app.preview;
  toDisplayRGBA8(app.display, app.outputEncoding, app.displayRGBA);
  const auto oldRGBA = app.displayRGBA;
  auto processor = std::make_unique<SchedulingSelfTestProcessor>();
  auto *probe = processor.get();
  probe->preserveInput = true;
  Node barrier;
  barrier.processor = std::move(processor);
  app.nodes.push_back(std::move(barrier));
  Node node;
  node.processor = std::make_unique<NativeExposureProcessor>();
  node.processor->setParameterValue("exposure", 1.0);
  app.nodes.push_back(std::move(node));
  if (ofxPluginIndex >= 0 && !addNode(app, ofxPluginIndex)) return false;
  RuntimeEvents events(app);
  SchedulingWorkerGuard guard{app, *probe};
  const bool active = work == ExportPreviewWork::Active;
  if (work != ExportPreviewWork::None) app.renderer.requestPreview();
  bool acquired = false;
  if (active) {
    app.renderer.start();
    {
      std::unique_lock<std::mutex> lock(probe->mutex);
      if (!probe->cv.wait_for(lock, std::chrono::seconds(2), [&] { return probe->calls == 1; })) return false;
    }
    if (!app.renderer.canEditParameters()) return false;
    const int generation = renderState(app).epoch;
    bool exclusive = false;
    std::thread releasePreview([&] {
      const bool waiting = events.wait(RuntimeEvent::ExportWaiting);
      const auto state = renderState(app);
      exclusive = waiting && state.previewBusy && !state.exportBusy &&
                  state.mutationDepth == 1 && state.epoch > generation;
      {
        std::lock_guard<std::mutex> lock(probe->mutex);
        probe->releasedCalls = 1;
      }
      probe->cv.notify_all();
    });
    acquired = startExport(app, files.output());
    releasePreview.join();
    if (!exclusive) return false;
  } else {
    acquired = startExport(app, files.output());
  }
  if (!acquired || app.renderer.canEditParameters()) return false;
  const int activeCalls = active ? 1 : 0;
  {
    std::unique_lock<std::mutex> lock(probe->mutex);
    if (!probe->cv.wait_for(lock, std::chrono::seconds(2), [&] { return probe->calls == activeCalls + 1; })) return false;
  }
  auto state = renderState(app);
  if (!state.exportBusy || state.previewBusy || state.previewPending) return false;
  app.renderer.requestDisplayRefresh();
  const bool mutateDuring = ending == ExportPreviewEnd::MutateDuring;
  const bool stopDuring = ending == ExportPreviewEnd::StopDuring;
  bool blocked = true;
  std::thread releaseExport;
  auto release = [&] {
    std::lock_guard<std::mutex> lock(probe->mutex);
    probe->release = true;
    probe->cv.notify_all();
  };
  if (mutateDuring || stopDuring) {
    releaseExport = std::thread([&] {
      blocked = events.wait(stopDuring ? RuntimeEvent::Stopping : RuntimeEvent::MutationWaiting);
      const auto waiting = renderState(app);
      blocked = blocked && waiting.exportBusy && !waiting.previewBusy &&
                (stopDuring ? waiting.stopping : waiting.mutationDepth == 1);
      release();
    });
  } else release();
  auto replaceExposure = [&] {
    DocumentMutation mutation(app);
    app.nodes[1].processor = std::make_unique<NativeExposureProcessor>();
    app.nodes[1].processor->setParameterValue("exposure", 2.0);
    mutation.changed();
  };
  if (mutateDuring) replaceExposure();
  if (stopDuring) app.renderer.shutdown();
  if (releaseExport.joinable()) releaseExport.join();
  app.renderer.joinExport();
  if (!blocked || !app.renderer.canEditParameters()) return false;
  if (ending == ExportPreviewEnd::Mutate) replaceExposure();
  if (ending == ExportPreviewEnd::StopAfter) app.renderer.shutdown();
  const bool expectPreview = !stopDuring && ending != ExportPreviewEnd::StopAfter;
  if (!active) {
    state = renderState(app);
    const bool normal = mutateDuring || ending == ExportPreviewEnd::Mutate;
    if (state.exportBusy || state.previewBusy || state.previewPending != expectPreview ||
        state.quietPending != (expectPreview && !normal)) return false;
  }
  if (expectPreview) app.renderer.start();
  if (!previewIdle(app)) return false;
  app.renderer.shutdown();
  Image exported;
  ColorEncoding exportedEncoding;
  bool decodedRaw = false;
  if (!loadImage(files.output(), exported, exportedEncoding, decodedRaw) || exported.w != 2 ||
      exported.h != 1 || !fs::is_regular_file(exportSidecarPath(files.output()))) return false;
  if (probe->width != 1 || probe->height != 1 ||
      probe->calls != activeCalls + 1 + (expectPreview ? 1 : 0)) return false;
  const auto &fullCall = probe->history[activeCalls];
  if (fullCall.width != 2 || fullCall.height != 1 || fullCall.interactive) return false;
  if (expectPreview) {
    const auto &previewCall = probe->history.back();
    if (previewCall.width != 1 || previewCall.height != 1 || !previewCall.interactive) return false;
  }
  Image expected = app.preview;
  if (expectPreview) {
    const float value = (mutateDuring || ending == ExportPreviewEnd::Mutate) ? 0.5f : 0.25f;
    expected.px = {value, value, value, 1.0f};
  }
  std::vector<unsigned char> expectedRGBA;
  toDisplayRGBA8(expected, app.outputEncoding, expectedRGBA);
  return app.display.w == 1 && app.display.h == 1 && app.display.px == expected.px &&
         app.displayRGBA == expectedRGBA && app.displayDirty == expectPreview &&
         (expectPreview ? app.displayGen > 0 && app.displayRGBA != oldRGBA : app.displayGen == 0);
}

enum class ExportStatusPreview { Preserve, Fail, ExplicitEdit };
enum class ExportStatusResult { Success, Warnings, Failure, WriteFailure };

static bool testExportPreviewStatus(ExportStatusResult result,
                                    ExportStatusPreview action = ExportStatusPreview::Preserve) {
  ExportTestFiles files;
  App app;
  app.preview.w = app.preview.h = 1;
  app.preview.px = {0.125f, 0.125f, 0.125f, 1.0f};
  app.full.w = 2;
  app.full.h = 1;
  app.full.px = {0.125f, 0.125f, 0.125f, 1.0f, 0.125f, 0.125f, 0.125f, 1.0f};
  app.display = app.preview;
  toDisplayRGBA8(app.display, app.outputEncoding, app.displayRGBA);
  auto processor = std::make_unique<SchedulingSelfTestProcessor>();
  auto *probe = processor.get();
  probe->preserveInput = true;
  if (result == ExportStatusResult::Failure) probe->exportFailure = ExportFailure::Result;
  Node barrier;
  barrier.processor = std::move(processor);
  app.nodes.push_back(std::move(barrier));
  Node exposure;
  exposure.processor = std::make_unique<NativeExposureProcessor>();
  exposure.processor->setParameterValue("exposure", 1.0);
  app.nodes.push_back(std::move(exposure));
  if (result == ExportStatusResult::Warnings) {
    Node missing;
    missing.id = "missing-node";
    missing.storedBackend = "native";
    missing.storedIdentifier = "org.rawnode.selftest.missing";
    app.nodes.push_back(std::move(missing));
    app.outputEncoding.gamma = TransferFunction::DaVinciIntermediate;
  }
  const std::string outPath = result == ExportStatusResult::WriteFailure
      ? (files.dir / "absent" / "test.png").string() : files.output();
  std::string exportStatus = "Exported test.png (2×1)";
  if (result == ExportStatusResult::Warnings)
    exportStatus += " — missing processors were bypassed"
        " — warning: ICC cannot fully represent DaVinci Intermediate scene values above 1.0;"
        " external apps may clip highlights";
  if (result == ExportStatusResult::Failure) exportStatus = "Export failed: Self-test export failure";
  if (result == ExportStatusResult::WriteFailure) exportStatus = "Export failed";

  RuntimeEvents events(app);
  SchedulingWorkerGuard guard{app, *probe};
  if (!startExport(app, outPath)) return false;
  {
    std::unique_lock<std::mutex> lock(probe->mutex);
    if (!probe->cv.wait_for(lock, std::chrono::seconds(2), [&] { return probe->calls == 1; }))
      return false;
  }
  {
    std::lock_guard<std::mutex> lock(probe->mutex);
    probe->failRender = action == ExportStatusPreview::Fail;
    probe->failFromCall = 2;
  }
  app.renderer.start();
  const bool observedPark = events.wait(RuntimeEvent::WorkerIdle);
  const auto parked = renderState(app);
  const bool idleDuringExport = observedPark && parked.exportBusy && !parked.previewBusy &&
      !parked.previewPending && !parked.quietPending && !parked.displayPending;
  {
    std::lock_guard<std::mutex> lock(probe->mutex);
    probe->releasedCalls = 1;  // Complete export, but hold the restored preview.
  }
  probe->cv.notify_all();
  app.renderer.joinExport();  // Only export completion can wake the parked preview here.
  if (!idleDuringExport || events.failed || events.releasedStatus != exportStatus) return false;
  const bool exported = result == ExportStatusResult::Success || result == ExportStatusResult::Warnings;
  if (fs::is_regular_file(outPath) != exported ||
      fs::is_regular_file(exportSidecarPath(outPath)) != exported) return false;
  {
    std::unique_lock<std::mutex> lock(probe->mutex);
    if (!probe->cv.wait_for(lock, std::chrono::seconds(2), [&] { return probe->calls == 2; }))
      return false;
  }
  if (app.getStatus() != exportStatus) return false;  // No "Rendering..." overwrite.
  if (!app.renderer.canEditParameters()) return false;  // Export released; preview is still active.
  {
    if (!renderState(app).previewBusy || renderState(app).previewPending || renderState(app).quietPending || renderState(app).displayPending)
      return false;  // Quiet metadata was consumed with the request.
  }
  if (action == ExportStatusPreview::ExplicitEdit) {
    app.nodes[1].processor->setParameterValue("exposure", 2.0);
    app.renderer.requestPreview();  // A later explicit request must report normal preview status.
  }
  {
    std::lock_guard<std::mutex> lock(probe->mutex);
    probe->release = true;
  }
  probe->cv.notify_all();
  if (!previewIdle(app)) return false;
  app.renderer.shutdown();
  {
    std::lock_guard<std::mutex> lock(probe->mutex);
    if (probe->calls != (action == ExportStatusPreview::ExplicitEdit ? 3 : 2)) return false;
    if (probe->history.back().width != 1 || !probe->history.back().interactive) return false;
  }
  const std::string expectedStatus = action == ExportStatusPreview::Fail
      ? "Render failed: Self-test render failure"
      : action == ExportStatusPreview::ExplicitEdit ? "1×1 preview" : exportStatus;
  Image expected = app.preview;
  const float value = action == ExportStatusPreview::Fail ? 0.125f
                    : action == ExportStatusPreview::ExplicitEdit ? 0.5f : 0.25f;
  expected.px = {value, value, value, 1.0f};
  std::vector<unsigned char> expectedRGBA;
  toDisplayRGBA8(expected, app.outputEncoding, expectedRGBA);
  std::lock_guard<std::mutex> lock(app.displayMutex);
  return app.getStatus() == expectedStatus && app.display.w == 1 && app.display.h == 1 &&
         app.display.px == expected.px && app.displayRGBA == expectedRGBA && app.displayDirty &&
         (action == ExportStatusPreview::Fail ? app.displayGen == 0 : app.displayGen > 0);
}

static bool testExportFailureCleanup(ExportFailure failure, bool asynchronous) {
  ExportTestFiles files;
  App app;
  if (!app.renderer.canEditParameters()) return false;
  app.preview = {{0.125f, 0.125f, 0.125f, 1.0f}, 1, 1};
  app.full = {{0.125f, 0.125f, 0.125f, 1.0f, 0.125f, 0.125f, 0.125f, 1.0f}, 2, 1};
  app.display = app.preview;
  auto processor = std::make_unique<SchedulingSelfTestProcessor>();
  auto *probe = processor.get();
  probe->release = probe->preserveInput = true;
  probe->setRenderSize(1, 1);
  probe->exportFailure = failure;
  Node first;
  first.processor = std::move(processor);
  app.nodes.push_back(std::move(first));
  auto following = std::make_unique<SchedulingSelfTestProcessor>();
  auto *nextProbe = following.get();
  nextProbe->release = nextProbe->preserveInput = true;
  nextProbe->setRenderSize(1, 1);
  Node second;
  second.processor = std::move(following);
  app.nodes.push_back(std::move(second));
  Node exposure;
  exposure.processor = std::make_unique<NativeExposureProcessor>();
  exposure.processor->setParameterValue("exposure", 1.0);
  app.nodes.push_back(std::move(exposure));
  RuntimeEvents events(app);
  SchedulingWorkerGuard guard{app, *probe};

  // Leave preview unstarted until export cleanup has been inspected.
  if (asynchronous) {
    const bool started = startExport(app, files.output());
    if (started != (failure != ExportFailure::CaptureException)) return false;
    app.renderer.joinExport();
  } else if (runExportJob(app, files.output())) {
    return false;
  }
  std::string message;
  switch (failure) {
    case ExportFailure::Result: message = "Self-test export failure"; break;
    case ExportFailure::RenderException: message = "Self-test export exception"; break;
    case ExportFailure::UnknownException: message = "Unknown exception"; break;
    case ExportFailure::SizeException: message = "Self-test full sizing failure"; break;
    case ExportFailure::RestoreException: message = "Self-test preview sizing failure"; break;
    case ExportFailure::CaptureException: message = "Self-test capture failure"; break;
    default: return false;
  }
  const std::string failureStatus = "Export failed: " + message;
  if (!app.renderer.canEditParameters() || events.failed || events.releasedStatus != failureStatus) return false;
  {
    if (renderState(app).exportBusy || renderState(app).previewBusy || !renderState(app).previewPending || !renderState(app).quietPending)
      return false;
  }
  if (renderState(app).exportThreadOwned || app.getStatus() != failureStatus ||
      fs::exists(files.output()) || fs::exists(exportSidecarPath(files.output()))) return false;
  for (auto *sizingProbe : {probe, nextProbe}) {
    std::lock_guard<std::mutex> lock(sizingProbe->mutex);
    if (sizingProbe->width != 1 || sizingProbe->height != 1) return false;
  }
  app.renderer.start();
  auto waitForPreview = [&] { return previewIdle(app); };
  if (!waitForPreview() || app.getStatus() != failureStatus) return false;
  {
    std::lock_guard<std::mutex> lock(app.displayMutex);
    if (app.display.px[0] != 0.25f || app.displayGen == 0) return false;
  }
  // A later explicit edit uses normal preview status again.
  app.nodes[2].processor->setParameterValue("exposure", 2.0);
  app.renderer.requestPreview();
  if (!waitForPreview() || app.getStatus() != "1×1 preview") return false;
  std::lock_guard<std::mutex> lock(app.displayMutex);
  return app.display.px[0] == 0.5f;
}

static bool testExportThreadLifecycle(bool stopDuring) {
  ExportTestFiles files;
  App app;
  app.preview = {{0.125f, 0.125f, 0.125f, 1.0f}, 1, 1};
  app.full = {{0.125f, 0.125f, 0.125f, 1.0f, 0.125f, 0.125f, 0.125f, 1.0f}, 2, 1};
  app.path = (files.dir / "source.nef").string();
  app.inputIsRaw = true;
  auto processor = std::make_unique<SchedulingSelfTestProcessor>();
  auto *probe = processor.get();
  probe->preserveInput = true;
  probe->release = !stopDuring;
  Node node;
  node.id = "export-probe";
  node.processor = std::move(processor);
  app.nodes.push_back(std::move(node));
  RuntimeEvents events(app);
  SchedulingWorkerGuard guard{app, *probe};
  if (stopDuring) app.renderer.start();
  if (!startExport(app, files.output()) || !renderState(app).exportThreadOwned) return false;
  if (stopDuring) {
    {
      std::unique_lock<std::mutex> lock(probe->mutex);
      if (!probe->cv.wait_for(lock, std::chrono::seconds(2), [&] { return probe->calls == 1; })) return false;
    }
    if (app.renderer.canEditParameters()) return false;
    bool stoppingOwnedExport = false;
    std::thread release([&] {
      const bool observed = events.wait(RuntimeEvent::Stopping);
      const auto state = renderState(app);
      stoppingOwnedExport = observed && state.exportBusy && !state.previewBusy;
      {
        std::lock_guard<std::mutex> lock(probe->mutex);
        probe->release = true;
      }
      probe->cv.notify_all();
    });
    app.renderer.shutdown();
    release.join();
    if (!stoppingOwnedExport) return false;
  } else {
    if (!RuntimeAccess::wait(app.renderer, [](auto state) { return !state.exportBusy; })) return false;
    if (!app.renderer.canEditParameters()) return false;
    {
      std::lock_guard<std::mutex> lock(probe->mutex);
      probe->release = false;
    }
    // Finished execution is still owned/joinable until reuse or explicit join.
    if (!renderState(app).exportThreadOwned || !startExport(app, (files.dir / "second.jpg").string())) return false;
    {
      std::unique_lock<std::mutex> lock(probe->mutex);
      if (!probe->cv.wait_for(lock, std::chrono::seconds(2), [&] { return probe->calls == 2; })) return false;
    }
    if (app.renderer.canEditParameters()) return false;
    {
      std::lock_guard<std::mutex> lock(probe->mutex);
      probe->release = true;
    }
    probe->cv.notify_all();
  }
  app.renderer.joinExport();
  app.renderer.joinExport();
  const auto state = renderState(app);
  if (!app.renderer.canEditParameters() || state.exportBusy || state.exportThreadOwned ||
      state.previewPending != !stopDuring || state.quietPending != !stopDuring) return false;
  PersistSidecar sidecar;
  if (!loadSidecarFile(exportSidecarPath(files.output()), sidecar) || sidecar.sourcePath != app.path ||
      sidecar.chain.nodes.size() != 1 || sidecar.chain.nodes[0].id != "export-probe") return false;
  app.renderer.shutdown();
  return probe->calls == (stopDuring ? 1 : 2) && probe->width == 1 && probe->height == 1 &&
         std::all_of(probe->history.begin(), probe->history.end(), [](const auto &call) {
           return call.width == 2 && call.height == 1 && !call.interactive;
         });
}

static void graphPreviewSource(App &app) {
  app.preview.w = app.preview.h = 1;
  app.preview.px = {0.125f, 0.125f, 0.125f, 0.75f};
  app.full = app.preview;
  app.display = app.preview;
  app.display.px = {0.01f, 0.01f, 0.01f, 0.75f};  // Stale last-good image.
}

struct PreviewWorkerGuard {
  App &app;
  ~PreviewWorkerGuard() {
    app.renderer.shutdown();
  }
};

// With no worker running, each cancellation and processor request advances
// the runtime cancellation epoch once. This checks sequencing independently
// of the resulting pixels:
// an inner wait/request cannot hide behind queue coalescing.
static bool finishGraphPreview(App &app, int before, float expected, bool quiet = false,
                               int width = 1, int height = 1) {
  if (renderState(app).epoch != before + 2 || !renderState(app).previewPending ||
      renderState(app).quietPending != quiet || renderState(app).mutationDepth != 0) return false;
  const std::string status = app.getStatus();
  PreviewWorkerGuard guard{app};
  app.renderer.start();
  if (!previewIdle(app)) return false;
  std::lock_guard<std::mutex> lock(app.displayMutex);
  if (app.display.w != width || app.display.h != height ||
      app.display.px.size() != (size_t)width * height * 4 ||
      !app.displayDirty || app.displayGen <= before) return false;
  for (size_t i = 0; i < app.display.px.size(); i += 4) {
    if (std::fabs(app.display.px[i + 3] - 0.75f) > 2e-6f) return false;
    for (int c = 0; c < 3; ++c)
      if (std::fabs(app.display.px[i + c] - expected) > 2e-6f) return false;
  }
  std::vector<unsigned char> rgba;
  toDisplayRGBA8(app.display, app.outputEncoding, rgba);
  const std::string normalStatus = std::to_string(width) + "×" + std::to_string(height) + " preview";
  return app.displayRGBA == rgba && app.getStatus() == (quiet ? status : normalStatus);
}

enum class GraphEditCase { AddExposure, AddCst, Remove, Reorder, Disable, Enable,
                           Append, AppendMissing, Replace, ReplaceMissing, ReplaceEmpty, Clear, RemoveLast };

static bool testGraphEditTransaction(GraphEditCase edit) {
  App app;
  graphPreviewSource(app);
  PersistNode exposure;
  exposure.id = "exposure";
  exposure.backend = "native";
  exposure.identifier = NativeExposureProcessor::kIdentifier;
  exposure.paramsJson["exposure"] = "1";
  exposure.enabled = edit != GraphEditCase::Enable;
  PersistNode cst;
  cst.id = "cst";
  cst.backend = "native";
  cst.identifier = NativeCstProcessor::kIdentifier;
  cst.paramsJson["output_gamma"] = "\"srgb\"";
  PersistChain initial;
  initial.nodes = {exposure, cst};
  initial.selectedNodeId = "cst";
  if (edit != GraphEditCase::ReplaceEmpty) applyChain(app, initial);
  const int before = renderState(app).epoch;
  float expected = (float)encodeTransfer(0.25, TransferFunction::SRGB);
  switch (edit) {
    case GraphEditCase::AddExposure:
      if (!addNativeExposureNode(app) || app.nodes.size() != 3 || app.selectedNode != 2) return false;
      break;
    case GraphEditCase::AddCst:
      if (!addNativeCstNode(app) || app.nodes.size() != 3) return false;
      break;
    case GraphEditCase::Remove:
      destroyNode(app, 1);
      if (app.nodes.size() != 1 || app.nodes[0].id != "exposure") return false;
      expected = 0.25f;
      break;
    case GraphEditCase::Reorder:
      moveNode(app, 1, 0);
      if (app.nodes[0].id != "cst" || app.nodes[1].id != "exposure" || app.selectedNode != 0) return false;
      expected = 2.0f * (float)encodeTransfer(0.125, TransferFunction::SRGB);
      break;
    case GraphEditCase::Disable:
      setNodeEnabled(app, 0, false);
      if (app.nodes[0].enabled) return false;
      expected = (float)encodeTransfer(0.125, TransferFunction::SRGB);
      break;
    case GraphEditCase::Enable:
      setNodeEnabled(app, 0, true);
      if (!app.nodes[0].enabled) return false;
      break;
    case GraphEditCase::Append:
    case GraphEditCase::AppendMissing: {
      PersistNode pasted = exposure;
      pasted.paramsJson["exposure"] = "3";
      pasted.paramsJson["unknown"] = "{\"curve\":[0,1]}";
      pasted.groupOpen["group"] = true;
      if (edit == GraphEditCase::AppendMissing) {
        pasted.backend = "dctl";
        pasted.identifier = "missing.dctl";
        pasted.enabled = false;
      }
      if (!appendPersistedNode(app, pasted, 0) || app.nodes.size() != 3 ||
          app.selectedNode != 1 || app.nodes[1].id == exposure.id || app.nodes[2].id != "cst" ||
          captureChain(app).nodes[1].paramsJson.at("unknown") != pasted.paramsJson.at("unknown") ||
          !app.nodes[1].groupOpen.at("group")) return false;
      if (edit == GraphEditCase::AppendMissing) {
        if (app.nodes[1].processor || app.nodes[1].enabled) return false;
      } else {
        if (!app.nodes[1].processor) return false;
        expected = (float)encodeTransfer(2.0, TransferFunction::SRGB);
      }
      break;
    }
    case GraphEditCase::Replace:
    case GraphEditCase::ReplaceMissing:
    case GraphEditCase::ReplaceEmpty: {
      PersistChain replacement;
      for (int i = 0; i < 10; ++i) {
        PersistNode node = exposure;
        node.id = "restored-" + std::to_string(i);
        node.paramsJson["exposure"] = "0.25";
        if (edit == GraphEditCase::ReplaceMissing && i == 5) {
          node.backend = "ofx";
          node.identifier = "org.rawnode.unavailable";
          node.paramsJson["future"] = "[1,2,3]";
        }
        replacement.nodes.push_back(node);
      }
      replacement.selectedNodeId = "restored-5";
      applyChain(app, replacement);
      if (app.nodes.size() != 10 || app.selectedNode != 5) return false;
      for (int i = 0; i < 10; ++i)
        if (app.nodes[i].id != replacement.nodes[i].id) return false;
      if (edit == GraphEditCase::ReplaceMissing &&
          (app.nodes[5].processor || captureChain(app).nodes[5].paramsJson.at("future") != "[1,2,3]"))
        return false;
      expected = 0.125f * std::exp2(edit == GraphEditCase::ReplaceMissing ? 2.25f : 2.5f);
      break;
    }
    case GraphEditCase::RemoveLast:
      destroyNode(app, 1);
      // The last removal must show the unprocessed source without processor work.
      [[fallthrough]];
    case GraphEditCase::Clear: {
      // Cancel setup work first: this edit must refresh the source even when
      // no pending/active work was interrupted by its own transaction.
      app.renderer.start();
      if (!previewIdle(app)) return false;
      if (renderState(app).previewPending || renderState(app).previewBusy || renderState(app).displayPending) return false;
      app.displayDirty = false;
      const int lastBefore = renderState(app).epoch;
      if (edit == GraphEditCase::RemoveLast) destroyNode(app, 0);
      else clearNodes(app);
      std::vector<unsigned char> sourceRGBA;
      toDisplayRGBA8(app.preview, app.inputEncoding, sourceRGBA);
      return app.nodes.empty() && app.selectedNode == -1 && !renderState(app).previewPending &&
             renderState(app).mutationDepth == 0 && renderState(app).epoch == lastBefore + 1 &&
             app.displayDirty && app.display.px == app.preview.px && app.displayRGBA == sourceRGBA;
    }
  }
  return finishGraphPreview(app, before, expected);
}

static bool testIdlePreviewRebuild() {
  App app;
  graphPreviewSource(app);
  app.full.w = 2560;
  app.full.h = 2;
  app.full.px.resize((size_t)app.full.w * app.full.h * 4);
  for (size_t i = 0; i < app.full.px.size(); i += 4) {
    for (int c = 0; c < 3; ++c) app.full.px[i + c] = 0.125f;
    app.full.px[i + 3] = 0.75f;
  }
  app.preview = app.full;
  app.previewRes = 3;
  Node node;
  node.processor = std::make_unique<NativeExposureProcessor>();
  node.processor->setParameterValue("exposure", 1.0);
  app.nodes.push_back(std::move(node));
  app.previewRes = 0;  // Production 720p selection: cap the long edge at 1280.
  const int before = renderState(app).epoch;
  rebuildPreview(app);
  if (app.full.w != 2560 || app.full.h != 2 || app.preview.w != 1280 || app.preview.h != 1)
    return false;
  return finishGraphPreview(app, before, 0.25f, false, 1280, 1);
}

static bool testGraphEditNoops() {
  App empty;
  graphPreviewSource(empty);
  const int beforeEmpty = renderState(empty).epoch;
  clearNodes(empty);
  applyChain(empty, {});
  if (renderState(empty).epoch != beforeEmpty || renderState(empty).previewPending || empty.displayDirty) return false;
  App app;
  graphPreviewSource(app);
  if (!addNativeExposureNode(app)) return false;
  const int before = renderState(app).epoch;
  moveNode(app, 0, 0);
  moveNode(app, 0, 3);
  destroyNode(app, -1);
  setNodeEnabled(app, 0, true);
  setNodeEnabled(app, 2, false);
  if (addNode(app, -1)) return false;
  return renderState(app).epoch == before && renderState(app).previewPending && !renderState(app).quietPending &&
         app.nodes.size() == 1 && app.nodes[0].enabled;
}

// A real OFX createInstance failure through the production adapter. The fixture
// owns its descriptor and does not add to the known plugin-loading leak.
struct FailingOfxFixture {
  OfxPlugin plugin{};
  int index = (int)gPlugins.size();
  FailingOfxFixture() {
    plugin.pluginIdentifier = "org.rawnode.selftest.failed-create";
    plugin.mainEntry = [](const char *action, const void *, OfxPropertySetHandle, OfxPropertySetHandle) {
      return std::strcmp(action, kOfxActionCreateInstance) == 0 ? kOfxStatFailed : kOfxStatReplyDefault;
    };
    PluginEntry entry{};
    entry.plugin = &plugin;
    entry.descriptor = std::make_unique<Effect>();
    gPlugins.push_back(std::move(entry));
  }
  ~FailingOfxFixture() { gPlugins.pop_back(); }
};

static bool testPartialMutationUnwind() {
  FailingOfxFixture fixture;
  fixture.plugin.mainEntry = [](const char *action, const void *, OfxPropertySetHandle, OfxPropertySetHandle) {
    if (std::strcmp(action, kOfxActionCreateInstance) == 0)
      throw std::runtime_error("Self-test restore exception");
    return kOfxStatReplyDefault;
  };
  App app;
  graphPreviewSource(app);
  if (!addNativeExposureNode(app)) return false;
  PersistNode exposure;
  exposure.id = "restored-first";
  exposure.backend = "native";
  exposure.identifier = NativeExposureProcessor::kIdentifier;
  exposure.paramsJson["exposure"] = "1";
  PersistNode failing;
  failing.backend = "ofx";
  failing.identifier = fixture.plugin.pluginIdentifier;
  PersistChain replacement;
  replacement.nodes = {exposure, failing};
  const int before = renderState(app).epoch;
  try {
    applyChain(app, replacement);
    return false;
  } catch (const std::runtime_error &) {
    if (app.nodes.size() != 1 || app.nodes[0].id != exposure.id) return false;
  }
  return finishGraphPreview(app, before, 0.25f);
}

// Exercise the actual OFX suite callback through OfxProcessor/renderEffect.
// The descriptor carries the fixture pointer; there is no global test token.
struct AbortOfxFixture {
  OfxPlugin plugin{};
  bool threaded = false, returnedEarly = false;
  std::atomic<unsigned> destroySlices{0};
  int index = (int)gPlugins.size();
  std::mutex mutex;
  std::condition_variable cv;
  int calls = 0, released = 0;
  bool releaseAll = false;
  std::vector<int> aborted;
  std::vector<bool> interactive;
  Effect *instance = nullptr;
  const OfxImageEffectSuiteV1 *suite = static_cast<const OfxImageEffectSuiteV1 *>(
      gOfxHost.fetchSuite(gOfxHost.host, kOfxImageEffectSuite, 1));
  AbortOfxFixture() {
    plugin.pluginIdentifier = "org.rawnode.selftest.abort";
    plugin.mainEntry = [](const char *action, const void *handle, OfxPropertySetHandle, OfxPropertySetHandle) {
      if (std::strcmp(action, kOfxImageEffectActionRender) != 0 &&
          std::strcmp(action, kOfxActionDestroyInstance) != 0) return kOfxStatReplyDefault;
      auto *effect = static_cast<Effect *>(const_cast<void *>(handle));
      auto &probe = *static_cast<AbortOfxFixture *>(effect->props.m.at("selftest-probe")[0].p);
      const auto *threads = static_cast<const OfxMultiThreadSuiteV1 *>(
          gOfxHost.fetchSuite(gOfxHost.host, kOfxMultiThreadSuite, 1));
      if (std::strcmp(action, kOfxActionDestroyInstance) == 0) {
        if (probe.threaded)
          return threads->multiThread([](unsigned, unsigned, void *arg) {
            ++static_cast<AbortOfxFixture *>(arg)->destroySlices;
          }, 2, &probe);
        return kOfxStatReplyDefault;
      }
      if (probe.threaded) {
        struct Slices {
          AbortOfxFixture &probe;
          Effect *effect;
          std::thread::id caller;
          bool helperEntered = false, helperFinished = false;
        } slices{probe, effect, std::this_thread::get_id()};
        const auto status = threads->multiThread([](unsigned, unsigned, void *arg) {
          auto &s = *static_cast<Slices *>(arg);
          std::unique_lock<std::mutex> lock(s.probe.mutex);
          if (std::this_thread::get_id() == s.caller || s.helperEntered) {
            s.probe.cv.wait(lock, [&] { return s.helperEntered; });
          } else {
            s.helperEntered = true;
            s.probe.waitAndAbort(s.effect, lock);
            s.helperFinished = true;
            s.probe.cv.notify_all();
          }
        }, 2, &slices);
        std::unique_lock<std::mutex> lock(probe.mutex);
        probe.returnedEarly |= status != kOfxStatOK || !slices.helperFinished;
        // Keep a failing mutant's borrowed stack/token alive during cleanup.
        probe.cv.wait(lock, [&] { return slices.helperFinished; });
      } else {
        std::unique_lock<std::mutex> lock(probe.mutex);
        probe.waitAndAbort(effect, lock);
      }
      std::copy(effect->src, effect->src + (size_t)effect->w * effect->h * 4, effect->dst);
      return kOfxStatOK;
    };
    PluginEntry entry{};
    entry.plugin = &plugin;
    entry.descriptor = std::make_unique<Effect>();
    propSetPointer(H(&entry.descriptor->props), "selftest-probe", 0, this);
    gPlugins.push_back(std::move(entry));
  }
  ~AbortOfxFixture() { gPlugins.pop_back(); }
  void waitAndAbort(Effect *effect, std::unique_lock<std::mutex> &lock) {
    instance = effect;
    ++calls;
    interactive.push_back(effect->cancellation.interactive());
    cv.notify_all();
    cv.wait(lock, [&] { return releaseAll || calls <= released; });
    aborted.push_back(suite->abort(reinterpret_cast<OfxImageEffectHandle>(effect)));
  }
  bool wait(int count) {
    std::unique_lock<std::mutex> lock(mutex);
    return cv.wait_for(lock, std::chrono::seconds(2), [&] { return calls == count; });
  }
  void release(int count) {
    std::lock_guard<std::mutex> lock(mutex);
    released = count;
    cv.notify_all();
  }
};

static bool testOfxRuntimeCancellation(bool threaded = false) {
  ExportTestFiles files;
  AbortOfxFixture fixture;
  fixture.threaded = threaded;
  App app;
  app.preview = {{0.125f, 0.125f, 0.125f, 1.0f}, 1, 1};
  app.full = app.preview;
  if (!addNode(app, fixture.index)) return false;
  struct Guard {
    App &app;
    AbortOfxFixture &fixture;
    ~Guard() {
      {
        std::lock_guard<std::mutex> lock(fixture.mutex);
        fixture.releaseAll = true;
      }
      fixture.cv.notify_all();
      app.renderer.shutdown();
    }
  } guard{app, fixture};
  app.renderer.start();
  if (!fixture.wait(1)) return false;
  app.renderer.requestPreview();
  fixture.release(1);
  if (!fixture.wait(2)) return false;
  {
    std::lock_guard<std::mutex> lock(fixture.mutex);
    if (fixture.aborted != std::vector<int>{1} || app.displayDirty) return false;
  }
  fixture.release(2);
  if (!previewIdle(app)) return false;
  if (!startExport(app, files.output()) || !fixture.wait(3)) return false;
  app.renderer.requestPreview();        // Export's empty token must not cancel.
  {
    std::lock_guard<std::mutex> lock(fixture.mutex);
    fixture.releaseAll = true;
  }
  fixture.cv.notify_all();
  app.renderer.joinExport();
  if (!previewIdle(app)) return false;
  app.renderer.shutdown();
  const bool ok = !fixture.returnedEarly && fixture.aborted == std::vector<int>({1, 0, 0, 0}) &&
         fixture.interactive == std::vector<bool>({true, true, false, true}) &&
         !fixture.instance->cancellation.interactive() &&
         !fixture.suite->abort(reinterpret_cast<OfxImageEffectHandle>(fixture.instance)) &&
         fs::is_regular_file(files.output());
  // DestroyInstance may still use the suite after renderer shutdown.
  clearNodes(app);
  return ok && (!threaded || fixture.destroySlices == 2);
}

enum class FailedDocumentEdit { Ofx, Ctl, RawMissing, RawReplacedByRaster, GradeRaw };

static bool testFailedDocumentEdit(FailedDocumentEdit edit, bool pending = true,
                                   bool recolorOnly = false) {
  ExportTestFiles files;
  FailingOfxFixture ofx;
  App app;
  graphPreviewSource(app);
  Node node;
  node.id = "exposure";
  node.processor = std::make_unique<NativeExposureProcessor>();
  node.processor->setParameterValue("exposure", 1.0);
  app.nodes.push_back(std::move(node));
  if (pending && !recolorOnly) app.renderer.requestPreview();
  if (!pending || recolorOnly) {
    // Idle failure starts with a valid cached preview and must retain it.
    app.display = app.preview;
    for (int i = 0; i < 3; ++i) app.display.px[i] = 0.25f;
    toDisplayRGBA8(app.display, app.outputEncoding, app.displayRGBA);
  }
  if (recolorOnly) {
    app.renderer.requestDisplayRefresh();
    if (renderState(app).previewPending || renderState(app).previewBusy || !renderState(app).displayPending) return false;
  }
  app.path = (files.dir / "missing.nef").string();
  app.inputIsRaw = true;
  app.inputEncoding = {RgbGamut::Rec709, TransferFunction::Linear};
  app.rawWorkingEncoding = {RgbGamut::Rec2020, TransferFunction::Linear};
  const Image oldFull = app.full, oldPreview = app.preview;
  const Image oldDisplay = app.display;
  const auto oldRGBA = app.displayRGBA;
  const ColorEncoding oldInput = app.inputEncoding, oldDefault = app.rawWorkingEncoding;
  const ColorEncoding oldOutput = app.outputEncoding;
  const std::string oldId = app.nodes[0].id;
  const ColorEncoding requested{RgbGamut::ACES_AP1, TransferFunction::Linear};
  if (edit == FailedDocumentEdit::RawReplacedByRaster) {
    app.path = files.output();
    if (!writeImage(app.full, app.path, app.outputEncoding)) return false;
  }
  const int before = renderState(app).epoch;
  std::string error;
  switch (edit) {
    case FailedDocumentEdit::Ofx:
      if (addNode(app, ofx.index)) return false;
      error = "Plugin failed to create an instance";
      break;
    case FailedDocumentEdit::Ctl:
      if (addCtlNode(app, (files.dir / "missing.ctl").string())) return false;
      error = app.getStatus();
      if (error.find("Could not load CTL:") != 0) return false;
      break;
    case FailedDocumentEdit::GradeRaw: {
      PersistGradeColor color;
      color.rawColorSpace = rgbGamutId(requested.gamut);
      color.rawGamma = transferFunctionId(requested.gamma);
      color.outputColorSpace = "display-p3";
      color.outputGamma = "srgb";
      if (applyGradeColor(app, color)) return false;
      error = "Could not reload RAW in " + colorEncodingName(requested);
      break;
    }
    case FailedDocumentEdit::RawMissing:
    case FailedDocumentEdit::RawReplacedByRaster:
      setRawWorkingEncoding(app, requested.gamut, requested.gamma);
      error = "Could not reload RAW in " + colorEncodingName(requested);
      break;
  }
  if (app.getStatus() != error || app.nodes.size() != 1 || app.nodes[0].id != oldId ||
      app.full.w != oldFull.w || app.full.h != oldFull.h || app.full.px != oldFull.px ||
      app.preview.w != oldPreview.w || app.preview.h != oldPreview.h || app.preview.px != oldPreview.px ||
      app.inputEncoding != oldInput || !app.inputIsRaw || app.rawWorkingEncoding != oldDefault ||
      app.outputEncoding != oldOutput || fs::exists(inputSidecarPath(app.path))) return false;
  if (renderState(app).displayPending) return false;
  if (!pending && !recolorOnly)
    return renderState(app).epoch == before + 1 && !renderState(app).previewPending &&
           !renderState(app).quietPending && renderState(app).mutationDepth == 0 &&
           app.display.px == oldDisplay.px && app.displayRGBA == oldRGBA;
  return finishGraphPreview(app, before, 0.25f, true);
}

static bool testUnavailableProcessorRestore(bool append, bool ctl) {
  ExportTestFiles files;
  FailingOfxFixture ofx;
  App app;
  graphPreviewSource(app);
  if (!addNativeExposureNode(app) || !addNativeCstNode(app) ||
      !app.nodes[0].processor->setParameterValue("exposure", 1.0)) return false;
  PersistNode missing;
  missing.id = "failed-processor";
  missing.backend = ctl ? "ctl" : "ofx";
  missing.identifier = ctl ? (files.dir / "missing.ctl").string() : ofx.plugin.pluginIdentifier;
  missing.label = "Unavailable";
  missing.enabled = false;
  missing.paramsJson["opaque"] = "{\"future\":true}";
  missing.groupOpen["group"] = true;
  PersistChain replacement = captureChain(app);
  replacement.nodes.insert(replacement.nodes.begin() + 1, missing);
  replacement.selectedNodeId = missing.id;
  const int before = renderState(app).epoch;
  if (append) {
    if (!appendPersistedNode(app, missing, 0)) return false;
  } else {
    applyChain(app, replacement);
  }
  if (app.nodes.size() != 3 || app.selectedNode != 1 || app.nodes[1].processor ||
      app.nodes[1].enabled || (app.nodes[1].id == missing.id) == append ||
      app.nodes[1].storedBackend != missing.backend ||
      captureChain(app).nodes[1].paramsJson.at("opaque") != missing.paramsJson.at("opaque") ||
      !app.nodes[1].groupOpen.at("group")) return false;
  return finishGraphPreview(app, before, 0.25f);
}

enum class ActiveGraphEdit { Add, Remove, Reorder, Disable, FailCtl, AddOfx };

static bool testGraphEditWaits(bool exporting, ActiveGraphEdit edit, int pluginIndex = -1) {
  ExportTestFiles files;
  App app;
  graphPreviewSource(app);
  app.full.w = 2;
  app.full.px.insert(app.full.px.end(), app.preview.px.begin(), app.preview.px.end());
  auto processor = std::make_unique<SchedulingSelfTestProcessor>();
  auto *probe = processor.get();
  probe->preserveInput = true;
  Node barrier;
  barrier.id = "barrier";
  barrier.processor = std::move(processor);
  app.nodes.push_back(std::move(barrier));
  if (!addNativeExposureNode(app) ||
      !app.nodes[1].processor->setParameterValue("exposure", 1.0)) return false;
  RuntimeEvents events(app);
  SchedulingWorkerGuard guard{app, *probe};
  if (exporting && !startExport(app, files.output())) return false;
  app.renderer.start();
  {
    std::unique_lock<std::mutex> lock(probe->mutex);
    if (!probe->cv.wait_for(lock, std::chrono::seconds(2), [&] { return probe->calls == 1; })) return false;
  }
  bool waited = false;
  std::thread observer([&] {
    const bool observed = events.wait(RuntimeEvent::MutationWaiting);
    const auto state = renderState(app);
    waited = observed && state.mutationDepth == 1 &&
             (exporting ? state.exportBusy && !state.previewBusy : state.previewBusy && !state.exportBusy) &&
             app.nodes.size() == 2 && app.nodes[1].enabled;
    {
      std::lock_guard<std::mutex> lock(probe->mutex);
      probe->release = true;
    }
    probe->cv.notify_all();
  });
  bool operationOk = true;
  switch (edit) {
    case ActiveGraphEdit::Add: operationOk = addNativeExposureNode(app); break;
    case ActiveGraphEdit::Remove: destroyNode(app, 1); break;
    case ActiveGraphEdit::Reorder: moveNode(app, 1, 0); break;
    case ActiveGraphEdit::Disable: setNodeEnabled(app, 1, false); break;
    case ActiveGraphEdit::FailCtl:
      operationOk = !addCtlNode(app, (files.dir / "missing.ctl").string());
      break;
    case ActiveGraphEdit::AddOfx: operationOk = addNode(app, pluginIndex); break;
  }
  observer.join();
  app.renderer.joinExport();
  if (!waited || !operationOk || !previewIdle(app) || renderState(app).mutationDepth != 0) return false;
  {
    std::lock_guard<std::mutex> lock(probe->mutex);
    if (probe->calls != 2) return false;  // Original owner, then one final preview.
  }
  if (exporting) {
    PersistSidecar exported;
    if (!loadSidecarFile(exportSidecarPath(files.output()), exported) ||
        exported.chain.nodes.size() != 2 || !exported.chain.nodes[1].enabled) return false;
  }
  const float expected = edit == ActiveGraphEdit::Remove || edit == ActiveGraphEdit::Disable ? 0.125f : 0.25f;
  std::lock_guard<std::mutex> lock(app.displayMutex);
  return app.display.px.size() == 4 && app.display.px[0] == expected && app.displayDirty &&
         (edit == ActiveGraphEdit::FailCtl ? app.getStatus().find("Could not load CTL:") == 0
                                         : app.getStatus() == "1×1 preview");
}

static int fail(const char *msg) {
  fprintf(stderr, "selftest FAILED: %s\n", msg);
  return 1;
}

static bool writeTinyTiff(const fs::path &p, bool halfFloat) {
  TIFF *tif = TIFFOpen(p.c_str(), "w");
  if (!tif) return false;
  TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, 2);
  TIFFSetField(tif, TIFFTAG_IMAGELENGTH, 2);
  TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 3);
  TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 16);
  TIFFSetField(tif, TIFFTAG_SAMPLEFORMAT, halfFloat ? SAMPLEFORMAT_IEEEFP : SAMPLEFORMAT_UINT);
  TIFFSetField(tif, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
  TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
  TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
  TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, 2);
  uint16_t row0[6], row1[6];
  if (halfFloat) {
    // half 1.0 = 0x3c00
    uint16_t one = 0x3c00, z = 0;
    row0[0] = one; row0[1] = z; row0[2] = z; row0[3] = z; row0[4] = one; row0[5] = z;
    row1[0] = z; row1[1] = z; row1[2] = one; row1[3] = one; row1[4] = one; row1[5] = one;
  } else {
    row0[0] = 65535; row0[1] = 0; row0[2] = 0; row0[3] = 0; row0[4] = 65535; row0[5] = 0;
    row1[0] = 0; row1[1] = 0; row1[2] = 65535; row1[3] = 65535; row1[4] = 65535; row1[5] = 65535;
  }
  const bool ok = TIFFWriteScanline(tif, row0, 0, 0) >= 0 && TIFFWriteScanline(tif, row1, 1, 0) >= 0;
  TIFFClose(tif);
  return ok;
}

static bool writeGray16Tiff(const fs::path &p, uint16_t value) {
  TIFF *tif = TIFFOpen(p.c_str(), "w");
  if (!tif) return false;
  TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, 1);
  TIFFSetField(tif, TIFFTAG_IMAGELENGTH, 1);
  TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 3);
  TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 16);
  TIFFSetField(tif, TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_UINT);
  TIFFSetField(tif, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
  TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
  TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
  uint16_t row[3] = {value, value, value};
  const bool ok = TIFFWriteScanline(tif, row, 0, 0) >= 0;
  TIFFClose(tif);
  return ok;
}

static bool makeTestRgbIcc(const cmsCIExyY &white, const cmsCIExyYTRIPLE &primaries,
                           double gamma, std::vector<uint8_t> &icc) {
  icc.clear();
  cmsToneCurve *curve = cmsBuildGamma(nullptr, gamma);
  if (!curve) return false;
  cmsToneCurve *curves[3] = {curve, curve, curve};
  cmsHPROFILE profile = cmsCreateRGBProfile(&white, &primaries, curves);
  cmsFreeToneCurve(curve);
  if (!profile) return false;

  cmsUInt32Number size = 0;
  bool ok = cmsSaveProfileToMem(profile, nullptr, &size) && size > 0;
  if (ok) {
    icc.resize(size);
    ok = cmsSaveProfileToMem(profile, icc.data(), &size) != 0;
    if (ok) icc.resize(size);
  }
  cmsCloseProfile(profile);
  if (!ok) icc.clear();
  return ok;
}

static bool writeRgba16TiffWithIcc(const fs::path &p, const uint16_t rgba[4],
                                   const std::vector<uint8_t> &icc) {
  TIFF *tif = TIFFOpen(p.c_str(), "w");
  if (!tif) return false;
  TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, 1);
  TIFFSetField(tif, TIFFTAG_IMAGELENGTH, 1);
  TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 4);
  TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 16);
  TIFFSetField(tif, TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_UINT);
  TIFFSetField(tif, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
  TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
  TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
  uint16_t extraSample = EXTRASAMPLE_UNASSALPHA;
  TIFFSetField(tif, TIFFTAG_EXTRASAMPLES, 1, &extraSample);
  if (!icc.empty())
    TIFFSetField(tif, TIFFTAG_ICCPROFILE, (uint32_t)icc.size(), (void *)icc.data());
  const bool ok = TIFFWriteScanline(tif, (void *)rgba, 0, 0) >= 0;
  TIFFClose(tif);
  return ok;
}

static bool writeGray16TiffWithIcc(const fs::path &p, uint16_t value,
                                   const std::vector<uint8_t> &icc) {
  const uint16_t rgba[4] = {value, value, value, 65535};
  return writeRgba16TiffWithIcc(p, rgba, icc);
}

// Renders a gray ramp through every installed filter plugin and writes export formats.
int runSelfTests() {
#if !defined(NDEBUG) && !defined(_WIN32)
  if (!testControlThreadAssertions()) return fail("renderer control-thread assertions");
  printf("ok  Debug control-thread contract (8 expected child assertions)\n");
#endif
  if (!testOfxThreadPool()) return fail("OFX host multithread lifetime");
  for (bool started : {false, true})
    for (bool pending : {false, true})
      if (!testRuntimeIdleLifecycle(started, pending)) return fail("runtime idle/pending start/shutdown");
  for (bool destroy : {false, true}) {
    if (!testRuntimeActiveShutdown(false, destroy)) return fail("runtime active preview shutdown/destruction");
    for (auto failure : {ExportFailure::None, ExportFailure::Result, ExportFailure::RenderException})
      if (!testRuntimeActiveShutdown(true, destroy, failure)) return fail("runtime active export shutdown/destruction");
  }
  if (!testNormalPreviewDominatesQuiet(false) || !testNormalPreviewDominatesQuiet(true))
    return fail("normal demand / final mutation decision / read-only runtime observation");
  if (!testRuntimeCancellation()) return fail("per-runtime token supersession and evaluator cancellation");
  if (!testOfxRuntimeCancellation()) return fail("runtime cancellation reaches OFX abort; export remains uncancellable");
  if (std::thread::hardware_concurrency() > 1 && !testOfxRuntimeCancellation(true))
    return fail("worker-slice OFX abort lifetime and DestroyInstance threading");
  printf("ok  Owned runtime lifecycle (12 cases), demand merging, observation and cancellation\n");

  for (bool half : {false, true}) {
    const fs::path p = fs::temp_directory_path() / (half ? "ofxrawhost-selftest-half.tif" : "ofxrawhost-selftest.tif");
    if (!writeTinyTiff(p, half)) return fail(half ? "tiff write half" : "tiff write");
    Image img;
    ColorEncoding encoding;
    bool decodedRaw = true;
    if (!loadImage(p.string(), img, encoding, decodedRaw) || decodedRaw || img.w != 2 || img.h != 2)
      return fail(half ? "tiff load half" : "tiff load");
    if (img.px[(size_t)1 * 2 * 4 + 0] < 0.9f) return fail(half ? "tiff pixels half" : "tiff pixels");
    // Processing buffers are linear: untagged float TIFF → Linear Rec.2020;
    // untagged 16-bit integer TIFF → sRGB decoded to Linear Rec.709.
    const ColorEncoding expected = half
        ? ColorEncoding{RgbGamut::Rec2020, TransferFunction::Linear}
        : ColorEncoding{RgbGamut::Rec709, TransferFunction::Linear};
    if (encoding != expected) return fail(half ? "tiff half colorspace" : "tiff uint colorspace");
    fs::remove(p);
  }

  {
    // Integer raster samples must actually be decoded to the linear encoding
    // reported by the canonical loader.
    const fs::path p = fs::temp_directory_path() / "rawnode-selftest-gray16.tif";
    if (!writeGray16Tiff(p, 32768)) return fail("gray16 tiff write");
    Image img;
    ColorEncoding encoding;
    bool decodedRaw = true;
    if (!loadImage(p.string(), img, encoding, decodedRaw) || decodedRaw ||
        encoding != ColorEncoding{RgbGamut::Rec709, TransferFunction::Linear})
      return fail("gray16 canonical encoding");
    const double encoded = 32768.0 / 65535.0;
    const double want = decodeTransfer(encoded, TransferFunction::SRGB);
    if (img.px.size() < 4 || std::fabs(img.px[0] - want) > 2e-5 ||
        std::fabs(img.px[1] - want) > 2e-5 || std::fabs(img.px[2] - want) > 2e-5)
      return fail("gray16 sRGB linearisation");
    fs::remove(p);
  }

  {
    // Third-party RGB ICC profiles are converted through lcms into the
    // canonical raster working encoding rather than being relabelled as the
    // nearest RawNode gamut.
    struct ProfileCase {
      const char *name;
      cmsCIExyY white;
      cmsCIExyYTRIPLE primaries;
      double gamma;
    };
    const ProfileCase cases[] = {
        {
            "adobe-rgb",
            {0.3127, 0.3290, 1.0},
            {{0.6400, 0.3300, 1.0}, {0.2100, 0.7100, 1.0}, {0.1500, 0.0600, 1.0}},
            2.2,
        },
        {
            "prophoto",
            {0.3457, 0.3585, 1.0},
            {{0.7347, 0.2653, 1.0}, {0.1596, 0.8404, 1.0}, {0.0366, 0.0001, 1.0}},
            1.8,
        },
    };

    constexpr uint16_t code = 32768;
    const double encoded = (double)code / 65535.0;
    for (const ProfileCase &profileCase : cases) {
      std::vector<uint8_t> icc;
      if (!makeTestRgbIcc(profileCase.white, profileCase.primaries, profileCase.gamma, icc))
        return fail("third-party ICC test profile");

      const fs::path p =
          fs::temp_directory_path() / (std::string("rawnode-selftest-") + profileCase.name + ".tif");
      if (!writeGray16TiffWithIcc(p, code, icc))
        return fail("third-party ICC TIFF write");

      Image img;
      ColorEncoding encoding;
      bool decodedRaw = true;
      if (!loadImage(p.string(), img, encoding, decodedRaw) || decodedRaw ||
          encoding != ColorEncoding{RgbGamut::Rec2020, TransferFunction::Linear})
        return fail("third-party ICC canonical encoding");

      const double expected = std::pow(encoded, profileCase.gamma);
      if (img.px.size() < 4 ||
          std::fabs(img.px[0] - expected) > 5e-4 ||
          std::fabs(img.px[1] - expected) > 5e-4 ||
          std::fabs(img.px[2] - expected) > 5e-4 ||
          img.px[3] != 1.0f)
        return fail("third-party ICC conversion");

      fs::remove(p);
    }

    // Neutral grey cannot prove that the gamut matrix ran. ProPhoto green is
    // deliberately outside Rec.2020 in this direction, so a correct D50 ->
    // D65/Rec.2020 transform produces negative red/blue components. Use an
    // unassociated alpha below 1 to verify cmsFLAGS_COPY_ALPHA at the same time.
    std::vector<uint8_t> proPhotoIcc;
    const cmsCIExyY proPhotoWhite = {0.3457, 0.3585, 1.0};
    const cmsCIExyYTRIPLE proPhotoPrimaries = {
        {0.7347, 0.2653, 1.0},
        {0.1596, 0.8404, 1.0},
        {0.0366, 0.0001, 1.0},
    };
    if (!makeTestRgbIcc(proPhotoWhite, proPhotoPrimaries, 1.8, proPhotoIcc))
      return fail("ProPhoto saturated ICC profile");

    constexpr uint16_t alphaCode = 19660;  // ~0.3
    const uint16_t greenRgba[4] = {0, 65535, 0, alphaCode};
    const fs::path greenPath =
        fs::temp_directory_path() / "rawnode-selftest-prophoto-green-alpha.tif";
    if (!writeRgba16TiffWithIcc(greenPath, greenRgba, proPhotoIcc))
      return fail("ProPhoto saturated TIFF write");

    Image green;
    ColorEncoding greenEncoding;
    bool greenDecodedRaw = true;
    if (!loadImage(greenPath.string(), green, greenEncoding, greenDecodedRaw) ||
        greenDecodedRaw ||
        greenEncoding != ColorEncoding{RgbGamut::Rec2020, TransferFunction::Linear} ||
        green.px.size() < 4)
      return fail("ProPhoto saturated ICC load");

    // Independently verified lcms/Bradford result for this test profile.
    if (std::fabs(green.px[0] - (-0.058f)) > 2e-3f ||
        std::fabs(green.px[1] - 1.081f) > 2e-3f ||
        std::fabs(green.px[2] - (-0.041f)) > 2e-3f ||
        std::fabs(green.px[3] - (float)alphaCode / 65535.0f) > 1e-6f)
      return fail("ProPhoto saturated gamut/alpha conversion");

    fs::remove(greenPath);
  }

  {
    // Minimal 1x1 RGB PNG, no iCCP (untagged LDR → sRGB).
    static const unsigned char kPng[] = {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00,
        0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x02, 0x00, 0x00, 0x00, 0x90, 0x77, 0x53, 0xde, 0x00, 0x00, 0x00,
        0x0c, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0xf8, 0xcf, 0xc0, 0x00, 0x00, 0x03, 0x01, 0x01, 0x00, 0xc9,
        0xfe, 0x92, 0xef, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};
    const fs::path p = fs::temp_directory_path() / "ofxrawhost-selftest-cs.png";
    FILE *f = fopen(p.c_str(), "wb");
    if (!f || fwrite(kPng, 1, sizeof kPng, f) != sizeof kPng) {
      if (f) fclose(f);
      return fail("png write");
    }
    fclose(f);
    Image img;
    ColorEncoding encoding;
    bool decodedRaw = true;
    if (!loadImage(p.string(), img, encoding, decodedRaw) || decodedRaw ||
        encoding != ColorEncoding{RgbGamut::Rec709, TransferFunction::Linear})
      return fail("png colorspace");
    fs::remove(p);
  }

  {
    // Workspace discovery and Open-dialog filtering share the same raster/RAW
    // extension definitions.
    for (const std::string &ext : rasterImageExtensions()) {
      if (!isSupportedImagePath(std::string("image") + ext) ||
          !isRasterImagePath(std::string("IMAGE") + ext))
        return fail("raster extension support");
    }
    for (const char *ext : {".nrw", ".erf", ".3fr", ".crw", ".iiq", ".mrw", ".x3f", ".srf", ".rwl"}) {
      if (!isSupportedImagePath(std::string("camera") + ext))
        return fail("expanded RAW extension support");
    }

    const auto filters = openImageDialogFilters();
    if (filters.size() != 2) return fail("image dialog filter shape");
    for (const std::string &ext : rasterImageExtensions())
      if (filters[1].find("*" + ext) == std::string::npos)
        return fail("raster dialog extension");
    for (const std::string &ext : rawImageExtensions())
      if (filters[1].find("*" + ext) == std::string::npos)
        return fail("RAW dialog extension");
  }

  {
    // RAW colour boundary: camera -> working-space matrix application happens
    // in float and must preserve values outside 0..1 rather than clipping them.
    const float camera[4] = {0.25f, 0.5f, 0.75f, 0.125f};
    const float matrix[3][4] = {
        {-1.0f, 0.0f, 0.0f, 0.5f},
        {0.0f, 0.0f, 2.5f, 0.0f},
        {0.0f, 1.0f, 0.0f, 1.0f},
    };
    float rgb[3] = {};
    applyCameraMatrix(camera, 4, matrix, rgb);
    if (std::fabs(rgb[0] + 0.1875f) > 1e-7f ||
        std::fabs(rgb[1] - 1.875f) > 1e-7f ||
        std::fabs(rgb[2] - 0.625f) > 1e-7f)
      return fail("RAW camera matrix float boundary");

    const float identityCamera[3][4] = {
        {1.0f, 0.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, 1.0f, 0.0f},
    };
    float working[3][4] = {};
    if (!makeCameraToWorkingMatrix(identityCamera, RgbGamut::Rec2020, working) ||
        std::fabs(working[0][0] - 0.627403896f) > 1e-6f ||
        std::fabs(working[1][1] - 0.919540395f) > 1e-6f ||
        std::fabs(working[2][2] - 0.895595253f) > 1e-6f)
      return fail("RAW Linear Rec.2020 matrix");

    if (!makeCameraToWorkingMatrix(identityCamera, RgbGamut::ACES_AP0, working) ||
        std::fabs(working[0][0] - 0.439632982f) > 1e-6f ||
        std::fabs(working[1][1] - 0.813439429f) > 1e-6f ||
        std::fabs(working[2][2] - 0.870912276f) > 1e-6f)
      return fail("RAW ACES2065-1 matrix");

    if (!makeCameraToWorkingMatrix(identityCamera, RgbGamut::ACES_AP1, working) ||
        std::fabs(working[0][0] - 0.613097402f) > 1e-6f ||
        std::fabs(working[1][1] - 0.916353879f) > 1e-6f ||
        std::fabs(working[2][2] - 0.869814634f) > 1e-6f)
      return fail("RAW ACES AP1 registry matrix");

    if (!makeCameraToWorkingMatrix(identityCamera, RgbGamut::DaVinciWideGamut, working) ||
        std::fabs(working[0][0] - 0.562767456f) > 1e-6f ||
        std::fabs(working[1][1] - 0.749577346f) > 1e-6f ||
        std::fabs(working[2][2] - 0.743332108f) > 1e-6f)
      return fail("RAW DWG registry matrix");

    if (!makeCameraToWorkingMatrix(identityCamera, RgbGamut::DisplayP3, working) ||
        std::fabs(working[0][0] - 0.822461969f) > 1e-6f ||
        std::fabs(working[1][1] - 0.966805801f) > 1e-6f ||
        std::fabs(working[2][2] - 0.910519929f) > 1e-6f)
      return fail("RAW Display P3 registry matrix");

    // A white-point adaptation must keep neutral white neutral.
    for (RgbGamut target : {RgbGamut::ACES_AP0, RgbGamut::ACES_AP1}) {
      double m[3][3] = {};
      if (!linearColorTransformMatrix(RgbGamut::Rec709, target, m))
        return fail("Bradford white transform setup");
      for (int row = 0; row < 3; ++row) {
        const double white = m[row][0] + m[row][1] + m[row][2];
        if (std::fabs(white - 1.0) > 2e-6)
          return fail("Bradford white neutrality");
      }
    }

    // AP0 must have a usable linear ICC interpretation for preview/output-tag
    // colour management, despite its imaginary primaries.
    std::vector<uint8_t> ap0Icc;
    if (!profileBytes(ColorEncoding{RgbGamut::ACES_AP0, TransferFunction::Linear}, ap0Icc) || ap0Icc.empty())
      return fail("ACES2065-1 ICC profile");
  }

  {
    // Native CST: gamut and transfer function are explicit, independent
    // controls. Verify colour maths, float range, alpha, and Sidecar V2.
    App cstApp;
    if (!addNativeCstNode(cstApp) || cstApp.nodes.size() != 1 || !cstApp.nodes[0].processor ||
        cstApp.nodes[0].processor->backend() != ProcessorBackend::Native ||
        cstApp.nodes[0].processor->identifier() != NativeCstProcessor::kIdentifier)
      return fail("native CST processor creation");

    const auto cstParams = cstApp.nodes[0].processor->parameters();
    if (cstParams.size() != 4 ||
        cstParams[0].id != "input_space" || cstParams[0].choices.size() != 6 ||
        cstParams[1].id != "input_gamma" || cstParams[1].choices.size() != 4 ||
        cstParams[2].id != "output_space" || cstParams[2].choices.size() != 6 ||
        cstParams[3].id != "output_gamma" || cstParams[3].choices.size() != 4)
      return fail("native CST parameters");

    Processor &cst = *cstApp.nodes[0].processor;
    if (!cst.setParameterValue("input_space", 0) ||
        !cst.setParameterValue("input_gamma", 0) ||
        !cst.setParameterValue("output_space", 1) ||
        !cst.setParameterValue("output_gamma", 0) ||
        cst.setParameterValue("output_space", 9) ||
        cst.setParameterValue("output_gamma", 9))
      return fail("native CST parameter validation");

    Image cstIn;
    cstIn.w = 2;
    cstIn.h = 1;
    cstIn.px = {
        1.0f, 0.0f, 0.0f, 0.25f,
        -0.2f, 0.5f, 1.4f, 0.75f,
    };
    Image cstOut;
    if (!renderChain(cstApp, cstIn, cstOut, {}).ok || cstOut.px.size() != cstIn.px.size())
      return fail("native CST render");

    if (std::fabs(cstOut.px[0] - 0.627403896f) > 1e-6f ||
        std::fabs(cstOut.px[1] - 0.069097289f) > 1e-6f ||
        std::fabs(cstOut.px[2] - 0.016391439f) > 1e-6f ||
        cstOut.px[3] != 0.25f)
      return fail("native CST Rec.709 to Rec.2020");

    // The second pixel deliberately contains a negative and a >1 component.
    if (std::fabs(cstOut.px[4] - 0.09979903f) > 1e-6f ||
        std::fabs(cstOut.px[5] - 0.46185798f) > 1e-6f ||
        std::fabs(cstOut.px[6] - 1.29456172f) > 1e-6f ||
        cstOut.px[7] != 0.75f)
      return fail("native CST float range/alpha");

    const PersistChain cstSaved = captureChain(cstApp);
    if (cstSaved.nodes.size() != 1 || cstSaved.nodes[0].backend != "native" ||
        cstSaved.nodes[0].identifier != NativeCstProcessor::kIdentifier ||
        cstSaved.nodes[0].paramsJson.at("input_space") != "\"rec709\"" ||
        cstSaved.nodes[0].paramsJson.at("input_gamma") != "\"linear\"" ||
        cstSaved.nodes[0].paramsJson.at("output_space") != "\"rec2020\"" ||
        cstSaved.nodes[0].paramsJson.at("output_gamma") != "\"linear\"")
      return fail("native CST persistence capture");

    App cstRestored;
    applyChain(cstRestored, cstSaved);
    if (cstRestored.nodes.size() != 1 || !cstRestored.nodes[0].processor ||
        cstRestored.nodes[0].processor->identifier() != NativeCstProcessor::kIdentifier)
      return fail("native CST persistence restore");

    Image cstRestoredOut;
    if (!renderChain(cstRestored, cstIn, cstRestoredOut, {}).ok || cstRestoredOut.px != cstOut.px)
      return fail("native CST restored render");

    Processor &restoredCst = *cstRestored.nodes[0].processor;

    // Every added gamut must round-trip through Rec.709 in linear light.
    for (int targetSpace : {2, 3, 4, 5}) {  // AP0, AP1, DWG, Display P3
      if (!restoredCst.setParameterValue("input_space", 0) ||
          !restoredCst.setParameterValue("input_gamma", 0) ||
          !restoredCst.setParameterValue("output_space", targetSpace) ||
          !restoredCst.setParameterValue("output_gamma", 0))
        return fail("native CST gamut round-trip setup");
      Image wide;
      if (!renderChain(cstRestored, cstIn, wide, {}).ok) return fail("native CST gamut forward render");

      if (!restoredCst.setParameterValue("input_space", targetSpace) ||
          !restoredCst.setParameterValue("output_space", 0))
        return fail("native CST gamut inverse setup");
      Image roundTrip;
      if (!renderChain(cstRestored, wide, roundTrip, {}).ok) return fail("native CST gamut inverse render");

      for (size_t i = 0; i < cstIn.px.size(); ++i) {
        if (i % 4 == 3) {
          if (roundTrip.px[i] != cstIn.px[i]) return fail("native CST gamut round-trip alpha");
        } else if (std::fabs(roundTrip.px[i] - cstIn.px[i]) > 3e-6f) {
          return fail("native CST gamut round trip");
        }
      }
    }

    // Transfer functions are independent of gamut. Use an identity Rec.709
    // gamut transform to check their published 18% grey mappings and inverse.
    Image grey;
    grey.w = 1;
    grey.h = 1;
    grey.px = {0.18f, 0.18f, 0.18f, 0.6f};
    const float expected18[] = {
        0.18f,
        0.46135613f,  // sRGB
        0.40884811f,  // exact Rec.709 camera OETF
        0.33604327f,  // DaVinci Intermediate
    };
    for (int gamma = 0; gamma < 4; ++gamma) {
      if (!restoredCst.setParameterValue("input_space", 0) ||
          !restoredCst.setParameterValue("output_space", 0) ||
          !restoredCst.setParameterValue("input_gamma", 0) ||
          !restoredCst.setParameterValue("output_gamma", gamma))
        return fail("native CST gamma forward setup");

      Image encoded;
      if (!renderChain(cstRestored, grey, encoded, {}).ok) return fail("native CST gamma forward render");
      for (int channel = 0; channel < 3; ++channel)
        if (std::fabs(encoded.px[(size_t)channel] - expected18[gamma]) > 2e-6f)
          return fail("native CST gamma 18 percent mapping");
      if (encoded.px[3] != grey.px[3]) return fail("native CST gamma alpha");

      if (!restoredCst.setParameterValue("input_gamma", gamma) ||
          !restoredCst.setParameterValue("output_gamma", 0))
        return fail("native CST gamma inverse setup");
      Image decoded;
      if (!renderChain(cstRestored, encoded, decoded, {}).ok) return fail("native CST gamma inverse render");
      for (int channel = 0; channel < 3; ++channel)
        if (std::fabs(decoded.px[(size_t)channel] - 0.18f) > 2e-6f)
          return fail("native CST gamma round trip");
    }

    // Blackmagic's published DI mapping explicitly includes negative linear
    // values; preserve that behaviour rather than clamping at zero.
    Image negative;
    negative.w = 1;
    negative.h = 1;
    negative.px = {-0.01f, -0.01f, -0.01f, 1.0f};
    if (!restoredCst.setParameterValue("input_gamma", 0) ||
        !restoredCst.setParameterValue("output_gamma", 3))
      return fail("native CST DI negative setup");
    Image negativeDi;
    if (!renderChain(cstRestored, negative, negativeDi, {}).ok ||
        std::fabs(negativeDi.px[0] - (-0.10444269f)) > 2e-6f)
      return fail("native CST DI negative mapping");

    // A bad upstream pixel is local data, not a frame-level render failure.
    Image badPixel;
    badPixel.w = 2;
    badPixel.h = 1;
    badPixel.px = {
        std::numeric_limits<float>::quiet_NaN(), 0.2f, 0.3f, 1.0f,
        0.2f, 0.3f, 0.4f, 0.5f,
    };
    if (!restoredCst.setParameterValue("input_space", (int)RgbGamut::Rec709) ||
        !restoredCst.setParameterValue("output_space", (int)RgbGamut::Rec2020) ||
        !restoredCst.setParameterValue("input_gamma", (int)TransferFunction::Linear) ||
        !restoredCst.setParameterValue("output_gamma", (int)TransferFunction::Linear))
      return fail("native CST non-finite setup");
    Image badOut;
    if (!renderChain(cstRestored, badPixel, badOut, {}).ok ||
        !std::isnan(badOut.px[0]) || !std::isnan(badOut.px[1]) || !std::isnan(badOut.px[2]) ||
        badOut.px[3] != 1.0f || !std::isfinite(badOut.px[4]))
      return fail("native CST non-finite pixel handling");

    Image hugeDi;
    hugeDi.w = 1;
    hugeDi.h = 1;
    hugeDi.px = {20.0f, 20.0f, 20.0f, 0.4f};
    if (!restoredCst.setParameterValue("input_space", (int)RgbGamut::Rec709) ||
        !restoredCst.setParameterValue("output_space", (int)RgbGamut::Rec709) ||
        !restoredCst.setParameterValue("input_gamma", (int)TransferFunction::DaVinciIntermediate) ||
        !restoredCst.setParameterValue("output_gamma", (int)TransferFunction::Linear))
      return fail("native CST DI overflow setup");
    Image hugeOut;
    if (!renderChain(cstRestored, hugeDi, hugeOut, {}).ok ||
        !std::isfinite(hugeOut.px[0]) || hugeOut.px[0] <= 1.0f ||
        hugeOut.px[3] != hugeDi.px[3])
      return fail("native CST DI overflow handling");

    // Exact Rec.709 constants make the OETF and inverse continuous and
    // monotonic at the breakpoint.
    constexpr double k709Beta = 0.018053968510807;
    const double encodedCut = encodeTransfer(k709Beta, TransferFunction::Rec709);
    if (std::fabs(encodedCut - 4.5 * k709Beta) > 1e-12 ||
        std::fabs(decodeTransfer(encodedCut, TransferFunction::Rec709) - k709Beta) > 1e-12 ||
        decodeTransfer(encodedCut + 1e-7, TransferFunction::Rec709) <
            decodeTransfer(encodedCut - 1e-7, TransferFunction::Rec709))
      return fail("Rec.709 exact breakpoint");

    PersistChain futureChoice = cstSaved;
    futureChoice.nodes[0].paramsJson["output_gamma"] = "\"future-transfer\"";
    App futureChoiceApp;
    applyChain(futureChoiceApp, futureChoice);
    const PersistChain futureChoiceSaved = captureChain(futureChoiceApp);
    if (futureChoiceSaved.nodes.empty() ||
        futureChoiceSaved.nodes[0].paramsJson.at("output_gamma") != "\"future-transfer\"" ||
        !hasUnknownProcessorChoiceIds(futureChoiceApp))
      return fail("future CST choice preservation");

    // Development builds briefly wrote stable choices numerically. Known
    // numeric values migrate to the current stable ID; invalid values remain
    // protected as unknown future/development data.
    PersistChain numericChoice = cstSaved;
    numericChoice.nodes[0].paramsJson["output_gamma"] = "3";
    App numericChoiceApp;
    applyChain(numericChoiceApp, numericChoice);
    const PersistChain numericChoiceSaved = captureChain(numericChoiceApp);
    if (hasUnknownProcessorChoiceIds(numericChoiceApp) ||
        numericChoiceSaved.nodes.empty() ||
        numericChoiceSaved.nodes[0].paramsJson.at("output_gamma") != "\"davinci-intermediate\"")
      return fail("numeric stable choice migration");

    PersistChain invalidNumericChoice = cstSaved;
    invalidNumericChoice.nodes[0].paramsJson["output_gamma"] = "99";
    App invalidNumericChoiceApp;
    applyChain(invalidNumericChoiceApp, invalidNumericChoice);
    const PersistChain invalidNumericChoiceSaved = captureChain(invalidNumericChoiceApp);
    if (!hasUnknownProcessorChoiceIds(invalidNumericChoiceApp) ||
        invalidNumericChoiceSaved.nodes.empty() ||
        invalidNumericChoiceSaved.nodes[0].paramsJson.at("output_gamma") != "99")
      return fail("invalid numeric stable choice protection");

    printf("ok  Native CST processor\n");
  }

  {
    for (auto edit : {GraphEditCase::AddExposure, GraphEditCase::AddCst, GraphEditCase::Remove,
                      GraphEditCase::Reorder, GraphEditCase::Disable, GraphEditCase::Enable,
                      GraphEditCase::Append, GraphEditCase::AppendMissing, GraphEditCase::Replace,
                      GraphEditCase::ReplaceMissing, GraphEditCase::ReplaceEmpty,
                      GraphEditCase::Clear, GraphEditCase::RemoveLast}) {
      if (!testGraphEditTransaction(edit)) {
        fprintf(stderr, "graph transaction case: %d\n", (int)edit);
        return fail("production graph transaction/request/result");
      }
    }
    if (!testPartialMutationUnwind()) return fail("partial restoration exception must release the gate and request preview");
    if (!testGraphEditNoops()) return fail("graph no-ops must retain work without requesting more");
    if (!testIdlePreviewRebuild()) return fail("idle production preview-resolution rebuild/request/result");
    printf("ok  Production graph transactions (13 edits, idle resolution rebuild, no-ops)\n");
    for (auto edit : {FailedDocumentEdit::Ofx, FailedDocumentEdit::Ctl, FailedDocumentEdit::RawMissing,
                      FailedDocumentEdit::RawReplacedByRaster, FailedDocumentEdit::GradeRaw}) {
      for (bool pending : {false, true}) {
        if (!testFailedDocumentEdit(edit, pending)) {
          fprintf(stderr, "failed document edit: %d pending=%d\n", (int)edit, pending);
          return fail("failed document edit state/quiet preview recovery/status");
        }
      }
    }
    if (!testFailedDocumentEdit(FailedDocumentEdit::Ofx, false, true))
      return fail("failed document edit must quietly recover interrupted display-only work");
    printf("ok  Failed OFX/CTL/RAW edits preserve state and recover quietly (11 cases)\n");
    for (bool append : {false, true})
      for (bool ctl : {false, true})
        if (!testUnavailableProcessorRestore(append, ctl))
          return fail("failed OFX/CTL restoration must retain placeholders in one transaction");
    printf("ok  Failed OFX/CTL creation preserves restored placeholders (4 transactions)\n");
    for (bool exporting : {false, true}) {
      for (auto edit : {ActiveGraphEdit::Add, ActiveGraphEdit::Remove, ActiveGraphEdit::Reorder,
                        ActiveGraphEdit::Disable, ActiveGraphEdit::FailCtl}) {
        if (!testGraphEditWaits(exporting, edit)) {
          fprintf(stderr, "active graph edit: %d exporting=%d\n", (int)edit, exporting);
          return fail("production graph edits must drain active preview/export ownership");
        }
      }
    }
    printf("ok  Production graph edits drain active preview/export (10 barrier cases)\n");
  }

  {
    const bool recolorThenFull = testRenderScheduling(false, true);
    const bool fullThenRecolor = testRenderScheduling(false, false);
    const bool busyRecolorThenFull = testRenderScheduling(true, true);
    const bool busyFullThenRecolor = testRenderScheduling(true, false);
    const bool activeRenderRecolor = testRenderScheduling(true, false, false);
    if (!recolorThenFull || !fullThenRecolor || !busyRecolorThenFull ||
        !busyFullThenRecolor || !activeRenderRecolor) {
      fprintf(stderr, "scheduling cases: recolor/full=%d full/recolor=%d busy recolor/full=%d "
                      "busy full/recolor=%d active recolor=%d\n",
              recolorThenFull, fullThenRecolor, busyRecolorThenFull,
              busyFullThenRecolor, activeRenderRecolor);
      return fail("preview render/recolor scheduling");
    }
    printf("ok  Preview render/recolor scheduling (5 cases)\n");
  }

  {
    if (!testRenderScheduling(false, true, true, true) ||
        !testRenderScheduling(false, false, true, true))
      return fail("failed full render must recolour cached display");
    printf("ok  Failed full render recolour fallback (both queue orders)\n");
    const bool supersededRecolor = testFailedRenderWithoutPendingRecolor(false);
    const bool cancelledRecolor = testFailedRenderWithoutPendingRecolor(true);
    if (!supersededRecolor || !cancelledRecolor) {
      fprintf(stderr, "failed-render scheduling: F2=%d F5=%d\n", supersededRecolor, cancelledRecolor);
      return fail("failed full render without pending recolour");
    }
    printf("ok  Failed full render without pending recolour (F2/F5)\n");
    if (!testRecolorMutationGate(true))
      return fail("shutdown cancels queued display recolour");
    printf("ok  Queued display recolour cancellation\n");
    if (!testRecolorMutationGate(false))
      return fail("display-only outermost mutation wakeup");
    printf("ok  Display-only outermost mutation wakeup\n");
  }

  {
    for (auto failure : {ExportFailure::RenderException, ExportFailure::UnknownException,
                         ExportFailure::SizeException, ExportFailure::RestoreException,
                         ExportFailure::CaptureException, ExportFailure::Result})
      for (bool asynchronous : {false, true})
        if (!testExportFailureCleanup(failure, asynchronous))
          return fail("production export failure/exception cleanup");
    printf("ok  Production export failure/exception cleanup and parameter editability (12 deterministic cases)\n");
    if (!testExportThreadLifecycle(false) || !testExportThreadLifecycle(true))
      return fail("owned export thread restart/shutdown");
    printf("ok  Owned export thread restart/shutdown and parameter editability\n");
    if (!testExportPreviewStatus(ExportStatusResult::Success) ||
        !testExportPreviewStatus(ExportStatusResult::Warnings) ||
        !testExportPreviewStatus(ExportStatusResult::Failure) ||
        !testExportPreviewStatus(ExportStatusResult::WriteFailure) ||
        !testExportPreviewStatus(ExportStatusResult::Success, ExportStatusPreview::Fail) ||
        !testExportPreviewStatus(ExportStatusResult::Success, ExportStatusPreview::ExplicitEdit))
      return fail("parked export preview wakeup/status preservation");
    printf("ok  Parked production export preview wakeup/status preservation (6 deterministic cases)\n");
  }

  {
    for (auto work : {ExportPreviewWork::Active, ExportPreviewWork::Pending, ExportPreviewWork::None})
      if (!testPreviewAfterExport(work)) return fail("preview restoration after export");
    for (auto ending : {ExportPreviewEnd::Mutate, ExportPreviewEnd::MutateDuring,
                        ExportPreviewEnd::StopDuring, ExportPreviewEnd::StopAfter})
      if (!testPreviewAfterExport(ExportPreviewWork::Pending, ending))
        return fail("preview after export cancellation/mutation/shutdown");
    printf("ok  Preview restoration and parameter editability after export (7 deterministic cases)\n");
  }

  {
    // Copy/paste uses a versioned transfer payload built from the same
    // PersistNode/Sidecar V2 representation as normal project persistence.
    App copyApp;
    if (!addNativeExposureNode(copyApp) || copyApp.nodes.size() != 1 ||
        !copyApp.nodes[0].processor ||
        !copyApp.nodes[0].processor->setParameterValue("exposure", 2.25))
      return fail("node copy setup");
    copyApp.nodes[0].enabled = false;

    PersistNode copied;
    if (!captureNode(copyApp, 0, copied) ||
        copied.backend != "native" ||
        copied.identifier != NativeExposureProcessor::kIdentifier ||
        copied.paramsJson.at("exposure") != "2.25")
      return fail("node copy capture");

    PersistChain transfer;
    transfer.selectedNodeId = copied.id;
    transfer.nodes.push_back(copied);
    const std::string payload = serializeTransferPayload("node", transfer);

    std::string transferKind;
    PersistChain decoded;
    if (!parseTransferPayload(payload, transferKind, decoded) ||
        transferKind != "node" || decoded.nodes.size() != 1 ||
        decoded.nodes[0].paramsJson.at("exposure") != "2.25")
      return fail("node copy transfer payload");

    const std::string originalId = copyApp.nodes[0].id;
    if (!appendPersistedNode(copyApp, decoded.nodes[0], 0) ||
        copyApp.nodes.size() != 2 || copyApp.selectedNode != 1 ||
        copyApp.nodes[1].id == originalId ||
        copyApp.nodes[1].enabled ||
        !copyApp.nodes[1].processor ||
        copyApp.nodes[1].processor->identifier() != NativeExposureProcessor::kIdentifier)
      return fail("node paste restore");

    bool exposureRestored = false;
    for (const ProcessorParameter &param : copyApp.nodes[1].processor->parameters()) {
      if (param.id == "exposure") {
        const double *value = std::get_if<double>(&param.value);
        exposureRestored = value && *value == 2.25;
      }
    }
    if (!exposureRestored) return fail("node paste parameter");

    // Missing/future processors must remain transferable rather than being
    // discarded just because this build cannot instantiate them.
    PersistNode future;
    future.id = "foreign-id";
    future.backend = "dctl";
    future.identifier = "FutureNode.dctl";
    future.label = "Future Node";
    future.enabled = true;
    future.paramsJson["future"] = "{\"curve\":[0,0.5,1]}";
    if (!appendPersistedNode(copyApp, future, copyApp.selectedNode) ||
        copyApp.nodes.size() != 3 || copyApp.selectedNode != 2 ||
        copyApp.nodes[2].processor ||
        copyApp.nodes[2].id == "foreign-id" ||
        copyApp.nodes[2].storedBackend != "dctl" ||
        copyApp.nodes[2].preservedParamsJson.at("future") != "{\"curve\":[0,0.5,1]}")
      return fail("node paste missing processor preservation");

    if (parseTransferPayload(
            "{\"format\":\"rawnode-transfer\",\"version\":2,\"kind\":\"node\",\"graph\":{\"nodes\":[]}}",
            transferKind, decoded))
      return fail("node transfer future version rejection");

    printf("ok  Node copy/paste transfer\n");
  }

  {
    // Full-grade transfer uses the same versioned payload but replaces the
    // destination chain as one coherent serial grade.
    App sourceGrade;
    if (!addNativeExposureNode(sourceGrade) || !addNativeCstNode(sourceGrade) ||
        sourceGrade.nodes.size() != 2 ||
        !sourceGrade.nodes[0].processor->setParameterValue("exposure", -1.25) ||
        !sourceGrade.nodes[1].processor->setParameterValue("output_space", 5))
      return fail("full grade copy setup");
    sourceGrade.nodes[0].enabled = false;
    sourceGrade.selectedNode = 1;
    {
      std::lock_guard<std::mutex> lock(sourceGrade.colorMutex);
      sourceGrade.outputEncoding = {RgbGamut::DisplayP3, TransferFunction::Rec709};
    }

    const PersistChain sourceChain = captureChain(sourceGrade);
    PersistGradeColor sourceColor = captureGradeColor(sourceGrade);
    // Exercise RAW colour fields in the envelope even though this synthetic
    // source app has no decoded RAW image attached.
    sourceColor.rawColorSpace = "davinci-wide-gamut";
    sourceColor.rawGamma = "davinci-intermediate";
    const std::string payload = serializeTransferPayload("grade", sourceChain, &sourceColor);
    std::string kind;
    PersistChain decoded;
    PersistGradeColor decodedColor;
    if (!parseTransferPayload(payload, kind, decoded, &decodedColor) ||
        kind != "grade" || decoded.nodes.size() != 2 ||
        decoded.selectedNodeId != sourceGrade.nodes[1].id ||
        decodedColor.rawColorSpace != "davinci-wide-gamut" ||
        decodedColor.rawGamma != "davinci-intermediate" ||
        decodedColor.outputColorSpace != "display-p3" ||
        decodedColor.outputGamma != "rec709-camera")
      return fail("full grade transfer payload");

    App destinationGrade;
    if (!addNativeExposureNode(destinationGrade) ||
        !destinationGrade.nodes[0].processor->setParameterValue("exposure", 4.0))
      return fail("full grade destination setup");
    if (!applyGradeColor(destinationGrade, decodedColor))
      return fail("full grade colour apply");
    applyChain(destinationGrade, decoded);

    ColorEncoding pastedOutput;
    {
      std::lock_guard<std::mutex> lock(destinationGrade.colorMutex);
      pastedOutput = destinationGrade.outputEncoding;
    }
    if (pastedOutput != ColorEncoding{RgbGamut::DisplayP3, TransferFunction::Rec709})
      return fail("full grade output colour restore");

    if (destinationGrade.nodes.size() != 2 || destinationGrade.selectedNode != 1 ||
        destinationGrade.nodes[0].id != sourceGrade.nodes[0].id ||
        destinationGrade.nodes[1].id != sourceGrade.nodes[1].id ||
        destinationGrade.nodes[0].enabled ||
        !destinationGrade.nodes[0].processor ||
        !destinationGrade.nodes[1].processor ||
        destinationGrade.nodes[0].processor->identifier() != NativeExposureProcessor::kIdentifier ||
        destinationGrade.nodes[1].processor->identifier() != NativeCstProcessor::kIdentifier)
      return fail("full grade paste restore");

    bool exposureRestored = false;
    for (const ProcessorParameter &param : destinationGrade.nodes[0].processor->parameters()) {
      if (param.id == "exposure") {
        const double *value = std::get_if<double>(&param.value);
        exposureRestored = value && *value == -1.25;
      }
    }
    bool cstRestored = false;
    for (const ProcessorParameter &param : destinationGrade.nodes[1].processor->parameters()) {
      if (param.id == "output_space") {
        const int *value = std::get_if<int>(&param.value);
        cstRestored = value && *value == 5;
      }
    }
    if (!exposureRestored || !cstRestored)
      return fail("full grade pasted parameters");

    printf("ok  Full grade copy/paste transfer\n");
  }

  {
    // Presets use their own versioned file envelope but retain the exact same
    // PersistChain/PersistNode representation as clipboard transfer and Sidecar V2.
    const fs::path nodePresetPath =
        fs::temp_directory_path() / "rawnode-selftest-node.rawnodepreset";
    const fs::path gradePresetPath =
        fs::temp_directory_path() / "rawnode-selftest-grade.rawnodepreset";
    const fs::path futurePresetPath =
        fs::temp_directory_path() / "rawnode-selftest-future.rawnodepreset";

    App presetSource;
    if (!addNativeExposureNode(presetSource) || !addNativeCstNode(presetSource) ||
        !presetSource.nodes[0].processor->setParameterValue("exposure", 1.75) ||
        !presetSource.nodes[1].processor->setParameterValue("output_gamma", 3))
      return fail("preset source setup");
    presetSource.nodes[0].enabled = false;
    presetSource.selectedNode = 0;

    PersistNode nodeState;
    if (!captureNode(presetSource, 0, nodeState))
      return fail("node preset capture");
    PersistChain nodePreset;
    nodePreset.selectedNodeId = nodeState.id;
    nodePreset.nodes.push_back(nodeState);
    if (!savePresetFile(nodePresetPath.string(), "node", nodePreset))
      return fail("node preset save");

    std::string kind;
    PersistChain loadedNodePreset;
    if (!loadPresetFile(nodePresetPath.string(), kind, loadedNodePreset) ||
        kind != "node" || loadedNodePreset.nodes.size() != 1 ||
        loadedNodePreset.nodes[0].identifier != NativeExposureProcessor::kIdentifier ||
        loadedNodePreset.nodes[0].enabled ||
        loadedNodePreset.nodes[0].paramsJson.at("exposure") != "1.75")
      return fail("node preset load");

    App nodePresetTarget;
    if (!appendPersistedNode(nodePresetTarget, loadedNodePreset.nodes[0]) ||
        nodePresetTarget.nodes.size() != 1 || !nodePresetTarget.nodes[0].processor)
      return fail("node preset apply");
    bool presetExposure = false;
    for (const ProcessorParameter &param : nodePresetTarget.nodes[0].processor->parameters()) {
      if (param.id == "exposure") {
        const double *value = std::get_if<double>(&param.value);
        presetExposure = value && *value == 1.75;
      }
    }
    if (!presetExposure) return fail("node preset parameter");

    {
      std::lock_guard<std::mutex> lock(presetSource.colorMutex);
      presetSource.outputEncoding = {RgbGamut::ACES_AP1, TransferFunction::Linear};
    }
    const PersistChain gradeState = captureChain(presetSource);
    const PersistGradeColor gradeColor = captureGradeColor(presetSource);
    if (!savePresetFile(gradePresetPath.string(), "grade", gradeState, &gradeColor))
      return fail("grade preset save");
    PersistChain loadedGradePreset;
    PersistGradeColor loadedGradeColor;
    if (!loadPresetFile(gradePresetPath.string(), kind, loadedGradePreset, &loadedGradeColor) ||
        kind != "grade" || loadedGradePreset.nodes.size() != 2 ||
        loadedGradeColor.outputColorSpace != "aces-ap1" ||
        loadedGradeColor.outputGamma != "linear")
      return fail("grade preset load");

    App gradePresetTarget;
    if (!addNativeExposureNode(gradePresetTarget))
      return fail("grade preset target setup");
    if (!applyGradeColor(gradePresetTarget, loadedGradeColor))
      return fail("grade preset colour apply");
    applyChain(gradePresetTarget, loadedGradePreset);
    ColorEncoding presetOutput;
    {
      std::lock_guard<std::mutex> lock(gradePresetTarget.colorMutex);
      presetOutput = gradePresetTarget.outputEncoding;
    }
    if (presetOutput != ColorEncoding{RgbGamut::ACES_AP1, TransferFunction::Linear})
      return fail("grade preset output colour restore");
    if (gradePresetTarget.nodes.size() != 2 ||
        !gradePresetTarget.nodes[0].processor ||
        !gradePresetTarget.nodes[1].processor ||
        gradePresetTarget.nodes[0].processor->identifier() != NativeExposureProcessor::kIdentifier ||
        gradePresetTarget.nodes[1].processor->identifier() != NativeCstProcessor::kIdentifier)
      return fail("grade preset apply");

    {
      std::ofstream future(futurePresetPath.string(), std::ios::binary);
      future << "{\"format\":\"rawnode-preset\",\"version\":2,\"kind\":\"grade\","
                "\"graph\":{\"selectedNodeId\":\"\",\"nodes\":[]}}";
      if (!future.good()) return fail("future preset write");
    }
    PersistChain rejected;
    if (loadPresetFile(futurePresetPath.string(), kind, rejected))
      return fail("future preset version rejection");

    if (savePresetFile(nodePresetPath.string(), "unknown", nodePreset))
      return fail("invalid preset kind save");

    fs::remove(nodePresetPath);
    fs::remove(gradePresetPath);
    fs::remove(futurePresetPath);
    printf("ok  Node and full-grade presets\n");
  }

  {
    // Real document switching must save image A before opening B, then restore
    // A's document colour/output state and node chain when returning to it.
    const fs::path imageA = fs::temp_directory_path() / "rawnode-selftest-switch-a.tif";
    const fs::path imageB = fs::temp_directory_path() / "rawnode-selftest-switch-b.tif";
    if (!writeTinyTiff(imageA, false) || !writeTinyTiff(imageB, false))
      return fail("document switch image write");

    App switched;
    openPath(switched, imageA.string(), true);
    {
      std::lock_guard<std::mutex> lock(switched.colorMutex);
      switched.outputEncoding = {RgbGamut::DisplayP3, TransferFunction::SRGB};
    }
    if (!addNativeExposureNode(switched) ||
        !switched.nodes[0].processor ||
        !switched.nodes[0].processor->setParameterValue("exposure", 1.5))
      return fail("document switch edit setup");
    switched.nodes[0].enabled = false;

    openPath(switched, imageB.string(), true);

    PersistSidecar savedA;
    const std::string sidecarA = inputSidecarPath(imageA.string());
    if (!loadSidecarFile(sidecarA, savedA) ||
        savedA.gui.outputColorSpace != rgbGamutId(RgbGamut::DisplayP3) ||
        savedA.gui.outputGamma != transferFunctionId(TransferFunction::SRGB) ||
        savedA.chain.nodes.size() != 1 ||
        savedA.chain.nodes[0].identifier != NativeExposureProcessor::kIdentifier ||
        savedA.chain.nodes[0].enabled ||
        savedA.chain.nodes[0].paramsJson.at("exposure") != "1.5")
      return fail("document switch sidecar save");

    openPath(switched, imageA.string(), true);
    ColorEncoding restoredOutput;
    {
      std::lock_guard<std::mutex> lock(switched.colorMutex);
      restoredOutput = switched.outputEncoding;
    }
    if (restoredOutput != ColorEncoding{RgbGamut::DisplayP3, TransferFunction::SRGB} ||
        switched.nodes.size() != 1 ||
        switched.nodes[0].enabled ||
        !switched.nodes[0].processor ||
        switched.nodes[0].processor->identifier() != NativeExposureProcessor::kIdentifier)
      return fail("document switch sidecar restore");

    bool restoredExposure = false;
    for (const ProcessorParameter &param : switched.nodes[0].processor->parameters()) {
      if (param.id == "exposure") {
        const double *value = std::get_if<double>(&param.value);
        restoredExposure = value && *value == 1.5;
      }
    }
    if (!restoredExposure) return fail("document switch node parameter restore");

    fs::remove(inputSidecarPath(imageA.string()));
    fs::remove(inputSidecarPath(imageB.string()));
    fs::remove(imageA);
    fs::remove(imageB);
    printf("ok  Document switch sidecar persistence\n");
  }

  {
    // Sidecar V2 round-trip: IDs/backend identity and opaque future parameter
    // JSON must survive even when this build cannot interpret the processor.
    const fs::path source = fs::temp_directory_path() / "rawnode-selftest-source.nef";
    PersistGui gui;
    PersistChain chain;
    chain.selectedNodeId = "node-future";

    PersistNode node;
    node.id = "node-future";
    node.backend = "dctl";
    node.identifier = "FutureTransform.dctl";
    node.label = "Future Transform";
    node.enabled = true;
    node.groupOpen["params"] = true;  // Must not shadow the sibling params object.
    node.paramsJson["amount"] = "0.75";
    node.paramsJson["futureData"] = "{\"curve\":[0,0.5,1],\"mode\":\"test\"}";
    node.paramsJson["unicodeText"] = "\"Caf\\u00e9 \\ud83c\\udf9e\"";
    chain.nodes.push_back(node);

    const ColorEncoding rawEncoding{RgbGamut::DaVinciWideGamut, TransferFunction::DaVinciIntermediate};
    if (!saveInputSidecar(source.string(), gui, chain, &rawEncoding))
      return fail("sidecar v2 save");

    PersistSidecar loaded;
    const std::string sidecar = inputSidecarPath(source.string());
    if (!loadSidecarFile(sidecar, loaded)) return fail("sidecar v2 load");
    if (loaded.format != "rawnode-sidecar" || loaded.version != 2) return fail("sidecar v2 version");
    if (!loaded.legacyRawWorkingSpace.empty() ||
        loaded.rawColorSpace != "davinci-wide-gamut" ||
        loaded.rawGamma != "davinci-intermediate")
      return fail("sidecar v2 RAW encoding");
    if (loaded.chain.selectedNodeId != "node-future" || loaded.chain.nodes.size() != 1)
      return fail("sidecar v2 node identity");
    const PersistNode &loadedNode = loaded.chain.nodes[0];
    if (loadedNode.backend != "dctl" || loadedNode.identifier != "FutureTransform.dctl" ||
        loadedNode.paramsJson.at("futureData") != "{\"curve\":[0,0.5,1],\"mode\":\"test\"}" ||
        loadedNode.paramsJson.at("amount") != "0.75" || !loadedNode.groupOpen.at("params"))
      return fail("sidecar v2 opaque state");

    std::string decodedUnicode;
    const std::string expectedUnicode = "Caf\xC3\xA9 \xF0\x9F\x8E\x9E";
    if (!parseJsonStringValue(loadedNode.paramsJson.at("unicodeText"), decodedUnicode) ||
        decodedUnicode != expectedUnicode)
      return fail("sidecar v2 unicode string");
    const std::string controlString = std::string("line1\nline2\t") + char(1);
    std::string decodedControl;
    if (!parseJsonStringValue(jsonStringValue(controlString), decodedControl) || decodedControl != controlString)
      return fail("sidecar v2 control string");

    App placeholderApp;
    applyChain(placeholderApp, loaded.chain);
    if (placeholderApp.nodes.size() != 1 || placeholderApp.nodes[0].processor ||
        placeholderApp.nodes[0].id != "node-future" ||
        placeholderApp.nodes[0].storedBackend != "dctl")
      return fail("sidecar v2 missing processor placeholder");
    const PersistChain recaptured = captureChain(placeholderApp);
    if (recaptured.nodes.size() != 1 || recaptured.nodes[0].id != "node-future" ||
        recaptured.nodes[0].paramsJson.at("futureData") != "{\"curve\":[0,0.5,1],\"mode\":\"test\"}")
      return fail("sidecar v2 missing processor preservation");

    fs::remove(sidecar);

    // Development builds briefly wrote colour display names instead of
    // stable IDs. They should load as known values and normalise on next save.
    const fs::path legacyNamesImage =
        fs::temp_directory_path() / "rawnode-selftest-legacy-colour-names.tif";
    if (!writeTinyTiff(legacyNamesImage, false))
      return fail("legacy colour names image write");
    PersistGui legacyNamesGui;
    legacyNamesGui.outputColorSpace = "Display P3";
    legacyNamesGui.outputGamma = "sRGB";
    if (!saveInputSidecar(legacyNamesImage.string(), legacyNamesGui, PersistChain{}, nullptr))
      return fail("legacy colour names sidecar initial save");
    App legacyNamesApp;
    openPath(legacyNamesApp, legacyNamesImage.string(), true);
    {
      std::lock_guard<std::mutex> lock(legacyNamesApp.colorMutex);
      if (legacyNamesApp.outputEncoding != ColorEncoding{RgbGamut::DisplayP3, TransferFunction::SRGB})
        return fail("legacy colour names migration");
    }
    if (!legacyNamesApp.sidecarWriteBlockedPath.empty())
      return fail("legacy colour names write protection");
    saveCurrentInputSidecar(legacyNamesApp);
    PersistSidecar normalisedNames;
    if (!loadSidecarFile(inputSidecarPath(legacyNamesImage.string()), normalisedNames) ||
        normalisedNames.gui.outputColorSpace != "display-p3" ||
        normalisedNames.gui.outputGamma != "srgb")
      return fail("legacy colour names normalisation");
    fs::remove(inputSidecarPath(legacyNamesImage.string()));
    fs::remove(legacyNamesImage);

    // Unknown future colour identifiers must be readable but write-protected,
    // rather than silently replaced by this build's fallback.
    const fs::path protectedImage = fs::temp_directory_path() / "rawnode-selftest-protected.tif";
    if (!writeTinyTiff(protectedImage, false)) return fail("protected sidecar image write");
    PersistChain emptyChain;
    const ColorEncoding protectedRaw{RgbGamut::Rec2020, TransferFunction::Linear};
    if (!saveInputSidecar(protectedImage.string(), gui, emptyChain, &protectedRaw))
      return fail("protected sidecar initial save");
    const std::string protectedSidecar = inputSidecarPath(protectedImage.string());
    std::string protectedJson;
    {
      std::ifstream in(protectedSidecar, std::ios::binary);
      protectedJson.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    const std::string knownGamma = "\"gamma\":\"linear\"";
    const size_t gammaPos = protectedJson.find(knownGamma);
    if (gammaPos == std::string::npos) return fail("protected sidecar gamma locate");
    protectedJson.replace(gammaPos, knownGamma.size(), "\"gamma\":\"Gamma 2.4\"");
    {
      std::ofstream out(protectedSidecar, std::ios::binary | std::ios::trunc);
      out << protectedJson;
      if (!out.good()) return fail("protected sidecar rewrite");
    }
    App protectedApp;
    openPath(protectedApp, protectedImage.string(), true);
    if (protectedApp.sidecarWriteBlockedPath != protectedImage.string())
      return fail("unknown colour sidecar write protection");
    saveCurrentInputSidecar(protectedApp);
    std::string protectedAfter;
    {
      std::ifstream in(protectedSidecar, std::ios::binary);
      protectedAfter.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    if (protectedAfter.find("\"gamma\":\"Gamma 2.4\"") == std::string::npos)
      return fail("unknown colour sidecar preservation");
    fs::remove(protectedSidecar);
    fs::remove(protectedImage);

    // RawNode-generated ICC descriptions carry stable encoding IDs so
    // wide-gamut exports round-trip through the canonical raster loader.
    Image tagged;
    tagged.w = 1;
    tagged.h = 1;
    tagged.px = {0.18f, 0.18f, 0.18f, 1.0f};
    for (ColorEncoding exportEncoding : {
             ColorEncoding{RgbGamut::ACES_AP1, TransferFunction::Linear},
             ColorEncoding{RgbGamut::DaVinciWideGamut, TransferFunction::DaVinciIntermediate},
             ColorEncoding{RgbGamut::DisplayP3, TransferFunction::SRGB},
             ColorEncoding{RgbGamut::Rec2020, TransferFunction::Rec709}}) {
      const fs::path taggedPath =
          fs::temp_directory_path() / (std::string("rawnode-selftest-tagged-") + rgbGamutId(exportEncoding.gamut) + ".png");
      if (!writeImage(tagged, taggedPath.string(), exportEncoding))
        return fail("ICC tagged export");
      Image reloaded;
      ColorEncoding reloadedEncoding;
      bool raw = true;
      if (!loadImage(taggedPath.string(), reloaded, reloadedEncoding, raw) || raw ||
          reloadedEncoding.gamut != exportEncoding.gamut ||
          reloadedEncoding.gamma != TransferFunction::Linear)
        return fail("ICC encoding round trip");
      fs::remove(taggedPath);
    }

    // Per-image sidecars must not mutate the RAW session/workspace default.
    const fs::path defaultImage = fs::temp_directory_path() / "rawnode-selftest-default-owner.tif";
    if (!writeTinyTiff(defaultImage, false)) return fail("RAW default owner image write");
    PersistGui oldPerImageGui;
    oldPerImageGui.outputColorSpace = "rec709";
    oldPerImageGui.outputGamma = "srgb";
    if (!saveInputSidecar(defaultImage.string(), oldPerImageGui, PersistChain{}, nullptr))
      return fail("RAW default owner sidecar write");
    const std::string defaultSidecar = inputSidecarPath(defaultImage.string());
    std::string defaultJson;
    {
      std::ifstream in(defaultSidecar, std::ios::binary);
      defaultJson.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    const std::string outputGammaField = "\"outputGamma\":\"srgb\",";
    const size_t outputGammaPos = defaultJson.find(outputGammaField);
    if (outputGammaPos == std::string::npos) return fail("RAW default owner JSON locate");
    defaultJson.insert(outputGammaPos + outputGammaField.size(),
                       "\"rawDefaultColorSpace\":\"rec709\",\"rawDefaultGamma\":\"linear\",");
    {
      std::ofstream out(defaultSidecar, std::ios::binary | std::ios::trunc);
      out << defaultJson;
      if (!out.good()) return fail("RAW default owner JSON rewrite");
    }
    App defaultOwner;
    defaultOwner.rawWorkingEncoding =
        {RgbGamut::DaVinciWideGamut, TransferFunction::DaVinciIntermediate};
    openPath(defaultOwner, defaultImage.string(), true);
    if (defaultOwner.rawWorkingEncoding !=
        ColorEncoding{RgbGamut::DaVinciWideGamut, TransferFunction::DaVinciIntermediate})
      return fail("per-image sidecar changed RAW default");
    fs::remove(inputSidecarPath(defaultImage.string()));
    fs::remove(defaultImage);

    // V1 remains readable and is normalised into the generic persistence model.
    const fs::path legacy = fs::temp_directory_path() / "rawnode-selftest-v1.ofxrawhost.json";
    static const char kV1[] =
        "{\"format\":\"ofxrawhost-sidecar\",\"version\":1,\"kind\":\"input\","
        "\"sourcePath\":\"old.nef\",\"inputColorSpace\":\"Linear Rec.2020\","
        "\"chain\":{\"selectedNode\":0,\"nodes\":[{\"pluginIdentifier\":\"example.ofx\","
        "\"pluginLabel\":\"Example\",\"enabled\":true,\"groupOpen\":{},"
        "\"params\":{\"gain\":1.25}}]}}";
    {
      std::ofstream legacyFile(legacy.string(), std::ios::binary);
      if (!legacyFile) return fail("sidecar v1 test write");
      legacyFile.write(kV1, sizeof(kV1) - 1);
      if (!legacyFile.good()) return fail("sidecar v1 test write");
    }

    PersistSidecar migrated;
    if (!loadSidecarFile(legacy.string(), migrated) || migrated.chain.nodes.size() != 1)
      return fail("sidecar v1 migration");
    if (migrated.chain.nodes[0].backend != "ofx" ||
        migrated.chain.nodes[0].identifier != "example.ofx" ||
        migrated.chain.nodes[0].paramsJson.at("gain") != "1.25")
      return fail("sidecar v1 normalisation");

    // Pre-V2 writers escaped only '"' and '\\' in string parameters, so legacy
    // multi-line values contain raw control characters. They must load intact,
    // including every parameter that follows them.
    {
      std::ofstream legacyFile(legacy.string(), std::ios::binary);
      legacyFile << "{\"format\":\"ofxrawhost-sidecar\",\"version\":1,\"kind\":\"input\","
                    "\"chain\":{\"selectedNode\":0,\"nodes\":[{\"pluginIdentifier\":\"example.ofx\","
                    "\"pluginLabel\":\"Example\",\"enabled\":true,\"groupOpen\":{},"
                    "\"params\":{\"a\":1,\"notes\":\"line1\nline2\tend\",\"z\":0.5}}]}}";
      if (!legacyFile.good()) return fail("sidecar v1 multiline test write");
    }
    PersistSidecar multiline;
    std::string notes;
    if (!loadSidecarFile(legacy.string(), multiline) || multiline.chain.nodes.size() != 1 ||
        multiline.chain.nodes[0].paramsJson.size() != 3 ||
        multiline.chain.nodes[0].paramsJson.at("z") != "0.5" ||
        !parseJsonStringValue(multiline.chain.nodes[0].paramsJson.at("notes"), notes) ||
        notes != "line1\nline2\tend")
      return fail("sidecar v1 legacy multiline string");

    // A malformed params object rejects the sidecar rather than restoring a partial node.
    {
      std::ofstream legacyFile(legacy.string(), std::ios::binary);
      legacyFile << "{\"format\":\"ofxrawhost-sidecar\",\"version\":1,\"kind\":\"input\","
                    "\"chain\":{\"selectedNode\":0,\"nodes\":[{\"pluginIdentifier\":\"example.ofx\","
                    "\"params\":{\"a\":1 \"b\":2}}]}}";
      if (!legacyFile.good()) return fail("sidecar v1 malformed test write");
    }
    PersistSidecar malformed;
    if (loadSidecarFile(legacy.string(), malformed)) return fail("sidecar malformed params rejection");
    fs::remove(legacy);

    const fs::path future = fs::temp_directory_path() / "rawnode-selftest-v3.rawnode.json";
    {
      std::ofstream futureFile(future.string(), std::ios::binary);
      futureFile << "{\"format\":\"rawnode-sidecar\",\"version\":3,\"graph\":{\"nodes\":[]}}";
      if (!futureFile.good()) return fail("sidecar v3 test write");
    }
    PersistSidecar futureSidecar;
    if (loadSidecarFile(future.string(), futureSidecar) || futureSidecar.version != 3)
      return fail("sidecar future version rejection");
    fs::remove(future);

    printf("ok  Sidecar V2\n");
  }

  Image src;
  src.w = 64;
  src.h = 48;
  src.px.assign((size_t)src.w * src.h * 4, 1.0f);
  for (int y = 0; y < src.h; ++y)
    for (int x = 0; x < src.w; ++x)
      for (int c = 0; c < 3; ++c) src.px[((size_t)y * src.w + x) * 4 + c] = 0.18f * std::exp2((x - src.w / 2) / 8.0f);

  // Row-order check: bottom-up means index 0 is the bottom row.
  Image order;
  order.w = 8;
  order.h = 8;
  order.px.assign(8 * 8 * 4, 0.0f);
  for (int x = 0; x < 8; ++x) {
    order.px[((size_t)7 * 8 + x) * 4 + 0] = 1.0f;  // top row in display = last bottom-up row
    order.px[((size_t)7 * 8 + x) * 4 + 3] = 1.0f;
  }
  if (order.px[0] > 0.1f || order.px[(size_t)7 * 8 * 4] < 0.5f) return fail("source rows are not bottom-up");

  loadPlugins();
  if (gPlugins.empty()) return fail("no OFX filter plugins found");

  // Standard CTL backend: load a real .ctl with a sibling import, render it
  // through Processor, persist/restore it through Sidecar V2, then verify a
  // missing script degrades to the normal preserved placeholder.
  {
    const fs::path ctlDir = fs::temp_directory_path() / "rawnode-selftest-ctl";
    fs::create_directories(ctlDir);
    const fs::path libPath = ctlDir / "GainLib.ctl";
    const fs::path scriptPath = ctlDir / "DoubleRGB.ctl";

    {
      std::ofstream lib(libPath.string(), std::ios::binary);
      lib << "float applyGain(float x, float gain) { return x * gain; }\n";
      if (!lib.good()) return fail("ctl library test write");
    }
    {
      std::ofstream script(scriptPath.string(), std::ios::binary);
      script <<
          "import \"GainLib\";\n"
          "void main(\n"
          "  input varying float rIn, input varying float gIn, input varying float bIn,\n"
          "  output varying float rOut, output varying float gOut, output varying float bOut,\n"
          "  output varying float aOut, input varying float aIn = 1.0,\n"
          "  input uniform float gain = 2.0, input uniform int mode = 0,\n"
          "  input uniform bool enabled = true)\n"
          "{\n"
          "  if (!enabled) { rOut = rIn; gOut = gIn; bOut = bIn; }\n"
          "  else if (mode == 1) { rOut = rIn; gOut = applyGain(gIn, gain); bOut = bIn; }\n"
          "  else { rOut = applyGain(rIn, gain); gOut = applyGain(gIn, gain); bOut = applyGain(bIn, gain); }\n"
          "  aOut = aIn;\n"
          "}\n";
      if (!script.good()) return fail("ctl script test write");
    }

    App ctlApp;
    if (!addCtlNode(ctlApp, scriptPath.string()) || ctlApp.nodes.size() != 1 ||
        !ctlApp.nodes[0].processor || ctlApp.nodes[0].processor->backend() != ProcessorBackend::CTL)
      return fail("ctl processor creation");

    {
      App preview;
      graphPreviewSource(preview);
      const int before = renderState(preview).epoch;
      if (!addCtlNode(preview, scriptPath.string()) || !finishGraphPreview(preview, before, 0.25f))
        return fail("CTL add transaction/request/result");
    }

    Image ctlOut;
    ProcessorResult ctlResult = renderChain(ctlApp, src, ctlOut, {});
    if (!ctlResult.ok || ctlOut.w != src.w || ctlOut.h != src.h || ctlOut.px.size() != src.px.size())
      return fail("ctl processor render");

    for (size_t i = 0; i + 3 < src.px.size(); i += 4) {
      for (int c = 0; c < 3; ++c)
        if (std::fabs(ctlOut.px[i + c] - src.px[i + c] * 2.0f) > 1e-6f)
          return fail("ctl RGB result");
      if (ctlOut.px[i + 3] != src.px[i + 3]) return fail("ctl alpha result");
    }

    const auto ctlParams = ctlApp.nodes[0].processor->parameters();
    const auto findCtlParam = [&](const char *id) -> const ProcessorParameter * {
      for (const ProcessorParameter &param : ctlParams)
        if (param.id == id) return &param;
      return nullptr;
    };
    const ProcessorParameter *gainParam = findCtlParam("gain");
    const ProcessorParameter *modeParam = findCtlParam("mode");
    const ProcessorParameter *enabledParam = findCtlParam("enabled");
    if (!gainParam || gainParam->type != ParameterType::Double || gainParam->hasRange ||
        !std::get_if<double>(&gainParam->defaultValue) || *std::get_if<double>(&gainParam->defaultValue) != 2.0 ||
        !modeParam || modeParam->type != ParameterType::Integer || modeParam->hasRange ||
        !std::get_if<int>(&modeParam->defaultValue) || *std::get_if<int>(&modeParam->defaultValue) != 0 ||
        !enabledParam || enabledParam->type != ParameterType::Boolean ||
        !std::get_if<bool>(&enabledParam->defaultValue) || !*std::get_if<bool>(&enabledParam->defaultValue))
      return fail("ctl parameter discovery/defaults");

    if (!ctlApp.nodes[0].processor->setParameterValue("gain", 3.0) ||
        !ctlApp.nodes[0].processor->setParameterValue("mode", 1) ||
        !ctlApp.nodes[0].processor->setParameterValue("enabled", true))
      return fail("ctl parameter set");

    Image ctlParamOut;
    if (!renderChain(ctlApp, src, ctlParamOut, {}).ok) return fail("ctl parameter render");
    for (size_t i = 0; i + 3 < src.px.size(); i += 4) {
      if (std::fabs(ctlParamOut.px[i + 0] - src.px[i + 0]) > 1e-6f ||
          std::fabs(ctlParamOut.px[i + 1] - src.px[i + 1] * 3.0f) > 1e-6f ||
          std::fabs(ctlParamOut.px[i + 2] - src.px[i + 2]) > 1e-6f ||
          ctlParamOut.px[i + 3] != src.px[i + 3])
        return fail("ctl parameter values");
    }

    if (!ctlApp.nodes[0].processor->setParameterValue("enabled", false))
      return fail("ctl bool parameter set");
    Image ctlDisabledOut;
    if (!renderChain(ctlApp, src, ctlDisabledOut, {}).ok || ctlDisabledOut.px != src.px)
      return fail("ctl bool parameter render");

    if (!ctlApp.nodes[0].processor->resetParameter("gain") ||
        !ctlApp.nodes[0].processor->resetParameter("mode") ||
        !ctlApp.nodes[0].processor->resetParameter("enabled"))
      return fail("ctl parameter reset");
    Image ctlResetOut;
    if (!renderChain(ctlApp, src, ctlResetOut, {}).ok || ctlResetOut.px != ctlOut.px)
      return fail("ctl parameter reset render");

    // Values are validated against CTL's 32-bit float storage and exact types,
    // and a rejected write leaves the current value untouched.
    Processor &ctlProc = *ctlApp.nodes[0].processor;
    const auto ctlGain = [&]() {
      for (const ProcessorParameter &param : ctlProc.parameters())
        if (param.id == "gain") return std::get<double>(param.value);
      return -1.0;
    };
    if (ctlProc.setParameterValue("gain", std::numeric_limits<double>::infinity()) ||
        ctlProc.setParameterValue("gain", std::numeric_limits<double>::quiet_NaN()) ||
        ctlProc.setParameterValue("gain", 1e39) ||               // overflows 32-bit float
        ctlProc.setParameterValue("gain", 2) ||                  // int for a float input
        ctlProc.setParameterValue("mode", 1.0) ||                // double for an int input
        ctlProc.setParameterValue("enabled", 1) ||               // int for a bool input
        ctlProc.setParameterValue("noSuchParameter", 1.0) || ctlGain() != 2.0)
      return fail("ctl parameter validation");
    if (!ctlProc.setParameterValue("gain", (double)std::numeric_limits<float>::max()) ||
        ctlGain() != (double)std::numeric_limits<float>::max() || !ctlProc.resetParameter("gain"))
      return fail("ctl parameter float range");

    // More pixels than one SIMD chunk (maxSamples() = 4096), so the parameter
    // snapshot written before rendering must hold for every chunk.
    Image ctlWide;
    ctlWide.w = 101;
    ctlWide.h = 77;  // 7777 pixels
    ctlWide.px.resize((size_t)ctlWide.w * ctlWide.h * 4);
    for (size_t i = 0; i < ctlWide.px.size(); ++i) ctlWide.px[i] = (i % 4 == 3) ? 0.5f : 0.001f * (float)(i % 997);
    if (!ctlProc.setParameterValue("gain", 3.0) || !ctlProc.setParameterValue("mode", 1))
      return fail("ctl multi-chunk parameter set");
    Image ctlWideOut;
    if (!ctlProc.render(ctlWide, ctlWideOut, {}).ok || ctlWideOut.px.size() != ctlWide.px.size())
      return fail("ctl multi-chunk render");
    for (size_t i = 0; i + 3 < ctlWide.px.size(); i += 4) {
      if (ctlWideOut.px[i + 0] != ctlWide.px[i + 0] ||
          std::fabs(ctlWideOut.px[i + 1] - ctlWide.px[i + 1] * 3.0f) > 1e-6f ||
          ctlWideOut.px[i + 2] != ctlWide.px[i + 2] || ctlWideOut.px[i + 3] != ctlWide.px[i + 3])
        return fail("ctl multi-chunk parameter values");
    }
    if (!ctlProc.resetParameter("gain") || !ctlProc.resetParameter("mode"))
      return fail("ctl multi-chunk parameter reset");

    // Only defaulted scalar uniform float/int/bool inputs are exposed. An
    // unqualified input is uniform in CTL; varying, half, unsigned int and
    // array inputs stay hidden but their defaults must still apply.
    {
      const fs::path exposurePath = ctlDir / "ExposureRules.ctl";
      {
        std::ofstream script(exposurePath.string(), std::ios::binary);
        script <<
            "void main(\n"
            "  input varying float rIn, input varying float gIn, input varying float bIn,\n"
            "  output varying float rOut, output varying float gOut, output varying float bOut,\n"
            "  input float unqualified = 2.0, input varying float varyingGain = 3.0,\n"
            "  input uniform half halfGain = 0.5, input uniform unsigned int uintGain = 4,\n"
            "  input uniform float arrayGain[2] = {5.0, 7.0})\n"
            "{\n"
            "  rOut = rIn * unqualified * varyingGain * halfGain * uintGain * arrayGain[1];\n"
            "  gOut = gIn; bOut = bIn;\n"
            "}\n";
        if (!script.good()) return fail("ctl exposure rules test write");
      }
      std::string exposureError;
      auto exposure = CtlProcessor::create(exposurePath.string(), &exposureError);
      if (!exposure) return fail(("ctl exposure rules load: " + exposureError).c_str());
      const auto exposed = exposure->parameters();
      if (exposed.size() != 1 || exposed[0].id != "unqualified" || exposed[0].type != ParameterType::Double ||
          exposed[0].hasRange || std::get<double>(exposed[0].defaultValue) != 2.0)
        return fail("ctl exposure rules parameters");

      const auto checkRed = [&](float factor) {
        Image out;
        if (!exposure->render(ctlWide, out, {}).ok || out.px.size() != ctlWide.px.size()) return false;
        for (size_t i = 0; i + 3 < ctlWide.px.size(); i += 4) {
          const float want = ctlWide.px[i] * factor;
          if (std::fabs(out.px[i] - want) > 1e-5f * std::max(1.0f, std::fabs(want))) return false;
        }
        return true;
      };
      if (!checkRed(2.0f * 3.0f * 0.5f * 4.0f * 7.0f)) return fail("ctl hidden input defaults");
      if (!exposure->setParameterValue("unqualified", 1.0) || !checkRed(3.0f * 0.5f * 4.0f * 7.0f))
        return fail("ctl unqualified uniform parameter");
      exposure.reset();
      fs::remove(exposurePath);
    }

    // Persist non-default values so Sidecar V2 proves CTL parameter state is
    // restored through the same backend-neutral path as OFX/native controls.
    if (!ctlApp.nodes[0].processor->setParameterValue("gain", 1.5) ||
        !ctlApp.nodes[0].processor->setParameterValue("mode", 1) ||
        !ctlApp.nodes[0].processor->setParameterValue("enabled", false))
      return fail("ctl persistence parameter setup");
    Image ctlSavedOut;
    if (!renderChain(ctlApp, src, ctlSavedOut, {}).ok) return fail("ctl persistence parameter render");

    const PersistChain ctlSaved = captureChain(ctlApp);
    if (ctlSaved.nodes.size() != 1 || ctlSaved.nodes[0].backend != "ctl" ||
        fs::path(ctlSaved.nodes[0].identifier).filename() != scriptPath.filename() ||
        std::strtod(ctlSaved.nodes[0].paramsJson.at("gain").c_str(), nullptr) != 1.5 ||
        std::strtol(ctlSaved.nodes[0].paramsJson.at("mode").c_str(), nullptr, 10) != 1 ||
        ctlSaved.nodes[0].paramsJson.at("enabled") != "false")
      return fail("ctl persistence capture");

    App ctlRestored;
    applyChain(ctlRestored, ctlSaved);
    if (ctlRestored.nodes.size() != 1 || !ctlRestored.nodes[0].processor ||
        ctlRestored.nodes[0].processor->backend() != ProcessorBackend::CTL)
      return fail("ctl persistence restore");

    const auto restoredCtlParams = ctlRestored.nodes[0].processor->parameters();
    bool restoredGain = false, restoredMode = false, restoredEnabled = false;
    for (const ProcessorParameter &param : restoredCtlParams) {
      if (param.id == "gain") {
        const double *v = std::get_if<double>(&param.value);
        restoredGain = v && *v == 1.5;
      } else if (param.id == "mode") {
        const int *v = std::get_if<int>(&param.value);
        restoredMode = v && *v == 1;
      } else if (param.id == "enabled") {
        const bool *v = std::get_if<bool>(&param.value);
        restoredEnabled = v && !*v;
      }
    }
    if (!restoredGain || !restoredMode || !restoredEnabled)
      return fail("ctl restored parameter values");

    Image ctlRestoredOut;
    if (!renderChain(ctlRestored, src, ctlRestoredOut, {}).ok || ctlRestoredOut.px != ctlSavedOut.px)
      return fail("ctl restored parameter render");

    auto cropIt = std::find_if(gPlugins.begin(), gPlugins.end(),
                               [](const PluginEntry &pe) { return pe.label == "Crop"; });
    if (cropIt == gPlugins.end()) return fail("bundled Crop plugin not found (ctl mixed)");
    const int cropIndex = (int)std::distance(gPlugins.begin(), cropIt);
    {
      App preview;
      graphPreviewSource(preview);
      const int before = renderState(preview).epoch;
      if (!addNode(preview, cropIndex) || !finishGraphPreview(preview, before, 0.125f) ||
          !testGraphEditWaits(false, ActiveGraphEdit::AddOfx, cropIndex) ||
          !testGraphEditWaits(true, ActiveGraphEdit::AddOfx, cropIndex))
        return fail("OFX add transaction and preview/export lifetime boundary");
      printf("ok  Production OFX/CTL add preview transactions and OFX lifetime barriers\n");
    }
    if (!testPreviewAfterExport(ExportPreviewWork::Active, ExportPreviewEnd::Resume, cropIndex))
      return fail("production export/preview with real OFX processor");
    printf("ok  Production export/preview with bundled OFX Crop\n");

    App ctlMixed;
    if (!addNode(ctlMixed, cropIndex) || !addCtlNode(ctlMixed, scriptPath.string()) ||
        !addNode(ctlMixed, cropIndex))
      return fail("OFX/CTL mixed node creation");
    if (!ctlMixed.nodes[0].processor->setParameterValue("crop", 40.0) ||
        !ctlMixed.nodes[2].processor->setParameterValue("crop", 40.0))
      return fail("OFX/CTL mixed crop setup");

    App ctlReference;
    if (!addNode(ctlReference, cropIndex) || !addNode(ctlReference, cropIndex) ||
        !ctlReference.nodes[0].processor->setParameterValue("crop", 40.0) ||
        !ctlReference.nodes[1].processor->setParameterValue("crop", 40.0))
      return fail("OFX/CTL mixed reference setup");

    Image ctlMixedOut, ctlReferenceOut;
    if (!renderChain(ctlMixed, src, ctlMixedOut, {}).ok ||
        !renderChain(ctlReference, src, ctlReferenceOut, {}).ok ||
        ctlMixedOut.w != ctlReferenceOut.w || ctlMixedOut.h != ctlReferenceOut.h)
      return fail("OFX/CTL mixed render");

    for (size_t i = 0; i + 3 < ctlReferenceOut.px.size(); i += 4) {
      for (int c = 0; c < 3; ++c)
        if (std::fabs(ctlMixedOut.px[i + c] - ctlReferenceOut.px[i + c] * 2.0f) > 1e-6f)
          return fail("OFX/CTL mixed RGB");
      if (ctlMixedOut.px[i + 3] != ctlReferenceOut.px[i + 3])
        return fail("OFX/CTL mixed alpha");
    }

    fs::remove(scriptPath);
    App ctlMissing;
    applyChain(ctlMissing, ctlSaved);
    if (ctlMissing.nodes.size() != 1 || ctlMissing.nodes[0].processor ||
        ctlMissing.nodes[0].storedBackend != "ctl" ||
        ctlMissing.nodes[0].storedIdentifier != ctlSaved.nodes[0].identifier)
      return fail("ctl missing script placeholder");

    // Syntax and import errors must surface CTL's own diagnostics rather than
    // its generic exception text ('Failed to load CTL module "module.<id>"',
    // 'Cannot find CTL function main.') or a stderr-only message.
    const fs::path badSyntaxPath = ctlDir / "BadSyntax.ctl";
    const fs::path badImportPath = ctlDir / "BadImport.ctl";
    {
      std::ofstream badSyntax(badSyntaxPath.string(), std::ios::binary);
      badSyntax << "void main(input varying float rIn {\n}\n";
      std::ofstream badImport(badImportPath.string(), std::ios::binary);
      badImport <<
          "import \"NoSuchModule\";\n"
          "void main(\n"
          "  input varying float rIn, input varying float gIn, input varying float bIn,\n"
          "  output varying float rOut, output varying float gOut, output varying float bOut)\n"
          "{\n"
          "  rOut = rIn; gOut = gIn; bOut = bIn;\n"
          "}\n";
      if (!badSyntax.good() || !badImport.good()) return fail("ctl error script test write");
    }
    std::string ctlError;
    if (CtlProcessor::create(badSyntaxPath.string(), &ctlError) || ctlError.rfind("BadSyntax.ctl:1: ", 0) != 0 ||
        ctlError.find("module.") != std::string::npos)
      return fail(("ctl syntax error message: " + ctlError).c_str());
    if (CtlProcessor::create(badImportPath.string(), &ctlError) ||
        ctlError.find("Cannot find CTL module \"NoSuchModule\"") == std::string::npos)
      return fail(("ctl import error message: " + ctlError).c_str());
    fs::remove(badSyntaxPath);
    fs::remove(badImportPath);

    fs::remove(libPath);
    fs::remove(ctlDir);
    printf("ok  Standard CTL processor\n");
  }

  // ART compatibility is an adapter on top of the standard CTL runtime. This
  // first seam proves ART_main execution, positional RGB channels, sibling
  // _artlib imports, ART scalar metadata/presentation, and normal Sidecar V2
  // persistence.
  {
    const fs::path artDir = fs::temp_directory_path() / "rawnode-selftest-art-ctl";
    fs::create_directories(artDir);
    const fs::path artLibPath = artDir / "_artlib.ctl";
    const fs::path artScriptPath = artDir / "ArtCompat.ctl";

    {
      std::ofstream lib(artLibPath.string(), std::ios::binary);
      lib << "float artScale(float x, float gain) { return x * gain; }\n";
      if (!lib.good()) return fail("ART CTL library test write");
    }
    {
      std::ofstream script(artScriptPath.string(), std::ios::binary);
      script <<
          "import \"_artlib\";\n"
          "void ART_main(\n"
          "  varying float R, varying float G, varying float B,\n"
          "  output varying float RR, output varying float GG, output varying float BB,\n"
          "  float gain, int mode, bool enabled)\n"
          "{\n"
          "  if (!enabled) { RR = R; GG = G; BB = B; }\n"
          "  else if (mode == 1) { RR = artScale(R, gain); GG = G; BB = B; }\n"
          "  else { RR = artScale(R, gain); GG = artScale(G, gain); BB = artScale(B, gain); }\n"
          "}\n";
      if (!script.good()) return fail("ART CTL script test write");
    }

    App artApp;
    if (!addCtlNode(artApp, artScriptPath.string()) || artApp.nodes.size() != 1 ||
        !artApp.nodes[0].processor || artApp.nodes[0].processor->backend() != ProcessorBackend::CTL)
      return fail("ART CTL processor creation");

    const auto artParams = artApp.nodes[0].processor->parameters();
    if (artParams.size() != 3 || artParams[0].id != "gain" || artParams[0].type != ParameterType::Double ||
        std::get<double>(artParams[0].defaultValue) != 0.0 ||
        artParams[1].id != "mode" || artParams[1].type != ParameterType::Integer ||
        std::get<int>(artParams[1].defaultValue) != 0 ||
        artParams[2].id != "enabled" || artParams[2].type != ParameterType::Boolean ||
        std::get<bool>(artParams[2].defaultValue))
      return fail("ART CTL scalar parameter fallback");

    Image artDefault;
    if (!renderChain(artApp, src, artDefault, {}).ok || artDefault.px != src.px)
      return fail("ART CTL default render");

    if (!artApp.nodes[0].processor->setParameterValue("gain", 2.0) ||
        !artApp.nodes[0].processor->setParameterValue("mode", 1) ||
        !artApp.nodes[0].processor->setParameterValue("enabled", true))
      return fail("ART CTL parameter set");

    Image artOut;
    if (!renderChain(artApp, src, artOut, {}).ok || artOut.px.size() != src.px.size())
      return fail("ART CTL render");
    for (size_t i = 0; i + 3 < src.px.size(); i += 4) {
      if (std::fabs(artOut.px[i + 0] - src.px[i + 0] * 2.0f) > 1e-6f ||
          artOut.px[i + 1] != src.px[i + 1] || artOut.px[i + 2] != src.px[i + 2] ||
          artOut.px[i + 3] != src.px[i + 3])
        return fail("ART CTL RGB/alpha result");
    }

    const PersistChain artSaved = captureChain(artApp);
    App artRestored;
    applyChain(artRestored, artSaved);
    if (artRestored.nodes.size() != 1 || !artRestored.nodes[0].processor ||
        artRestored.nodes[0].processor->backend() != ProcessorBackend::CTL)
      return fail("ART CTL persistence restore");
    Image artRestoredOut;
    if (!renderChain(artRestored, src, artRestoredOut, {}).ok || artRestoredOut.px != artOut.px)
      return fail("ART CTL restored render");

    // @ART-param metadata supplies defaults with ART's documented precedence:
    // metadata default, then the CTL default, then zero. Each layer is used by
    // one parameter here, and gain's metadata default must beat its CTL one.
    const fs::path artMetaPath = artDir / "ArtMeta.ctl";
    {
      std::ofstream script(artMetaPath.string(), std::ios::binary);
      script <<
          "// @ART-label: \"$CTL_META_TEST;ART metadata demo\"\n"
          "// @ART-param: [\"enabled\", \"Enabled\", true]\n"
          "// @ART-param: [\"gain\", \"$CTL_GAIN;Gain\", 0.0, 4.0, 1.5, 0.01, \"$CTL_TONE;Tone\", \"$CTL_GAIN_HELP;Gain amount\"]\n"
          "// @ART-param: [\"mode\", \"$CTL_MODE;Mode\", [[\"$CTL_ALL;All\", 0], [\"$CTL_RED_ONLY;Red only\", 3]], 3, \"$CTL_TONE;Tone\"]\n"
          "// @ART-param: [\"bias\", \"Bias\", -1.0, 1.0]\n"
          "// @ART-param: [\"steps\", \"Steps\", 0, 10]\n"
          "// @ART-preset: [\"boost\", \"$CTL_PRESET_BOOST;Boost\", {\"gain\": 2.0, \"mode\": 0, \"steps\": 8}]\n"
          "// @ART-preset: [\"disabled\", \"Disabled\", {\"enabled\": false}]\n"
          "// @ART-preset: [\"gainOnly\", \"Gain only\", {\"gain\": 2.0}]\n"
          "// @ART-preset: [\"empty\", \"Empty\", {}]\n"
          "void ART_main(\n"
          "  varying float r, varying float g, varying float b,\n"
          "  output varying float ro, output varying float go, output varying float bo,\n"
          "  int mode, bool enabled, float bias, float gain = 9.0, int steps = 4)\n"
          "{\n"
          "  if (!enabled) { ro = r; go = g; bo = b; }\n"
          "  else {\n"
          "    ro = r * gain + bias; go = g * gain; bo = b * steps / 4.0;\n"
          "    if (mode == 3) go = g;\n"
          "  }\n"
          "}\n";
      if (!script.good()) return fail("ART metadata script test write");
    }
    App artMeta;
    if (!addCtlNode(artMeta, artMetaPath.string())) return fail("ART metadata script load");
    {
      bool ok = artMeta.nodes[0].processor->displayName() == "ART metadata demo";
      int seen = 0;
      int groups = 0;
      const std::string toneGroup = "__art_group__:$CTL_TONE;Tone";
      for (const ProcessorParameter &param : artMeta.nodes[0].processor->parameters()) {
        if (param.type == ParameterType::Group) {
          ++groups;
          ok = ok && param.id == toneGroup && param.label == "Tone";
          continue;
        }
        if (param.id == "__rawnode_art_preset") {
          ok = ok && param.label == "Preset" && param.type == ParameterType::Choice && !param.persistValue &&
               param.choices == std::vector<std::string>({"(None)", "Boost", "Disabled", "Gain only", "Empty"}) &&
               param.choiceValues == std::vector<int>({0, 1, 2, 3, 4}) && std::get<int>(param.value) == 0;
          continue;
        }
        ++seen;
        if (param.id == "gain") {
          ok = ok && param.label == "Gain" && param.parent == toneGroup && param.hint == "Gain amount" &&
               param.hasRange && param.min == 0.0 && param.max == 4.0 && std::fabs(param.step - 0.01) < 1e-12 &&
               std::get<double>(param.defaultValue) == 1.5 && std::get<double>(param.value) == 1.5;
        } else if (param.id == "mode") {
          ok = ok && param.label == "Mode" && param.parent == toneGroup && param.type == ParameterType::Choice &&
               param.choices == std::vector<std::string>({"All", "Red only"}) &&
               param.choiceValues == std::vector<int>({0, 3}) && std::get<int>(param.defaultValue) == 3;
        } else if (param.id == "enabled") {
          ok = ok && param.label == "Enabled" && std::get<bool>(param.defaultValue);
        } else if (param.id == "bias") {
          ok = ok && param.hasRange && param.min == -1.0 && param.max == 1.0 &&
               std::get<double>(param.defaultValue) == 0.0;
        } else if (param.id == "steps") {
          ok = ok && param.hasRange && param.min == 0.0 && param.max == 10.0 && param.step == 1.0 &&
               std::get<int>(param.defaultValue) == 4;
        } else {
          ok = false;
        }
      }
      if (!ok || seen != 5 || groups != 1) return fail("ART @ART-param presentation metadata");

      // Controls follow @ART-param line order (not ART_main argument order),
      // and a group appears where its first member does, as in ART's panel.
      std::vector<std::string> order;
      for (const ProcessorParameter &param : artMeta.nodes[0].processor->parameters()) order.push_back(param.id);
      if (order != std::vector<std::string>({"__rawnode_art_preset", "enabled", toneGroup, "gain", "mode", "bias", "steps"}))
        return fail("ART @ART-param control order");
    }
    Image artMetaOut;
    if (!renderChain(artMeta, src, artMetaOut, {}).ok) return fail("ART metadata render");
    for (size_t i = 0; i + 3 < src.px.size(); i += 4) {
      if (std::fabs(artMetaOut.px[i] - src.px[i] * 1.5f) > 1e-6f || artMetaOut.px[i + 1] != src.px[i + 1] ||
          std::fabs(artMetaOut.px[i + 2] - src.px[i + 2]) > 1e-6f)
        return fail("ART metadata default render");
    }

    // Choice metadata can map menu indices to explicit CTL integer values.
    if (!artMeta.nodes[0].processor->setParameterValue("mode", 0))
      return fail("ART explicit choice value set");
    Image artChoiceOut;
    if (!renderChain(artMeta, src, artChoiceOut, {}).ok)
      return fail("ART explicit choice value render");
    for (size_t i = 0; i + 3 < src.px.size(); i += 4)
      if (std::fabs(artChoiceOut.px[i + 1] - src.px[i + 1] * 1.5f) > 1e-6f)
        return fail("ART explicit choice value result");

    // A non-default explicit choice value round-trips through Sidecar V2 as the
    // declared value (0), not a menu index, and restores the same render.
    {
      const PersistChain choiceSaved = captureChain(artMeta);
      if (choiceSaved.nodes[0].paramsJson.at("mode") != "0")
        return fail("ART explicit choice value Sidecar V2 capture");
      App choiceRestored;
      applyChain(choiceRestored, choiceSaved);
      bool restoredMode = false;
      if (choiceRestored.nodes.size() == 1 && choiceRestored.nodes[0].processor) {
        for (const ProcessorParameter &param : choiceRestored.nodes[0].processor->parameters())
          if (param.id == "mode") restoredMode = std::get<int>(param.value) == 0;
      }
      Image choiceRestoredOut;
      if (!restoredMode || !renderChain(choiceRestored, src, choiceRestoredOut, {}).ok ||
          choiceRestoredOut.px != artChoiceOut.px)
        return fail("ART explicit choice value Sidecar V2 restore");
    }

    if (!artMeta.nodes[0].processor->setParameterValue("mode", 3))
      return fail("ART explicit choice value restore");

    // @ART-preset is exposed as a transient dropdown (1 boost, 2 disabled,
    // 3 gainOnly, 4 empty). Applying a preset updates only its partial
    // parameter map; the selector shows the last explicitly chosen preset while
    // the values it maps still match, and Sidecar V2 stores only parameters.
    Processor &artPresets = *artMeta.nodes[0].processor;
    const auto presetValue = [&](const char *id) -> ParameterValue {
      for (const ProcessorParameter &param : artPresets.parameters())
        if (param.id == id) return param.value;
      return {};
    };
    const auto shownPreset = [&](Processor &processor) {
      for (const ProcessorParameter &param : processor.parameters())
        if (param.id == "__rawnode_art_preset") return std::get<int>(param.value);
      return -1;
    };
    if (shownPreset(artPresets) != 0) return fail("ART preset selector before any choice");

    // Partial map: parameters the preset does not name keep their values,
    // including a non-default edit.
    if (!artPresets.setParameterValue("bias", 1.0) || !artPresets.setParameterValue("__rawnode_art_preset", 1))
      return fail("ART preset apply");
    if (presetValue("gain") != ParameterValue(2.0) || presetValue("mode") != ParameterValue(0) ||
        presetValue("steps") != ParameterValue(8) || presetValue("bias") != ParameterValue(1.0) ||
        presetValue("enabled") != ParameterValue(true) || shownPreset(artPresets) != 1)
      return fail("ART partial preset map");
    // Editing a parameter the preset does not control keeps it shown.
    if (!artPresets.setParameterValue("bias", 0.0) || shownPreset(artPresets) != 1)
      return fail("ART preset selector after unrelated edit");

    Image artPresetOut;
    if (!renderChain(artMeta, src, artPresetOut, {}).ok) return fail("ART preset render");
    for (size_t i = 0; i + 3 < src.px.size(); i += 4) {
      if (std::fabs(artPresetOut.px[i + 0] - src.px[i + 0] * 2.0f) > 1e-6f ||
          std::fabs(artPresetOut.px[i + 1] - src.px[i + 1] * 2.0f) > 1e-6f ||
          std::fabs(artPresetOut.px[i + 2] - src.px[i + 2] * 2.0f) > 1e-6f)
        return fail("ART preset rendered result");
    }

    // Overlapping presets: gainOnly's map is a subset of boost's values, yet
    // the selector shows whichever was explicitly chosen.
    if (!artPresets.setParameterValue("__rawnode_art_preset", 3) || shownPreset(artPresets) != 3)
      return fail("ART overlapping preset shows gainOnly");
    if (!artPresets.setParameterValue("__rawnode_art_preset", 1) || shownPreset(artPresets) != 1)
      return fail("ART overlapping preset shows boost");
    // Editing a parameter the chosen preset controls shows "(None)".
    if (!artPresets.setParameterValue("steps", 7) || shownPreset(artPresets) != 0)
      return fail("ART preset selector after controlled edit");
    if (!artPresets.setParameterValue("steps", 8))
      return fail("ART preset controlled edit restore");
    // An empty preset changes nothing and is never shown as selected.
    if (!artPresets.setParameterValue("__rawnode_art_preset", 4) || shownPreset(artPresets) != 0 ||
        presetValue("gain") != ParameterValue(2.0) || presetValue("steps") != ParameterValue(8))
      return fail("ART empty preset");
    // Resetting the transient selector is a no-op: no parameter changes.
    if (artPresets.resetParameter("__rawnode_art_preset") || presetValue("gain") != ParameterValue(2.0) ||
        presetValue("steps") != ParameterValue(8) || presetValue("mode") != ParameterValue(0))
      return fail("ART preset selector reset must not reset parameters");

    {
      if (!artPresets.setParameterValue("__rawnode_art_preset", 1)) return fail("ART preset reapply");
      const PersistChain presetSaved = captureChain(artMeta);
      const auto &presetJson = presetSaved.nodes[0].paramsJson;
      if (presetJson.count("__rawnode_art_preset") != 0)
        return fail("ART preset selector must not be saved in Sidecar V2");
      if (presetJson.at("gain") != "2" || presetJson.at("mode") != "0" || presetJson.at("steps") != "8")
        return fail("ART preset Sidecar V2 capture");
      // Restored values are authoritative; the selector is not inferred.
      App presetRestored;
      applyChain(presetRestored, presetSaved);
      Image presetRestoredOut;
      if (presetRestored.nodes.size() != 1 || !presetRestored.nodes[0].processor ||
          shownPreset(*presetRestored.nodes[0].processor) != 0 ||
          !renderChain(presetRestored, src, presetRestoredOut, {}).ok || presetRestoredOut.px != artPresetOut.px)
        return fail("ART preset Sidecar V2 restore");
    }
    for (const char *id : {"gain", "mode", "steps", "enabled", "bias"})
      if (!artPresets.resetParameter(id)) return fail("ART parameter reset after preset tests");

    // Untouched parameters reach Sidecar V2 with ART's defaults, not zeros.
    // Restore the explicit choice default after the preset/reset tests.
    if (!artMeta.nodes[0].processor->setParameterValue("mode", 3))
      return fail("ART metadata mode restore after preset");
    const PersistChain artMetaSaved = captureChain(artMeta);
    const auto &metaJson = artMetaSaved.nodes[0].paramsJson;
    if (metaJson.at("gain") != "1.5" || metaJson.at("mode") != "3" || metaJson.at("enabled") != "true" ||
        metaJson.at("bias") != "0" || metaJson.at("steps") != "4")
      return fail("ART metadata defaults in Sidecar V2");
    App artMetaRestored;
    applyChain(artMetaRestored, artMetaSaved);
    Image artMetaRestoredOut;
    if (artMetaRestored.nodes.size() != 1 || !artMetaRestored.nodes[0].processor ||
        !renderChain(artMetaRestored, src, artMetaRestoredOut, {}).ok || artMetaRestoredOut.px != artMetaOut.px)
      return fail("ART metadata restored render");

    // ART's Adjuster widgets round scalar floats to the number of decimal
    // places implied by the GUI step, then clamp to the declared range. This
    // affects defaults, presets, direct edits and restored Sidecar V2 values.
    const fs::path artAdjusterPath = artDir / "ArtAdjuster.ctl";
    {
      std::ofstream script(artAdjusterPath.string(), std::ios::binary);
      script <<
          "// @ART-param: [\"gain\", \"Gain\", 0.0, 4.0, 0.697437, 0.01]\n"
          "// @ART-param: [\"mix\", \"Mix\", -1.0, 1.0, 0.0, 0.1]\n"
          "// @ART-param: [\"count\", \"Count\", 0, 10, 4]\n"
          "// @ART-param: [\"fine\", \"Fine\", 0.005, 1.0, 0.5, 0.01]\n"
          "// @ART-param: [\"derived\", \"Derived\", 0.0, 0.7, 0.35]\n"
          "// @ART-preset: [\"outside\", \"Outside\", {\"gain\": 9.0, \"mix\": 0.26, \"count\": 99}]\n"
          "void ART_main(varying float r, varying float g, varying float b,\n"
          "  output varying float ro, output varying float go, output varying float bo,\n"
          "  float gain, float mix, int count, float fine, float derived)\n"
          "{ ro = r * gain + mix; go = g * (fine + derived); bo = b * count / 4.0; }\n";
      if (!script.good()) return fail("ART adjuster normalisation script write");
    }
    App artAdjuster;
    if (!addCtlNode(artAdjuster, artAdjusterPath.string()))
      return fail("ART adjuster normalisation script load");
    Processor &adjuster = *artAdjuster.nodes[0].processor;
    const auto adjusterValue = [&](const char *id, bool defaults) -> ParameterValue {
      for (const ProcessorParameter &param : adjuster.parameters())
        if (param.id == id) return defaults ? param.defaultValue : param.value;
      return {};
    };
    if (adjusterValue("gain", true) != ParameterValue(0.7) ||
        adjusterValue("gain", false) != ParameterValue(0.7))
      return fail("ART adjuster default precision");
    if (!adjuster.setParameterValue("__rawnode_art_preset", 1) ||
        adjusterValue("gain", false) != ParameterValue(4.0) ||
        adjusterValue("mix", false) != ParameterValue(0.3) ||
        adjusterValue("count", false) != ParameterValue(10))
      return fail("ART preset rounding and clamping");
    if (!adjuster.setParameterValue("gain", 0.697437) ||
        !adjuster.setParameterValue("mix", -1.26) ||
        !adjuster.setParameterValue("count", -2) ||
        adjusterValue("gain", false) != ParameterValue(0.7) ||
        adjusterValue("mix", false) != ParameterValue(-1.0) ||
        adjusterValue("count", false) != ParameterValue(0))
      return fail("ART direct scalar rounding and clamping");
    // A bound finer than the step is shaped again after clamping, as in
    // Adjuster::getValue(): clamp(0) = 0.005, which rounds to 0.01.
    if (!adjuster.setParameterValue("fine", 0.0) ||
        adjusterValue("fine", false) != ParameterValue(0.01))
      return fail("ART scalar shaping after clamp");
    // ART derives decimal precision with std::pow(). The exact digit count
    // for a binary floating-point step can differ between libm implementations
    // (for example macOS vs Linux), so compute the expected value with ART's
    // own loop rather than hard-coding one platform's result.
    const double expectedStep = (0.7 - 0.0) / 100.0;
    int expectedDigits = 0;
    while (std::fabs(expectedStep * std::pow(10.0, expectedDigits) -
                     std::floor(expectedStep * std::pow(10.0, expectedDigits))) > 1e-12)
      ++expectedDigits;
    const double expectedScale = std::pow(10.0, expectedDigits);
    const auto shapeExpected = [&](double v) {
      const double shaped = std::round(v * expectedScale) / expectedScale;
      return std::isfinite(shaped) ? shaped : v;
    };
    double expectedDerived = shapeExpected(0.123456789);
    expectedDerived = std::clamp(expectedDerived, 0.0, 0.7);
    expectedDerived = shapeExpected(expectedDerived);
    bool derivedStep = false;
    for (const ProcessorParameter &param : adjuster.parameters())
      if (param.id == "derived") derivedStep = param.step == expectedStep;
    if (!derivedStep || !adjuster.setParameterValue("derived", 0.123456789) ||
        adjusterValue("derived", false) != ParameterValue(expectedDerived))
      return fail("ART scalar decimal places from derived step");

    const PersistChain adjusterSaved = captureChain(artAdjuster);
    App adjusterRestored;
    applyChain(adjusterRestored, adjusterSaved);
    if (adjusterRestored.nodes.size() != 1 || !adjusterRestored.nodes[0].processor)
      return fail("ART adjuster Sidecar V2 restore");
    bool restoredGain = false, restoredMix = false, restoredCount = false;
    for (const ProcessorParameter &param : adjusterRestored.nodes[0].processor->parameters()) {
      if (param.id == "gain") restoredGain = param.value == ParameterValue(0.7);
      else if (param.id == "mix") restoredMix = param.value == ParameterValue(-1.0);
      else if (param.id == "count") restoredCount = param.value == ParameterValue(0);
    }
    if (!restoredGain || !restoredMix || !restoredCount)
      return fail("ART adjuster Sidecar V2 normalisation");

    fs::remove(artAdjusterPath);
    fs::remove(artMetaPath);

    // Entry-point selection and ART contract errors.
    const auto artLoadError = [&](const char *name, const std::string &source) {
      const fs::path path = artDir / name;
      {
        std::ofstream script(path.string(), std::ios::binary);
        script << source;
      }
      std::string error;
      const bool loaded = CtlProcessor::create(path.string(), &error) != nullptr;
      fs::remove(path);
      return loaded ? std::string("<loaded>") : error;
    };
    const auto contains = [](const std::string &text, const char *part) { return text.find(part) != std::string::npos; };
    const std::string artRgb =
        "varying float r, varying float g, varying float b, "
        "output varying float ro, output varying float go, output varying float bo";
    {
      // A script defining both entry points uses standard main().
      const fs::path bothPath = artDir / "Both.ctl";
      {
        std::ofstream script(bothPath.string(), std::ios::binary);
        script << "void main(input varying float rIn, input varying float gIn, input varying float bIn,\n"
                  "  output varying float rOut, output varying float gOut, output varying float bOut)\n"
                  "{ rOut = rIn * 2.0; gOut = gIn; bOut = bIn; }\n"
                  "void ART_main(" << artRgb << ") { ro = r * 10.0; go = g; bo = b; }\n";
      }
      auto both = CtlProcessor::create(bothPath.string());
      Image bothOut;
      if (!both || !both->render(src, bothOut, {}).ok || std::fabs(bothOut.px[0] - src.px[0] * 2.0f) > 1e-6f)
        return fail("CTL main() preferred over ART_main()");
      fs::remove(bothPath);
    }
    if (artLoadError("Neither.ctl", "float f(float x) { return x; }\n") !=
        "CTL script defines neither main() nor ART_main()")
      return fail("CTL missing entry point error");
    if (!contains(artLoadError("FourOut.ctl", "void ART_main(" + artRgb + ", output varying float extra)"
                                              " { ro = r; go = g; bo = b; extra = r; }\n"),
                  "exactly three varying float RGB outputs"))
      return fail("ART exactly three outputs");
    if (!contains(artLoadError("TwoIn.ctl", "void ART_main(varying float r, varying float g,"
                                            " output varying float ro, output varying float go, output varying float bo)"
                                            " { ro = r; go = g; bo = g; }\n"),
                  "three varying float RGB inputs"))
      return fail("ART three inputs");
    if (!contains(artLoadError("Curve.ctl", "void ART_main(" + artRgb + ", float curve[4]) { ro = r * curve[0]; go = g; bo = b; }\n"),
                  "is a curve (float array); ART curve parameters are not supported yet"))
      return fail("ART curve parameter error");
    if (!contains(artLoadError("VaryingParam.ctl", "void ART_main(" + artRgb + ", varying float k) { ro = r * k; go = g; bo = b; }\n"),
                  "must be uniform, not varying"))
      return fail("ART varying parameter error");
    if (!contains(artLoadError("UnknownMeta.ctl", "// @ART-param: [\"nope\", \"Nope\", 0.0, 1.0, 0.5]\n"
                                                  "void ART_main(" + artRgb + ") { ro = r; go = g; bo = b; }\n"),
                  "@ART-param refers to unknown ART_main parameter nope"))
      return fail("ART unknown @ART-param error");
    if (!contains(artLoadError("BadMeta.ctl", "// @ART-param: [\"k\", \"K\", 0.0, 1.0, \"high\"]\n"
                                              "void ART_main(" + artRgb + ", float k) { ro = r * k; go = g; bo = b; }\n"),
                  "invalid @ART-param definition for k"))
      return fail("ART malformed @ART-param error");
    if (!contains(artLoadError("UnknownPreset.ctl",
                                            "// @ART-param: [\"k\", \"K\", 0.0, 2.0, 1.0]\n"
                                            "// @ART-preset: [\"bad\", \"Bad\", {\"missing\": 1.0}]\n"
                                            "void ART_main(" + artRgb + ", float k) { ro = r * k; go = g; bo = b; }\n"),
                  "@ART-preset refers to unknown ART_main parameter missing"))
      return fail("ART unknown @ART-preset parameter error");
    if (!contains(artLoadError("BadPreset.ctl",
                                            "// @ART-param: [\"k\", \"K\", 0.0, 2.0, 1.0]\n"
                                            "// @ART-preset: [\"bad\", \"Bad\", {\"k\": true}]\n"
                                            "void ART_main(" + artRgb + ", float k) { ro = r * k; go = g; bo = b; }\n"),
                  "invalid value for ART preset parameter k"))
      return fail("ART malformed @ART-preset value error");
    if (!contains(artLoadError("DuplicatePreset.ctl",
                               "// @ART-param: [\"k\", \"K\", 0.0, 2.0, 1.0]\n"
                               "// @ART-preset: [\"same\", \"First\", {\"k\": 0.5}]\n"
                               "// @ART-preset: [\"same\", \"Second\", {\"k\": 1.5}]\n"
                               "void ART_main(" + artRgb + ", float k) { ro = r * k; go = g; bo = b; }\n"),
                  "duplicate @ART-preset definition for same"))
      return fail("ART duplicate @ART-preset key error");
    for (const char *malformed : {
             "// @ART-preset: [\"bad\", \"Bad\", {\"k\": }]\n",          // invalid JSON
             "// @ART-preset: [\"bad\", \"Bad\", [0.5]]\n",               // map is not an object
             "// @ART-preset: [\"bad\", \"Bad\"]\n",                      // missing map
             "// @ART-preset: [\"bad\", 7, {\"k\": 0.5}]\n",              // label is not a string
             "// @ART-preset: {\"k\": 0.5}\n"}) {                        // not an array
      if (!contains(artLoadError("MalformedPreset.ctl",
                                 std::string("// @ART-param: [\"k\", \"K\", 0.0, 2.0, 1.0]\n") + malformed +
                                     "void ART_main(" + artRgb + ", float k) { ro = r * k; go = g; bo = b; }\n"),
                    "invalid @ART-preset definition"))
        return fail(("ART malformed @ART-preset definition error: " + std::string(malformed)).c_str());
    }
    if (!contains(artLoadError("NoLib.ctl", "import \"_artlib_missing\";\n"
                                            "void ART_main(" + artRgb + ") { ro = r; go = g; bo = b; }\n"),
                  "Cannot find CTL module \"_artlib_missing\""))
      return fail("ART missing library import error");

    fs::remove(artScriptPath);
    fs::remove(artLibPath);
    fs::remove(artDir);
    printf("ok  ART CTL entry point\n");
  }

  // Phase 4 mixed-backend seam: OFX -> native Exposure -> OFX must render,
  // persist, restore, and render identically through the generic interfaces.
  {
    auto cropIt = std::find_if(gPlugins.begin(), gPlugins.end(),
                               [](const PluginEntry &pe) { return pe.label == "Crop"; });
    if (cropIt == gPlugins.end()) return fail("bundled Crop plugin not found (mixed)");
    const int cropIndex = (int)std::distance(gPlugins.begin(), cropIt);

    // Crop changes the image size on both sides of the native node, so the
    // native processor must handle an input that is not the source size.
    App mixed;
    if (!addNode(mixed, cropIndex) || !addNativeExposureNode(mixed) || !addNode(mixed, cropIndex))
      return fail("mixed processor node creation");
    if (mixed.nodes.size() != 3 || !mixed.nodes[1].processor ||
        mixed.nodes[1].processor->backend() != ProcessorBackend::Native)
      return fail("mixed processor backend");

    const double exposureEv = 1.0 / 3.0;  // not representable in 6 decimals
    if (!mixed.nodes[0].processor->setParameterValue("crop", 40.0) ||
        !mixed.nodes[1].processor->setParameterValue("exposure", exposureEv) ||
        !mixed.nodes[2].processor->setParameterValue("crop", 40.0))
      return fail("mixed processor parameter set");

    // Reference: the same two crops without the native node.
    App cropOnly;
    if (!addNode(cropOnly, cropIndex) || !addNode(cropOnly, cropIndex) ||
        !cropOnly.nodes[0].processor->setParameterValue("crop", 40.0) ||
        !cropOnly.nodes[1].processor->setParameterValue("crop", 40.0))
      return fail("mixed reference chain");
    Image cropOut;
    if (!renderChain(cropOnly, src, cropOut, {}).ok || cropOut.w >= src.w || cropOut.h >= src.h)
      return fail("mixed reference render");

    Image mixedOut;
    ProcessorResult mixedResult = renderChain(mixed, src, mixedOut, {});
    if (!mixedResult.ok || mixedOut.w != cropOut.w || mixedOut.h != cropOut.h ||
        mixedOut.px.size() != cropOut.px.size())
      return fail("mixed processor render size");

    const float gain = (float)std::exp2(exposureEv);
    for (size_t i = 0; i + 3 < cropOut.px.size(); i += 4) {
      for (int c = 0; c < 3; ++c) {
        const float want = cropOut.px[i + c] * gain;
        if (std::fabs(mixedOut.px[i + c] - want) > 1e-6f * std::max(1.0f, std::fabs(want)))
          return fail("native exposure gain");
      }
      if (mixedOut.px[i + 3] != cropOut.px[i + 3]) return fail("native exposure alpha");
    }

    const PersistChain saved = captureChain(mixed);
    if (saved.nodes.size() != 3 || saved.nodes[1].backend != "native" ||
        saved.nodes[1].identifier != "rawnode.native.exposure")
      return fail("native exposure persistence capture");
    if (std::strtod(saved.nodes[1].paramsJson.at("exposure").c_str(), nullptr) != exposureEv)
      return fail("native exposure full-precision capture");

    App restored;
    applyChain(restored, saved);
    if (restored.nodes.size() != 3 || !restored.nodes[0].processor || !restored.nodes[1].processor ||
        !restored.nodes[2].processor || restored.nodes[1].processor->backend() != ProcessorBackend::Native)
      return fail("native exposure persistence restore");

    Image restoredOut;
    ProcessorResult restoredResult = renderChain(restored, src, restoredOut, {});
    if (!restoredResult.ok || restoredOut.w != mixedOut.w || restoredOut.h != mixedOut.h ||
        restoredOut.px != mixedOut.px)
      return fail("mixed processor restored render");

    printf("ok  Mixed OFX/Native processors\n");
  }

  // Generic processor/parameter seam: exercise the same Crop plugin through
  // Processor rather than touching OFX Param/Effect objects directly.
  {
    auto it = std::find_if(gPlugins.begin(), gPlugins.end(),
                           [](const PluginEntry &pe) { return pe.label == "Crop"; });
    if (it == gPlugins.end()) return fail("bundled Crop plugin not found (generic)");
    const int pluginIndex = (int)std::distance(gPlugins.begin(), it);
    auto processor = OfxProcessor::create(pluginIndex);
    if (!processor) return fail("OfxProcessor::create: Crop");

    bool foundCrop = false;
    for (const ProcessorParameter &param : processor->parameters()) {
      if (param.id != "crop") continue;
      foundCrop = param.type == ParameterType::Double;
      break;
    }
    if (!foundCrop) return fail("generic crop parameter missing");

    if (!processor->setParameterValue("crop", 80.0)) return fail("generic crop parameter set");
    Image out;
    ProcessorResult result = processor->render(src, out, {});
    if (!result.ok || out.w >= src.w || out.h >= src.h) return fail("generic Crop render");

    if (!processor->resetParameter("crop")) return fail("generic crop parameter reset");
    result = processor->render(src, out, {});
    if (!result.ok || out.w != src.w || out.h != src.h || out.px != src.px)
      return fail("generic Crop reset/render");

    printf("ok  Generic processor parameters\n");
  }

  // Bundled Crop plugin: defaults must be an identity pass-through with a
  // full-size RoD; the crop slider must shrink the RoD and change the rendered
  // output. Checked first so a flaky third-party plugin later in the list
  // cannot mask a regression here.
  {
    auto it = std::find_if(gPlugins.begin(), gPlugins.end(),
                           [](const PluginEntry &pe) { return pe.label == "Crop"; });
    if (it == gPlugins.end()) return fail("bundled Crop plugin not found");
    auto e = createInstance(*it);
    if (!e) return fail("createInstance: Crop");

    // At default (crop=0): RoD matches source size and render is identity.
    int ow = src.w, oh = src.h;
    queryOutputSize(it->plugin, e.get(), src.w, src.h, &ow, &oh);
    if (ow != src.w || oh != src.h) return fail("crop RoD at default != source size");
    Image out;
    out.w = src.w;
    out.h = src.h;
    out.px.assign(src.px.size(), -1.0f);
    if (renderEffect(it->plugin, e.get(), src.px.data(), out.px.data(), src.w, src.h, src.w, src.h, {}) != kOfxStatOK)
      return fail("render: Crop (defaults)");
    if (out.px != src.px) return fail("crop defaults are not identity");

    // At crop=80: RoD should shrink and render to the smaller output should differ.
    Param *crop = findParam(e.get(), "crop");
    if (!crop || crop->v.empty()) return fail("crop param missing");
    crop->v[0] = 80;
    ow = src.w; oh = src.h;
    queryOutputSize(it->plugin, e.get(), src.w, src.h, &ow, &oh);
    if (ow >= src.w || oh >= src.h) return fail("crop RoD did not shrink at 80%");
    out.w = ow;
    out.h = oh;
    out.px.assign((size_t)ow * oh * 4, -1.0f);
    if (renderEffect(it->plugin, e.get(), src.px.data(), out.px.data(), src.w, src.h, ow, oh, {}) != kOfxStatOK)
      return fail("render: Crop (zoomed)");
    bool finite = true, changed = false;
    for (float v : out.px) {
      finite &= std::isfinite(v);
      changed |= v != -1.0f;
    }
    if (!finite || !changed) return fail("crop zoom output");
    if (out.px == src.px) return fail("crop slider had no effect");

    // At crop=0 the window fills the source, so both pan ranges must fall back
    // to half the crop size (not zero). Panning ±100 should slide the window
    // past the source edge, producing black where no source data exists.
    crop->v[0] = 0;
    Param *offsetX = findParam(e.get(), "offsetX");
    Param *offsetY = findParam(e.get(), "offsetY");
    if (!offsetX || offsetX->v.empty()) return fail("offsetX param missing");
    if (!offsetY || offsetY->v.empty()) return fail("offsetY param missing");
    ow = src.w; oh = src.h;
    queryOutputSize(it->plugin, e.get(), src.w, src.h, &ow, &oh);
    out.w = ow; out.h = oh;
    out.px.assign((size_t)ow * oh * 4, -1.0f);
    offsetX->v[0] = 0; offsetY->v[0] = 0;
    if (renderEffect(it->plugin, e.get(), src.px.data(), out.px.data(), src.w, src.h, ow, oh, {}) != kOfxStatOK)
      return fail("render: Crop (centered)");
    for (float v : out.px)
      if (v == 0.0f) return fail("centered crop should have no black pixels");
    offsetY->v[0] = 100;
    std::fill(out.px.begin(), out.px.end(), -1.0f);
    if (renderEffect(it->plugin, e.get(), src.px.data(), out.px.data(), src.w, src.h, ow, oh, {}) != kOfxStatOK)
      return fail("render: Crop (Y offset)");
    changed = false;
    for (float v : out.px)
      changed |= v == 0.0f;
    if (!changed) return fail("Y offset produced no black fill");
    offsetX->v[0] = 100; offsetY->v[0] = 0;
    std::fill(out.px.begin(), out.px.end(), -1.0f);
    if (renderEffect(it->plugin, e.get(), src.px.data(), out.px.data(), src.w, src.h, ow, oh, {}) != kOfxStatOK)
      return fail("render: Crop (X offset)");
    changed = false;
    for (float v : out.px)
      changed |= v == 0.0f;
    if (!changed) return fail("X offset produced no black fill");
    callAction(it->plugin, kOfxActionDestroyInstance, e.get());
    printf("ok  Crop zoom\n");
  }

  for (auto &pe : gPlugins) {
    auto e = createInstance(pe);
    if (!e) return fail(("createInstance: " + pe.label).c_str());
    int ow = src.w, oh = src.h;
    queryOutputSize(pe.plugin, e.get(), src.w, src.h, &ow, &oh);
    Image out;
    out.w = ow;
    out.h = oh;
    out.px.assign((size_t)ow * oh * 4, -1.0f);
    const OfxStatus st = renderEffect(pe.plugin, e.get(), src.px.data(), out.px.data(), src.w, src.h, ow, oh, {});
    callAction(pe.plugin, kOfxActionDestroyInstance, e.get());
    if (st != kOfxStatOK) return fail(("render: " + pe.label).c_str());
    bool finite = true, touched = false;
    for (float v : out.px) {
      finite &= std::isfinite(v);
      touched |= v != -1.0f;
    }
    if (!finite || !touched) return fail(("output: " + pe.label).c_str());
    const fs::path dir = fs::temp_directory_path();
    bool written = true;
    for (const char *ext : {"png", "jpg"}) {
      const fs::path p = dir / ("ofxrawhost-selftest." + std::string(ext));
      written &= writeImage(out, p.string());
      fs::remove(p);
    }
    if (!written) return fail("export");
    printf("ok  %s\n", pe.label.c_str());
  }
  return 0;
}
