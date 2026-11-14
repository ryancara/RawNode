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
#include "ui/Themes.h"
#include "color/TransferFunction.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>

#if defined(__linux__)
#include "portable-file-dialogs.h"
#endif
#ifndef _WIN32
#include <grp.h>
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

bool photoMarkActions() {
  Files files;
  const auto a = photo(files, "a.png");
  const auto unopened = files.path("b.NEF");
  write(unopened, "Intentionally undecodable: metadata actions must not open this photograph");
  App app; app.folderDir = files.dir.string(); refreshFilmstrip(app);
  CHECK(app.filmstrip[0].mark == PhotoMark::Unmarked && app.filmstrip[1].mark == PhotoMark::Unmarked);
  CHECK(!fs::exists(inputSidecarPath(a)) && !fs::exists(inputSidecarPath(unopened)));
  // Public semantic action works without an open document, widgets or decoder.
  for (auto mark : {PhotoMark::Pick, PhotoMark::Reject, PhotoMark::Unmarked}) {
    CHECK(assignPhotoMark(app, unopened, mark));
    CHECK(!read(inputSidecarPath(unopened)).grade && photoMark(read(inputSidecarPath(unopened))) == mark);
    CHECK(app.filmstrip[1].mark == mark && app.path.empty() && app.full.px.empty());
    CHECK(app.nodes.size() == 1 && app.history.size() == 0);
    CHECK(!app.currentPhotoMark);
    refreshFilmstrip(app);
    CHECK(app.filmstrip[1].mark == mark);
  }
  CHECK(section(inputSidecarPath(unopened), "metadata") == "{}");

  // Independent hand-authored complete grade: exact formatting, opaque nested
  // parameters, >53-bit integers, positions and non-default colours must survive.
  write(inputSidecarPath(a), file(missing + ',' + exposure + ',' + source, "gain",
      R"(,"positions":{"gain":{"x":-3.25,"y":19}})"));
  CHECK(openPath(app, a));
  CHECK(app.currentPhotoMark == PhotoMark::Unmarked);
  const auto grade = section(inputSidecarPath(a), "grade");
  CHECK(setOutputEncoding(app, {RgbGamut::Rec2020, TransferFunction::Linear}));
  const auto live = capturedGrade(app);  // Unsaved live edits remain authoritative.
  const auto historySize = app.history.size(), cursor = app.history.cursor(), historyBytes = app.history.bytes();
  const auto before = RenderRuntimeTestAccess::snapshot(app.renderer);
  const auto preview = app.preview;
  for (auto mark : {PhotoMark::Pick, PhotoMark::Reject, PhotoMark::Unmarked}) {
    CHECK(assignPhotoMark(app, a, mark));
    CHECK(section(inputSidecarPath(a), "grade") == grade && photoMark(read(inputSidecarPath(a))) == mark);
    CHECK(app.filmstrip[0].mark == mark && app.filmstripIndex == 0 && app.path == a);
    CHECK(app.currentPhotoMark == mark);
    CHECK(app.history.size() == historySize && app.history.cursor() == cursor && app.history.bytes() == historyBytes);
    CHECK(app.preview == preview && capturedGrade(app) == live);
    const auto after = RenderRuntimeTestAccess::snapshot(app.renderer);
    CHECK(after.epoch == before.epoch && after.previewPending == before.previewPending &&
          after.displayPending == before.displayPending && after.mutationDepth == before.mutationDepth);
  }
  CHECK(saveCurrentInputSidecar(app));
  CHECK(photoMark(read(inputSidecarPath(a))) == PhotoMark::Unmarked);
  CHECK(read(inputSidecarPath(a)).grade->color.outputColorSpace == "rec2020");
  return true;
}

bool photoMarkFailures() {
  Files files;
  // Folder opening canonicalizes aliases such as macOS /var -> /private/var.
  files.dir = fs::canonical(files.dir);
  const auto a = photo(files, "a.png"), b = photo(files, "b.png");
  App app; CHECK(openFolder(app, files.dir.string()));
  CHECK(assignPhotoMark(app, a, PhotoMark::Pick));
  const auto original = bytes(inputSidecarPath(a));
  app.persistenceWarning = "Existing grade save warning";
  for (auto fault : {Fault::Read, Fault::PermissionStat, Fault::PermissionCheck, Fault::TempCreate,
                     Fault::Write, Fault::Flush, Fault::PermissionCopy, Fault::Close, Fault::Replace}) {
    Options options; options.beforeSave = [fault] { return fault; };
    const auto directory = entries(files.dir);
    CHECK(!document_detail::assignPhotoMark(app, a, PhotoMark::Reject, options));
    CHECK(bytes(inputSidecarPath(a)) == original && entries(files.dir) == directory);
    CHECK(app.filmstrip[0].mark == PhotoMark::Pick && app.path == a && app.filmstripIndex == 0);
    CHECK(app.currentPhotoMark == PhotoMark::Pick);
    CHECK(!app.photoMarkWarning.empty() && app.persistenceWarning == "Existing grade save warning");
    app.setStatus("Ordinary renderer status");
    CHECK(!app.photoMarkWarning.empty());
  }
  CHECK(assignPhotoMark(app, a, PhotoMark::Reject) && app.photoMarkWarning.empty());
  CHECK(app.persistenceWarning == "Existing grade save warning");
  const auto sidecar = inputSidecarPath(b);
  for (const auto &invalid : {std::string("broken JSON"), std::string(R"({"format":"rawnode","version":999})"),
                              std::string(R"({"format":"rawnode","version":1,"metadata":{"mark":"unknown"}})")}) {
    write(sidecar, invalid); refreshFilmstrip(app);
    CHECK(!app.filmstrip[1].mark && !app.filmstrip[1].markError.empty());
    CHECK(!assignPhotoMark(app, b, PhotoMark::Unmarked));
    CHECK(bytes(sidecar) == invalid && !app.filmstrip[1].mark);
  }
  CHECK(openPath(app, b) && app.isPhotoProtected());
  CHECK(!app.currentPhotoMark && !app.filmstrip[1].mark);
  // Active protection still refuses after the disk file is repaired. Reopening
  // is the established way to leave read-only source viewing.
  write(sidecar, R"({"format":"rawnode","version":1})");
  const auto repaired = bytes(sidecar);
  refreshFilmstrip(app);
  CHECK(!app.filmstrip[1].mark && !app.filmstrip[1].markError.empty());
  CHECK(!assignPhotoMark(app, b, PhotoMark::Pick) && bytes(sidecar) == repaired);
  CHECK(openPath(app, b) && !app.isPhotoProtected());
  CHECK(assignPhotoMark(app, b, PhotoMark::Pick));
  CHECK(fs::remove(sidecar));
  write(legacyInputSidecarPath(b), "protected legacy bytes");
  refreshFilmstrip(app);
  CHECK(!app.filmstrip[1].mark && !app.filmstrip[1].markError.empty());
  CHECK(!assignPhotoMark(app, b, PhotoMark::Pick));
  CHECK(bytes(legacyInputSidecarPath(b)) == "protected legacy bytes" && !fs::exists(sidecar));
  CHECK(fs::remove(legacyInputSidecarPath(b)));
  fs::create_directory(sidecar); refreshFilmstrip(app);
  CHECK(!app.filmstrip[1].mark && !assignPhotoMark(app, b, PhotoMark::Unmarked));
  CHECK(fs::is_directory(sidecar));
  return true;
}

