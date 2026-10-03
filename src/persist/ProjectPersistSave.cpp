#include "persist/ProjectPersist.h"
#include "persist/ProjectPersistPriv.h"

#include "imgio/ImageIO.h"
#include "color/LinearColorTransform.h"
#include "color/TransferFunction.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

std::string jsonEscape(const std::string &s) {
  std::string o;
  o.reserve(s.size() + 8);
  for (unsigned char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\b': o += "\\b"; break;
      case '\f': o += "\\f"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[7];
          std::snprintf(buf, sizeof buf, "\\u%04x", (unsigned)c);
          o += buf;
        } else {
          o += (char)c;
        }
    }
  }
  return o;
}

std::string jsonStringValue(const std::string &value) {
  return std::string("\"") + jsonEscape(value) + "\"";
}

void appendGuiJson(std::ostringstream &o, const PersistGui &g, bool includeSessionDefaults) {
  // Kept in V2 for behaviour compatibility. Essential edit reconstruction
  // lives in graph/document fields; layout state may move fully to workspace state later.
  o << "\"gui\":{"
    << "\"outputColorSpace\":\"" << jsonEscape(g.outputColorSpace) << "\","
    << "\"outputGamma\":\"" << jsonEscape(g.outputGamma) << "\",";
  if (includeSessionDefaults) {
    o << "\"rawDefaultColorSpace\":\"" << jsonEscape(g.rawDefaultColorSpace) << "\","
      << "\"rawDefaultGamma\":\"" << jsonEscape(g.rawDefaultGamma) << "\",";
  }
  o << "\"exportFormat\":" << g.exportFormat << ","
    << "\"jpegQuality\":" << g.jpegQuality << ","
    << "\"previewRes\":" << g.previewRes << ","
    << "\"themeIndex\":" << g.themeIndex << ","
    << "\"showLeft\":" << (g.showLeft ? "true" : "false") << ","
    << "\"showRight\":" << (g.showRight ? "true" : "false") << ","
    << "\"showFilmstrip\":" << (g.showFilmstrip ? "true" : "false") << "}";
}

void appendChainJson(std::ostringstream &o, const PersistChain &chain) {
  o << "\"graph\":{"
    << "\"selectedNodeId\":\"" << jsonEscape(chain.selectedNodeId) << "\","
    << "\"nodes\":[";

  for (size_t i = 0; i < chain.nodes.size(); ++i) {
    const PersistNode &n = chain.nodes[i];
    if (i) o << ',';
    o << '{'
      << "\"id\":\"" << jsonEscape(n.id) << "\","
      << "\"backend\":\"" << jsonEscape(n.backend) << "\","
      << "\"identifier\":\"" << jsonEscape(n.identifier) << "\","
      << "\"label\":\"" << jsonEscape(n.label) << "\","
      << "\"enabled\":" << (n.enabled ? "true" : "false") << ","
      << "\"ui\":{\"groupOpen\":{";

    size_t gi = 0;
    for (const auto &kv : n.groupOpen) {
      if (gi++) o << ',';
      o << '"' << jsonEscape(kv.first) << "\":" << (kv.second ? "true" : "false");
    }

    o << "}},\"params\":{";
    size_t pi = 0;
    for (const auto &kv : n.paramsJson) {
      if (pi++) o << ',';
      o << '"' << jsonEscape(kv.first) << "\":" << kv.second;
    }
    o << "}}";
  }

  // Node-array order is the authoritative serial order in V2. Explicit graph
  // connections are intentionally deferred until the renderer can execute them.
  o << "]}";
}

bool writeFile(const fs::path &path, const std::string &body) {
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  std::ofstream f(path, std::ios::binary);
  if (!f) return false;
  f << body;
  return f.good();
}

std::string workspaceProjectPath(const std::string &workspaceDir) {
  return (fs::path(workspaceDir) / "workspace.ofxrawhost.json").string();
}

std::string inputSidecarPath(const std::string &imagePath) { return imagePath + ".rawnode.json"; }

std::string legacyInputSidecarPath(const std::string &imagePath) { return imagePath + ".ofxrawhost.json"; }

std::string exportSidecarPath(const std::string &exportPath) {
  fs::path p(exportPath);
  return (p.parent_path() / (p.stem().string() + ".rawnode.json")).string();
}

bool isSupportedImagePath(const std::string &path) {
  if (isHostMetadataPath(path)) return false;
  std::string e = fs::path(path).extension().string();
  for (char &c : e) c = (char)tolower((unsigned char)c);
  if (e == ".exr" || e == ".tif" || e == ".tiff" || e == ".png" || e == ".jpg" || e == ".jpeg") return true;
  for (const std::string &rawExt : rawImageExtensions())
    if (e == rawExt) return true;
  return false;
}

