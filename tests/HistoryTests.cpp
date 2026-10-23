#include "HistoryTests.h"

#include "AppState.h"
#include "DocumentMutation.h"
#include "GraphCommands.h"
#include "GraphEvaluator.h"
#include "NodeGraph.h"
#include "RenderRuntimeTestAccess.h"
#include "persist/DocumentActionsPriv.h"
#include "ui/GraphLayout.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>

#define CHECK(expression) do { if (!(expression)) { \
  std::fprintf(stderr, "history check failed at line %d: %s\n", __LINE__, #expression); return false; \
} } while (false)

namespace {
namespace fs = std::filesystem;
struct Files {
  fs::path dir = fs::temp_directory_path() / ("rawnode-history-" +
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  Files() { fs::create_directory(dir); }
  ~Files() { std::error_code ignored; fs::remove_all(dir, ignored); }
  std::string path(const char *name) const { return (dir / name).string(); }
};
void write(const std::string &path, const std::string &body) {
  std::ofstream out(path, std::ios::binary); out << body; out.close();
  if (!out) throw std::runtime_error("Cannot write history fixture");
}
std::string bytes(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("Cannot read history fixture");
  return {std::istreambuf_iterator<char>(in), {}};
}
std::string photo(const Files &files, const char *name) {
  Image image; image.w = image.h = 1; image.px = {.25f, .5f, .75f, 1};
  const auto path = files.path(name);
  if (!writeImage(image, path, {RgbGamut::Rec709, TransferFunction::Linear})) throw std::runtime_error("Cannot write history image");
  return path;
}
double exposure(const App &app, const NodeId &id) {
  return std::get<double>(findNode(app, id)->processor->parameters()[0].value);
}
struct MutationCount { int completed = 0, deepest = 0; App *app; };
void observe(void *context, RenderRuntimeTestAccess::Event event) noexcept {
  auto &count = *static_cast<MutationCount *>(context);
  if (event != RenderRuntimeTestAccess::Event::MutationCompleted) return;
  ++count.completed;
  count.deepest = std::max(count.deepest, RenderRuntimeTestAccess::duringObservation(count.app->renderer).mutationDepth);
}

bool traversalAndDiscreteEdits() {
  Files files; App app;
  MutationCount count{0, 0, &app}; RenderRuntimeTestAccess::observe(app.renderer, &count, observe);
  const auto path = photo(files, "steps.png");
  CHECK(openPath(app, path));
  CHECK(app.history.size() == 1 && app.history.cursor() == 0 && !app.history.canUndo() && !app.history.canRedo());
  CHECK(count.deepest == 1); // Open/save/baseline never nest capture ownership.
  CHECK(addNativeExposureNode(app)); const auto first = *app.selectedNodeId();
  CHECK(app.history.size() == 2 && app.history.cursor() == 1);
  setNodeEnabled(app, first, false);
  CHECK(app.history.size() == 3 && !findNode(app, first)->enabled);
  CHECK(app.history.undo(app) && findNode(app, first)->enabled && app.history.cursor() == 1);
  CHECK(app.history.undo(app) && !findNode(app, first) && app.history.cursor() == 0);
  CHECK(!app.history.undo(app));
  CHECK(app.history.redo(app) && findNode(app, first) && app.history.size() == 3);
  CHECK(app.history.redo(app) && !findNode(app, first)->enabled && !app.history.redo(app));
  const auto size = app.history.size(), memory = app.history.bytes();
  setNodeEnabled(app, first, false);
  CHECK(graph::setDocumentOutput(app, app.documentOutput).ok());
  CHECK(setOutputEncoding(app, app.outputEncoding));
  findNode(app, first)->groupOpen["view-only"] = false;
  CHECK(app.history.recordCompletedEdit(app) && app.history.size() == size && app.history.bytes() == memory);
  CHECK(app.history.undo(app) && app.history.canRedo());
  CHECK(app.history.recordCompletedEdit(app) && app.history.canRedo()); // No-op does not discard redo.
  const auto branchCursor = app.history.cursor();
  CHECK(addNativeExposureNode(app)); const auto second = *app.selectedNodeId();
  CHECK(app.history.size() == branchCursor + 2);
  CHECK(!app.history.canRedo() && app.history.cursor() + 1 == app.history.size());
  CHECK(graph::setDocumentOutput(app, {first, ports::image}).ok());
  CHECK(app.history.undo(app) && app.documentOutput.nodeId == second);
  CHECK(app.history.redo(app) && app.documentOutput.nodeId == first);
  const ColorEncoding output{RgbGamut::DisplayP3, TransferFunction::Linear};
  CHECK(setOutputEncoding(app, output));
  CHECK(app.history.undo(app) && app.outputEncoding != output);
  CHECK(app.history.redo(app) && app.outputEncoding == output);
  CHECK(graph::disconnect(app, {second, std::nullopt, ports::image}).ok());
  CHECK(app.history.undo(app) && findNode(app, second)->fixedInputs.at(ports::image));
  CHECK(app.history.redo(app) && !findNode(app, second)->fixedInputs.at(ports::image));
  CHECK(count.deepest == 1);
  return true;
}

bool pasteReplaceAndAppend() {
  Files files; App app;
  CHECK(openPath(app, photo(files, "paste.png")) && addNativeExposureNode(app));
  const auto id = *app.selectedNodeId();
  PersistFragment fragment; std::vector<std::string> warnings;
  CHECK(captureNodeFragment(app, {id}, fragment, warnings));
  auto size = app.history.size();
  CHECK(pasteFragment(app, fragment) && app.history.size() == size + 1 && app.nodes.size() == 3);
  CHECK(app.history.undo(app) && app.nodes.size() == 2 && app.history.redo(app) && app.nodes.size() == 3);
  PersistGrade grade; CHECK(captureGrade(app, grade));
  size = app.history.size();
  CHECK(applyGrade(app, grade) && app.history.size() == size); // Unchanged Replace is a no-op.
  const auto preset = files.path("preset.rawnode.json");
  CHECK(savePreset(app, preset)); PersistGrade loaded; std::string error;
  CHECK(loadPresetFile(preset, loaded, error));
  CHECK(appendGrade(app, loaded) && app.history.size() == size + 1 && app.nodes.size() == 5);
  CHECK(app.history.undo(app) && app.nodes.size() == 3);
  CHECK(app.history.redo(app) && app.nodes.size() == 5);
  CHECK(applyGrade(app, loaded) && app.nodes.size() == 3 && app.history.size() == size + 2);
  CHECK(app.history.undo(app) && app.nodes.size() == 5);
  return true;
}

bool unrecordedEditsAndBounds() {
  Files files; App app;
  CHECK(openPath(app, photo(files, "bounds.png")) && addNativeExposureNode(app));
  const auto id = *app.selectedNodeId();
  app.history = GradeHistory{2}; CHECK(app.history.reset(app));
  MutationCount count{0, 0, &app}; RenderRuntimeTestAccess::observe(app.renderer, &count, observe);
  for (int value = 1; value <= 5; ++value) {
    for (int frame = 0; frame < 8; ++frame) CHECK(setNodeParameterValue(app, id, "exposure", double(value)));
    const auto before = count.completed;
    CHECK(app.history.hasUnrecordedEdit() && !app.history.undo(app) && !app.history.redo(app));
    CHECK(count.completed == before); // Pending gestures never capture or wait.
    CHECK(app.history.recordCompletedEdit(app)); // Future Stage 3 gesture completion.
    CHECK(count.completed == before + 1 && app.history.size() <= 3);
  }
  CHECK(app.history.size() == 3 && app.history.cursor() == 2);
  CHECK(app.history.undo(app) && exposure(app, id) == 4);
  CHECK(app.history.undo(app) && exposure(app, id) == 3 && !app.history.undo(app));
  CHECK(app.history.redo(app) && app.history.redo(app) && exposure(app, id) == 5 && !app.history.redo(app));
  const auto size = app.history.size(), cursor = app.history.cursor(), memory = app.history.bytes();
  CHECK(ui::moveGraphNode(app, id, {-10, 22}));
  CHECK(app.history.hasUnrecordedEdit() && !app.history.canUndo());
  CHECK(saveCurrentInputSidecar(app));
  CHECK(app.history.size() == size && app.history.cursor() == cursor && app.history.bytes() == memory && app.history.hasUnrecordedEdit());
  CHECK(app.history.recordCompletedEdit(app));
  CHECK(app.history.undo(app) && app.nodePositions.at(id.value).x != -10);
  CHECK(app.history.redo(app) && app.nodePositions.at(id.value).x == -10);
  // The production default retains a baseline plus exactly 100 undo steps.
  app.history = GradeHistory{}; CHECK(app.history.reset(app));
  for (int i = 0; i < 110; ++i) setNodeEnabled(app, id, !findNode(app, id)->enabled);
  CHECK(app.history.size() == 101 && app.history.cursor() == 100);
  for (int i = 0; i < 100; ++i) CHECK(app.history.undo(app));
  CHECK(!app.history.undo(app) && app.history.cursor() == 0);
  for (int i = 0; i < 100; ++i) CHECK(app.history.redo(app));
  CHECK(!app.history.redo(app) && app.history.cursor() == 100);
  return true;
}

bool measurementsAndMemoryLimit() {
  App sample;
  for (const int count : {0, 1, 16, 100}) {
    while (sample.nodes.size() < size_t(count + 1)) CHECK(addNativeExposureNode(sample));
    document_detail::GradeSnapshot snapshot;
    CHECK(document_detail::captureGradeSnapshot(sample, snapshot));
    std::printf("history measurement: %d native processors = %zu accounted bytes\n", count, GradeHistory::snapshotBytes(snapshot));
  }
  Files files; App app;
  CHECK(openPath(app, photo(files, "memory.png")) && addNativeExposureNode(app));
  const auto id = *app.selectedNodeId();
  const auto one = app.history.bytes();
  app.history = GradeHistory{100, one * 2}; CHECK(app.history.reset(app));
  for (int i = 0; i < 12; ++i) {
    setNodeEnabled(app, id, i % 2 != 0);
    CHECK(app.history.bytes() <= one * 2);
  }
  CHECK(app.history.size() < 13); // Memory bound, well below the step bound.
  const auto oldSize = app.history.size(), oldCursor = app.history.cursor(), oldBytes = app.history.bytes();
  findNode(app, id)->preservedParamsJson["opaque"] = jsonStringValue(std::string(2 * 1024 * 1024, 'x'));
  app.history.noteUnrecordedEdit();
  document_detail::GradeSnapshot large;
  CHECK(document_detail::captureGradeSnapshot(app, large));
  const auto largeBytes = GradeHistory::snapshotBytes(large);
  CHECK(largeBytes >= 2 * 1024 * 1024);
  std::printf("history measurement: native plus 2 MiB opaque payload = %zu accounted bytes\n", largeBytes);
  CHECK(!app.history.recordCompletedEdit(app) && app.history.hasUnrecordedEdit());
  CHECK(app.history.size() == oldSize && app.history.cursor() == oldCursor && app.history.bytes() == oldBytes);
  CHECK(!app.history.undo(app) && !app.history.redo(app));
  findNode(app, id)->preservedParamsJson.erase("opaque");
  CHECK(app.history.recordCompletedEdit(app) && !app.history.hasUnrecordedEdit());
  return true;
}

bool reconstructionAndFailure() {
  Files files; const auto script = files.path("gain.ctl");
  const std::string ctl = "void main(input varying float rIn, input varying float gIn, input varying float bIn,\n"
      "output varying float rOut, output varying float gOut, output varying float bOut,\n"
      "output varying float aOut, input varying float aIn = 1.0, input uniform float gain = 3.0)\n"
      "{ rOut=rIn*gain; gOut=gIn*gain; bOut=bIn*gain; aOut=aIn; }\n";
  write(script, ctl); App app;
  const auto path = photo(files, "rebuild.png");
  CHECK(openPath(app, path) && addNativeExposureNode(app)); const auto native = *app.selectedNodeId();
  CHECK(addCtlNode(app, script)); const auto id = *app.selectedNodeId();
  const auto sidecar = bytes(inputSidecarPath(path));
  CHECK(destroyNode(app, id) && !findNode(app, id));
  const auto size = app.history.size(), cursor = app.history.cursor(), memory = app.history.bytes();
  const auto *owner = findNode(app, native)->processor.get(); const auto *pixels = app.full.px.data();
  fs::remove(script);
  CHECK(!app.history.undo(app));
  CHECK(app.history.size() == size && app.history.cursor() == cursor && app.history.bytes() == memory);
  CHECK(findNode(app, native)->processor.get() == owner && app.full.px.data() == pixels && !findNode(app, id));
  CHECK(app.getStatus().find("Could not reconstruct working processor") != std::string::npos);
  write(script, ctl);
  CHECK(app.history.undo(app) && findNode(app, id)->processor->backend() == ProcessorBackend::CTL);
  CHECK(app.history.size() == size && app.history.cursor() == cursor - 1); // Replay never records recursively.
  {
    document_detail::DocumentMutation owned(app);
    auto samples = std::make_shared<Image>(); samples->w = samples->h = 1; samples->px = {.25f, -.5f, 2, .375f};
    const auto result = evaluateGraph({app.nodes, app.primarySource, app.documentOutput}, app.documentOutput,
                                     {EvaluationPurpose::Preview, 1, 1, 1, 1}, samples);
    CHECK(result.state == EvaluationState::Available);
    CHECK(std::get<ImageValue>(result.value)->px == (std::vector<float>{.75f, -1.5f, 6, .375f}));
  }
  CHECK(app.history.redo(app) && !findNode(app, id) && app.history.size() == size);
  CHECK(bytes(inputSidecarPath(path)) == sidecar);
  return true;
}

bool photoLifetimeAndGuards() {
  Files files; const auto a = photo(files, "a.png"), b = photo(files, "b.png");
  App app; CHECK(openPath(app, a) && addNativeExposureNode(app));
  const auto id = *app.selectedNodeId();
  CHECK(app.history.undo(app) && app.history.canRedo());
  const auto size = app.history.size(), cursor = app.history.cursor(), memory = app.history.bytes();
  CHECK(saveCurrentInputSidecar(app) && app.history.size() == size && app.history.cursor() == cursor && app.history.bytes() == memory);
  document_detail::ActionTestOptions failed;
  failed.decode = [](const std::string &, Image &, ColorEncoding &, bool &, ColorEncoding) { return false; };
  CHECK(!document_detail::openPhoto(app, b, {}, failed));
  CHECK(app.path == a && app.history.size() == size && app.history.cursor() == cursor && app.history.canRedo());
  failed = {}; failed.beforeSave = [] { return persist_detail::FileFault::Replace; };
  CHECK(!document_detail::openPhoto(app, b, {}, failed));
  CHECK(!document_detail::openFolder(app, files.dir.string(), {}, failed));
  CHECK(app.path == a && app.history.size() == size && app.history.cursor() == cursor && app.history.canRedo());
  // If an export guard regresses, the observer releases the worker at the
  // attempted drain so this test fails deterministically instead of hanging.
  struct ExportProbe { std::mutex mutex; std::condition_variable cv; bool release = false, waited = false; } probe;
  RenderRuntimeTestAccess::observe(app.renderer, &probe, [](void *context, auto event) noexcept {
    if (event != RenderRuntimeTestAccess::Event::MutationWaiting) return;
    auto &probe = *static_cast<ExportProbe *>(context);
    std::lock_guard<std::mutex> lock(probe.mutex);
    probe.waited = probe.release = true;
    probe.cv.notify_all();
  });
  {
    auto exporting = app.renderer.acquireExport(); CHECK(bool(exporting));
    exporting.start([&]() noexcept {
      std::unique_lock<std::mutex> lock(probe.mutex);
      probe.cv.wait(lock, [&] { return probe.release; });
    });
  }
  document_detail::GradeSnapshot untouched; untouched.workingProcessors["untouched"] = true;
  const bool captured = document_detail::captureGradeSnapshot(app, untouched);
  const bool undone = app.history.undo(app), redone = app.history.redo(app);
  const bool recorded = app.history.recordCompletedEdit(app);
  {
    std::lock_guard<std::mutex> lock(probe.mutex); probe.release = true; probe.cv.notify_all();
  }
  app.renderer.joinExport();
  RenderRuntimeTestAccess::observe(app.renderer, nullptr, nullptr);
  CHECK(!captured && !undone && !redone && !recorded && !probe.waited);
  CHECK(untouched.workingProcessors.count("untouched"));
  CHECK(app.history.size() == size && app.history.cursor() == cursor);
  CHECK(app.history.recordCompletedEdit(app) && app.history.canRedo());
  {
    document_detail::DocumentMutation owned(app);
    document_detail::GradeSnapshot untouched;
    CHECK(!document_detail::captureGradeSnapshot(app, untouched) && !app.history.redo(app));
  }
  CHECK(openPath(app, a) && app.history.size() == 1 && app.history.cursor() == 0 && !app.history.canRedo());
  CHECK(addNativeExposureNode(app) && openPath(app, b) && app.history.size() == 1);
  write(inputSidecarPath(a), "invalid");
  CHECK(openPath(app, a) && app.isPhotoProtected() && app.history.size() == 0);
  CHECK(!app.history.undo(app) && !app.history.redo(app) && !app.history.recordCompletedEdit(app));
  CHECK(openFolder(app, files.dir.string()) && app.history.size() == 0); // Auto-open protected a.png.
  CHECK(!findNode(app, id));
  return true;
}
}

bool testGradeHistory() {
  for (const auto &test : {std::make_pair("traversal, no-ops, branch truncation and discrete graph/colour edits", traversalAndDiscreteEdits),
       std::make_pair("node/grade paste and preset Replace/Append record one completed batch", pasteReplaceAndAppend),
       std::make_pair("unrecorded gestures, Save Grade independence and step bounds", unrecordedEditsAndBounds),
       std::make_pair("actual snapshot accounting, memory eviction and oversize recovery", measurementsAndMemoryLimit),
       std::make_pair("deleted CTL reconstruction, exact pixels and atomic failed replay", reconstructionAndFailure),
       std::make_pair("photo lifetime, cancelled/failed navigation, protection and capture/export guards", photoLifetimeAndGuards)}) {
    if (!test.second()) { std::fprintf(stderr, "history group failed: %s\n", test.first); return false; }
    std::printf("ok  Grade history: %s\n", test.first);
  }
  return true;
}