bool photoMarkFiltersAndRefresh() {
  Files files;
  files.dir = fs::canonical(files.dir);
  const auto a = photo(files, "a.png"), b = photo(files, "b.png");
  const auto raw = files.path("c.NEF"); write(raw, "RAW discovery fixture");
  const auto bad = photo(files, "d.png"); write(inputSidecarPath(bad), "invalid");
  write(inputSidecarPath(a), R"({"format":"rawnode","version":1,"metadata":{"mark":"\u0070ick"}})");
  write(inputSidecarPath(b), R"({"format":"rawnode","version":1,"metadata":{"mark":"unmarked"}})");
  App app; CHECK(openFolder(app, files.dir.string()));
  CHECK(assignPhotoMark(app, raw, PhotoMark::Reject));
  auto visible = [&] {
    std::vector<std::string> paths;
    for (const auto &entry : app.filmstrip) if (filmstripEntryVisible(app, entry)) paths.push_back(entry.path);
    return paths;
  };
  CHECK(visible() == (std::vector<std::string>{a, b, raw, bad}));
  for (auto mark : {PhotoMark::Pick, PhotoMark::Unmarked, PhotoMark::Reject}) {
    app.filmstripMarkFilter = mark;
    const auto expected = mark == PhotoMark::Pick ? a : mark == PhotoMark::Unmarked ? b : raw;
    CHECK(visible() == std::vector<std::string>{expected});
    app.filmstripTab = 1;
    CHECK(visible() == (mark == PhotoMark::Reject ? std::vector<std::string>{raw} : std::vector<std::string>{}));
    app.filmstripTab = 2;
    CHECK(visible() == (mark == PhotoMark::Reject ? std::vector<std::string>{} : std::vector<std::string>{expected}));
    app.filmstripTab = 0;
  }
  app.filmstripMarkFilter = PhotoMark::Pick;
  CHECK(assignPhotoMark(app, a, PhotoMark::Reject));
  CHECK(visible().empty() && app.path == a && app.filmstripIndex == 0);  // No auto-advance.
  CHECK(app.currentPhotoMark == PhotoMark::Reject);
  app.filmstripMarkFilter = PhotoMark::Unmarked;
  CHECK(visible() == std::vector<std::string>{b});
  Options failSave; failSave.beforeSave = [] { return Fault::Replace; };
  CHECK(!document_detail::openPhoto(app, visible().front(), {}, failSave));
  CHECK(app.path == a && app.filmstripIndex == 0);  // Normal departure protection.
  CHECK(app.currentPhotoMark == PhotoMark::Reject);
  CHECK(openPath(app, visible().front()) && app.path == b && app.filmstripIndex == 1);
  CHECK(app.currentPhotoMark == PhotoMark::Unmarked);
  CHECK(photoMark(read(inputSidecarPath(a))) == PhotoMark::Reject);

  // No per-frame reads: visibility uses the snapshot until an explicit refresh.
  write(inputSidecarPath(a), R"({"format":"rawnode","version":1,"metadata":{"mark":"pick"}})");
  CHECK(app.filmstrip[0].mark == PhotoMark::Reject);
  refreshFilmstrip(app);
  CHECK(app.filmstrip[0].mark == PhotoMark::Pick && app.filmstrip[2].mark == PhotoMark::Reject);
  CHECK(fs::remove(inputSidecarPath(raw))); refreshFilmstrip(app);
  CHECK(app.filmstrip[2].mark == PhotoMark::Unmarked);
  Files next; next.dir = fs::canonical(next.dir);
  const auto nextPhoto = photo(next, "a.png");
  CHECK(openFolder(app, next.dir.string()));
  CHECK(app.filmstrip.size() == 1 && app.filmstrip[0].mark == PhotoMark::Unmarked && app.path == nextPhoto);
  app.folderDir.clear(); refreshFilmstrip(app);
  CHECK(app.filmstrip.empty() && app.filmstripIndex == -1);
  return true;
}

bool photoMarkOpeningAndIdentity() {
  Files files;
  const auto a = photo(files, "a.png"), b = photo(files, "b.png");
  App app; app.folderDir = files.dir.string(); refreshFilmstrip(app);
  CHECK(!app.currentPhotoMark && app.filmstrip[0].mark == PhotoMark::Unmarked);
  // Change the fixture after scanning, so opening must replace a stale snapshot.
  write(inputSidecarPath(a), R"({"format":"rawnode","version":1,"metadata":{"mark":"pick"}})");
  app.photoMarkWarning = "Previous mark save failure";
  CHECK(openPath(app, a));
  CHECK(app.currentPhotoMark == PhotoMark::Pick && app.filmstrip[0].mark == PhotoMark::Pick);
  CHECK(app.photoMarkWarning.empty());
  CHECK(assignPhotoMark(app, b, PhotoMark::Reject));
  CHECK(app.currentPhotoMark == PhotoMark::Pick && app.filmstrip[1].mark == PhotoMark::Reject);
  app.photoMarkWarning = "Keep on failed navigation";
  Options failDecode;
  failDecode.decode = [](const std::string &, Image &, ColorEncoding &, bool &, ColorEncoding) { return false; };
  CHECK(!document_detail::openPhoto(app, b, {}, failDecode));
  CHECK(app.path == a && app.currentPhotoMark == PhotoMark::Pick && app.filmstripIndex == 0);
  CHECK(app.filmstrip[0].mark == PhotoMark::Pick && app.photoMarkWarning == "Keep on failed navigation");
  // A known mark in a syntactically valid but incompatible grade is still Unknown
  // after protected opening. Refresh alone need not instantiate processors.
  write(inputSidecarPath(b), file(exposure + ',' + source, "gain", "", R"({"mark":"reject"})"));
  auto incompatible = bytes(inputSidecarPath(b));
  incompatible.replace(incompatible.find("\"exposure\":1.0"), 14, "\"exposure\":true");
  write(inputSidecarPath(b), incompatible);
  refreshFilmstrip(app);
  CHECK(app.filmstrip[1].mark == PhotoMark::Reject);
  CHECK(openPath(app, b) && app.isPhotoProtected());
  CHECK(!app.currentPhotoMark && !app.filmstrip[1].mark && !app.filmstrip[1].markError.empty());
  CHECK(app.photoMarkWarning.empty() && bytes(inputSidecarPath(b)) == incompatible);
  const auto spelling = (files.dir / "." / "b.png").string();
  CHECK(!assignPhotoMark(app, spelling, PhotoMark::Unmarked));
  CHECK(!app.currentPhotoMark && bytes(inputSidecarPath(b)) == incompatible);

  CHECK(openPath(app, a));
  const auto aliasSpelling = (files.dir / "." / "a.png").string();
  CHECK(assignPhotoMark(app, aliasSpelling, PhotoMark::Reject));
  CHECK(app.currentPhotoMark == PhotoMark::Reject && app.filmstrip[0].mark == PhotoMark::Reject);
  CHECK(photoMark(read(inputSidecarPath(a))) == PhotoMark::Reject);
  // A directory symlink changes the spelling without changing sidecar identity.
  std::error_code ec;
  fs::create_directory_symlink(files.dir, files.path("alias"), ec);
  if (!ec) {
    const auto alias = files.path("alias/a.png");
    CHECK(assignPhotoMark(app, alias, PhotoMark::Pick));
    CHECK(app.currentPhotoMark == PhotoMark::Pick && app.filmstrip[0].mark == PhotoMark::Pick);
    CHECK(openPath(app, alias) && app.filmstripIndex == 0 && app.currentPhotoMark == PhotoMark::Pick);
    CHECK(app.filmstrip[0].mark == PhotoMark::Pick);
    CHECK(openPath(app, b) && app.isPhotoProtected());
    CHECK(!assignPhotoMark(app, files.path("alias/b.png"), PhotoMark::Unmarked));
    CHECK(!app.currentPhotoMark && bytes(inputSidecarPath(b)) == incompatible);
    CHECK(openPath(app, alias) && app.currentPhotoMark == PhotoMark::Pick);
    CHECK(assignPhotoMark(app, a, PhotoMark::Unmarked));
    CHECK(app.currentPhotoMark == PhotoMark::Unmarked && app.filmstrip[0].mark == PhotoMark::Unmarked);
  } else std::printf("SKIP photo mark directory alias: %s\n", ec.message().c_str());

  const auto unsupported = files.path("unsupported.txt"); write(unsupported, "not a photograph");
  const auto before = entries(files.dir);
  for (const auto &path : {files.path("missing.png"), unsupported, files.dir.string()}) {
    const auto mark = app.currentPhotoMark;
    CHECK(!assignPhotoMark(app, path, PhotoMark::Pick));
    CHECK(app.currentPhotoMark == mark && entries(files.dir) == before);
    CHECK(!fs::exists(inputSidecarPath(path)) && !app.photoMarkWarning.empty());
  }
  // Direct opening needs no folder entry to display the current mark.
  App direct;
  CHECK(openPath(direct, a) && direct.currentPhotoMark == PhotoMark::Unmarked && direct.filmstrip.empty());
  CHECK(assignPhotoMark(direct, a, PhotoMark::Reject) && direct.currentPhotoMark == PhotoMark::Reject);
  CHECK(app.currentPhotoMark == PhotoMark::Unmarked);  // Separate App presentation is transient.
  return true;
}

