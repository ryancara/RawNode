#include "PhotoTrashTests.h"

#include "AppState.h"
#include "NodeGraph.h"
#include "RenderRuntimeTestAccess.h"
#include "persist/DocumentActionsPriv.h"
#include "ui/Filmstrip.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>

#define CHECK(expression) do { if (!(expression)) { \
  std::fprintf(stderr, "photo Trash check failed at line %d: %s\n", __LINE__, #expression); return false; \
} } while (false)

namespace {
namespace fs = std::filesystem;
using Decision = SaveFailureDecision;
using Fault = persist_detail::FileFault;
struct Files {
  fs::path dir = fs::temp_directory_path() /
      ("rawnode-trash-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::path held = dir / "controlled-moves";
  std::vector<std::string> calls;
  std::string fail;
  Files() { fs::create_directories(held); }
  ~Files() { std::error_code ignored; fs::remove_all(dir, ignored); }
  std::string path(const char *name) const { return (dir / name).string(); }
  std::string photo(const char *name) {
    const auto path = this->path(name);
    Image image; image.w = image.h = 1; image.px = {0.25f, 0.5f, 0.75f, 1};
    if (!writeImage(image, path, {RgbGamut::Rec709, TransferFunction::Linear})) throw std::runtime_error("fixture image");
    return path;
  }
  document_detail::ActionTestOptions options() {
    document_detail::ActionTestOptions options;
    options.moveToTrash = [&](const std::string &path, std::string &error) {
      // This seam can only rename this test's own files. It never calls native
      // Trash, including when a regression supplies an unrelated target.
      if (fs::path(path).parent_path() != dir) throw std::runtime_error("Trash escaped fixture");
      calls.push_back(path);
      if (path == fail) { error = "Injected native Trash failure"; return false; }
      std::error_code ec;
      fs::rename(path, held / fs::path(path).filename(), ec);
      error = ec.message();
      return !ec;
    };
    return options;
  }
};
void write(const std::string &path, const std::string &data) { std::ofstream(path, std::ios::binary) << data; }
std::string bytes(const fs::path &path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), {}};
}
void scan(App &app, const Files &files) { app.folderDir = files.dir.string(); refreshFilmstrip(app); }
PhotoTrashResult execute(App &app, const PhotoTrashRequest &request, Files &files, const SaveFailurePrompt &prompt = {}) {
  return document_detail::trashPhotographs(app, request, prompt, files.options());
}

bool confirmationEligibilityAndWording() {
  Files files;
  const auto a = files.photo("a.png"), b = files.photo("b.png"), missing = files.photo("missing.png");
  App app; scan(app, files);
  app.filmstripSelection = {a};
  const auto single = capturePhotoTrashRequest(app);
  CHECK(photoTrashConfirmationText(single) ==
      "Move 1 photograph to Trash?\n\n1 selected photograph. Associated RawNode sidecars will also be moved.");
  app.filmstripSelection = {a, b};
  CHECK(photoTrashConfirmationText(capturePhotoTrashRequest(app)) ==
      "Move 2 photographs to Trash?\n\n2 selected photographs. Associated RawNode sidecars will also be moved.");
  app.filmstripMarkFilter = PhotoMark::Pick;
  app.filmstrip[0].mark = PhotoMark::Pick;
  app.filmstrip[1].mark = PhotoMark::Reject;
  app.filmstrip[2].mark = PhotoMark::Reject;
  fs::rename(missing, files.path("moved-away.png"));
  const auto stale = files.path("not-in-filmstrip.png");
  write(stale, "unrelated file added after the scan");
  app.filmstripSelection = {a, b, missing, stale};
  const auto mixed = capturePhotoTrashRequest(app);
  CHECK(mixed.photographs.size() == 4 && mixed.hiddenCount == 1);
  CHECK(photoTrashConfirmationText(mixed) ==
      "Move 2 photographs to Trash?\n\n4 selected items: 2 eligible photographs, including 1 hidden by current filters; "
      "2 already ineligible. Associated RawNode sidecars will also be moved.");
  app.filmstripSelection = {a, missing, stale};
  const auto oneEligible = capturePhotoTrashRequest(app);
  CHECK(oneEligible.photographs.size() == 3 && oneEligible.hiddenCount == 0);
  CHECK(photoTrashConfirmationText(oneEligible) ==
      "Move 1 photograph to Trash?\n\n3 selected items: 1 eligible photograph; "
      "2 already ineligible. Associated RawNode sidecars will also be moved.");
  app.filmstripSelection = {missing};
  const auto ineligibleOnly = capturePhotoTrashRequest(app);
  CHECK(ineligibleOnly.hiddenCount == 0);
  CHECK(photoTrashConfirmationText(ineligibleOnly) ==
      "Move 0 photographs to Trash?\n\n1 selected item: 0 eligible photographs; "
      "1 already ineligible. Associated RawNode sidecars will also be moved.");
  // Wording is based on the immutable capture. Execution still processes and
  // reports the two ineligible entries; it cannot substitute the new selection.
  app.filmstripSelection = {stale};
  const auto result = execute(app, mixed, files);
  CHECK(result.moved == 2 && result.failed == 2 && result.errors.size() == 2);
  CHECK(files.calls == (std::vector<std::string>{a, b}) && bytes(stale) == "unrelated file added after the scan");
  PhotoTrashResult singleResult; singleResult.moved = 1;
  CHECK(singleResult.message() == "Moved 1 photograph to Trash; 0 failed.");
  return true;
}

