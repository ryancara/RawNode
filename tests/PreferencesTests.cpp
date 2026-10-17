#include "PreferencesTests.h"
#include "AppState.h"
#include "NodeGraph.h"
#include "RenderRuntimeTestAccess.h"
#include "persist/PreferencesPriv.h"
#include "persist/DocumentActionsPriv.h"
#include "ui/ImGuiBackend.h"
#include "ui/DockLayout.h"
#include "imgui.h"
#include "imgui_internal.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>

#define CHECK(expression) do { if (!(expression)) { \
  std::fprintf(stderr, "preferences check failed at line %d: %s\n", __LINE__, #expression); return false; \
} } while (false)

namespace {
namespace fs = std::filesystem;
using Fault = persist_detail::FileFault;
struct Files {
  fs::path dir = fs::temp_directory_path() /
      ("rawnode-preferences-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  Files() { fs::create_directory(dir); }
  ~Files() { std::error_code ignored; fs::remove_all(dir, ignored); }
  std::string path(const std::string &name) const { return (dir / name).string(); }
};
struct CurrentDirectory {
  fs::path previous = fs::current_path();
  explicit CurrentDirectory(const fs::path &dir) { fs::current_path(dir); }
  ~CurrentDirectory() { fs::current_path(previous); }
};
void write(const std::string &path, const std::string &text) {
  std::ofstream out(path, std::ios::binary); out << text; out.close();
  if (!out) throw std::runtime_error("Cannot write fixture: " + path);
}
std::string bytes(const std::string &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("Cannot read fixture: " + path);
  return {std::istreambuf_iterator<char>(input), {}};
}
std::set<std::string> entries(const fs::path &dir) {
  std::set<std::string> result;
  for (const auto &entry : fs::directory_iterator(dir)) result.insert(entry.path().filename().string());
  return result;
}
const std::string header = R"("format":"rawnode-preferences","version":1)";
// Authored independently of the writer. Both pairs deliberately differ from
// built-in defaults, and from the separate saved-grade fixture below.
const std::string configured = "{" + header + R"(,
  "rawWorking":{"colorSpace":"aces-ap1","gamma":"rec709-camera"},
  "output":{"colorSpace":"display-p3","gamma":"linear"},
  "themeIndex":1,"exportFormat":0,"jpegQuality":73,"previewRes":0,
  "showLeft":false,"showRight":false,"showFilmstrip":false,
  "leftW":312.5,"rightW":388.25,"filmstripH":144.5})";
const ColorEncoding storedRaw{RgbGamut::ACES_AP1, TransferFunction::Rec709};
const ColorEncoding storedOutput{RgbGamut::DisplayP3, TransferFunction::Linear};
const ColorEncoding fallbackRaw{RgbGamut::Rec2020, TransferFunction::Linear};
const ColorEncoding fallbackOutput{RgbGamut::Rec709, TransferFunction::SRGB};