#ifndef _WIN32
bool readOnlyPhotoMark() {
  Files files;
  const auto path = photo(files, "readonly.png");
  App app; app.folderDir = files.dir.string(); refreshFilmstrip(app);
  CHECK(openPath(app, path) && assignPhotoMark(app, path, PhotoMark::Pick));
  const auto original = bytes(inputSidecarPath(path));
  fs::permissions(inputSidecarPath(path), fs::perms::owner_read);
  CHECK(access(inputSidecarPath(path).c_str(), W_OK) != 0);
  CHECK(!assignPhotoMark(app, path, PhotoMark::Reject));
  CHECK(app.currentPhotoMark == PhotoMark::Pick && app.filmstrip[0].mark == PhotoMark::Pick);
  CHECK(bytes(inputSidecarPath(path)) == original && !app.photoMarkWarning.empty());
  return true;
}

bool photoMarkReadOnlyPermissions() {
  if (geteuid() != 0) return readOnlyPhotoMark();
  // Same ordinary-user contract as SidecarFileTests: root runs a child with
  // its own fixtures after dropping privileges, rather than skipping refusal.
  const pid_t child = fork();
  CHECK(child >= 0);
  if (child == 0) {
    if (setgroups(0, nullptr) != 0 || setgid(65534) != 0 || setuid(65534) != 0) _exit(2);
    _exit(readOnlyPhotoMark() ? 0 : 1);
  }
  int status = 0;
  CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
  return true;
}
#endif

// Real ImGui frames, following the existing UI tests; thumbnail IDs are drawing
// tokens only. No GL upload, worker or alternative input path is involved.
struct FilmstripFrame {
  ImGuiContext *previous = ImGui::GetCurrentContext(), *context = ImGui::CreateContext();
  ImGuiWindow *window = nullptr;
  ImGuiTable *toolbar = nullptr;
  ImRect thumbnail;
  std::string renderedText;
  SaveFailurePrompt prompt = [](const std::string &) { return Decision::Cancel; };
  bool showOther = false;
  bool showFilmstripWidgets = false;
  ImRect filmstripButton, filmstripInput;
  ImRect textInput;
  char text[32] = "editable text";
  FilmstripFrame() {
    auto &io = ImGui::GetIO(); io.IniFilename = nullptr; io.ConfigInputTrickleEventQueue = false;
    unsigned char *pixels; int width, height; io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
  }
  ~FilmstripFrame() { ImGui::DestroyContext(context); ImGui::SetCurrentContext(previous); }
  void tick(App &app, float width = 800, float height = 96, float scale = 1, ImVec2 position = {20, 20}) {
    auto &io = ImGui::GetIO(); io.DisplaySize = {1600, 900}; io.DeltaTime = 1.0f / 60;
    io.DisplayFramebufferScale = {scale, scale};
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(position);
    ImGui::SetNextWindowSize({width, showFilmstripWidgets ? std::max(height, 180.0f) : height});
    ImGui::Begin("Filmstrip test", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove);
    if (showFilmstripWidgets) {
      ImGui::Button("Held widget"); filmstripButton = context->LastItemData.Rect;
      ImGui::SameLine();
      ImGui::InputText("Filmstrip text", text, sizeof text); filmstripInput = context->LastItemData.Rect;
    }
    ImGui::LogToBuffer(0);
    drawFilmstripPanel(app, prompt);
    renderedText = context->LogBuffer.c_str();
    ImGui::LogFinish();
    window = ImGui::GetCurrentWindow();
    thumbnail = context->LastItemData.Rect;
    toolbar = context->Tables.GetByKey(window->GetID("##filmstripToolbar"));
    ImGui::End();
    if (showOther) {
      ImGui::SetNextWindowPos({20, 200}); ImGui::SetNextWindowSize({800, 600});
      ImGui::Begin("Other test", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove);
      ImGui::InputText("Text", text, sizeof text);
      textInput = context->LastItemData.Rect;
      drawNodesPanel(app);
      ImGui::End();
    }
    ImGui::EndFrame();
  }
  void click(App &app, ImVec2 point) {
    ImGui::GetIO().AddMousePosEvent(point.x, point.y); tick(app);
    ImGui::GetIO().AddMouseButtonEvent(0, true); tick(app);
    ImGui::GetIO().AddMouseButtonEvent(0, false); tick(app); tick(app);
  }
  // Selection fixtures use equal square thumbnails. Derive positions from the
  // actual final item rectangle, including the current theme's spacing/padding.
  ImRect thumbnailButton(const App &app, int index) const {
    int after = 0;
    for (int i = index + 1; i < (int)app.filmstrip.size(); ++i)
      if (filmstripEntryVisible(app, app.filmstrip[i])) ++after;
    const float offset = after * (thumbnail.GetWidth() + ImGui::GetStyle().ItemSpacing.x);
    return {{thumbnail.Min.x - offset, thumbnail.Min.y}, {thumbnail.Max.x - offset, thumbnail.Max.y}};
  }
  void select(App &app, int index, bool toggle = false, bool range = false) {
    auto &io = ImGui::GetIO();
    const auto modifier = io.ConfigMacOSXBehaviors ? ImGuiMod_Super : ImGuiMod_Ctrl;
    if (toggle) io.AddKeyEvent(modifier, true);
    if (range) io.AddKeyEvent(ImGuiMod_Shift, true);
    click(app, thumbnailButton(app, index).GetCenter());
    if (toggle) io.AddKeyEvent(modifier, false);
    if (range) io.AddKeyEvent(ImGuiMod_Shift, false);
    tick(app);
  }
  void selectAll(App &app, ImGuiKey modifier) {
    auto &io = ImGui::GetIO();
    io.AddKeyEvent(modifier, true); io.AddKeyEvent(ImGuiKey_A, true); tick(app);
    io.AddKeyEvent(ImGuiKey_A, false); io.AddKeyEvent(modifier, false); tick(app);
  }
  void typeTab(App &app, int tab) {
    const auto *bar = context->TabBars.GetByKey(ImHashStr("##filmstripTabs", 0, toolbar->ID));
    click(app, {bar->BarRect.Min.x + bar->Tabs[tab].Offset + bar->Tabs[tab].Width * 0.5f,
                bar->BarRect.GetCenter().y});
  }
  int thumbnailColorVertices(const App &app, int index, ImGuiCol color) const {
    const auto rect = thumbnailButton(app, index);
    int count = 0;
    for (const auto &vertex : window->DrawList->VtxBuffer)
      if (rect.Contains(vertex.pos) && vertex.col == ImGui::GetColorU32(color)) ++count;
    return count;
  }
  ImRect markButton(PhotoMark mark) const {
    float x = toolbar->Columns[1].WorkMinX;
    const char *labels[] = {"Pick", "Unmarked", "Reject"};
    const PhotoMark marks[] = {PhotoMark::Pick, PhotoMark::Unmarked, PhotoMark::Reject};
    for (int i = 0; i < 3; ++i) {
      const float width = ImGui::CalcTextSize(labels[i]).x + 2 * ImGui::GetStyle().FramePadding.x;
      if (mark == marks[i]) return {{x, toolbar->WorkRect.Min.y},
                                    {x + width, toolbar->WorkRect.Max.y}};
      x += width + ImGui::GetStyle().ItemSpacing.x;
    }
    return {};
  }
  bool highlighted(PhotoMark mark) const {
    const auto rect = markButton(mark);
    for (const auto &vertex : window->DrawList->VtxBuffer)
      if (rect.Contains(vertex.pos) && vertex.col == ImGui::GetColorU32(ImGuiCol_HeaderActive)) return true;
    return false;
  }
};