bool sidecarsAndIndependentDocument() {
  Files files;
  const auto a = files.photo("a.png"), b = files.photo("b.png"), c = files.photo("c.png");
  const auto d = files.photo("d.png");
  App app; scan(app, files); CHECK(openPath(app, a));
  CHECK(addNativeExposureNode(app));
  const auto preview = app.preview;
  const auto pixels = app.full.px.data();
  const auto node = app.nodes.back().processor.get();
  const auto historySize = app.history.size(), cursor = app.history.cursor(), historyBytes = app.history.bytes();
  const auto runtime = RenderRuntimeTestAccess::snapshot(app.renderer);
  const auto sidecarA = bytes(inputSidecarPath(a));
  // Invalid/newer-version/protected sidecars are opaque files to this action.
  write(inputSidecarPath(b), "invalid JSON, retain exact bytes");
  write(legacyInputSidecarPath(b), "legacy, also opaque");
  write(inputSidecarPath(c), R"({"format":"rawnode","version":999,"unknown":true})");
  const auto cBytes = bytes(inputSidecarPath(c));
  write(files.path("b.rawnode.json"), "unrelated stem-only sidecar");
  fs::permissions(inputSidecarPath(b), fs::perms::owner_read);
  app.filmstripSelection = {b, c, d}; app.filmstripAnchor = d;
  app.filmstripMarkFilter = PhotoMark::Pick;
  app.filmstrip[1].mark = PhotoMark::Pick;
  // A surviving completed thumbnail should not be queued for another decode.
  app.filmstrip[0].thumbPending = false; app.filmstrip[0].thumbFailed = true;
  app.filmstrip[0].tw = 17; app.filmstrip[0].th = 11;
  app.thumbQueue.push_back(b); app.thumbQueued.insert(b);
  app.thumbReady.push_back({b, {}, 0, 0, 256, ThumbReady::Kind::Fail});
  const auto generation = app.filmstripGen.load();
  const auto request = capturePhotoTrashRequest(app);
  CHECK(request.photographs.size() == 3 && request.hiddenCount == 2);
  const auto text = photoTrashConfirmationText(request);
  CHECK(text.find("3 photographs") != std::string::npos && text.find("2 hidden") != std::string::npos);
  CHECK(text.find("Associated RawNode sidecars") != std::string::npos);
  // Selection/filter changes after confirmation must not substitute new paths.
  app.filmstripSelection = {a, b, c, d}; app.filmstripMarkFilter = PhotoMark::Reject;
  const auto result = execute(app, request, files);
  CHECK(result.moved == 3 && result.failed == 0 && result.errors.empty());
  CHECK(files.calls == (std::vector<std::string>{b, inputSidecarPath(b), legacyInputSidecarPath(b), c, inputSidecarPath(c), d}));
  CHECK(bytes(files.held / fs::path(inputSidecarPath(b)).filename()) == "invalid JSON, retain exact bytes");
  CHECK(bytes(files.held / fs::path(inputSidecarPath(c)).filename()) == cBytes);
  CHECK(bytes(files.path("b.rawnode.json")) == "unrelated stem-only sidecar");
  CHECK(app.path == a && app.preview == preview && app.full.px.data() == pixels && app.nodes.back().processor.get() == node);
  CHECK(app.history.size() == historySize && app.history.cursor() == cursor && app.history.bytes() == historyBytes);
  const auto after = RenderRuntimeTestAccess::snapshot(app.renderer);
  CHECK(after.epoch == runtime.epoch && after.previewPending == runtime.previewPending);
  CHECK(bytes(inputSidecarPath(a)) == sidecarA);
  CHECK(app.filmstripSelection == (std::unordered_set<std::string>{a}) && app.filmstripAnchor.empty());
  CHECK(app.filmstrip.size() == 1 && app.filmstripIndex == 0 && app.filmstripMarkFilter == PhotoMark::Reject);
  CHECK(!app.filmstrip[0].thumbPending && app.filmstrip[0].thumbFailed && app.filmstrip[0].tw == 17);
  CHECK(app.filmstripGen == generation + 1 && app.thumbQueue.empty() && app.thumbQueued.empty() && app.thumbReady.empty());
  return true;
}

