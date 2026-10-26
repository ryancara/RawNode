#include "PhotoDocumentTests.h"

#include "AppState.h"
#include "DocumentMutation.h"
#include "Export.h"
#include "GraphCommands.h"
#include "GraphEvaluator.h"
#include "NodeGraph.h"
#include "RenderRuntimeTestAccess.h"
#include "GraphPersistenceTestAccess.h"
#include "GraphDepthFixtures.h"
#include "persist/DocumentActionsPriv.h"
#include "persist/Json.h"
#include "ui/Filmstrip.h"
#include "ui/UiContext.h"
#include "color/TransferFunction.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>

#if defined(__linux__)
#include "portable-file-dialogs.h"
#include <sys/wait.h>
#include <unistd.h>
#endif

#define CHECK(expression) do { if (!(expression)) { \
  std::fprintf(stderr, "photo document check failed at line %d: %s\n", __LINE__, #expression); return false; \
} } while (false)

namespace {
namespace fs = std::filesystem;
using Options = document_detail::ActionTestOptions;
using Fault = persist_detail::FileFault;
using Decision = SaveFailureDecision;

struct Files {
  fs::path dir = fs::temp_directory_path() /
      ("rawnode-photo-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  Files() { fs::create_directory(dir); }
  ~Files() { std::error_code ignored; fs::remove_all(dir, ignored); }
  std::string path(const char *name) const { return (dir / name).string(); }
};
void write(const std::string &path, const std::string &text) {
  std::ofstream out(path, std::ios::binary); out << text; out.close();
  if (!out) throw std::runtime_error("Cannot write fixture: " + path);
}
std::string bytes(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("Cannot read fixture: " + path);
  return {std::istreambuf_iterator<char>(in), {}};
}
std::set<std::string> entries(const fs::path &dir) {
  std::set<std::string> result;
  for (const auto &entry : fs::directory_iterator(dir)) result.insert(entry.path().filename().string());
  return result;
}
std::string section(const std::string &path, const char *key) {
  persist_json::Value root; std::string error;
  if (!persist_json::read(bytes(path), root, error)) throw std::runtime_error(error);
  const auto found = root.object.find(key);
  return found == root.object.end() ? "" : found->second.raw;
}
PersistFile read(const std::string &path, bool testOperations = false) {
  PersistFile file; std::string error;
  if (!persist_detail::decodeGraphFile(bytes(path), file, error, nullptr, testOperations)) throw std::runtime_error(error);
  return file;
}
std::string photo(const Files &files, const char *name, float value = 0.25f) {
  const auto path = files.path(name);
  Image image; image.w = 3; image.h = 2;
  for (int i = 0; i < 6; ++i) image.px.insert(image.px.end(), {value, value, value, 1.0f});
  if (!writeImage(image, path, {RgbGamut::Rec709, TransferFunction::Linear})) throw std::runtime_error("Cannot write photo");
  return path;
}
bool idle(App &app) {
  return RenderRuntimeTestAccess::wait(app.renderer, [](auto state) {
    return !state.previewBusy && !state.previewPending && !state.displayPending;
  });
}
std::string capturedGrade(const App &app) {
  PersistFile file; file.grade.emplace();
  file.grade->graph = captureGraph({app.nodes, app.primarySource, app.documentOutput});
  file.grade->color = captureGradeColor(app);
  file.grade->positions = app.nodePositions;
  std::string json, error;
  if (!encodeGraphFile(file, json, error)) throw std::runtime_error(error);
  return json;
}

#if defined(__linux__)
bool linuxDialogDecisions() {
  Files helpers;
  // The library discovers helpers through /bin/sh + which. Script only the
  // external processes, exercising its real executor, exit mapping and UI helper.
  write(helpers.path("which"), "#!/bin/sh\n[ \"$1\" = kdialog ]\n");
  fs::permissions(helpers.path("which"), fs::perms::owner_all);
  const auto child = fork();
  CHECK(child >= 0);
  if (child == 0) {
    // Keep PATH and the library's cached discovery entirely in this child.
    if (setenv("PATH", helpers.dir.c_str(), 1) != 0) _exit(1);
    const auto check = [&] {
      pfd::settings::rescan();
      CHECK(pfd::settings::available());
      for (const auto &reply : {std::make_pair(0, Decision::Retry),
                               std::make_pair(1, Decision::Discard),
                               std::make_pair(2, Decision::Cancel),
                               std::make_pair(3, Decision::Cancel)}) {
        write(helpers.path("kdialog"), "#!/bin/sh\n[ \"$1\" = --warningyesnocancel ] || exit 3\nexit " +
              std::to_string(reply.first) + "\n");
        fs::permissions(helpers.path("kdialog"), fs::perms::owner_all);
        CHECK(promptSaveFailure("deterministic save failure") == reply.second);
      }
      pfd::settings::rescan();
      CHECK(pfd::settings::available() && fs::is_regular_file(helpers.path("kdialog")));
      // Discovery is cached: a helper disappearing now must never mean No.
      CHECK(fs::remove(helpers.path("kdialog")));
      CHECK(promptSaveFailure("helper disappeared after discovery") == Decision::Cancel);
      write(helpers.path("which"), "#!/bin/sh\nexit 1\n");
      pfd::settings::rescan();
      CHECK(!pfd::settings::available());
      CHECK(promptSaveFailure("no helper available") == Decision::Cancel);
      return true;
    };
    _exit(check() ? 0 : 1);
  }
  int status = 0;
  CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
  return true;
}
#endif
const std::string colors = R"("color":{"rawWorking":{"colorSpace":"rec2020","gamma":"linear"},"output":{"colorSpace":"display-p3","gamma":"srgb"}})";
const std::string source = R"({"id":"camera-root","kind":"source"})";
std::string file(const std::string &nodes = source, const std::string &output = "camera-root",
                 const std::string &layout = "", const std::string &metadata = "", const std::string &color = colors) {
  return "{\"format\":\"rawnode\",\"version\":1,\"grade\":{" + color +
      ",\"graph\":{\"primarySource\":\"camera-root\",\"documentOutput\":{\"node\":\"" + output +
      "\",\"port\":\"image\"},\"nodes\":[" + nodes + "]}" + layout + "}" +
      (metadata.empty() ? "" : ",\"metadata\":" + metadata) + "}\n";
}
const std::string missing = R"({"id":"unavailable","kind":"image-processor","backend":"ctl","identifier":"/unavailable/photograph.ctl","label":"Keep me","enabled":false,"inputs":{"image":{"node":"camera-root","port":"image"}},"params":{"huge":9007199254740993,"precise":0.12345678901234567890123456789,"nested":{"x":[true,null,"\u00e9"]}}})";
const std::string exposure = R"({"id":"gain","kind":"image-processor","backend":"native","identifier":"rawnode.native.exposure","enabled":true,"inputs":{"image":{"node":"camera-root","port":"image"}},"params":{"exposure":1.0}})";

bool scanAndInitialGrades() {
  Files files;
  const auto a = photo(files, "a.png"), b = photo(files, "b.png", 0.5f);
  const auto before = entries(files.dir);
  CHECK(listFolderImages(files.dir.string()).size() == 2);
  App scan; scan.folderDir = files.dir.string(); refreshFilmstrip(scan);
  CHECK(scan.filmstrip.size() == 2);
  std::vector<unsigned char> rgba; int w = 0, h = 0;
  CHECK(loadThumbnailRGBA(a, 16, rgba, w, h) && !rgba.empty());
  CHECK(entries(files.dir) == before);
  App app;
  app.rawWorkingEncoding = {RgbGamut::ACES_AP1, TransferFunction::Linear};
  app.defaultOutputEncoding = {RgbGamut::Rec2020, TransferFunction::Rec709};
  CHECK(openPath(app, a));
  const auto saved = read(inputSidecarPath(a));
  CHECK(saved.grade && saved.grade->graph.nodes.size() == 1 && !saved.metadataJson);
  CHECK(saved.grade->graph.documentOutput.nodeId == saved.grade->graph.primarySource);
  CHECK(saved.grade->color.rawColorSpace == "aces-ap1" && saved.grade->color.outputColorSpace == "rec2020");
  CHECK(!fs::exists(inputSidecarPath(b)) && app.persistenceWarning.empty());
  const auto metadata = R"({ "mark" : "pick" })";
  write(inputSidecarPath(b), std::string(R"({"format":"rawnode","version":1,"metadata":)") + metadata + '}');
  CHECK(openPath(app, b));
  CHECK(read(inputSidecarPath(b)).grade && section(inputSidecarPath(b), "metadata") == metadata);
  Files folder;
  const auto first = photo(folder, "a.png"), second = photo(folder, "b.png");
  App browsing;
  CHECK(openFolder(browsing, folder.dir.string()));
  // Folder opening resolves aliases such as macOS /var -> /private/var.
  CHECK(fs::equivalent(browsing.path, first) && fs::exists(inputSidecarPath(first)) && !fs::exists(inputSidecarPath(second)));
  return true;
}

bool folderImagesStayLocal() {
  Files files;
  fs::create_directories(files.dir / "0-shoots" / "nested");
  fs::create_directories(files.dir / "archive" / "2025");
  fs::create_directory(files.dir / "empty");
  // Create direct files out of filename order. RAW fixtures exercise discovery;
  // real JPEGs exercise folder auto-open and photo switching below.
  const auto raw = files.path("z.NEF");
  write(raw, "RAW discovery fixture");
  const auto second = photo(files, "m.JPEG", 0.5f);
  const auto first = photo(files, "a.jpg");
  write(files.path("document.pdf"), "unsupported document");
  write(inputSidecarPath(second), R"({"format":"rawnode","version":1,"metadata":{}})");
  write(files.path("old.nef.ofxrawhost.json"), "legacy metadata");
  write(files.path("0-shoots/photo.NEF"), "nested RAW fixture");
  photo(files, "0-shoots/photo.jpg");
  write(files.path("0-shoots/nested/photo.NEF"), "deep RAW fixture");
  photo(files, "0-shoots/nested/photo.jpg");
  photo(files, "archive/2025/photo.jpg");

  const std::vector<std::string> expected{first, second, raw};
  CHECK(listFolderImages(files.dir.string()) == expected);
  CHECK(listFolderImages(files.path("archive")).empty());
  CHECK(listFolderImages(files.path("empty")).empty());
  // Direct file symlinks keep their existing regular-file treatment, including
  // when their target happens to live inside a subfolder.
  std::error_code error;
  const auto link = files.path("s-link.jpg");
  fs::create_symlink(files.path("0-shoots/photo.jpg"), link, error);
  if (!error) {
    CHECK(listFolderImages(files.dir.string()) == (std::vector<std::string>{first, second, link, raw}));
    CHECK(fs::remove(link));
  } else {
    std::printf("SKIP folder scan file symlink: %s\n", error.message().c_str());
  }

  App browsing;
  CHECK(openFolder(browsing, files.dir.string()));
  CHECK(browsing.filmstrip.size() == expected.size() && browsing.filmstripIndex == 0);
  for (size_t i = 0; i < expected.size(); ++i)
    CHECK(fs::equivalent(browsing.filmstrip[i].path, expected[i]));
  CHECK(fs::equivalent(browsing.path, first) && browsing.preview && !browsing.full.px.empty());
  CHECK(openPath(browsing, second) && browsing.filmstripIndex == 1);
  const auto preview = browsing.preview;
  CHECK(openFolder(browsing, files.path("archive")));
  CHECK(browsing.filmstrip.empty() && browsing.filmstripIndex == -1);
  CHECK(fs::equivalent(browsing.path, second) && browsing.preview == preview);

  App empty;
  CHECK(openFolder(empty, files.path("archive")));
  CHECK(empty.filmstrip.empty() && empty.filmstripIndex == -1);
  CHECK(empty.path.empty() && empty.preview && empty.preview->px.empty() && empty.full.px.empty());
  CHECK(empty.getStatus().find("(no images)") != std::string::npos);
  CHECK(!fs::exists(inputSidecarPath(files.path("0-shoots/photo.jpg"))));
  CHECK(!fs::exists(inputSidecarPath(files.path("0-shoots/nested/photo.jpg"))));
  CHECK(!fs::exists(inputSidecarPath(files.path("archive/2025/photo.jpg"))));
  return true;
}

bool restorationAndUnchangedSave() {
  Files files;
  const auto a = photo(files, "a.png"), b = photo(files, "b.png");
  const auto path = inputSidecarPath(a);
  const auto metadata = std::string("{ \n \"mark\" : \"unmarked\" \n}");
  // Shuffled storage, off-spine installed processor, non-default Source/output,
  // unavailable CTL, raw opaque precision and positions are independent input.
  const auto original = file(missing + ',' + exposure + ',' + source, "unavailable",
      R"(,"positions":{"unavailable":{"x":-12.5,"y":8.25},"gain":{"x":1,"y":2}})", metadata);
  write(path, original);
  int saves = 0;
  Options options; options.beforeSave = [&] { ++saves; return Fault::None; };
  App app;
  CHECK(document_detail::openPhoto(app, a, {}, options));
  CHECK(saves == 0 && bytes(path) == original && app.primarySource == NodeId{"camera-root"});
  CHECK(app.documentOutput == (OutputRef{NodeId{"unavailable"}, ports::image}));
  CHECK(app.nodes.size() == 3 && !app.selectedNodeId() && app.nodePositions.size() == 2);
  CHECK(findNode(app, NodeId{"unavailable"})->isMissingProcessor());
  CHECK(findNode(app, NodeId{"gain"})->processor && app.outputEncoding.gamut == RgbGamut::DisplayP3);
  app.renderer.start(); CHECK(idle(app));
  CHECK(document_detail::openPhoto(app, b, {}, options));
  CHECK(saves == 2); // A departure and B initial save, with no edit notifications.
  const auto persisted = read(path);
  CHECK(persisted.grade && persisted.grade->graph.nodes.size() == 3 && persisted.grade->positions.size() == 2);
  CHECK(section(path, "metadata") == metadata);
  CHECK(persisted.grade->graph.nodes[0].paramsJson.at("huge") == "9007199254740993");
  CHECK(persisted.grade->graph.nodes[0].paramsJson.at("precise") == "0.12345678901234567890123456789");
  CHECK(persisted.grade->graph.nodes[0].paramsJson.at("nested") == R"({"x":[true,null,"\u00e9"]})");
  CHECK(persisted.grade->graph.nodes[0].identifier == "/unavailable/photograph.ctl" && !persisted.grade->graph.nodes[0].enabled);
  CHECK(document_detail::openPhoto(app, a, {}, options)); CHECK(idle(app));
  CHECK(app.nodes.size() == 3 && app.nodePositions.at("unavailable").x == -12.5);
  CHECK(graph::deleteNode(app, NodeId{"gain"}).ok() && app.nodePositions.count("gain") == 0);
  app.renderer.shutdown();
  // Noncommutative native operations in reversed storage order: exposure first,
  // then the sRGB OETF. Executing array order produces a different pixel.
  const std::string cst = R"({"id":"encode","kind":"image-processor","backend":"native","identifier":"rawnode.native.cst","enabled":true,"inputs":{"image":{"node":"gain","port":"image"}},"params":{"input_space":"rec709","input_gamma":"linear","output_space":"rec709","output_gamma":"srgb"}})";
  const auto orderedFile = file(cst + ',' + exposure + ',' + source, "encode");
  write(path, orderedFile);
  App ordered; CHECK(openPath(ordered, a) && bytes(path) == orderedFile);
  ordered.renderer.start(); CHECK(idle(ordered));
  {
    std::lock_guard<std::mutex> lock(ordered.displayMutex);
    CHECK(ordered.processed.result.state == EvaluationState::Available);
    CHECK(ordered.processed.result.requested == (OutputRef{NodeId{"encode"}, ports::image}));
    const auto output = std::get<ImageValue>(ordered.processed.result.value);
    const double expected = 1.055 * std::pow(2.0 * ordered.full.px[0], 1.0 / 2.4) - 0.055;
    CHECK(std::abs(output->px[0] - expected) < 1e-5 && output->px[3] == ordered.full.px[3]);
  }
  ordered.renderer.shutdown();
  return true;
}

bool slotRestoration() {
  Files files; const auto a = photo(files, "a.png"), b = photo(files, "b.png");
  const auto slotted = R"({"id":"mix","kind":"test-slotted","enabled":true,"inputs":{"reference":null},"slots":[{"id":"3","settings":{},"inputs":{"image":{"node":"camera-root","port":"image"}}},{"id":"1","settings":{"enabled":false},"inputs":{"image":null,"mask":null}}],"nextSlotId":"5"})";
  write(inputSidecarPath(a), file(std::string(slotted) + ',' + source, "mix"));
  Options options; options.testOperations = true;
  App app;
  CHECK(document_detail::openPhoto(app, a, {}, options));
  const auto *mix = findNode(app, NodeId{"mix"});
  CHECK(mix && mix->slots[0].id == SlotId{3} && mix->slots[1].id == SlotId{1} && !mix->slots[1].settings.enabled);
  CHECK(mix->nextSlotIdForPersistence() == 5 && !mix->fixedInputs.at(ports::reference));
  CHECK(document_detail::openPhoto(app, b, {}, options));
  CHECK(document_detail::openPhoto(app, a, {}, options));
  CHECK(graph::deleteSlot(app, NodeId{"mix"}, SlotId{3}).ok());
  CHECK(graph::deleteSlot(app, NodeId{"mix"}, SlotId{1}).ok());
  CHECK(document_detail::openPhoto(app, b, {}, options));
  CHECK(document_detail::openPhoto(app, a, {}, options));
  const auto added = graph::addSlot(app, NodeId{"mix"});
  CHECK(added.ok() && added.addedSlot == SlotId{5});
  App production; CHECK(openPath(production, a) && production.isPhotoProtected());
  return true;
}

bool protectionAndDecodeFailure() {
  Files files;
  const auto a = photo(files, "a.png"), b = photo(files, "b.png");
  const auto bad = files.path("broken.png"); write(bad, "not an image");
  const auto invalidParameter = file(std::string(exposure).replace(exposure.find("1.0"), 3, "true") + ',' + source, "gain");
  for (const auto &original : {std::string("{"), std::string(R"({"format":"rawnode","version":999})"),
       file(std::string(source).replace(source.find("source"), 6, "future")), invalidParameter,
       std::string(R"({"format":"rawnode-sidecar","version":2,"chain":{}})"),
       depth_test::fileJson(kMaxGraphDependencyDepth + 1)}) {
    write(inputSidecarPath(a), original);
    const auto before = entries(files.dir);
    App app; CHECK(openPath(app, a));
    CHECK(app.isPhotoProtected() && app.nodes.size() == 1 && !app.persistenceWarning.empty());
    if (original == depth_test::fileJson(kMaxGraphDependencyDepth + 1))
      CHECK(depth_test::depthError(app.persistenceWarning));
    CHECK(app.photoRawWorkingEncoding == (ColorEncoding{RgbGamut::Rec709, TransferFunction::Linear}));
    CHECK(!addNativeExposureNode(app) && !addNativeCstNode(app) && !addCtlNode(app, "absent.ctl"));
    Node node; node.id = NodeId{"forbidden"}; CHECK(!graph::addNode(app, std::move(node)).ok());
    CHECK(!graph::setDocumentOutput(app, {app.primarySource, ports::image}).ok());
    CHECK(!graph::addSlot(app, app.primarySource).ok());
    const auto input = app.inputEncoding, output = app.outputEncoding;
    setRawWorkingEncoding(app, RgbGamut::ACES_AP0, TransferFunction::SRGB);
    CHECK(!setOutputEncoding(app, {RgbGamut::ACES_AP0, TransferFunction::Linear}));
    CHECK(app.inputEncoding == input && app.outputEncoding == output && !app.renderer.canEditParameters());
    CHECK(!setNodeParameterValue(app, app.primarySource, "exposure", 1.0));
    CHECK(!resetNodeParameter(app, app.primarySource, "exposure") && !activateNodeParameter(app, app.primarySource, "button"));
    CHECK(!pasteFromClipboard(app) && !copyGrade(app) && !savePreset(app, files.path("forbidden.rawnode.json")));
    CHECK(!applyGrade(app, {}) && !appendPersistedNode(app, PersistNode{}));
    clearNodes(app); applyChain(app, {}); setNodeEnabled(app, app.primarySource, false);
    CHECK(!saveCurrentInputSidecar(app) && preparePhotoDeparture(app));
    app.setStatus("Ordinary render/export status");
    CHECK(!app.persistenceWarning.empty() && app.nodes.size() == 1);
    CHECK(bytes(inputSidecarPath(a)) == original && entries(files.dir) == before);
    const auto *oldPixels = app.full.px.data(); const auto oldPreview = app.preview;
    CHECK(!openPath(app, bad));
    CHECK(app.path == a && app.full.px.data() == oldPixels && app.preview == oldPreview && app.isPhotoProtected());
    CHECK(!fs::exists(inputSidecarPath(bad)) && entries(files.dir) == before);
    CHECK(openPath(app, b) && !app.isPhotoProtected() && app.persistenceWarning.empty());
    CHECK(bytes(inputSidecarPath(a)) == original);
  }
  fs::remove(inputSidecarPath(a));
  const auto legacy = legacyInputSidecarPath(a);
  write(legacy, "legacy state is deliberately not parsed");
  const auto before = entries(files.dir);
  App protectedLegacy; CHECK(openPath(protectedLegacy, a) && protectedLegacy.isPhotoProtected());
  CHECK(preparePhotoDeparture(protectedLegacy) && !saveCurrentInputSidecar(protectedLegacy));
  CHECK(!fs::exists(inputSidecarPath(a)) && entries(files.dir) == before && bytes(legacy) == "legacy state is deliberately not parsed");
  fs::remove(legacy);
  write(inputSidecarPath(a), file());
  Options unreadable; unreadable.readFault = Fault::Read;
  App app; CHECK(document_detail::openPhoto(app, a, {}, unreadable) && app.isPhotoProtected());
  CHECK(bytes(inputSidecarPath(a)) == file());
  return true;
}

bool failureDecisions() {
  Files files; const auto a = photo(files, "a.png"), b = photo(files, "b.png");
  App app;
  int saves = 0;
  bool fail = true;
  Options options; options.beforeSave = [&] { ++saves; return fail ? Fault::Replace : Fault::None; };
  CHECK(document_detail::openPhoto(app, a, {}, options));
  CHECK(app.path == a && !app.isPhotoProtected() && !app.persistenceWarning.empty() && !fs::exists(inputSidecarPath(a)));
  const auto warning = app.persistenceWarning;
  app.renderer.start(); app.setStatus("Export completed"); CHECK(idle(app));
  CHECK(app.persistenceWarning == warning && addNativeExposureNode(app));
  const auto id = *app.selectedNodeId();
  CHECK(setNodeParameterValue(app, id, "exposure", 1.5));
  const auto pixels = app.full.px.data(); const auto preview = app.preview;
  const auto before = entries(files.dir);
  const auto cancel = [](const std::string &) { return Decision::Cancel; };
  CHECK(!document_detail::openPhoto(app, b, cancel, options));
  CHECK(app.path == a && app.full.px.data() == pixels && app.preview == preview && app.persistenceWarning == warning);
  CHECK(entries(files.dir) == before && app.renderer.canEditParameters());
  CHECK(!document_detail::openFolder(app, files.dir.string(), cancel, options) && app.folderDir.empty());
  // This is the exact departure helper called before shutdown intent/teardown.
  const auto runtime = RenderRuntimeTestAccess::snapshot(app.renderer);
  CHECK(!document_detail::leavePhoto(app, cancel, options));
  const auto after = RenderRuntimeTestAccess::snapshot(app.renderer);
  CHECK(!after.stopping && after.previewThreadOwned == runtime.previewThreadOwned && !app.quit);
  CHECK(setNodeParameterValue(app, id, "exposure", -1.0)); app.renderer.requestPreview(); CHECK(idle(app));
  int prompts = 0;
  const auto retry = [&](const std::string &error) {
    ++prompts; if (error.find(inputSidecarPath(a)) == std::string::npos) throw std::runtime_error("Missing save path");
    fail = false; return Decision::Retry;
  };
  CHECK(document_detail::openPhoto(app, b, retry, options) && prompts == 1);
  CHECK(app.path == b && app.persistenceWarning.empty() && read(inputSidecarPath(a)).grade->graph.nodes.size() == 2);
  CHECK(read(inputSidecarPath(a)).grade->graph.nodes[1].paramsJson.at("exposure") == "-1");
  // An unchanged departure still tries saving and offers the same choices.
  fail = true; const auto savedB = bytes(inputSidecarPath(b)); const auto count = saves;
  CHECK(!document_detail::leavePhoto(app, cancel, options) && saves == count + 1);
  const auto discard = [](const std::string &) { return Decision::Discard; };
  const auto samePhotoWarning = app.persistenceWarning;
  CHECK(document_detail::openPhoto(app, b, discard, options));
  CHECK(app.persistenceWarning == samePhotoWarning); // No successful save or actual photo switch.
  CHECK(document_detail::openPhoto(app, a, discard, options));
  CHECK(app.path == a && bytes(inputSidecarPath(b)) == savedB);
  CHECK(document_detail::leavePhoto(app, discard, options) && !app.quit && !RenderRuntimeTestAccess::snapshot(app.renderer).stopping);
  fail = false;
  CHECK(document_detail::leavePhoto(app, {}, options) && app.persistenceWarning.empty());
  app.renderer.shutdown();
  return true;
}

bool folderDiscardOnlyOnce() {
  Files oldFolder, newFolder;
  const auto a = photo(oldFolder, "a.png"), b = photo(newFolder, "b.png", 0.5f);
  write(inputSidecarPath(a), file()); write(inputSidecarPath(b), file());
  App app; CHECK(openPath(app, a));
  const auto original = bytes(inputSidecarPath(a));
  app.renderer.start(); CHECK(idle(app));
  int saves = 0, prompts = 0;
  Options options; options.beforeSave = [&] { ++saves; return Fault::Replace; };
  const auto discard = [&](const std::string &) { ++prompts; return Decision::Discard; };
  CHECK(document_detail::openFolder(app, newFolder.dir.string(), discard, options));
  CHECK(saves == 1 && prompts == 1 && bytes(inputSidecarPath(a)) == original);
  CHECK(fs::equivalent(app.path, b) && fs::equivalent(app.folderDir, newFolder.dir));
  CHECK(app.filmstrip.size() == 1 && app.filmstripIndex == 0 && fs::equivalent(app.filmstrip[0].path, b));
  CHECK(!app.isPhotoProtected() && app.persistenceWarning.empty() && idle(app));
  CHECK(addNativeExposureNode(app) && app.renderer.canEditParameters() && saveCurrentInputSidecar(app));
  app.renderer.shutdown();
  return true;
}

bool samePhotoAndAtomicFailures() {
  Files files; const auto a = photo(files, "a.png"), b = photo(files, "b.png");
  App app; CHECK(openPath(app, a) && addNativeExposureNode(app));
  const auto id = *app.selectedNodeId();
  CHECK(setNodeParameterValue(app, id, "exposure", 2.0));
  app.nodePositions[id.value] = {-2, 3};
  CHECK(openPath(app, a));
  CHECK(findNode(app, id) && app.nodePositions.at(id.value).x == -2);
  CHECK(findNode(app, id)->processor->parameters()[0].value == ParameterValue{2.0});
  const auto path = inputSidecarPath(a), original = bytes(path); const auto before = entries(files.dir);
  for (const auto fault : {Fault::TempCreate, Fault::Write, Fault::Flush, Fault::Close, Fault::Replace}) {
    Options options; options.beforeSave = [=] { return fault; };
    CHECK(!document_detail::openPhoto(app, b, {}, options));
    CHECK(bytes(path) == original && entries(files.dir) == before && app.path == a && app.nodePositions.size() == 2);
    CHECK(app.nodePositions.at(app.primarySource.value).x == 0 && app.nodePositions.at(app.primarySource.value).y == 0);
    CHECK(app.nodePositions.at(id.value).x == -2 && app.nodePositions.at(id.value).y == 3);
    CHECK(!app.persistenceWarning.empty());
  }
  CHECK(saveCurrentInputSidecar(app) && app.persistenceWarning.empty());
  // The strict codec refuses invalid backend/parameter bytes before replacing
  // any file. Never transliterate a live processor identity or value.
  Node missingNode; missingNode.id = NodeId{"bad-path"}; missingNode.storedBackend = "ctl";
  missingNode.storedIdentifier = std::string("/tmp/") + char(0xff) + ".ctl";
  missingNode.fixedInputs.at(ports::image) = OutputRef{app.primarySource, ports::image};
  CHECK(graph::addNode(app, std::move(missingNode)).ok());
  CHECK(!saveCurrentInputSidecar(app));
  CHECK(bytes(path) == original && findNode(app, NodeId{"bad-path"})->storedIdentifier.find(char(0xff)) != std::string::npos);
  CHECK(graph::deleteNode(app, NodeId{"bad-path"}).ok());
  findNode(app, id)->preservedParamsJson["future-string"] = std::string("\"") + char(0xff) + '"';
  CHECK(!saveCurrentInputSidecar(app) && bytes(path) == original);
  findNode(app, id)->preservedParamsJson.erase("future-string");
  CHECK(saveCurrentInputSidecar(app));
  return true;
}

bool syntheticRaw(const std::string &path, Image &out, ColorEncoding &detected, bool &raw, ColorEncoding requested) {
  if (path.find("failed") != std::string::npos) return false;
  out.w = 1600; out.h = 2; raw = true; detected = requested;
  const float red = path.find("b.nef") != std::string::npos ? 0.5f : -0.125f;
  for (int i = 0; i < out.w * out.h; ++i)
    out.px.insert(out.px.end(), {float(encodeTransfer(red, requested.gamma)),
        float(encodeTransfer(1.5, requested.gamma)), float(encodeTransfer(0.25, requested.gamma)), 0.75f});
  return true;
}

bool failedDecodeRetainsDocument() {
  Files files; const auto a = files.path("a.nef"), failed = files.path("failed.nef");
  const std::string oldColours = R"("color":{"rawWorking":{"colorSpace":"aces-ap1","gamma":"srgb"},"output":{"colorSpace":"aces-ap0","gamma":"linear"}})";
  write(inputSidecarPath(a), file(missing + ',' + exposure + ',' + source, "gain",
      R"(,"positions":{"unavailable":{"x":-12.5,"y":8.25},"gain":{"x":1,"y":2}})", "", oldColours));
  // Different graph, endpoints, positions and both colour pairs are staged for
  // the failing photo. Publishing any of them before decoding must be caught.
  write(inputSidecarPath(failed), "{\"format\":\"rawnode\",\"version\":1,\"grade\":{" + colors +
      R"(,"graph":{"primarySource":"incoming-source","documentOutput":{"node":"incoming-source","port":"image"},"nodes":[{"id":"incoming-source","kind":"source"}]}}})");
  Options options; options.decode = syntheticRaw;
  App app; app.previewRes = 0;
  CHECK(document_detail::openPhoto(app, a, {}, options));
  app.renderer.start(); CHECK(idle(app));
  const auto grade = capturedGrade(app), path = app.path;
  const auto input = app.inputEncoding, raw = app.photoRawWorkingEncoding, output = app.outputEncoding;
  const auto rawDefault = app.rawWorkingEncoding, outputDefault = app.defaultOutputEncoding;
  const auto full = app.full, previewPixels = *app.preview;
  const auto *fullStorage = app.full.px.data();
  const auto *processor = findNode(app, NodeId{"gain"})->processor.get();
  const auto preview = app.preview;
  CHECK(!document_detail::openPhoto(app, failed, {}, options));
  CHECK(capturedGrade(app) == grade && app.nodes.size() == 3);
  CHECK(app.primarySource == NodeId{"camera-root"} && app.documentOutput == (OutputRef{NodeId{"gain"}, ports::image}));
  CHECK(findNode(app, NodeId{"gain"})->processor.get() == processor && findNode(app, NodeId{"unavailable"})->isMissingProcessor());
  CHECK(app.path == path && app.inputIsRaw && !app.isPhotoProtected());
  CHECK(app.inputEncoding == input && app.photoRawWorkingEncoding == raw && app.outputEncoding == output);
  CHECK(app.rawWorkingEncoding == rawDefault && app.defaultOutputEncoding == outputDefault);
  CHECK(app.full.w == full.w && app.full.h == full.h && app.full.px == full.px && app.full.px.data() == fullStorage);
  CHECK(app.preview == preview && app.preview->w == previewPixels.w && app.preview->h == previewPixels.h && app.preview->px == previewPixels.px);
  CHECK(idle(app) && app.renderer.canEditParameters());
  CHECK(setNodeParameterValue(app, NodeId{"gain"}, "exposure", -1.0));
  app.renderer.requestPreview(); CHECK(idle(app) && saveCurrentInputSidecar(app));
  app.renderer.shutdown();
  return true;
}

bool rawControlsAndReplace() {
  Files files; const auto b = files.path("b.nef");
  Options options; options.decode = syntheticRaw;
  App app; app.previewRes = 0;
  app.rawWorkingEncoding = {RgbGamut::ACES_AP0, TransferFunction::Linear};
  const auto sessionDefault = app.rawWorkingEncoding;
  CHECK(document_detail::openPhoto(app, files.path("a.nef"), {}, options));
  app.renderer.start(); CHECK(idle(app) && app.preview->px[0] == -0.125f);
  CHECK(document_detail::openPhoto(app, b, {}, options));
  CHECK(idle(app));
  // Independent sRGB OETF reference for b.nef's scene-linear red of 0.5.
  // A 1e-6 tolerance covers float storage and constant-image downsampling.
  const double encodedRed = 1.055 * std::pow(0.5, 1.0 / 2.4) - 0.055;
  const ColorEncoding changed{RgbGamut::ACES_AP1, TransferFunction::SRGB};
  document_detail::setRawWorkingEncoding(app, changed.gamut, changed.gamma, options);
  CHECK(app.inputEncoding == changed && app.photoRawWorkingEncoding == changed && app.rawWorkingEncoding == sessionDefault);
  CHECK(std::abs(app.full.px[0] - encodedRed) < 1e-6 && app.full.px[3] == 0.75f);
  CHECK(idle(app) && std::abs(app.preview->px[0] - encodedRed) < 1e-6);
  CHECK(saveCurrentInputSidecar(app));
  auto saved = read(inputSidecarPath(b));
  CHECK(saved.grade->color.rawColorSpace == "aces-ap1" && saved.grade->color.rawGamma == "srgb");
  const auto full = app.full.px, previewPixels = app.preview->px;
  const auto preview = app.preview;
  Options failed = options;
  failed.decode = [](const std::string &, Image &out, ColorEncoding &detected, bool &raw, ColorEncoding requested) {
    out.w = out.h = 1; out.px = {99,99,99,1}; detected = requested; raw = true;
    return false;
  };
  document_detail::setRawWorkingEncoding(app, RgbGamut::Rec709, TransferFunction::Linear, failed);
  CHECK(app.full.w == 1600 && app.full.h == 2 && app.full.px == full && app.preview == preview && app.preview->px == previewPixels);
  CHECK(app.inputEncoding == changed && app.photoRawWorkingEncoding == changed && app.rawWorkingEncoding == sessionDefault);
  CHECK(idle(app) && saveCurrentInputSidecar(app));
  saved = read(inputSidecarPath(b));
  CHECK(saved.grade->color.rawColorSpace == "aces-ap1" && saved.grade->color.rawGamma == "srgb");

  // Full-grade Replace is the operation used by grade paste/presets.
  // Return B to linear first, so Replace has to decode it in the incoming pair.
  document_detail::setRawWorkingEncoding(app, sessionDefault.gamut, sessionDefault.gamma, options);
  CHECK(app.full.px[0] == 0.5f && idle(app));
  PersistGrade incoming;
  incoming.graph.primarySource = NodeId{"incoming-source"};
  PersistNode root; root.id = "incoming-source"; root.operation = NodeOperation::Source;
  incoming.graph.nodes.push_back(root);
  PersistNode gain; gain.id = "incoming-gain"; gain.backend = "native";
  gain.identifier = "rawnode.native.exposure"; gain.paramsJson["exposure"] = "1";
  gain.inputs[ports::image] = OutputRef{incoming.graph.primarySource, ports::image};
  gain.inputs[ports::mask] = std::nullopt;
  incoming.graph.nodes.push_back(gain);
  incoming.graph.documentOutput = {NodeId{gain.id}, ports::image};
  const PersistGradeColor colour{"rec2020", "srgb", "display-p3", "srgb"};
  incoming.color = colour;
  CHECK(document_detail::applyGrade(app, incoming, options));
  const ColorEncoding incomingRaw{RgbGamut::Rec2020, TransferFunction::SRGB};
  CHECK(app.path == b && app.inputEncoding == incomingRaw && app.photoRawWorkingEncoding == incomingRaw);
  CHECK(app.rawWorkingEncoding == sessionDefault && app.outputEncoding == (ColorEncoding{RgbGamut::DisplayP3, TransferFunction::SRGB}));
  CHECK(app.nodes.size() == 2 && std::abs(app.full.px[0] - encodedRed) < 1e-6 && idle(app));
  CHECK(std::abs(app.preview->px[0] - encodedRed) < 1e-6 && app.preview->px[3] == 0.75f);
  {
    std::lock_guard<std::mutex> lock(app.displayMutex);
    CHECK(app.processed.result.state == EvaluationState::Available && app.processed.result.requested == app.documentOutput);
    const auto result = std::get<ImageValue>(app.processed.result.value);
    // A.nef's synthetic red is negative: this also catches stale source pixels.
    CHECK(std::abs(result->px[0] - 2.0 * encodedRed) < 1e-6 && result->px[3] == 0.75f);
  }
  CHECK(saveCurrentInputSidecar(app));
  saved = read(inputSidecarPath(b));
  CHECK(saved.grade->color.rawColorSpace == "rec2020" && saved.grade->color.rawGamma == "srgb");
  CHECK(saved.grade->graph.nodes.size() == 2 && saved.grade->graph.documentOutput.nodeId == NodeId{"incoming-gain"});
  app.renderer.shutdown();
  return true;
}

bool historyNativeCtlAndMissing() {
  Files files;
  const auto script = files.path("history.ctl");
  write(script,
      "void main(input varying float rIn, input varying float gIn, input varying float bIn,\n"
      "output varying float rOut, output varying float gOut, output varying float bOut,\n"
      "output varying float aOut, input varying float aIn = 1.0, input uniform float gain = 2.0)\n"
      "{ rOut = rIn * gain; gOut = gIn * gain; bOut = bIn * gain; aOut = aIn; }\n");
  const auto path = photo(files, "history.png");
  App app; CHECK(openPath(app, path) && addNativeExposureNode(app));
  const auto native = *app.selectedNodeId();
  CHECK(setNodeParameterValue(app, native, "exposure", 1.0) && addCtlNode(app, script));
  const auto ctl = *app.selectedNodeId();
  CHECK(setNodeParameterValue(app, ctl, "gain", 3.0));
  // A missing processor whose identifier is now available must stay missing.
  Node missingNode; missingNode.id = NodeId{"missing"}; missingNode.storedBackend = "ctl";
  missingNode.storedIdentifier = script; missingNode.preservedParamsJson["gain"] = "7";
  missingNode.fixedInputs.at(ports::image) = OutputRef{app.primarySource, ports::image};
  CHECK(graph::addNode(app, std::move(missingNode)).ok());
  app.nodePositions = {{native.value, {-12.5, 8.25}}, {ctl.value, {123, -42}}};
  document_detail::GradeSnapshot snapshot;
  CHECK(document_detail::captureGradeSnapshot(app, snapshot));
  CHECK(snapshot.workingProcessors.size() == 3 && snapshot.workingProcessors.at(native.value) &&
        snapshot.workingProcessors.at(ctl.value) && !snapshot.workingProcessors.at("missing"));
  // Storage order is deliberately unrelated to dependency order.
  std::reverse(snapshot.grade.graph.nodes.begin(), snapshot.grade.graph.nodes.end());
  CHECK(setNodeParameterValue(app, native, "exposure", -1.0));
  app.nodePositions[native.value] = {9, 10};
  CHECK(selectNode(app, native)); std::snprintf(app.paramFilter, sizeof(app.paramFilter), "exposure");
  findNode(app, native)->groupOpen["view-only"] = false;
  app.renderer.requestInspection({native, ports::image});
  const auto sidecar = bytes(inputSidecarPath(path));
  const auto *oldNative = findNode(app, native)->processor.get();
  const auto *oldCtl = findNode(app, ctl)->processor.get();
  app.renderer.start(); CHECK(idle(app));
  CHECK(document_detail::restoreGradeSnapshot(app, snapshot) && idle(app));
  CHECK(app.nodes.size() == 4 && app.primarySource == snapshot.grade.graph.primarySource &&
        app.documentOutput == (OutputRef{ctl, ports::image}) && documentStateError(app).empty());
  CHECK(findNode(app, native)->processor.get() != oldNative && findNode(app, ctl)->processor.get() != oldCtl);
  CHECK(findNode(app, native)->fixedInputs.at(ports::image) == (OutputRef{app.primarySource, ports::image}));
  CHECK(findNode(app, ctl)->fixedInputs.at(ports::image) == (OutputRef{native, ports::image}));
  CHECK(findNode(app, NodeId{"missing"})->isMissingProcessor() &&
        findNode(app, NodeId{"missing"})->preservedParamsJson.at("gain") == "7");
  CHECK(app.nodePositions.at(native.value).x == -12.5 && app.nodePositions.at(native.value).y == 8.25 &&
        app.nodePositions.at(ctl.value).x == 123 && app.nodePositions.at(ctl.value).y == -42);
  CHECK(app.selectedNodeId() == native && std::string(app.paramFilter) == "exposure" &&
        !findNode(app, native)->groupOpen.at("view-only"));
  {
    std::lock_guard<std::mutex> lock(app.displayMutex);
    CHECK(app.processed.result.requested == (OutputRef{native, ports::image}));
  }
  // Independently: Exposure +1 multiplies by 2, CTL gain 3 multiplies by 3.
  // Binary fractions and integer factors are exact, including extended RGB/alpha.
  auto samples = std::make_shared<Image>(); samples->w = samples->h = 1;
  samples->px = {0.25f, -0.5f, 2.0f, 0.375f};
  {
    document_detail::DocumentMutation ownership(app);
    const auto result = evaluateGraph({app.nodes, app.primarySource, app.documentOutput}, app.documentOutput,
                                     {EvaluationPurpose::Preview, 1, 1, 1, 1}, samples);
    CHECK(result.state == EvaluationState::Available);
    CHECK(std::get<ImageValue>(result.value)->px == (std::vector<float>{1.5f, -3.0f, 12.0f, 0.375f}));
  }
  CHECK(bytes(inputSidecarPath(path)) == sidecar);
  // Remove a working script: refusal must preserve the original compiled instance.
  const auto scriptText = bytes(script);
  fs::remove(script);
  const auto *liveNative = findNode(app, native)->processor.get();
  const auto *liveCtl = findNode(app, ctl)->processor.get();
  CHECK(!document_detail::restoreGradeSnapshot(app, snapshot));
  CHECK(app.getStatus().find("Could not reconstruct working processor") != std::string::npos && idle(app));
  CHECK(findNode(app, native)->processor.get() == liveNative && findNode(app, ctl)->processor.get() == liveCtl);
  CHECK(app.selectedNodeId() == native && app.nodePositions.at(native.value).x == -12.5);
  // Availability must be complete; malformed internal snapshots fail closed.
  auto incomplete = snapshot; incomplete.workingProcessors.erase(ctl.value);
  CHECK(!document_detail::restoreGradeSnapshot(app, incomplete));
  CHECK(findNode(app, ctl)->processor.get() == liveCtl);
  // Native identity unavailable: normal grade replacement may preserve a missing
  // plugin, whereas history refuses to lose a processor recorded as working.
  write(script, scriptText); // Isolate native failure from the earlier CTL fault.
  auto unavailable = snapshot;
  for (auto &node : unavailable.grade.graph.nodes) if (node.id == native.value) node.identifier = "unavailable.native";
  CHECK(!document_detail::restoreGradeSnapshot(app, unavailable));
  CHECK(app.getStatus().find(native.value + " (native: unavailable.native)") != std::string::npos);
  CHECK(findNode(app, native)->processor.get() == liveNative && bytes(inputSidecarPath(path)) == sidecar);
  // Removed selection and inspection clear; surviving virtual Output remains valid.
  document_detail::GradeSnapshot sourceOnly;
  App empty; CHECK(document_detail::captureGradeSnapshot(empty, sourceOnly));
  CHECK(document_detail::restoreGradeSnapshot(app, sourceOnly) && idle(app));
  CHECK(!app.selectedNodeId() && std::string(app.paramFilter).empty());
  {
    std::lock_guard<std::mutex> lock(app.displayMutex);
    CHECK(app.processed.result.requested == app.documentOutput);
  }
  selectOutput(app);
  CHECK(document_detail::restoreGradeSnapshot(app, sourceOnly) && app.outputSelected());
  // Ordinary replacement still resets Output selection/filter and inspection.
  CHECK(applyGrade(app, sourceOnly.grade) && !app.outputSelected());
  CHECK(idle(app)); app.renderer.shutdown();
  return true;
}

bool historyReusedNodeIdentity() {
  Files files;
  const auto script = files.path("identity.ctl");
  write(script,
      "void main(input varying float rIn, input varying float gIn, input varying float bIn,\n"
      "output varying float rOut, output varying float gOut, output varying float bOut)\n"
      "{ rOut = rIn; gOut = gIn; bOut = bIn; }\n");
  App app;
  CHECK(openPath(app, photo(files, "identity.png")) && addCtlNode(app, script));
  const auto reused = *app.selectedNodeId();
  // Keep document Output distinct from the inspected node so retaining stale
  // inspection cannot accidentally look like a successful reset.
  CHECK(addNativeExposureNode(app));
  const auto output = app.documentOutput;
  CHECK(output.nodeId != reused);
  document_detail::GradeSnapshot ctl;
  CHECK(document_detail::captureGradeSnapshot(app, ctl));
  auto native = ctl;
  for (auto &node : native.grade.graph.nodes) if (node.id == reused.value) {
    node.backend = "native";
    node.identifier = "rawnode.native.exposure";
    node.paramsJson = {{"exposure", "1"}};
  }
  app.renderer.start();
  // Reproduce CTL -> Exposure under the same NodeId, then replay back to CTL.
  // Neither processor may inherit the unrelated processor's view state.
  for (const auto *snapshot : {&native, &ctl}) {
    CHECK(selectNode(app, reused));
    std::snprintf(app.paramFilter, sizeof(app.paramFilter), "old processor");
    findNode(app, reused)->groupOpen["old-processor-group"] = false;
    app.renderer.requestInspection({reused, ports::image});
    CHECK(idle(app));
    CHECK(document_detail::restoreGradeSnapshot(app, *snapshot) && idle(app));
    CHECK(!app.selectedNodeId() && std::string(app.paramFilter).empty());
    CHECK(findNode(app, reused)->groupOpen.count("old-processor-group") == 0);
    CHECK(findNode(app, reused)->storedBackend == (snapshot == &native ? "native" : "ctl"));
    CHECK(app.documentOutput == output && documentStateError(app).empty());
    {
      std::lock_guard<std::mutex> lock(app.displayMutex);
      CHECK(app.processed.result.requested == output);
    }
    // Reconstructing the same logical processor must still retain its state.
    CHECK(selectNode(app, reused));
    std::snprintf(app.paramFilter, sizeof(app.paramFilter), "same processor");
    findNode(app, reused)->groupOpen["same-processor-group"] = false;
    app.renderer.requestInspection({reused, ports::image});
    CHECK(idle(app));
    CHECK(document_detail::restoreGradeSnapshot(app, *snapshot) && idle(app));
    CHECK(app.selectedNodeId() == reused && std::string(app.paramFilter) == "same processor");
    CHECK(!findNode(app, reused)->groupOpen.at("same-processor-group"));
    {
      std::lock_guard<std::mutex> lock(app.displayMutex);
      CHECK(app.processed.result.requested == (OutputRef{reused, ports::image}));
    }
  }
  app.renderer.shutdown();
  return true;
}

bool historyRawFailureSafety() {
  Files files; Options options; options.decode = syntheticRaw;
  App app; CHECK(document_detail::openPhoto(app, files.path("a.nef"), {}, options));
  CHECK(addNativeExposureNode(app)); const auto id = *app.selectedNodeId();
  app.nodePositions[id.value] = {-2, 3};
  document_detail::GradeSnapshot snapshot;
  CHECK(document_detail::captureGradeSnapshot(app, snapshot));
  snapshot.grade.color = {"rec2020", "srgb", "display-p3", "srgb"};
  snapshot.grade.positions[id.value] = {100, 200};
  for (auto &node : snapshot.grade.graph.nodes) if (node.id == id.value) node.paramsJson["exposure"] = "2";
  const auto preview = app.preview; const auto full = app.full;
  const auto *storage = app.full.px.data(); const auto *processor = findNode(app, id)->processor.get();
  const auto input = app.inputEncoding, raw = app.photoRawWorkingEncoding, output = app.outputEncoding;
  const auto source = app.primarySource; const auto endpoint = app.documentOutput;
  const auto sidecar = bytes(inputSidecarPath(app.path));
  std::snprintf(app.paramFilter, sizeof(app.paramFilter), "keep");
  findNode(app, id)->groupOpen["keep"] = true;
  app.renderer.requestInspection({source, ports::image});
  app.renderer.start(); CHECK(idle(app));
  for (int failure = 0; failure < 3; ++failure) {
    Options failed = options;
    if (failure == 0) failed.decode = [](const std::string &, Image &out, ColorEncoding &, bool &, ColorEncoding) {
      out.w = out.h = 1; out.px = {99,99,99,1}; return false;
    };
    if (failure == 1) failed.decode = [](const std::string &, Image &, ColorEncoding &, bool &raw, ColorEncoding) {
      raw = false; return true;
    };
    if (failure == 2) failed.preparePreview = [](const Image &, ColorEncoding, int) -> std::shared_ptr<const Image> {
      throw std::runtime_error("Injected RAW preview preparation failure");
    };
    CHECK(!document_detail::restoreGradeSnapshot(app, snapshot, failed));
    CHECK(!app.getStatus().empty() && idle(app));
    CHECK(findNode(app, id)->processor.get() == processor && app.nodes.size() == 2);
    CHECK(app.primarySource == source && app.documentOutput == endpoint && app.nodePositions.at(id.value).x == -2);
    CHECK(app.full.px == full.px && app.full.px.data() == storage && app.preview == preview);
    CHECK(app.inputEncoding == input && app.photoRawWorkingEncoding == raw && app.outputEncoding == output);
    CHECK(app.selectedNodeId() == id && std::string(app.paramFilter) == "keep" && findNode(app, id)->groupOpen.at("keep"));
    CHECK(bytes(inputSidecarPath(app.path)) == sidecar);
    std::lock_guard<std::mutex> lock(app.displayMutex);
    CHECK(app.processed.result.requested == (OutputRef{source, ports::image}));
  }
  // Export guard returns before attempting a mutation (which would wait on the lease).
  {
    auto exporting = app.renderer.acquireExport(); CHECK(bool(exporting));
    CHECK(!document_detail::restoreGradeSnapshot(app, snapshot, options));
    CHECK(findNode(app, id)->processor.get() == processor);
  }
  app.sidecarWriteBlockedPath = app.path;
  CHECK(!document_detail::restoreGradeSnapshot(app, snapshot, options));
  app.sidecarWriteBlockedPath.clear();
  CHECK(document_detail::restoreGradeSnapshot(app, snapshot, options) && idle(app));
  CHECK(app.inputEncoding == (ColorEncoding{RgbGamut::Rec2020, TransferFunction::SRGB}));
  // a.nef has scene red -0.125 and green 1.5; sRGB OETF then +2 EV.
  // The negative branch is linear; green uses the independent power formula.
  {
    document_detail::DocumentMutation ownership(app);
    const auto result = evaluateGraph({app.nodes, app.primarySource, app.documentOutput}, app.documentOutput,
        {EvaluationPurpose::Preview, app.full.w, app.full.h, app.preview->w, app.preview->h}, app.preview);
    CHECK(result.state == EvaluationState::Available);
    const auto image = std::get<ImageValue>(result.value);
    CHECK(std::abs(image->px[0] - 4 * 12.92 * -0.125) < 1e-5);
    CHECK(std::abs(image->px[1] - 4 * (1.055 * std::pow(1.5, 1.0/2.4) - 0.055)) < 1e-5 && image->px[3] == 0.75f);
  }
  CHECK(bytes(inputSidecarPath(app.path)) == sidecar);
  CHECK(idle(app)); app.renderer.shutdown();
  return true;
}

bool twoPhotoColourOwnership() {
  Files files; const auto a = files.path("a.nef"), b = files.path("b.nef");
  const std::string colorA = R"("color":{"rawWorking":{"colorSpace":"rec2020","gamma":"linear"},"output":{"colorSpace":"display-p3","gamma":"srgb"}})";
  const std::string colorB = R"("color":{"rawWorking":{"colorSpace":"davinci-wide-gamut","gamma":"davinci-intermediate"},"output":{"colorSpace":"aces-ap1","gamma":"linear"}})";
  write(inputSidecarPath(a), file(source, "camera-root", "", "", colorA));
  write(inputSidecarPath(b), file(source, "camera-root", "", "", colorB));
  App app; app.previewRes = 0;
  app.rawWorkingEncoding = {RgbGamut::Rec709, TransferFunction::Rec709};
  app.defaultOutputEncoding = {RgbGamut::ACES_AP0, TransferFunction::Linear};
  const auto rawDefault = app.rawWorkingEncoding, outputDefault = app.defaultOutputEncoding;
  Options options; options.decode = syntheticRaw;
  for (int pass = 0; pass < 3; ++pass) for (bool second : {false, true}) {
    const auto path = second ? b : a;
    CHECK(document_detail::openPhoto(app, path, {}, options));
    const ColorEncoding expectedRaw = second ? ColorEncoding{RgbGamut::DaVinciWideGamut, TransferFunction::DaVinciIntermediate}
                                              : ColorEncoding{RgbGamut::Rec2020, TransferFunction::Linear};
    const ColorEncoding expectedOutput = second ? ColorEncoding{RgbGamut::ACES_AP1, TransferFunction::Linear}
                                                 : ColorEncoding{RgbGamut::DisplayP3, TransferFunction::SRGB};
    CHECK(app.inputIsRaw && app.inputEncoding == expectedRaw && app.photoRawWorkingEncoding == expectedRaw);
    CHECK(app.outputEncoding == expectedOutput && app.rawWorkingEncoding == rawDefault && app.defaultOutputEncoding == outputDefault);
    CHECK(app.full.w == 1600 && app.preview->w == 1280 && app.preview->px[3] == 0.75f);
    const auto red = second ? 0.5 : -0.125;
    CHECK(std::abs(app.full.px[0] - encodeTransfer(red, expectedRaw.gamma)) < 1e-6);
    CHECK(std::abs(app.preview->px[0] - app.full.px[0]) < 1e-6);
    CHECK(std::abs(app.preview->px[1] - encodeTransfer(1.5, expectedRaw.gamma)) < 1e-6);
  }
  const auto full = app.full.px; const auto preview = app.preview; const auto path = app.path;
  CHECK(!document_detail::openPhoto(app, files.path("failed.nef"), {}, options));
  CHECK(app.full.px == full && app.preview == preview && app.path == path && app.inputIsRaw);
  CHECK(!fs::exists(inputSidecarPath(files.path("failed.nef"))));
  CHECK(setOutputEncoding(app, {RgbGamut::Rec709, TransferFunction::SRGB}) && app.defaultOutputEncoding == outputDefault);
  write(inputSidecarPath(a), "invalid");
  CHECK(document_detail::openPhoto(app, a, {}, options) && app.isPhotoProtected());
  CHECK(app.inputEncoding == (ColorEncoding{RgbGamut::Rec709, TransferFunction::Linear}) && app.full.px[0] == -0.125f);
  return true;
}
}  // namespace

bool testPhotoDocuments() {
  for (const auto &test : {
#if defined(__linux__)
       std::make_pair("Linux dialog exit mapping and safe helper absence", linuxDialogDecisions),
#endif
       std::make_pair("scan, thumbnails, Source-only and metadata-only initial grades", scanAndInitialGrades),
       std::make_pair("folder scanning and opening include only direct images", folderImagesStayLocal),
       std::make_pair("shuffled full graph, off-spine nodes, precision, positions and unchanged saves", restorationAndUnchangedSave),
       std::make_pair("Slots and allocation history through the production open/departure path", slotRestoration),
       std::make_pair("protected views, entry-point refusal, legacy protection and failed source decode", protectionAndDecodeFailure),
       std::make_pair("persistent warnings and Retry/Discard/Cancel before navigation/shutdown", failureDecisions),
       std::make_pair("folder auto-open after Discard saves and prompts only once", folderDiscardOnlyOnce),
       std::make_pair("same-photo save-before-reload, atomic failures and strict backend UTF-8", samePhotoAndAtomicFailures),
       std::make_pair("history native/CTL reconstruction, topology, pixels and missing/working availability", historyNativeCtlAndMissing),
       std::make_pair("history NodeId reuse clears unrelated UI state and matching identities preserve it", historyReusedNodeIdentity),
       std::make_pair("history RAW decode/preparation failure preserves ownership and UI state", historyRawFailureSafety),
       std::make_pair("two-photo synthetic RAW colour ownership and coherent full/preview pixels", twoPhotoColourOwnership),
       std::make_pair("failed decoding retains the full prior document and usable editor", failedDecodeRetainsDocument),
       std::make_pair("photo RAW controls and full-grade Replace retain defaults and save actual decoding", rawControlsAndReplace)}) {
    if (!test.second()) { std::fprintf(stderr, "photo document group failed: %s\n", test.first); return false; }
    std::printf("ok  Step 7 photo lifecycle: %s\n", test.first);
  }
  return true;
}