bool filmstripSelectionFrames() {
  for (const bool mac : {false, true}) {
    Files files;
    const auto a = photo(files, "a.png"), b = photo(files, "b.png"), d = photo(files, "d.png");
    const auto c = files.path("c.NEF"), e = files.path("e.NEF");
    write(c, "Selection must not decode RAW"); write(e, "Selection must not decode RAW");
    write(inputSidecarPath(b), "Protected sidecars do not prevent selection");
    App app; app.folderDir = files.dir.string(); refreshFilmstrip(app);
    CHECK(app.filmstrip.size() == 5);
    FilmstripFrame ui; ImGui::GetIO().ConfigMacOSXBehaviors = mac;
    ui.tick(app); ui.tick(app);
    ui.select(app, 4, false, true); // No anchor: select the click without opening.
    CHECK(app.path.empty() && app.filmstripSelection == (std::unordered_set<std::string>{e}) && app.filmstripAnchor == e);
    ui.select(app, 0);
    CHECK(app.filmstripSelection == (std::unordered_set<std::string>{a}) && app.filmstripAnchor == a);
    CHECK(app.path == a && app.filmstripIndex == 0);
    const auto *pixels = app.full.px.data();
    const auto *previewPixels = app.preview->px.data();
    const auto grade = capturedGrade(app), saved = bytes(inputSidecarPath(a));
    const auto beforeFiles = entries(files.dir);
    const auto historySize = app.history.size(), historyCursor = app.history.cursor();
    const auto before = RenderRuntimeTestAccess::snapshot(app.renderer);
    const auto demand = app.renderer.previewDemand();
    const int thumbGeneration = app.filmstripGen.load();
    const auto thumbQueue = app.thumbQueue;
    // A modifier selection must not finish an Inspector gesture either.
    app.parameterGesture.emplace(); app.parameterGesture->item = 123;
    app.parameterGesture->photoPath = a; app.parameterGesture->node = app.primarySource;
    ui.select(app, 1, true);
    CHECK(app.filmstripSelection == (std::unordered_set<std::string>{a, b}) && app.filmstripAnchor == b);
    ui.select(app, 4, true); // Move the anchor away before deselecting b.
    CHECK(app.filmstripAnchor == e);
    ui.select(app, 1, true);
    CHECK(app.filmstripSelection == (std::unordered_set<std::string>{a, e}) && app.filmstripAnchor == b);
    ui.select(app, 4, true);
    CHECK(app.filmstripSelection == (std::unordered_set<std::string>{a}) && app.filmstripAnchor == e);
    ui.select(app, 1, true);
    ui.select(app, 1, true);
    CHECK(app.filmstripSelection == (std::unordered_set<std::string>{a}) && app.filmstripAnchor == b);
    ui.select(app, 4, false, true);
    CHECK(app.filmstripSelection == (std::unordered_set<std::string>{b, c, d, e}) && app.filmstripAnchor == b);
    ui.select(app, 2, false, true);
    CHECK(app.filmstripSelection == (std::unordered_set<std::string>{b, c})); // Shrinking a range replaces it.
    ui.select(app, 0, true, true);
    CHECK(app.filmstripSelection == (std::unordered_set<std::string>{a, b, c}));
    ui.select(app, 4, false, true); // Restore a range before filtering.
    ui.typeTab(app, 1);
    CHECK(app.filmstripSelection == (std::unordered_set<std::string>{b, c, d, e}));
    ui.select(app, 4, false, true); // Hidden b anchor must not select c.
    CHECK(app.filmstripSelection == (std::unordered_set<std::string>{e}) && app.filmstripAnchor == e);
    ui.select(app, 2, false, true);
    CHECK(app.filmstripSelection == (std::unordered_set<std::string>{c, e}) && app.filmstripAnchor == e);
    app.filmstrip[2].mark = PhotoMark::Pick;
    app.filmstrip[4].mark = PhotoMark::Pick;
    app.filmstripMarkFilter = PhotoMark::Pick; ui.tick(app); ui.tick(app);
    ui.select(app, 2, false, true);
    CHECK(app.filmstripSelection == (std::unordered_set<std::string>{c, e}));
    app.filmstrip[4].mark = PhotoMark::Reject;
    ui.tick(app); ui.tick(app);
    CHECK(app.filmstripSelection == (std::unordered_set<std::string>{c, e})); // Hidden e retained.
    ui.selectAll(app, mac ? ImGuiMod_Super : ImGuiMod_Ctrl);
    CHECK(app.filmstripSelection == (std::unordered_set<std::string>{c}) && app.filmstripAnchor == c);
    app.filmstripMarkFilter.reset(); ui.tick(app); ui.tick(app);
    ui.select(app, 4, true); // First visible is c; the previous anchor is e.
    CHECK(app.filmstripAnchor == e);
    ui.selectAll(app, mac ? ImGuiMod_Super : ImGuiMod_Ctrl);
    CHECK(app.filmstripSelection == (std::unordered_set<std::string>{c, e}) && app.filmstripAnchor == c);
    const auto after = RenderRuntimeTestAccess::snapshot(app.renderer);
    CHECK(app.path == a && app.filmstripIndex == 0 && app.full.px.data() == pixels && app.preview->px.data() == previewPixels);
    CHECK(capturedGrade(app) == grade && bytes(inputSidecarPath(a)) == saved && entries(files.dir) == beforeFiles);
    CHECK(app.history.size() == historySize && app.history.cursor() == historyCursor);
    CHECK(app.parameterGesture && app.parameterGesture->item == 123);
    CHECK(before.epoch == after.epoch && before.previewPending == after.previewPending &&
          before.displayPending == after.displayPending && before.mutationDepth == after.mutationDepth);
    CHECK(app.renderer.previewDemand() == demand);
    CHECK(app.filmstripGen == thumbGeneration && app.thumbQueue == thumbQueue);
    app.parameterGesture.reset();
    app.filmstripMarkFilter = PhotoMark::Unmarked; ui.tick(app); ui.tick(app);
    ui.selectAll(app, mac ? ImGuiMod_Super : ImGuiMod_Ctrl);
    CHECK(app.filmstripSelection.empty() && app.filmstripAnchor.empty());

    // Both filters constrain a range in original visible order; hidden entries
    // between its endpoints must not sneak into the set.
    ui.typeTab(app, 0); app.filmstripMarkFilter = PhotoMark::Pick;
    app.filmstrip[0].mark = PhotoMark::Pick; app.filmstrip[4].mark = PhotoMark::Pick;
    ui.tick(app); ui.tick(app);
    ui.select(app, 0, true); ui.select(app, 4, false, true);
    CHECK(app.filmstripSelection == (std::unordered_set<std::string>{a, c, e}));
    app.filmstripMarkFilter.reset(); ui.tick(app); ui.tick(app);
    CHECK(app.filmstripSelection == (std::unordered_set<std::string>{a, c, e}));
    ui.select(app, 3); // Plain click replaces a multi-selection and opens d.
    CHECK(app.filmstripSelection == (std::unordered_set<std::string>{d}) && app.filmstripAnchor == d);
    CHECK(app.path == d && app.filmstripIndex == 3);
    ui.select(app, 1); // Protected photograph still opens through the usual path.
    CHECK(app.path == b && app.isPhotoProtected() && app.filmstripSelection == (std::unordered_set<std::string>{b}));
    // An absent anchor adds a new click; a hidden anchor also retains an
    // already selected click, rather than toggling it off or clearing others.
    app.filmstripSelection = {a, e}; app.filmstripAnchor.clear();
    ui.select(app, 2, true, true);
    CHECK(app.filmstripSelection == (std::unordered_set<std::string>{a, c, e}) && app.filmstripAnchor == c);
    app.filmstripMarkFilter = PhotoMark::Pick; app.filmstripAnchor = b;
    ui.tick(app); ui.tick(app);
    CHECK(!filmstripEntryVisible(app, app.filmstrip[1]));
    ui.select(app, 4, true, true);
    CHECK(app.filmstripSelection == (std::unordered_set<std::string>{a, c, e}) && app.filmstripAnchor == e);
  }
  return true;
}