bool partialFailures() {
  Files files;
  const auto a = files.photo("a.png"), b = files.photo("b.png"), c = files.photo("c.png");
  write(inputSidecarPath(a), "keep on photo failure");
  write(inputSidecarPath(b), "orphan must remain");
  App app; scan(app, files);
  app.filmstripSelection = {a, b, c}; app.filmstripAnchor = a;
  auto options = files.options(); const auto move = options.moveToTrash;
  options.moveToTrash = [&](const std::string &path, std::string &error) {
    if (path == a || path == inputSidecarPath(b)) { files.calls.push_back(path); error = "injected"; return false; }
    return move(path, error);
  };
  const auto result = document_detail::trashPhotographs(app, capturePhotoTrashRequest(app), {}, options);
  CHECK(result.moved == 2 && result.failed == 1 && result.errors.size() == 2);
  CHECK(files.calls == (std::vector<std::string>{a, b, inputSidecarPath(b), c}));
  CHECK(bytes(inputSidecarPath(a)) == "keep on photo failure" && bytes(inputSidecarPath(b)) == "orphan must remain");
  CHECK(result.message().find("sidecar remains at " + inputSidecarPath(b)) != std::string::npos);
  CHECK(app.filmstripSelection == (std::unordered_set<std::string>{a}) && app.filmstripAnchor == a);
  CHECK(app.filmstrip.size() == 1 && !fs::exists(b) && !fs::exists(c));
  // Retrying cannot act on the orphan via an absent photograph.
  app.filmstripSelection = {b};
  const auto absent = capturePhotoTrashRequest(app);
  CHECK(absent.photographs.size() == 1 && !absent.photographs.front().error.empty());
  CHECK(execute(app, absent, files).failed == 1);
  CHECK(bytes(inputSidecarPath(b)) == "orphan must remain");
  return true;
}

