#include "Export.h"

#include "AppState.h"
#include "NodeGraph.h"
#include "ui/Filmstrip.h"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;
using document_detail::RestoredPhotoGrade;

namespace {

std::string filename(const std::string &path) { return fs::u8path(path).filename().u8string(); }

bool sameFile(const std::string &left, const std::string &right) {
  if (left.empty() || right.empty()) return false;
  const auto a = fs::u8path(left), b = fs::u8path(right);
  std::error_code error;
  return a.lexically_normal() == b.lexically_normal() || fs::equivalent(a, b, error);
}

std::string nameKey(std::string name) {
  for (char &ch : name) if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
  return name;
}

bool isLivePhoto(const App &app, const std::string &path) {
  if (app.path.empty()) return false;
  const auto open = fs::u8path(app.path), selected = fs::u8path(path);
  if (open.lexically_normal() == selected.lexically_normal()) return true;
  // Sidecars are keyed by the directory entry's name. Distinct links to the
  // same image data can have independent grades.
  std::error_code error;
  const auto openParent = open.has_parent_path() ? open.parent_path() : fs::path(".");
  const auto selectedParent = selected.has_parent_path() ? selected.parent_path() : fs::path(".");
  if (!fs::equivalent(openParent, selectedParent, error)) return false;
  return open.filename() == selected.filename() ||
         (nameKey(open.filename().u8string()) == nameKey(selected.filename().u8string()) &&
          sameFile(inputSidecarPath(app.path), inputSidecarPath(path)));
}

std::vector<std::string> sourcePaths(const App &app) {
  std::vector<std::string> paths;
  paths.reserve(app.filmstrip.size() + 1);
  if (!app.path.empty()) paths.push_back(app.path);
  for (const auto &entry : app.filmstrip) paths.push_back(entry.path);
  return paths;
}

std::vector<ExportTarget> batchTargets(const std::vector<std::string> &paths,
                                       const std::string &folder, const char *extension) {
  std::unordered_map<std::string, size_t> stemCounts;
  for (const auto &path : paths) ++stemCounts[nameKey(fs::u8path(path).stem().u8string())];
  std::unordered_set<std::string> reserved;
  for (const auto &[stem, count] : stemCounts)
    if (count == 1) reserved.insert(stem + extension);

  std::vector<ExportTarget> targets;
  targets.reserve(paths.size());
  const auto directory = fs::u8path(folder);
  for (const auto &path : paths) {
    const auto sourceName = fs::u8path(path).filename();
    const auto stem = sourceName.stem().u8string();
    auto name = stem + extension;
    if (stemCounts.at(nameKey(stem)) > 1) {
      const auto base = sourceName.u8string();
      name = base + extension;
      for (size_t suffix = 2; !reserved.insert(nameKey(name)).second; ++suffix)
        name = base + "-" + std::to_string(suffix) + extension;
    }
    targets.push_back({path, (directory / fs::u8path(name)).u8string()});
  }
  return targets;
}

std::optional<ExportPhotoResult> checkDestination(const std::string &destination, ExistingExport existing,
                                                 const std::vector<std::string> &sources,
                                                 const std::vector<ExportTarget> &targets, size_t index) {
  std::error_code error;
  const auto status = fs::symlink_status(fs::u8path(destination), error);
  if (status.type() == fs::file_type::not_found && (!error || error == std::errc::no_such_file_or_directory))
    return std::nullopt;
  if (error) return ExportPhotoResult{ExportOutcome::Failed, "Could not inspect destination", {}};
  if (std::any_of(sources.begin(), sources.end(), [&](const auto &path) { return sameFile(path, destination); }))
    return ExportPhotoResult{ExportOutcome::Failed, "Cannot overwrite a source photograph", {}};
  const auto end = targets.begin() + index;
  if (std::any_of(targets.begin(), end, [&](const ExportTarget &target) {
        return sameFile(target.destination, destination);
      }))
    return ExportPhotoResult{ExportOutcome::Failed, "Destination aliases another export in this batch", {}};
  if (!fs::is_regular_file(status))
    return ExportPhotoResult{ExportOutcome::Failed, "Could not write image", {}};
  if (existing == ExistingExport::Skip)
    return ExportPhotoResult{ExportOutcome::Skipped, "Destination already exists", {}};
  return std::nullopt;
}

struct PhotoForExport {
  GraphView graph;
  const Image *liveSource;
  ColorEncoding input;
  ColorEncoding raw;
  ColorEncoding output;
};

PhotoForExport livePhoto(const App &app) {
  std::lock_guard<std::mutex> lock(app.colorMutex);
  return {{app.nodes, app.primarySource, app.documentOutput}, &app.full,
          app.inputEncoding, app.photoRawWorkingEncoding, app.outputEncoding};
}

ExportPhotoResult exportPhoto(App &app, const ExportTarget &target, PhotoForExport photo,
                              int jpegQuality, ExistingExport existing, const std::vector<std::string> &sources,
                              const std::vector<ExportTarget> &targets, size_t index) noexcept {
  ExportPhotoResult result;
  try {
    Image decoded;
    if (!photo.liveSource) {
      bool raw = false;
      if (!loadImage(target.source, decoded, photo.input, raw, photo.raw))
        return {ExportOutcome::Failed, "Could not decode photograph", {}};
    }
    const Image &source = photo.liveSource ? *photo.liveSource : decoded;
    const EvaluationContext context{EvaluationPurpose::Export, source.w, source.h, source.w, source.h};
    auto evaluated = app.renderer.evaluateOwned(photo.graph, context, ImageValue(&source, [](const Image *) {}));
    const auto *image = std::get_if<ImageValue>(&evaluated.value);
    for (const auto &diagnostic : evaluated.diagnostics) result.warnings.push_back(diagnostic.message);
    if (evaluated.state != EvaluationState::Available || !image || !*image) {
      if (evaluated.state == EvaluationState::Cancelled) result.message = "Export cancelled";
      else if (evaluated.state == EvaluationState::Unavailable) result.message = "Document output unavailable";
      return result;
    }

    const Image *pixels = image->get();
    Image converted;
    if (photo.graph.documentOutput == OutputRef{photo.graph.primarySource, ports::image} && photo.input != photo.output) {
      // With Source as Output the input encoding is known. Graded outputs
      // retain the editor's existing manual interpretation of graph pixels.
      if (!transformImageEncoding(*pixels, converted, photo.input, photo.output))
        return {ExportOutcome::Failed, "Unsupported source colour transform", {}};
      pixels = &converted;
    }
    if (auto blocked = checkDestination(target.destination, existing, sources, targets, index)) return *blocked;
    if (!writeImage(*pixels, target.destination, photo.output, jpegQuality))
      return {ExportOutcome::Failed, "Could not write image", {}};

    result.outcome = ExportOutcome::Exported;
    result.message = filename(target.destination) + " (" + std::to_string(pixels->w) + "×" +
                     std::to_string(pixels->h) + ")";
    if (photo.output.gamma == TransferFunction::DaVinciIntermediate)
      result.warnings.push_back("warning: ICC cannot fully represent DaVinci Intermediate scene values above 1.0; external apps may clip highlights");
  } catch (const std::exception &error) {
    try { result.message = error.what(); } catch (...) {}
  } catch (...) {
    try { result.message = "Unknown exception"; } catch (...) {}
  }
  return result;
}

void publishPhoto(App &app, const ExportPhotoResult &result) noexcept {
  try {
    std::string status = result.outcome == ExportOutcome::Exported ? "Exported" :
                         result.outcome == ExportOutcome::Skipped ? "Export skipped" : "Export failed";
    if (result.message == "Export cancelled") status = result.message;
    else if (!result.message.empty())
      status += (result.outcome == ExportOutcome::Exported ? " " : ": ") + result.message;
    for (const auto &warning : result.warnings) status += " — " + warning;
    app.setStatus(status);
  } catch (...) {}
}

bool directExport(App &app, const std::string &destination, bool async) {
  if (app.full.px.empty() || app.isPhotoProtected() || destination.empty() || app.renderer.exportInProgress())
    return false;
  auto lease = app.renderer.acquireExport();
  if (!lease) return false;
  try {
    if (sameFile(app.path, destination)) {
      app.setStatus("Export refused: Cannot overwrite the currently opened source photograph");
      return false;
    }
    auto sources = sourcePaths(app);
    if (std::any_of(sources.begin(), sources.end(), [&](const auto &path) { return sameFile(path, destination); })) {
      app.setStatus("Export refused: Cannot overwrite a source photograph");
      return false;
    }
    app.setStatus("Exporting full resolution...");
    auto photo = livePhoto(app);
    const int quality = app.jpegQuality;
    auto work = [&app, photo, quality, target = ExportTarget{app.path, destination},
                 sources = std::move(sources)]() noexcept {
      const auto result = exportPhoto(app, target, photo, quality, ExistingExport::Overwrite, sources, {}, 0);
      publishPhoto(app, result);
      return result.outcome == ExportOutcome::Exported;
    };
    if (!async) return work();
    lease.start(std::move(work));
    return true;
  } catch (const std::exception &error) {
    publishPhoto(app, {ExportOutcome::Failed, error.what(), {}});
  } catch (...) {
    publishPhoto(app, {ExportOutcome::Failed, "Unknown exception", {}});
  }
  return false;
}

} // namespace