bool isHostMetadataPath(const std::string &path) {
  const auto hasSuffix = [&](const char *suffix) {
    const size_t n = std::strlen(suffix);
    return path.size() >= n && path.compare(path.size() - n, n, suffix) == 0;
  };
  return hasSuffix(".rawnode.json") || hasSuffix(".ofxrawhost.json");
}

std::vector<std::string> openImageDialogFilters() {
  std::string patterns = "*.exr *.tif *.tiff *.png *.jpg *.jpeg";
  for (const std::string &ext : rawImageExtensions())
    patterns += " *" + ext;
  return {"Images", patterns};
}

std::vector<std::string> listWorkspaceImages(const std::string &workspaceDir) {
  std::vector<std::string> out;
  std::error_code ec;
  if (!fs::is_directory(workspaceDir, ec)) return out;
  for (const auto &ent : fs::recursive_directory_iterator(workspaceDir, fs::directory_options::skip_permission_denied, ec)) {
    if (ec) break;
    if (!ent.is_regular_file()) continue;
    const std::string p = ent.path().string();
    if (isHostMetadataPath(p)) continue;
    if (!isSupportedImagePath(p)) continue;
    out.push_back(p);
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::string relativeToWorkspace(const std::string &workspaceDir, const std::string &absPath) {
  std::error_code ec;
  fs::path base = fs::weakly_canonical(fs::path(workspaceDir), ec);
  fs::path file = fs::weakly_canonical(fs::path(absPath), ec);
  if (ec) return absPath;
  auto mm = std::mismatch(base.begin(), base.end(), file.begin(), file.end());
  if (mm.first == base.end()) {
    fs::path rel;
    for (auto it = mm.second; it != file.end(); ++it) rel /= *it;
    return rel.string();
  }
  return absPath;
}

bool saveWorkspaceProject(const std::string &workspaceDir, const PersistGui &gui, const std::string &activeImageRel) {
  std::ostringstream o;
  o << '{'
    << "\"format\":\"ofxrawhost-workspace\","
    << "\"version\":1,"
    << "\"activeImage\":\"" << jsonEscape(activeImageRel) << "\",";
  appendGuiJson(o, gui, true);
  o << '}';
  return writeFile(workspaceProjectPath(workspaceDir), o.str());
}

static std::string iso8601Now() {
  using namespace std::chrono;
  const auto now = system_clock::now();
  const std::time_t t = system_clock::to_time_t(now);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  char buf[32];
  std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                tm.tm_hour, tm.tm_min, tm.tm_sec);
  return buf;
}

static void appendSidecarHeader(std::ostringstream &o, const std::string &kind,
                                const std::string &sourcePath, ColorEncoding inputEncoding,
                                const ColorEncoding *rawEncoding) {
  const bool raw = rawEncoding != nullptr;
  const std::string inputName = colorEncodingName(inputEncoding);

  o << '{'
    << "\"format\":\"rawnode-sidecar\","
    << "\"version\":2,"
    << "\"kind\":\"" << jsonEscape(kind) << "\","
    << "\"source\":\"" << jsonEscape(sourcePath) << "\","
    << "\"inputColorSpace\":\"" << jsonEscape(inputName) << "\","
    << "\"workingSpace\":\"" << jsonEscape(inputName) << "\",";

  if (raw) {
    o << "\"raw\":{"
      << "\"colorSpace\":\"" << jsonEscape(rgbGamutId(rawEncoding->gamut)) << "\","
      << "\"gamma\":\"" << jsonEscape(transferFunctionId(rawEncoding->gamma)) << "\"},";
  } else {
    o << "\"raw\":{},";
  }
}

bool saveInputSidecar(const std::string &imagePath, ColorEncoding inputEncoding, const PersistGui &gui,
                      const PersistChain &chain, const ColorEncoding *rawEncoding) {
  std::ostringstream o;
  appendSidecarHeader(o, "input", fs::path(imagePath).filename().string(), inputEncoding, rawEncoding);
  appendGuiJson(o, gui, false);
  o << ',';
  appendChainJson(o, chain);
  o << '}';
  return writeFile(inputSidecarPath(imagePath), o.str());
}

bool saveExportSidecar(const std::string &exportPath, const std::string &sourceImagePath,
                       ColorEncoding inputEncoding, const PersistGui &gui,
                       const PersistChain &chain, const ColorEncoding *rawEncoding) {
  std::ostringstream o;
  appendSidecarHeader(o, "export", sourceImagePath, inputEncoding, rawEncoding);
  o << "\"exportedAt\":\"" << iso8601Now() << "\",";
  appendGuiJson(o, gui, false);
  o << ',';
  appendChainJson(o, chain);
  o << '}';
  return writeFile(exportSidecarPath(exportPath), o.str());
}