bool changedAndSymlinkPaths() {
  Files files;
  const auto a = files.photo("a.png"), b = files.photo("b.png"), c = files.photo("c.png");
  App app; scan(app, files); app.filmstripSelection = {a, b, c};
  const auto request = capturePhotoTrashRequest(app);
  fs::rename(a, files.path("original-a.png")); files.photo("a.png");
  fs::rename(b, files.path("original-b.png"));
  std::error_code ec; fs::create_symlink(files.path("original-b.png"), b, ec);
  if (ec) write(b, "substituted file");
  fs::rename(c, files.path("original-c.png"));
  const auto result = execute(app, request, files);
  CHECK(result.moved == 0 && result.failed == 3 && files.calls.empty());
  CHECK(fs::exists(a) && fs::exists(files.path("original-a.png")) && fs::exists(files.path("original-b.png")));
  CHECK(!app.filmstripSelection.count(c));
  // A sidecar symlink is left in place after the independent photograph moves.
  const auto d = files.photo("d.png");
  write(files.path("unrelated.json"), "must not follow");
  fs::create_symlink(files.path("unrelated.json"), inputSidecarPath(d), ec);
  if (!ec) {
    refreshFilmstrip(app); app.filmstripSelection = {d};
    const auto linked = execute(app, capturePhotoTrashRequest(app), files);
    CHECK(linked.moved == 1 && linked.errors.size() == 1);
    CHECK(fs::is_symlink(inputSidecarPath(d)) && bytes(files.path("unrelated.json")) == "must not follow");
  }
  // A different current folder cannot inherit a confirmed operation.
  refreshFilmstrip(app); app.filmstripSelection = {a};
  const auto old = capturePhotoTrashRequest(app); Files other; scan(app, other);
  const auto before = files.calls.size();
  CHECK(execute(app, old, files).failed == 1 && files.calls.size() == before && fs::exists(a));
  // Same spelling, different folder: membership alone cannot authorize its files.
  const auto folder = files.dir / "folder";
  fs::create_directory(folder);
  const auto nested = (folder / "nested.png").string(); fs::rename(a, nested);
  app.folderDir = folder.string(); refreshFilmstrip(app); app.filmstripSelection = {nested};
  const auto folderRequest = capturePhotoTrashRequest(app);
  fs::rename(folder, files.dir / "old-folder"); fs::create_directory(folder);
  write(nested, "replacement folder photograph");
  CHECK(execute(app, folderRequest, files).failed == 1 && files.calls.size() == before);
  CHECK(bytes(nested) == "replacement folder photograph");
  // Editing a source in place after confirmation also invalidates the snapshot.
  app.folderDir = files.dir.string(); refreshFilmstrip(app);
  const auto edited = files.photo("edited.png"); refreshFilmstrip(app); app.filmstripSelection = {edited};
  const auto editRequest = capturePhotoTrashRequest(app); write(edited, "modified in place");
  CHECK(execute(app, editRequest, files).failed == 1 && files.calls.size() == before);
  CHECK(bytes(edited) == "modified in place");
  return true;
}