ExportSelection selectedPhotographsForExport(const App &app) {
  ExportSelection selection;
  for (const auto &entry : app.filmstrip) {
    if (!app.filmstripSelection.count(entry.path)) continue;
    selection.paths.push_back(entry.path);
    if (!filmstripEntryVisible(app, entry)) ++selection.hidden;
  }
  return selection;
}

bool canExportCurrent(const App &app) {
  return !app.path.empty() && !app.full.px.empty() && !app.isPhotoProtected();
}

struct ExportSession::State {
  struct Active {
    explicit Active(App &app) : app(app), lease(app.renderer.acquireExport()) {}
    ~Active() { if (working) app.renderer.joinExport(); }

    App &app;
    RenderRuntime::Export lease;
    std::optional<RestoredPhotoGrade> restored;
    std::atomic<bool> finished{false};
    bool working = false;
    ExportPhotoResult result;
  };

  State(App &app, ExportScope scope, const std::string &destination, ExistingExport existing)
      : app(app), existing(existing), quality(app.jpegQuality), sources(sourcePaths(app)) {
    if (destination.empty() || app.renderer.exportInProgress()) return;
    if (scope == ExportScope::Selected)
      targets = batchTargets(selectedPhotographsForExport(app).paths, destination, app.exportFormat == 1 ? ".jpg" : ".png");
    else if (canExportCurrent(app))
      targets.push_back({app.path, destination});
    if (targets.empty()) return;

    results.reserve(targets.size());
    active = std::make_unique<Active>(app);
    if (!active->lease) active.reset();
  }