bool filmstripPlainClickChangedMark() {
  for (const bool mac : {false, true}) {
    Files files;
    const auto a = photo(files, "a.png"), b = photo(files, "b.png"), c = photo(files, "c.png");
    App app; app.folderDir = files.dir.string(); refreshFilmstrip(app);
    CHECK(openPath(app, a));
    CHECK(assignPhotoMark(app, b, PhotoMark::Pick) && assignPhotoMark(app, c, PhotoMark::Pick));
    app.filmstripMarkFilter = PhotoMark::Pick;
    FilmstripFrame ui; ImGui::GetIO().ConfigMacOSXBehaviors = mac;
    ui.tick(app); ui.tick(app); ui.select(app, 2, true);
    CHECK(app.path == a && app.filmstripSelection == (std::unordered_set<std::string>{c}));
    // Change the unopened photo on disk without refreshing the visible snapshot.
    write(inputSidecarPath(b), R"({"format":"rawnode","version":1,"metadata":{"mark":"reject"}})");
    CHECK(app.filmstrip[1].mark == PhotoMark::Pick && filmstripEntryVisible(app, app.filmstrip[1]));
    ui.select(app, 1);
    CHECK(app.path == b && app.filmstripIndex == 1 && app.currentPhotoMark == PhotoMark::Reject);
    CHECK(!filmstripEntryVisible(app, app.filmstrip[1]));
    CHECK(app.filmstripSelection == (std::unordered_set<std::string>{b}) && app.filmstripAnchor == b);
    CHECK(read(inputSidecarPath(b)).grade && photoMark(read(inputSidecarPath(b))) == PhotoMark::Reject);
  }
  return true;
}