bool currentPhotoTransitions() {
  for (const bool failure : {false, true}) {
    Files files; const auto a = files.photo("a.png"), b = files.photo("b.png");
    App app; scan(app, files); CHECK(openPath(app, a)); CHECK(addNativeExposureNode(app));
    app.filmstripSelection = {a}; app.filmstripAnchor = a;
    const auto preview = app.preview; const auto gradeSize = app.history.size();
    if (failure) files.fail = a;
    const auto result = execute(app, capturePhotoTrashRequest(app), files);
    CHECK(result.moved == (failure ? 0 : 1) && result.failed == (failure ? 1 : 0));
    if (failure) {
      CHECK(app.path == a && app.preview == preview && app.history.size() == gradeSize);
      CHECK(app.nodes.size() == 2 && saveCurrentInputSidecar(app));
      CHECK(files.calls == (std::vector<std::string>{a}));
    } else {
      CHECK(app.path == b && app.filmstripIndex == 0 && app.history.size() == 1);
      CHECK(!fs::exists(inputSidecarPath(a)) && saveCurrentInputSidecar(app));
      CHECK(!fs::exists(inputSidecarPath(a)) && app.filmstripSelection.empty());
    }
  }
  // A native error after actual movement still retires a disappeared source,
  // while obeying the failure contract: no sidecar is moved.
  {
    Files partial; const auto a = partial.photo("a.png"), b = partial.photo("b.png");
    App app; scan(app, partial); CHECK(openPath(app, a)); app.filmstripSelection = {a};
    auto options = partial.options(); const auto move = options.moveToTrash;
    options.moveToTrash = [&](const std::string &path, std::string &error) {
      if (!move(path, error)) return false;
      error = "Injected failure after native movement"; return false;
    };
    const auto result = document_detail::trashPhotographs(app, capturePhotoTrashRequest(app), {}, options);
    CHECK(result.moved == 0 && result.failed == 1 && app.path == b);
    CHECK(partial.calls == (std::vector<std::string>{a}) && fs::exists(inputSidecarPath(a)));
    const auto orphan = bytes(inputSidecarPath(a));
    CHECK(saveCurrentInputSidecar(app) && bytes(inputSidecarPath(a)) == orphan);
  }
  // Last-photo removal retires source, saved grade, history and presentation.
  Files files; const auto a = files.photo("last.png");
  App app; scan(app, files); CHECK(openPath(app, a)); CHECK(addNativeExposureNode(app));
  app.displayRGBA = {1, 2, 3, 4}; app.display = {1, 1};
  app.filmstripSelection = {a};
  files.fail = inputSidecarPath(a);
  const auto result = execute(app, capturePhotoTrashRequest(app), files);
  CHECK(result.moved == 1 && result.failed == 0 && result.errors.size() == 1);
  CHECK(app.path.empty() && app.full.px.empty() && !app.preview && app.history.size() == 0);
  CHECK(app.nodes.size() == 1 && app.documentOutput.nodeId == app.primarySource);
  CHECK(app.filmstrip.empty() && app.filmstripIndex == -1 && app.filmstripSelection.empty());
  CHECK(app.displayRGBA.empty() && app.display.w == 0 && !app.processed.request);
  const auto orphan = bytes(inputSidecarPath(a));
  CHECK(preparePhotoDeparture(app) && saveCurrentInputSidecar(app));
  CHECK(bytes(inputSidecarPath(a)) == orphan);
  app.renderer.start(); CHECK(RenderRuntimeTestAccess::wait(app.renderer, [](auto state) { return !state.previewPending && !state.previewBusy; }));
  const auto next = files.photo("next.png"); CHECK(openPath(app, next) && app.path == next);
  CHECK(bytes(inputSidecarPath(a)) == orphan);
  return true;
}

bool saveDecisionsAndExport() {
  for (const auto decision : {Decision::Cancel, Decision::Discard, Decision::Retry}) {
    Files files; const auto a = files.photo("a.png"), b = files.photo("b.png");
    App app; scan(app, files); CHECK(openPath(app, a)); app.filmstripSelection = {a, b};
    const auto sidecar = bytes(inputSidecarPath(a)); const auto preview = app.preview;
    auto options = files.options(); int saves = 0, prompts = 0;
    options.beforeSave = [&] { return saves++ == 0 ? Fault::Replace : Fault::None; };
    const auto result = document_detail::trashPhotographs(app, capturePhotoTrashRequest(app),
        [&](const std::string &) { ++prompts; return decision; }, options);
    CHECK(prompts == 1 && saves == (decision == Decision::Retry ? 2 : 1));
    if (decision == Decision::Cancel) {
      CHECK(result.cancelled && files.calls.empty() && fs::exists(a) && fs::exists(b));
      CHECK(app.path == a && app.preview == preview && app.filmstripSelection.size() == 2);
      CHECK(bytes(inputSidecarPath(a)) == sidecar);
    } else CHECK(result.moved == 2 && app.path.empty() && !fs::exists(inputSidecarPath(a)));
  }
  Files files; const auto a = files.photo("a.png"), b = files.photo("b.png");
  App app; scan(app, files); CHECK(openPath(app, a));
  {
    auto exporting = app.renderer.acquireExport(); CHECK(exporting);
    app.filmstripSelection = {a, b};
    const auto result = execute(app, capturePhotoTrashRequest(app), files);
    CHECK(result.moved == 0 && result.failed == 2 && files.calls.empty());
    CHECK(fs::exists(a) && fs::exists(b) && app.path == a);
    app.filmstripSelection = {b};
    CHECK(execute(app, capturePhotoTrashRequest(app), files).moved == 1);
    CHECK(app.path == a && app.renderer.exportInProgress());
  }
  app.filmstripSelection = {a};
  CHECK(execute(app, capturePhotoTrashRequest(app), files).moved == 1 && app.path.empty());
  return true;
}