bool codecAndOptionalFields() {
  Files files;
  const auto path = files.path("preferences.json");
  Preferences p; p.themeIndex = 0; p.rawWorking = storedRaw;
  std::string warning;
  CHECK(loadPreferences(path, p, warning) && warning.empty());
  CHECK(p.rawWorking == fallbackRaw && p.output == fallbackOutput && p.themeIndex == 2);
  App missing;
  CHECK(initialisePreferences(missing, {path, ""}) && missing.preferencesWarning.empty());
  CHECK(missing.rawWorkingEncoding == fallbackRaw && missing.defaultOutputEncoding == fallbackOutput);
  write(path, configured);
  App startup;
  CHECK(initialisePreferences(startup, {path, ""}) && startup.preferencesWarning.empty());
  CHECK(startup.rawWorkingEncoding == storedRaw && startup.defaultOutputEncoding == storedOutput);
  CHECK(startup.themeIndex == 1 && startup.previewRes == 0 && startup.exportFormat == 0 && startup.jpegQuality == 73);
  CHECK(!startup.showLeft && !startup.showRight && !startup.showFilmstrip);
  CHECK(startup.leftW == 312.5f && startup.rightW == 388.25f && startup.filmstripH == 144.5f);
  CHECK(savePreferences(path, capturePreferences(startup), warning) && warning.empty());
  CHECK(loadPreferences(path, p, warning) && warning.empty());
  CHECK(p.rawWorking == storedRaw && p.output == storedOutput && p.themeIndex == 1 && p.jpegQuality == 73);
  CHECK(p.leftW == 312.5f && p.rightW == 388.25f && p.filmstripH == 144.5f);
  CHECK(!p.showLeft && !p.showRight && !p.showFilmstrip && p.exportFormat == 0 && p.previewRes == 0);

  // A minimal file retains its valid theme and accepts future optional fields.
  write(path, "{" + header + R"(,"themeIndex":1,"future":{"key":[1,2]}})");
  CHECK(loadPreferences(path, p, warning) && warning.empty());
  CHECK(p.themeIndex == 1 && p.jpegQuality == 92 && p.previewRes == 1 && p.showLeft && p.showRight && p.showFilmstrip);
  CHECK(p.rawWorking == fallbackRaw && p.output == fallbackOutput);
  for (const auto *field : {"rawWorking", "output"}) {
    const std::string other = std::string(field) == "rawWorking" ? "output" : "rawWorking";
    write(path, "{" + header + ",\"" + other +
          R"(":{"colorSpace":"display-p3","gamma":"linear"}})");
    CHECK(loadPreferences(path, p, warning) && warning.empty());
    CHECK((std::string(field) == "rawWorking" ? p.rawWorking : p.output) ==
          (std::string(field) == "rawWorking" ? fallbackRaw : fallbackOutput));
    CHECK((std::string(field) == "rawWorking" ? p.output : p.rawWorking) == storedOutput);
    for (const std::string bad : {"null", "7", "[]", R"({})", R"({"colorSpace":"rec709"})",
         R"({"gamma":"linear"})", R"({"colorSpace":false,"gamma":"linear"})",
         R"({"colorSpace":"unknown","gamma":"linear"})", R"({"colorSpace":"rec709","gamma":"unknown"})"}) {
      write(path, "{" + header + ",\"" + field + "\":" + bad + ",\"" + other +
            R"(":{"colorSpace":"display-p3","gamma":"linear"},"themeIndex":1})");
      CHECK(loadPreferences(path, p, warning) && !warning.empty() && p.themeIndex == 1);
      CHECK((std::string(field) == "rawWorking" ? p.rawWorking : p.output) ==
            (std::string(field) == "rawWorking" ? fallbackRaw : fallbackOutput));
      CHECK((std::string(field) == "rawWorking" ? p.output : p.rawWorking) == storedOutput);
    }
  }
  write(path, "{" + header + R"(,"jpegQuality":0,"showLeft":3,"filmstripH":1,"themeIndex":1})");
  CHECK(loadPreferences(path, p, warning) && !warning.empty());
  CHECK(p.jpegQuality == 92 && p.showLeft && p.filmstripH == 96 && p.themeIndex == 1);
  write(path, R"({"format":"rawnode-preferences","version":9})");
  CHECK(!loadPreferences(path, p, warning) && !warning.empty());
  write(path, "{");
  CHECK(!loadPreferences(path, p, warning) && !warning.empty());
  return true;
}