bool filmstripSelectionLifecycle() {
  Files files; files.dir = fs::canonical(files.dir);
  const auto a = photo(files, "a.png"), b = photo(files, "b.png");
  const auto broken = files.path("c.png"); write(broken, "Broken image");
  App app; CHECK(openFolder(app, files.dir.string()));
  FilmstripFrame ui; ui.tick(app); ui.tick(app);
  ui.select(app, 0, true); ui.select(app, 1, true);
  const auto selection = app.filmstripSelection;
  const auto anchor = app.filmstripAnchor;
  const auto pixels = app.full.px.data();
  ui.select(app, 2);
  CHECK(app.path == a && app.full.px.data() == pixels && app.filmstripIndex == 0);
  CHECK(app.filmstripSelection == selection && app.filmstripAnchor == anchor);

  // A directory in place of the sidecar deterministically fails the real save.
  fs::remove(inputSidecarPath(a)); fs::create_directory(inputSidecarPath(a));
  int prompts = 0;
  ui.prompt = [&](const std::string &) { ++prompts; return Decision::Cancel; };
  ui.select(app, 1);
  CHECK(prompts == 1 && app.path == a && app.full.px.data() == pixels);
  CHECK(app.filmstripSelection == selection && app.filmstripAnchor == anchor);
  Files next; next.dir = fs::canonical(next.dir); photo(next, "next.png");
  CHECK(!openFolder(app, next.dir.string(), ui.prompt));
  CHECK(app.folderDir == files.dir && app.filmstripSelection == selection && app.filmstripAnchor == anchor);
  ui.prompt = [&](const std::string &) {
    ++prompts; fs::remove(inputSidecarPath(a)); return Decision::Retry;
  };
  ui.select(app, 1);
  CHECK(prompts == 3 && app.path == b && app.filmstripSelection == (std::unordered_set<std::string>{b}));
  fs::remove(inputSidecarPath(b)); fs::create_directory(inputSidecarPath(b));
  ui.prompt = [&](const std::string &) { ++prompts; return Decision::Discard; };
  ui.select(app, 0);
  CHECK(prompts == 4 && app.path == a && app.filmstripSelection == (std::unordered_set<std::string>{a}));
  fs::remove(inputSidecarPath(b));
  ui.select(app, 1, true);
  const auto both = app.filmstripSelection;
  CHECK(assignPhotoMark(app, a, PhotoMark::Pick) && assignPhotoMark(app, b, PhotoMark::Reject));
  CHECK(app.filmstripSelection == both && app.filmstripAnchor == b);
  refreshFilmstrip(app);
  CHECK(app.filmstripSelection == both && app.filmstripAnchor == b);
  // Reopening the same canonical folder (including an alias spelling) is a refresh.
  CHECK(openFolder(app, (files.dir / ".").string()));
  CHECK(app.filmstripSelection == both && app.filmstripAnchor == b);
  fs::remove(b); refreshFilmstrip(app);
  CHECK(app.filmstripSelection == (std::unordered_set<std::string>{a}) && app.filmstripAnchor.empty());
  CHECK(openFolder(app, next.dir.string()));
  CHECK(app.filmstripSelection.empty() && app.filmstripAnchor.empty());
  app.folderDir.clear(); refreshFilmstrip(app);
  CHECK(app.filmstripSelection.empty() && app.filmstripAnchor.empty());
  return true;
}

bool filmstripSelectionFocusAndThemes() {
  for (const bool mac : {false, true}) {
    App app; app.folderDir = "No filesystem reads needed";
    for (const auto &path : {"a.png", "b.NEF", "c.png"}) {
      FilmstripEntry entry; entry.path = path; entry.mark = PhotoMark::Unmarked;
      entry.tex = 1; entry.tw = entry.th = 16; entry.thumbPending = false;
      app.filmstrip.push_back(entry);
    }
    FilmstripFrame ui; auto &io = ImGui::GetIO(); io.ConfigMacOSXBehaviors = mac;
    ui.tick(app); ui.tick(app);
    ui.select(app, 0, true); // Give the filmstrip focus without opening a photo.
    ui.selectAll(app, mac ? ImGuiMod_Ctrl : ImGuiMod_Super); // Wrong native modifier.
    CHECK(app.filmstripSelection == (std::unordered_set<std::string>{"a.png"}));
    ui.selectAll(app, mac ? ImGuiMod_Super : ImGuiMod_Ctrl);
    CHECK(app.filmstripSelection.size() == 3);
    ui.showFilmstripWidgets = true; ui.tick(app); ui.tick(app);
    app.filmstripSelection = {"a.png"};
    const auto held = ui.filmstripButton.GetCenter();
    io.AddMousePosEvent(held.x, held.y); ui.tick(app);
    io.AddMouseButtonEvent(0, true); ui.tick(app);
    CHECK(ui.context->NavWindow == ui.window && ImGui::IsAnyItemActive() && !io.WantTextInput);
    ui.selectAll(app, mac ? ImGuiMod_Super : ImGuiMod_Ctrl);
    CHECK(app.filmstripSelection == (std::unordered_set<std::string>{"a.png"}));
    io.AddMouseButtonEvent(0, false); ui.tick(app);
    ui.click(app, ui.filmstripInput.GetCenter());
    CHECK(ui.context->NavWindow == ui.window && ImGui::GetInputTextState(ImGui::GetActiveID()));
    ui.selectAll(app, mac ? ImGuiMod_Super : ImGuiMod_Ctrl);
    const auto *filmstripTextState = ImGui::GetInputTextState(ImGui::GetActiveID());
    CHECK(filmstripTextState && filmstripTextState->HasSelection());
    CHECK(app.filmstripSelection == (std::unordered_set<std::string>{"a.png"}));
    // Text capture remains true on the frame Enter deactivates the input.
    // This isolates the text guard from the independently tested active-item guard.
    io.AddKeyEvent(ImGuiKey_Enter, true);
    const auto modifier = mac ? ImGuiMod_Super : ImGuiMod_Ctrl;
    io.AddKeyEvent(modifier, true); io.AddKeyEvent(ImGuiKey_A, true); ui.tick(app);
    CHECK(ui.context->NavWindow == ui.window && !ImGui::IsAnyItemActive() && io.WantTextInput);
    CHECK(app.filmstripSelection == (std::unordered_set<std::string>{"a.png"}));
    io.AddKeyEvent(ImGuiKey_A, false); io.AddKeyEvent(modifier, false);
    io.AddKeyEvent(ImGuiKey_Enter, false); ui.tick(app);
    ui.showFilmstripWidgets = false; ui.tick(app); ui.tick(app);
    // Both selected/open states are independent, including their combination.
    for (int theme = 0; theme < themeCount(); ++theme) {
      applyTheme(theme);
      int selectionVertices[2][2], openVertices[2][2];
      for (const bool selected : {false, true}) for (const bool open : {false, true}) {
        app.filmstripSelection.clear(); if (selected) app.filmstripSelection.insert("a.png");
        app.filmstripIndex = open ? 0 : -1;
        io.AddMousePosEvent(-1, -1); ui.tick(app); ui.tick(app);
        selectionVertices[selected][open] = ui.thumbnailColorVertices(app, 0, ImGuiCol_HeaderActive);
        openVertices[selected][open] = ui.thumbnailColorVertices(app, 0, ImGuiCol_Text);
        CHECK(!ui.thumbnailColorVertices(app, 1, ImGuiCol_HeaderActive));
        CHECK(ui.toolbar->WorkRect.GetHeight() <= ImGui::GetFrameHeight() + 2 && ui.window->ScrollMax.y == 0);
      }
      // Text also draws the mark indicator. Observe the added outline geometry,
      // rather than merely the presence of its colour somewhere in the frame.
      CHECK(ImGui::GetColorU32(ImGuiCol_Text) != ImGui::GetColorU32(ImGuiCol_HeaderActive));
      for (int state = 0; state < 2; ++state) {
        CHECK(selectionVertices[1][state] > selectionVertices[0][state]);
        CHECK(openVertices[state][1] > openVertices[state][0]);
      }
    }
    applyTheme(2); app.filmstripIndex = -1;
    app.filmstripSelection = {"a.png"}; ui.showOther = true;
    for (const auto view : {NodesView::Graph, NodesView::List}) {
      app.nodesView = view;
      CHECK(addNativeExposureNode(app)); const auto node = *app.selectedNodeId();
      ui.tick(app); ui.tick(app);
      ImGuiWindow *child = nullptr;
      for (auto *window : ui.context->Windows)
        if (std::string(window->Name).find(view == NodesView::Graph ? "Other test/##graphCanvasRegion" :
                                                                    "Other test/nodeList") != std::string::npos) child = window;
      CHECK(child);
      ui.click(app, {child->InnerRect.Max.x - 20, child->InnerRect.Max.y - 20});
      selectNode(app, node);
      ui.selectAll(app, mac ? ImGuiMod_Super : ImGuiMod_Ctrl);
      CHECK(app.filmstripSelection == (std::unordered_set<std::string>{"a.png"}) && findNode(app, node));
      // Existing Delete still belongs to the focused Graph/List editor.
      io.AddKeyEvent(ImGuiKey_Delete, true); ui.tick(app);
      io.AddKeyEvent(ImGuiKey_Delete, false); ui.tick(app);
      CHECK(!findNode(app, node) && app.filmstripSelection == (std::unordered_set<std::string>{"a.png"}));
    }
    ui.click(app, ui.textInput.GetCenter());
    CHECK(ImGui::GetInputTextState(ImGui::GetActiveID()));
    ui.selectAll(app, mac ? ImGuiMod_Super : ImGuiMod_Ctrl);
    const auto *textState = ImGui::GetInputTextState(ImGui::GetActiveID());
    CHECK(textState && textState->HasSelection());
    CHECK(app.filmstripSelection == (std::unordered_set<std::string>{"a.png"}));
    io.AddInputCharacter('x'); ui.tick(app);
    CHECK(std::string(ui.text) == "x"); // Native text Select All was preserved.
    CHECK(addNativeExposureNode(app));
    const auto selectedNode = *app.selectedNodeId();
    ui.select(app, 2, true); // Mouse focus transfers back to Filmstrip.
    ui.selectAll(app, mac ? ImGuiMod_Super : ImGuiMod_Ctrl);
    CHECK(app.filmstripSelection.size() == 3);
    const auto nodes = app.nodes.size();
    for (const auto key : {ImGuiKey_Delete, ImGuiKey_Backspace}) {
      io.AddKeyEvent(key, true); ui.tick(app); io.AddKeyEvent(key, false); ui.tick(app);
      CHECK(findNode(app, selectedNode) && app.selectedNodeId() == selectedNode);
      CHECK(app.filmstripSelection == (std::unordered_set<std::string>{"a.png", "b.NEF", "c.png"}));
    }
    CHECK(app.filmstripSelection.size() == 3 && app.filmstrip.size() == 3 && app.nodes.size() == nodes);
  }
  return true;
}