// Test-owned barriers release on failure and never rely on sleeps.
struct Gate {
  std::mutex mutex;
  std::condition_variable cv;
  int entered = 0, released = 0;
  void hold(int turn) {
    std::unique_lock<std::mutex> lock(mutex);
    entered = turn; cv.notify_all();
    cv.wait_for(lock, std::chrono::seconds(3), [&] { return released >= turn; });
  }
  bool wait(int turn) {
    std::unique_lock<std::mutex> lock(mutex);
    return cv.wait_for(lock, std::chrono::seconds(2), [&] { return entered >= turn; });
  }
  void release(int turn) { std::lock_guard<std::mutex> lock(mutex); released = turn; cv.notify_all(); }
};

bool previewAndThumbnailLifetime() {
  Files files; const auto a = files.photo("a.png"), b = files.photo("b.png");
  Gate previewGate, thumbnailGate;
  App app; scan(app, files); CHECK(openPath(app, a));
  struct Guard {
    App &app; Gate &preview; Gate &thumbnail;
    ~Guard() {
      preview.release(100); thumbnail.release(100);
      app.quit = true; app.thumbCv.notify_all();
      if (app.thumbThread.joinable()) app.thumbThread.join();
      app.renderer.shutdown();
    }
  } guard{app, previewGate, thumbnailGate};
  int loads = 0;
  app.thumbThread = startFilmstripThumbThread(&app,
      [&](const std::string &, int, std::vector<unsigned char> &, int &, int &) {
        thumbnailGate.hold(++loads); return false;
      });
  requestFilmstripThumb(app, 0, true); CHECK(thumbnailGate.wait(1));
  app.filmstripSelection = {b};
  CHECK(execute(app, capturePhotoTrashRequest(app), files).moved == 1);
  CHECK(!app.filmstrip[0].thumbLoading && app.filmstrip[0].thumbPending);
  requestFilmstripThumb(app, 0, true); // New generation, same path as the blocked job.
  thumbnailGate.release(1); CHECK(thumbnailGate.wait(2));
  {
    std::lock_guard<std::mutex> lock(app.thumbMutex);
    CHECK(app.thumbReady.empty()); // Old success/failure/cancellation cannot publish.
  }
  CHECK(app.filmstrip[0].thumbLoading && !app.filmstrip[0].thumbFailed);
  thumbnailGate.release(100);

  RenderRuntimeTestAccess::evaluation(app.renderer, nullptr, [](void *context) {
    static_cast<Gate *>(context)->hold(1);
  }, &previewGate);
  RenderRuntimeTestAccess::observe(app.renderer, &previewGate, [](void *context, auto event) noexcept {
    if (event == RenderRuntimeTestAccess::Event::MutationWaiting) static_cast<Gate *>(context)->release(100);
  });
  app.renderer.start(); CHECK(previewGate.wait(1));
  auto options = files.options(); const auto move = options.moveToTrash;
  bool owned = true;
  options.moveToTrash = [&](const std::string &path, std::string &error) {
    const auto state = RenderRuntimeTestAccess::snapshot(app.renderer);
    owned &= state.mutationDepth > 0 && !state.previewBusy && !state.exportBusy;
    return move(path, error);
  };
  app.filmstripSelection = {a};
  const auto result = document_detail::trashPhotographs(app, capturePhotoTrashRequest(app), {}, options);
  CHECK(owned && result.moved == 1 && app.path.empty());
  return true;
}