bool atomicFailuresAndPaths() {
  Files files;
  const auto home = files.path("home"), xdg = files.path("config");
  std::string error;
  const auto config = preferences_detail::configPaths(home, xdg, error);
  CHECK(error.empty());
#if defined(__APPLE__)
  const auto expected = fs::path(home) / "Library/Application Support/RawNode";
#else
  const auto expected = fs::path(xdg) / "RawNode";
#endif
  CHECK(config.preferences == (expected / "preferences.json").string());
  CHECK(config.layout == (expected / "layout.ini").string());
  const auto fallback = preferences_detail::configPaths(home, "", error);
#ifndef _WIN32
  CHECK(fallback.preferences == (fs::path(home) /
#ifdef __APPLE__
      "Library/Application Support/RawNode/preferences.json"
#else
      ".config/RawNode/preferences.json"
#endif
      ).string());
#endif
  CHECK(preferences_detail::configPaths("", "", error).preferences.empty() && !error.empty());
  write(files.path("blocker"), "not a directory");
  const auto blocked = preferences_detail::configPaths(files.path("blocker/child"), files.path("blocker/child"), error);
  CHECK(blocked.preferences.empty() && blocked.layout.empty() && !error.empty());
  write(config.preferences, configured);
  const auto original = bytes(config.preferences);
  Preferences p;
  for (const auto fault : {Fault::PermissionStat, Fault::PermissionCheck, Fault::TempCreate,
       Fault::Write, Fault::Flush, Fault::PermissionCopy, Fault::Close, Fault::Replace}) {
    CHECK(!preferences_detail::save(config.preferences, p, error, fault) && !error.empty());
    CHECK(bytes(config.preferences) == original);
    CHECK(entries(expected) == std::set<std::string>{"preferences.json"});
  }
  CHECK(!savePreferences("", p, error) && !error.empty());
  CHECK(!savePreferences(files.path("missing/preferences.json"), p, error));
#ifndef _WIN32
  fs::create_symlink(config.preferences, files.path("linked.json"));
  CHECK(!savePreferences(files.path("linked.json"), p, error) && bytes(config.preferences) == original);
#endif
  return true;
}

bool decodeRaw(const std::string &, Image &image, ColorEncoding &detected, bool &raw, ColorEncoding requested) {
  image.w = image.h = 2; image.px.assign(16, 0.25f);
  detected = requested; raw = true;
  return true;
}
std::string newPhoto(const fs::path &dir, const char *name) {
  const auto path = (dir / name).string(); write(path, "synthetic RAW"); return path;
}
bool idle(App &app) {
  return RenderRuntimeTestAccess::wait(app.renderer, [](auto state) {
    return !state.previewBusy && !state.previewPending && !state.displayPending;
  });
}