bool photoMarkToolbarFrames() {
  App app; app.folderDir = "filmstrip-fixture"; app.path = "current.png";
  for (const auto mark : {std::optional<PhotoMark>(PhotoMark::Pick), std::optional<PhotoMark>(PhotoMark::Unmarked),
                          std::optional<PhotoMark>(PhotoMark::Reject), std::optional<PhotoMark>()}) {
    FilmstripEntry entry; entry.path = "thumbnail.png"; entry.mark = mark;
    entry.tex = 1; entry.tw = entry.th = 16; entry.thumbPending = false;
    app.filmstrip.push_back(std::move(entry));
  }
  FilmstripFrame ui;
  for (int theme = 0; theme < themeCount(); ++theme) {
    applyTheme(theme);
    for (const float width : {360.0f, 800.0f, 1280.0f}) for (const float height : {96.0f, 160.0f})
      for (const float scale : {1.0f, 2.0f}) for (const bool warning : {false, true}) {
        app.photoMarkWarning = warning ? std::string(500, 'w') : "";
        // Moving the floating window also exercises non-origin clipping.
        ui.tick(app, width, height, scale, {110, 130}); ui.tick(app, width, height, scale, {110, 130});
        CHECK((ui.renderedText.find('!') != std::string::npos) == warning);
        CHECK(ui.toolbar && ui.toolbar->ColumnsCount == 3);
        CHECK(ui.toolbar->WorkRect.GetHeight() <= ImGui::GetFrameHeight() + 2);
        CHECK(ui.thumbnail.Min.y >= ui.toolbar->WorkRect.Max.y);
        CHECK(ui.thumbnail.Max.y <= ui.window->InnerClipRect.Max.y + 0.5f);
        if (ui.thumbnail.GetHeight() < ImGui::GetTextLineHeight() + 4)
          std::fprintf(stderr, "filmstrip theme=%d size=%.0fx%.0f scale=%.0f warning=%d toolbar=%.1f thumbnail=%.1f\n",
                       theme, width, height, scale, warning, ui.toolbar->WorkRect.GetHeight(), ui.thumbnail.GetHeight());
        CHECK(ui.thumbnail.GetHeight() >= ImGui::GetTextLineHeight() + 4);
        CHECK(ui.markButton(PhotoMark::Reject).Max.x < ui.toolbar->Columns[2].WorkMinX);
        CHECK(ui.toolbar->Columns[2].WorkMaxX <= ui.window->InnerClipRect.Max.x + 0.5f);
        CHECK(ui.window->ScrollMax.y == 0);  // No vertical scroll hiding footers.
      }
    for (const auto mark : {PhotoMark::Pick, PhotoMark::Unmarked, PhotoMark::Reject}) {
      app.currentPhotoMark = mark;
      ui.tick(app); ui.tick(app);
      for (const auto choice : {PhotoMark::Pick, PhotoMark::Unmarked, PhotoMark::Reject}) {
        if (ui.highlighted(choice) != (choice == mark))
          std::fprintf(stderr, "filmstrip highlight theme=%d mark=%d choice=%d actual=%d header=%x button=%x\n",
                       theme, int(mark), int(choice), ui.highlighted(choice), ImGui::GetColorU32(ImGuiCol_HeaderActive),
                       ImGui::GetColorU32(ImGuiCol_Button));
        CHECK(ui.highlighted(choice) == (choice == mark));
      }
    }
    app.currentPhotoMark.reset(); ui.tick(app);
    CHECK(!ui.highlighted(PhotoMark::Pick) && !ui.highlighted(PhotoMark::Unmarked) && !ui.highlighted(PhotoMark::Reject));
  }
  applyTheme(2);
  const std::string warningText = "Could not save photo mark: /photos/current.png.rawnode.json: permission denied";
  app.photoMarkWarning = warningText; ui.tick(app); ui.tick(app);
  // Hover the rendered indicator at the end of the filter cell, then observe
  // tooltip text through the same ImGui logging used for widget labels.
  const auto &warningColumn = ui.toolbar->Columns[2];
  ImGui::GetIO().AddMousePosEvent(warningColumn.ContentMaxXUnfrozen - ImGui::CalcTextSize("!").x * 0.5f,
                                 ui.toolbar->WorkRect.GetCenter().y);
  ui.tick(app); ui.tick(app);
  CHECK(ui.renderedText.find(warningText) != std::string::npos);
  app.photoMarkWarning.clear(); ImGui::GetIO().AddMousePosEvent(-1, -1);
  ui.tick(app); ui.tick(app);
  CHECK(ui.renderedText.find('!') == std::string::npos);
  CHECK(ui.renderedText.find(warningText) == std::string::npos);
  // Select each existing type tab through real input after moving it into the table.
  for (const int tab : {1, 2, 0}) {
    const auto *bar = ui.context->TabBars.GetByKey(ImHashStr("##filmstripTabs", 0, ui.toolbar->ID));
    CHECK(bar && bar->Tabs.Size == 3);
    const auto point = ImVec2(bar->BarRect.Min.x + bar->Tabs[tab].Offset + bar->Tabs[tab].Width * 0.5f,
                              bar->BarRect.GetCenter().y);
    ui.click(app, point); CHECK(app.filmstripTab == tab);
  }
  // The same controls remain highlighted without a folder or any visible entry.
  app.folderDir.clear(); app.filmstrip.clear(); app.currentPhotoMark = PhotoMark::Reject;
  ui.tick(app); ui.tick(app); CHECK(ui.highlighted(PhotoMark::Reject));
  app.folderDir = "filmstrip-fixture"; app.filmstripMarkFilter = PhotoMark::Pick;
  FilmstripEntry hidden; hidden.path = app.path; hidden.mark = PhotoMark::Reject;
  app.filmstrip.push_back(hidden);
  CHECK(!filmstripEntryVisible(app, app.filmstrip[0]));
  ui.tick(app); ui.tick(app); CHECK(ui.highlighted(PhotoMark::Reject));
  Files files; const auto target = photo(files, "direct.png");
  App direct; CHECK(openPath(direct, target));
  ui.tick(direct); ui.tick(direct); CHECK(ui.highlighted(PhotoMark::Unmarked));
  ui.click(direct, ui.markButton(PhotoMark::Pick).GetCenter());
  CHECK(direct.currentPhotoMark == PhotoMark::Pick && ui.highlighted(PhotoMark::Pick));
  CHECK(photoMark(read(inputSidecarPath(target))) == PhotoMark::Pick);
  write(inputSidecarPath(target), "invalid sidecar: force UI mark failure");
  ui.click(direct, ui.markButton(PhotoMark::Reject).GetCenter());
  CHECK(direct.currentPhotoMark == PhotoMark::Pick && ui.highlighted(PhotoMark::Pick));
  CHECK(!direct.photoMarkWarning.empty());
  return true;
}

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