  PhotoForExport prepare(const std::string &path, std::vector<std::string> &warnings) {
    if (isLivePhoto(app, path)) {
      if (app.isPhotoProtected())
        throw std::runtime_error(app.persistenceWarning.empty() ? "The current grade is protected" : app.persistenceWarning);
      return livePhoto(app);
    }

    PersistFile file;
    std::string error;
    if (readGraphSidecar(inputSidecarPath(path), file, error, &warnings) == SidecarReadStatus::Error)
      throw std::runtime_error(error);
    if (file.grade) active->restored = document_detail::restorePhotoGrade(*file.grade);
    else {
      std::lock_guard<std::mutex> lock(app.colorMutex);
      active->restored = RestoredPhotoGrade{document_detail::sourceOnlyGraph(),
                                           app.rawWorkingEncoding, app.defaultOutputEncoding};
    }
    const auto &grade = *active->restored;
    return {{grade.graph.nodes, grade.graph.primarySource, grade.graph.output}, nullptr, {}, grade.raw, grade.output};
  }

  App &app;
  ExistingExport existing;
  int quality;
  std::vector<std::string> sources;
  std::vector<ExportTarget> targets;
  std::vector<ExportPhotoResult> results;
  // Retire the borrowed work first, then the plan and results.
  std::unique_ptr<Active> active;
};

ExportSession::ExportSession(App &app, ExportScope scope, const std::string &destination, ExistingExport existing)
    : state_(std::make_unique<State>(app, scope, destination, existing)) {}

ExportSession::~ExportSession() = default;
bool ExportSession::running() const { return bool(state_->active); }
const std::vector<ExportTarget> &ExportSession::targets() const { return state_->targets; }
const std::vector<ExportPhotoResult> &ExportSession::results() const { return state_->results; }

void ExportSession::advance() {
  auto &state = *state_;
  if (!state.active) return;
  auto &active = *state.active;
  if (active.working) {
    if (!active.finished.load(std::memory_order_acquire)) return;
    state.app.renderer.joinExport();
    active.working = false;
    active.restored.reset();
    state.results.push_back(std::move(active.result));
  } else if (!cancelRequested_ && state.results.size() < state.targets.size()) {
    const size_t index = state.results.size();
    const auto &target = state.targets[index];
    try {
      if (auto blocked = checkDestination(target.destination, state.existing, state.sources, state.targets, index)) {
        state.results.push_back(std::move(*blocked));
      } else {
        std::vector<std::string> warnings;
        const auto photo = state.prepare(target.source, warnings);
        active.finished.store(false, std::memory_order_relaxed);
        active.lease.startItem([&state, &active, index, photo, warnings = std::move(warnings)]() noexcept {
          active.result = exportPhoto(state.app, state.targets[index], photo, state.quality,
                                      state.existing, state.sources, state.targets, index);
          if (!warnings.empty()) {
            try { active.result.warnings.insert(active.result.warnings.end(), warnings.begin(), warnings.end()); }
            catch (...) {}
          }
          active.finished.store(true, std::memory_order_release);
        });
        active.working = true;
        return;
      }
    } catch (const std::exception &error) {
      active.restored.reset();
      state.results.push_back({ExportOutcome::Failed, error.what(), {}});
    } catch (...) {
      active.restored.reset();
      state.results.push_back({ExportOutcome::Failed, "Unknown exception", {}});
    }
  }

  if (cancelRequested_ || state.results.size() == state.targets.size()) {
    if (state.targets.size() == 1 && !state.results.empty()) publishPhoto(state.app, state.results.back());
    state.active.reset();
  }
}

bool runExportJob(App &app, const std::string &outPath) { return directExport(app, outPath, false); }
bool startExport(App &app, const std::string &outPath) { return directExport(app, outPath, true); }