bool protectedAndFailedReplacement() {
  Files files; const auto a = files.photo("a.png"), b = files.photo("b.png");
  write(inputSidecarPath(a), "malformed protected grade");
  App app; scan(app, files); CHECK(openPath(app, a) && app.isPhotoProtected());
  app.filmstripSelection = {a};
  auto options = files.options();
  options.decode = [](const std::string &, Image &, ColorEncoding &, bool &, ColorEncoding) { return false; };
  int prompts = 0;
  const auto result = document_detail::trashPhotographs(app, capturePhotoTrashRequest(app),
      [&](const std::string &) { ++prompts; return Decision::Cancel; }, options);
  CHECK(result.moved == 1 && prompts == 0 && app.path.empty() && app.history.size() == 0);
  CHECK(app.filmstrip.size() == 1 && app.filmstrip[0].path == b);
  CHECK(bytes(files.held / fs::path(inputSidecarPath(a)).filename()) == "malformed protected grade");
  CHECK(preparePhotoDeparture(app) && !fs::exists(inputSidecarPath(a)));
  // A replaced current source must be rejected before saving the live grade.
  CHECK(openPath(app, b)); app.filmstripSelection = {b};
  const auto request = capturePhotoTrashRequest(app);
  fs::rename(b, files.path("old-b.png")); files.photo("b.png");
  write(inputSidecarPath(b), "belongs to replacement");
  const auto before = files.calls.size();
  CHECK(execute(app, request, files).failed == 1 && files.calls.size() == before);
  CHECK(bytes(inputSidecarPath(b)) == "belongs to replacement");
  // A replacement while Retry/Discard/Cancel is open must not receive a Retry save.
  CHECK(openPath(app, b, [](const std::string &) { return Decision::Discard; }) && app.isPhotoProtected());
  write(inputSidecarPath(b), "{\"format\":\"rawnode\",\"version\":1}");
  CHECK(openPath(app, b) && !app.isPhotoProtected());
  const auto retryRequest = capturePhotoTrashRequest(app);
  options = files.options(); options.beforeSave = [] { return Fault::Replace; };
  int retryPrompts = 0;
  const auto changedDuringPrompt = document_detail::trashPhotographs(app, retryRequest, [&](const std::string &) {
    // A broken identity guard must fail below, not Retry forever.
    if (++retryPrompts > 1) return Decision::Cancel;
    fs::rename(b, files.path("retry-original.png")); files.photo("b.png");
    write(inputSidecarPath(b), "new source owns this sidecar");
    return Decision::Retry;
  }, options);
  CHECK(retryPrompts == 1);
  CHECK(changedDuringPrompt.cancelled && changedDuringPrompt.failed == 1 && files.calls.size() == before);
  CHECK(bytes(inputSidecarPath(b)) == "new source owns this sidecar");
  return true;
}
}  // namespace

bool testPhotoTrash() {
  for (const auto &test : {
      std::make_pair("singular/plural confirmation separates eligible, ineligible and filter-hidden paths", confirmationEligibilityAndWording),
      std::make_pair("opaque exact sidecars, hidden snapshot, non-current document and thumbnail invariants", sidecarsAndIndependentDocument),
      std::make_pair("photo/sidecar partial failures, continuation and remaining selection", partialFailures),
      std::make_pair("missing/replaced paths, symlinks and changed folder refuse unrelated files", changedAndSymlinkPaths),
      std::make_pair("current failure/success, last photo, orphan protection and usable empty editor", currentPhotoTransitions),
      std::make_pair("Retry/Discard/Cancel and active-export source restriction", saveDecisionsAndExport),
      std::make_pair("preview execution drained and obsolete thumbnail generation cannot publish", previewAndThumbnailLifetime),
      std::make_pair("protected current source, failed replacement and no substituted-source save", protectedAndFailedReplacement)}) {
    if (!test.second()) { std::fprintf(stderr, "FAIL photo Trash: %s\n", test.first); return false; }
  }
  std::puts("PASS photo Trash tests (controlled fixture moves only)");
  return true;
}