bool maskOnlyDocumentBoundaries() {
  Files files; App app;
  const auto a = photo(files, "mask-mode-a.png"), b = photo(files, "mask-mode-b.png", .5f);
  CHECK(openPath(app, a) && addNativeExposureNode(app) && addNativeRadialMaskNode(app));
  const auto radial = *app.selectedNodeId();
  PersistGrade grade; CHECK(captureGrade(app, grade));
  CHECK(saveCurrentInputSidecar(app));
  const auto savedBytes = bytes(inputSidecarPath(a));
  const auto historySize = app.history.size(), historyCursor = app.history.cursor();
  struct Guard { App &app; ~Guard() { app.renderer.shutdown(); } } guard{app};
  app.renderer.start(); CHECK(idle(app));
  const auto checkPhoto = [&] {
    if (maskOnlyPreview(app) || app.renderer.inspectionEndpoint()) return false;
    std::lock_guard<std::mutex> lock(app.displayMutex);
    return app.processed.result.state == EvaluationState::Available &&
        app.processed.result.requested == app.documentOutput && app.processed.result.valueType == ValueType::Image;
  };
  CHECK(setMaskOnlyPreview(app, true) && idle(app));
  CHECK(saveCurrentInputSidecar(app) && bytes(inputSidecarPath(a)) == savedBytes);
  CHECK(app.history.size() == historySize && app.history.cursor() == historyCursor);
  // Replace with exactly the same IDs still resets transient inspection.
  CHECK(applyGrade(app, grade) && idle(app) && checkPhoto());
  CHECK(selectNode(app, radial) && setMaskOnlyPreview(app, true) && idle(app));
  CHECK(openPath(app, b) && idle(app) && checkPhoto());
  CHECK(openPath(app, a) && idle(app) && checkPhoto());
  CHECK(findNode(app, radial) && selectNode(app, radial) && !maskOnlyPreview(app));
  CHECK(setMaskOnlyPreview(app, true) && idle(app));
  CHECK(openPath(app, a) && idle(app) && checkPhoto()); // Same-photo reopen too.
  // Reused IDs with a changed output interface cannot retain Mask inspection.
  CHECK(selectNode(app, radial) && setMaskOnlyPreview(app, true) && idle(app));
  for (auto &node : grade.graph.nodes) if (node.id == radial.value) {
    node.operation = NodeOperation::ImageProcessor; node.identifier = "rawnode.native.exposure";
    node.paramsJson = {{"exposure", "0"}};
    node.inputs.clear(); node.inputs[ports::image] = grade.graph.documentOutput;
    node.inputs[ports::mask] = std::nullopt;
  }
  CHECK(applyGrade(app, grade) && idle(app) && checkPhoto());
  CHECK(selectNode(app, radial) && !selectedMaskOutput(app) && !setMaskOnlyPreview(app, true));
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
       std::make_pair("filmstrip plain/toggle/range/visible-all selection with native modifiers and no editing/render effects", filmstripSelectionFrames),
       std::make_pair("plain-click opening selects the clicked path even when refreshed marks hide it", filmstripPlainClickChangedMark),
       std::make_pair("filmstrip selection survives failed/cancelled navigation, marking and refresh; clears on folder changes", filmstripSelectionLifecycle),
       std::make_pair("filmstrip selected/open theme states and keyboard isolation from Graph/List/text", filmstripSelectionFocusAndThemes),
       std::make_pair("single-row filmstrip geometry, warning footprint and active marks across themes/DPI", photoMarkToolbarFrames),
       std::make_pair("photo opening, Unknown protection, warnings, aliases and invalid mark targets", photoMarkOpeningAndIdentity),
#ifndef _WIN32
       std::make_pair("ordinary-user read-only photo marking, including a root test runner", photoMarkReadOnlyPermissions),
#endif
       std::make_pair("UI-independent photo mark transitions preserve exact grades, history and render demand", photoMarkActions),
       std::make_pair("failed/protected mark writes preserve bytes, displayed marks and grade warnings", photoMarkFailures),
       std::make_pair("independent mark/type filters, normal navigation and folder refresh", photoMarkFiltersAndRefresh),
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
       std::make_pair("Mask Only is transient across save, photo switching/reopen and grade/identity replacement", maskOnlyDocumentBoundaries),
       std::make_pair("history RAW decode/preparation failure preserves ownership and UI state", historyRawFailureSafety),
       std::make_pair("two-photo synthetic RAW colour ownership and coherent full/preview pixels", twoPhotoColourOwnership),
       std::make_pair("failed decoding retains the full prior document and usable editor", failedDecodeRetainsDocument),
       std::make_pair("photo RAW controls and full-grade Replace retain defaults and save actual decoding", rawControlsAndReplace)}) {
    if (!test.second()) { std::fprintf(stderr, "photo document group failed: %s\n", test.first); return false; }
    std::printf("ok  Step 7 photo lifecycle: %s\n", test.first);
  }
  return true;
}
