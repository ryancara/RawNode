#include "ExportSafetyTests.h"

#include "AppState.h"
#include "Export.h"
#include "NodeGraph.h"
#include "RenderRuntimeTestAccess.h"
#include "imgio/ImageIOPriv.h"
#include "persist/DocumentActionsPriv.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>

#define CHECK(expression) do { if (!(expression)) { \
  std::fprintf(stderr, "export safety check failed at line %d: %s\n", __LINE__, #expression); return false; \
} } while (false)

namespace {
namespace fs = std::filesystem;
constexpr ColorEncoding kEncoding{RgbGamut::Rec709, TransferFunction::SRGB};
constexpr const char *kRefused = "Export refused: Cannot overwrite the currently opened source photograph";

struct Files {
  fs::path dir = fs::temp_directory_path() /
      ("rawnode-export-safety-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  Files() { fs::create_directory(dir); }
  ~Files() { std::error_code ignored; fs::remove_all(dir, ignored); }
  std::string path(const std::string &name) const { return (dir / name).string(); }
};

std::string bytes(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("Cannot read fixture: " + path);
  return {std::istreambuf_iterator<char>(in), {}};
}

Image raster(int edge = 1) {
  Image image; image.w = image.h = edge;
  // Fixed pseudo-random pixels make the large PNG and JPEG exceed stdio's
  // buffer, independently of compression, without a timing or RNG dependency.
  uint32_t seed = 1;
  for (int i = 0; i < edge * edge; ++i) {
    for (int c = 0; c < 3; ++c) {
      seed = seed * 1664525u + 1013904223u;
      image.px.push_back(float(seed >> 24) / 255.0f);
    }
    image.px.push_back(1.0f);
  }
  return image;
}

bool idle(App &app) {
  return RenderRuntimeTestAccess::wait(app.renderer, [](auto state) {
    return !state.previewBusy && !state.previewPending && !state.displayPending;
  });
}

bool usable(App &app) {
  const auto state = RenderRuntimeTestAccess::snapshot(app.renderer);
  return !state.exportBusy && !state.exportThreadOwned && !state.mutationDepth &&
         app.renderer.canEditParameters();
}

bool exported(App &app, const std::string &path, bool asynchronous, bool existingSidecar = false) {
  const bool accepted = asynchronous ? startExport(app, path) : runExportJob(app, path);
  app.renderer.joinExport();
  CHECK(accepted && idle(app) && usable(app));
  CHECK(app.getStatus().find("Exported ") == 0);
  CHECK(fs::exists(inputSidecarPath(path)) == existingSidecar);
  Image result; ColorEncoding encoding; bool raw = false;
  CHECK(loadImage(path, result, encoding, raw));
  CHECK(result.w == app.full.w && result.h == app.full.h && !raw);
  return true;
}

bool writerCases() {
  Files files;
  std::vector<uint8_t> profile;
  CHECK(profileBytes(kEncoding, profile) && !profile.empty());
  for (bool large : {false, true}) for (bool tagged : {false, true}) for (const char *ext : {".png", ".jpg"}) {
    const auto image = raster(large ? 128 : 1);
    const auto icc = tagged ? profile : std::vector<uint8_t>{};
    const auto good = files.path(std::string("encoded") + ext);
    CHECK(writeImageWithIcc(image, good, icc, 92));
    Image decoded; ColorEncoding encoding; bool raw = false;
    CHECK(loadImage(good, decoded, encoding, raw) && decoded.w == image.w && decoded.h == image.h);
    std::vector<uint8_t> extracted;
    const bool found = std::string(ext) == ".png" ? extractPngIcc(good, extracted) : extractJpgIcc(good, extracted);
    CHECK(found == tagged && extracted == icc);
    // Missing parents exercise ordinary open failures on every platform.
    CHECK(!writeImageWithIcc(image, files.path(std::string("missing/output") + ext), icc, 92));
#if defined(__linux__)
    // Measure the real failing operation with the exact encoded bytes, default
    // buffering and fwrite/fclose sequence used by the production writer.
    const auto encoded = bytes(good);
    FILE *stream = std::fopen("/dev/full", "wb");
    CHECK(stream);
    errno = 0;
    const auto written = std::fwrite(encoded.data(), 1, encoded.size(), stream);
    const int writeError = errno;
    errno = 0;
    const int closed = std::fclose(stream);
    const int closeError = errno;
    if (large) CHECK(written < encoded.size() && writeError == ENOSPC);
    else CHECK(written == encoded.size() && closed == EOF && closeError == ENOSPC);
    std::printf("ok  /dev/full %s %s: fwrite=%zu/%zu, fclose=%d\n", ext, tagged ? "ICC" : "untagged",
                written, encoded.size(), closed);
    const auto full = files.path(std::string("full") + ext);
    std::error_code error;
    fs::remove(full, error);
    fs::create_symlink("/dev/full", full);
    CHECK(!writeImageWithIcc(image, full, icc, 92));
#endif
  }
  return true;
}

bool exportWriteFailure(bool asynchronous, const char *ext, bool large) {
  Files files;
  App app;
  app.full = raster(large ? 128 : 1);
  app.preview = std::make_shared<Image>(app.full);
  CHECK(addNativeExposureNode(app));
  app.renderer.start();
  CHECK(idle(app));
  auto failedPath = files.path(std::string("missing/failed") + ext);
#if defined(__linux__)
  failedPath = files.path(std::string("full") + ext);
  fs::create_symlink("/dev/full", failedPath);
#endif
  const bool accepted = asynchronous ? startExport(app, failedPath) : runExportJob(app, failedPath);
  app.renderer.joinExport();
  CHECK(accepted == asynchronous && idle(app) && usable(app));
  CHECK(app.getStatus() == "Export failed: Could not write image");
  CHECK(!fs::exists(inputSidecarPath(failedPath)));
  CHECK(exported(app, files.path(std::string("recovered") + ext), asynchronous));
  return true;
}

enum class Alias { Exact, RelativeDestination, RelativeSource, Dot, Symlink, Hardlink, SourceSymlink };

bool sourceRefusal(bool asynchronous, const char *ext, Alias alias) {
  Files files;
  const auto source = files.path(std::string("source") + ext);
  CHECK(writeImage(raster(2), source));
  std::string opened = source, destination = source;
  if (alias == Alias::RelativeDestination) destination = fs::relative(source).string();
  if (alias == Alias::RelativeSource) opened = fs::relative(source).string();
  if (alias == Alias::Dot) destination = (files.dir / "." / fs::path(source).filename()).string();
  if (alias == Alias::Symlink || alias == Alias::Hardlink || alias == Alias::SourceSymlink) {
    const auto link = files.path(std::string("alias") + ext);
    std::error_code error;
    if (alias == Alias::Hardlink) fs::create_hard_link(source, link, error);
    else fs::create_symlink(source, link, error);
    if (error) {
      std::printf("SKIP export source alias %d: %s\n", int(alias), error.message().c_str());
      return true;
    }
    if (alias == Alias::SourceSymlink) opened = link;
    else destination = link;
    CHECK(fs::equivalent(opened, destination));
  }
  App app;
  CHECK(openPath(app, opened) && addNativeExposureNode(app));
  const auto node = *app.selectedNodeId();
  CHECK(setNodeParameterValue(app, node, "exposure", 1.0) && saveCurrentInputSidecar(app));
  const auto original = bytes(source), sidecar = bytes(inputSidecarPath(opened));
  const auto pixels = app.full.px;
  app.renderer.start();
  CHECK(idle(app));
  const bool accepted = asynchronous ? startExport(app, destination) : runExportJob(app, destination);
  app.renderer.joinExport();
  CHECK(!accepted && idle(app) && usable(app));
  CHECK(app.getStatus() == kRefused);
  CHECK(bytes(source) == original && bytes(destination) == original);
  CHECK(bytes(inputSidecarPath(opened)) == sidecar && app.full.px == pixels && app.path == opened);
  CHECK(app.selectedNodeId() == node && fs::equivalent(opened, destination));
  // A rejected request releases the lease, restores preview quietly, and leaves
  // both editing and another async/sync export usable.
  CHECK(setNodeParameterValue(app, node, "exposure", 0.5) && idle(app));
  CHECK(exported(app, files.path(std::string("recovered") + ext), asynchronous));
  CHECK(bytes(source) == original && bytes(inputSidecarPath(opened)) == sidecar);
  return true;
}

bool ordinaryOverwrites(bool asynchronous, bool rawSource) {
  Files files;
  const auto source = files.path(rawSource ? "DSC_001.NEF" : "portrait.jpg");
  App app;
  if (rawSource) {
    std::ofstream fixture(source); fixture << "synthetic RAW source"; fixture.close();
    // Reuse the existing decode seam; this tests RAW export policy and the
    // real photo lifecycle, without claiming camera-RAW decoder coverage.
    document_detail::ActionTestOptions options;
    options.decode = [](const std::string &, Image &out, ColorEncoding &encoding, bool &raw, ColorEncoding working) {
      out = raster(2); encoding = working; raw = true; return true;
    };
    CHECK(document_detail::openPhoto(app, source, {}, options) && app.inputIsRaw);
  } else {
    CHECK(writeImage(raster(2), source) && openPath(app, source));
  }
  CHECK(addNativeExposureNode(app) && saveCurrentInputSidecar(app));
  const auto original = bytes(source), sidecar = bytes(inputSidecarPath(source));
  const auto derivative = files.path(rawSource ? "DSC_001.jpg" : "portrait-final.jpg");
  app.renderer.start(); CHECK(idle(app));
  CHECK(exported(app, derivative, asynchronous)); // Nonexistent destination is available.
  CHECK(setNodeParameterValue(app, *app.selectedNodeId(), "exposure", 1.0) && idle(app));
  const auto earlierExport = bytes(derivative);
  CHECK(exported(app, derivative, asynchronous) && bytes(derivative) != earlierExport);
  // Another editable photograph in the filmstrip remains an ordinary target.
  const auto other = files.path("other.jpg");
  CHECK(writeImage(raster(2), other));
  App otherApp; CHECK(openPath(otherApp, other));
  const auto otherSidecar = bytes(inputSidecarPath(other)), otherBefore = bytes(other);
  FilmstripEntry entry; entry.path = other; app.filmstrip.push_back(std::move(entry));
  CHECK(exported(app, other, asynchronous, true));
  CHECK(bytes(other) != otherBefore && bytes(inputSidecarPath(other)) == otherSidecar);
  CHECK(bytes(source) == original && bytes(inputSidecarPath(source)) == sidecar);
  return true;
}
} // namespace

bool testExportSafety() {
  bool ok = writerCases();
  for (bool asynchronous : {false, true}) {
    for (const char *ext : {".png", ".jpg"}) {
      for (bool large : {false, true}) ok = exportWriteFailure(asynchronous, ext, large) && ok;
      for (auto alias : {Alias::Exact, Alias::RelativeDestination, Alias::RelativeSource, Alias::Dot,
                         Alias::Symlink, Alias::Hardlink, Alias::SourceSymlink})
        ok = sourceRefusal(asynchronous, ext, alias) && ok;
    }
    for (bool raw : {false, true}) ok = ordinaryOverwrites(asynchronous, raw) && ok;
  }
  if (ok) std::printf("ok  Export safety: four writer branches, failure status/recovery, active-source aliases and ordinary overwrites\n");
  return ok;
}