bool startupPhotoFolderAndShutdown() {
  Files files;
  fs::create_directory(files.path("cwd")); fs::create_directory(files.path("a")); fs::create_directory(files.path("b"));
  CurrentDirectory cwd(files.path("cwd"));
  std::string warning;
  const auto config = preferences_detail::configPaths(files.path("home"), files.path("config"), warning);
  CHECK(warning.empty());
  write(config.preferences, configured);
  const auto a = newPhoto(files.path("a"), "a.nef"), b = newPhoto(files.path("b"), "b.nef");
  // Retired files are deliberately plausible but contain conflicting choices.
  const std::string legacy = R"({"format":"ofxrawhost-workspace","version":1,"activeImage":"missing.nef","gui":{"themeIndex":0,"showLeft":true,"rawDefaultColorSpace":"rec709","rawDefaultGamma":"linear"}})";
  write(files.path("a/workspace.ofxrawhost.json"), legacy);
  write(files.path("a/.ofxrawhost-layout.ini"), "legacy layout A\n");
  write(files.path("cwd/ofxrawhost.ini"), "legacy CWD layout\n");
  App app;
  CHECK(initialisePreferences(app, config));
  document_detail::ActionTestOptions options; options.decode = decodeRaw;
  CHECK(document_detail::openFolder(app, files.path("a"), {}, options));
  CHECK(fs::equivalent(app.path, a));
  // Critical startup oracle: decoded pixels and new persisted grade consume the
  // hand-authored pairs, so merely reading/storing them cannot pass this test.
  CHECK(app.inputEncoding == storedRaw && app.photoRawWorkingEncoding == storedRaw && app.outputEncoding == storedOutput);
  PersistFile grade; std::string error;
  CHECK(readGraphSidecar(inputSidecarPath(a), grade, error) == SidecarReadStatus::Loaded && grade.grade);
  CHECK(grade.grade->color.rawColorSpace == "aces-ap1" && grade.grade->color.rawGamma == "rec709-camera");
  CHECK(grade.grade->color.outputColorSpace == "display-p3" && grade.grade->color.outputGamma == "linear");
  CHECK(app.themeIndex == 1 && !app.showLeft);
  document_detail::setRawWorkingEncoding(app, RgbGamut::DaVinciWideGamut, TransferFunction::DaVinciIntermediate, options);
  CHECK(setOutputEncoding(app, {RgbGamut::ACES_AP0, TransferFunction::SRGB}));
  CHECK(app.rawWorkingEncoding == storedRaw && app.defaultOutputEncoding == storedOutput);
  CHECK(document_detail::openFolder(app, files.path("b"), {}, options));
  CHECK(fs::equivalent(app.path, b) && app.inputEncoding == storedRaw && app.outputEncoding == storedOutput);
  CHECK(app.themeIndex == 1 && !app.showLeft && app.rawWorkingEncoding == storedRaw && app.defaultOutputEncoding == storedOutput);
  CHECK(document_detail::openPhoto(app, a, {}, options));
  CHECK(app.inputEncoding == (ColorEncoding{RgbGamut::DaVinciWideGamut, TransferFunction::DaVinciIntermediate}));
  CHECK(app.outputEncoding == (ColorEncoding{RgbGamut::ACES_AP0, TransferFunction::SRGB}));
  CHECK(app.rawWorkingEncoding == storedRaw && app.defaultOutputEncoding == storedOutput);
  // A separately authored saved grade must take precedence over global defaults.
  const auto saved = newPhoto(files.path("b"), "saved.nef");
  write(inputSidecarPath(saved), R"({"format":"rawnode","version":1,"grade":{"color":{"rawWorking":{"colorSpace":"rec709","gamma":"linear"},"output":{"colorSpace":"aces-ap0","gamma":"srgb"}},"graph":{"primarySource":"saved","documentOutput":{"node":"saved","port":"image"},"nodes":[{"id":"saved","kind":"source"}]}}})");
  CHECK(document_detail::openPhoto(app, saved, {}, options));
  CHECK(app.inputEncoding == (ColorEncoding{RgbGamut::Rec709, TransferFunction::Linear}));
  CHECK(app.outputEncoding == (ColorEncoding{RgbGamut::ACES_AP0, TransferFunction::SRGB}));

  PersistGrade preset = *grade.grade;
  preset.color.rawColorSpace = "aces-ap0"; preset.color.rawGamma = "linear";
  preset.color.outputColorSpace = "rec2020"; preset.color.outputGamma = "srgb";
  CHECK(document_detail::applyGrade(app, preset, options));
  CHECK(app.photoRawWorkingEncoding == (ColorEncoding{RgbGamut::ACES_AP0, TransferFunction::Linear}));
  CHECK(app.outputEncoding == (ColorEncoding{RgbGamut::Rec2020, TransferFunction::SRGB}));
  CHECK(capturePreferences(app).rawWorking == storedRaw && capturePreferences(app).output == storedOutput);
  app.renderer.start(); CHECK(idle(app));
  const auto originalPreferences = bytes(config.preferences);
  options.beforeSave = [] { return Fault::Replace; };
  app.jpegQuality = 81;
  CHECK(!document_detail::prepareShutdown(app, config.preferences, {}, options));
  CHECK(!app.quit && bytes(config.preferences) == originalPreferences && app.path == saved);
  // Cancellation leaves the actual renderer/editor available for another edit.
  CHECK(addNativeExposureNode(app)); app.renderer.requestPreview(); CHECK(idle(app));
  options.beforeSave = {};
  CHECK(document_detail::prepareShutdown(app, config.preferences, {}, options));
  Preferences p;
  CHECK(loadPreferences(config.preferences, p, warning) && p.jpegQuality == 81 && p.rawWorking == storedRaw && p.output == storedOutput);
  CHECK(readGraphSidecar(inputSidecarPath(saved), grade, error) == SidecarReadStatus::Loaded && grade.grade->graph.nodes.size() == 2);
  CHECK(bytes(files.path("a/workspace.ofxrawhost.json")) == legacy);
  CHECK(bytes(files.path("a/.ofxrawhost-layout.ini")) == "legacy layout A\n");
  CHECK(entries(files.path("cwd")) == std::set<std::string>{"ofxrawhost.ini"});
  CHECK(bytes(files.path("cwd/ofxrawhost.ini")) == "legacy CWD layout\n");
  CHECK(!fs::exists(files.path("b/workspace.ofxrawhost.json")) && !fs::exists(files.path("b/.ofxrawhost-layout.ini")));
  CHECK(!fs::exists(files.path("a/preferences.json")) && !fs::exists(files.path("b/layout.ini")));
  app.renderer.shutdown();
  // Preferences save without any photo/folder having been opened.
  App empty; CHECK(initialisePreferences(empty, config)); empty.themeIndex = 0;
  CHECK(prepareShutdown(empty, config.preferences));
  CHECK(loadPreferences(config.preferences, p, warning) && p.themeIndex == 0);
  return true;
}

struct ImGuiContextScope {
  ImGuiContextScope() { ImGui::CreateContext(); }
  ~ImGuiContextScope() { ImGui::DestroyContext(); }
};
void beginFrame() {
  auto &io = ImGui::GetIO(); io.DisplaySize = {1200, 800}; io.DeltaTime = 1.0f / 60;
  unsigned char *pixels; int width, height;
  io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
  ImGui::NewFrame();
}
bool globalLayout() {
  Files files; fs::create_directory(files.path("cwd")); fs::create_directory(files.path("folder"));
  CurrentDirectory cwd(files.path("cwd"));
  const std::string layout = files.path("layout.ini");
  write(layout, "[Window][Preferences test]\nPos=31,47\nSize=300,200\nCollapsed=0\n\n");
  {
    ImGuiContextScope context;
    ImGuiBackend_ConfigureLayout(layout);
    CHECK(std::string(ImGui::GetIO().IniFilename) == layout);
    beginFrame(); ImGui::Begin("Preferences test");
    CHECK(ImGui::GetWindowPos().x == 31 && ImGui::GetWindowPos().y == 47);
    ImGui::SetWindowPos({79, 83}); ImGui::End(); ImGui::EndFrame();
    const auto *stablePath = ImGui::GetIO().IniFilename;
    App app; CHECK(openFolder(app, files.path("folder")));
    CHECK(ImGui::GetIO().IniFilename == stablePath && std::string(stablePath) == layout);
    ImGui::SaveIniSettingsToDisk(stablePath);
  }
  {
    ImGuiContextScope context; ImGuiBackend_ConfigureLayout(layout);
    beginFrame(); ImGui::Begin("Preferences test");
    CHECK(ImGui::GetWindowPos().x == 79 && ImGui::GetWindowPos().y == 83);
    ImGui::End(); ImGui::EndFrame();
  }
  {
    ImGuiContextScope context; ImGuiBackend_ConfigureLayout("");
    CHECK(ImGui::GetIO().IniFilename == nullptr);
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    App app;
    beginFrame(); DockLayout::BeginMainDockSpace(app); ImGui::EndFrame();
    CHECK(ImGui::DockBuilderGetNode(ImHashStr("OfxRawHostDock", 0, ImHashStr("##DockHost"))) != nullptr);
  }
  CHECK(entries(files.path("cwd")).empty() && entries(files.path("folder")).empty());
  return true;
}
}  // namespace

bool testPreferences() {
  for (const auto &test : {
       std::make_pair("codec, optional fields and pair fallbacks", codecAndOptionalFields),
       std::make_pair("config paths and checked atomic failures", atomicFailuresAndPaths),
       std::make_pair("startup colours, folder retirement and shutdown cancellation", startupPhotoFolderAndShutdown),
       std::make_pair("global native ImGui layout", globalLayout)}) {
    if (!test.second()) { std::fprintf(stderr, "FAILED preferences: %s\n", test.first); return false; }
    std::printf("ok  Step 7 preferences: %s\n", test.first);
  }
  return true;
}
