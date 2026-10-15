#include "persist/ProjectPersist.h"
#include "persist/ProjectPersistPriv.h"
#include "persist/Json.h"

#include <cmath>
#include <limits>
#include <locale>
#include <stdexcept>

#include "imgio/ImageIO.h"
#include "color/LinearColorTransform.h"
#include "color/TransferFunction.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
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

static std::string serializeGraphEnvelope(const char *format, const std::string &kind,
                                          const PersistChain &chain,
                                          const PersistGradeColor *color) {
  std::ostringstream o;
  o << '{'
    << "\"format\":\"" << jsonEscape(format) << "\","
    << "\"version\":1,"
    << "\"kind\":\"" << jsonEscape(kind) << "\",";
  if (color && kind == "grade") {
    o << "\"color\":{"
      << "\"rawColorSpace\":\"" << jsonEscape(color->rawColorSpace) << "\","
      << "\"rawGamma\":\"" << jsonEscape(color->rawGamma) << "\","
      << "\"outputColorSpace\":\"" << jsonEscape(color->outputColorSpace) << "\","
      << "\"outputGamma\":\"" << jsonEscape(color->outputGamma) << "\"},";
  }
  appendChainJson(o, chain);
  o << '}';
  return o.str();
}

std::string serializeTransferPayload(const std::string &kind, const PersistChain &chain,
                                     const PersistGradeColor *color) {
  return serializeGraphEnvelope("rawnode-transfer", kind, chain, color);
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

bool isSupportedImagePath(const std::string &path) {
  if (isHostMetadataPath(path)) return false;
  return isRasterImagePath(path) || isRawImagePath(path);
}

bool isHostMetadataPath(const std::string &path) {
  const auto hasSuffix = [&](const char *suffix) {
    const size_t n = std::strlen(suffix);
    return path.size() >= n && path.compare(path.size() - n, n, suffix) == 0;
  };
  return hasSuffix(".rawnode.json") || hasSuffix(".ofxrawhost.json");
}

std::vector<std::string> openImageDialogFilters() {
  std::string patterns;
  const auto appendPatterns = [&](const std::vector<std::string> &extensions) {
    for (const std::string &ext : extensions) {
      if (!patterns.empty()) patterns += ' ';
      patterns += "*" + ext;
    }
  };
  appendPatterns(rasterImageExtensions());
  appendPatterns(rawImageExtensions());
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

bool savePresetFile(const std::string &path, const std::string &kind,
                    const PersistChain &chain, const PersistGradeColor *color) {
  if (kind != "node" && kind != "grade") return false;
  return writeFile(fs::path(path), serializeGraphEnvelope("rawnode-preset", kind, chain, color));
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

static void appendSidecarHeader(std::ostringstream &o, const std::string &kind,
                                const std::string &sourcePath,
                                const ColorEncoding *rawEncoding) {
  const bool raw = rawEncoding != nullptr;

  o << '{'
    << "\"format\":\"rawnode-sidecar\","
    << "\"version\":2,"
    << "\"kind\":\"" << jsonEscape(kind) << "\","
    << "\"source\":\"" << jsonEscape(sourcePath) << "\",";

  if (raw) {
    o << "\"raw\":{"
      << "\"colorSpace\":\"" << jsonEscape(rgbGamutId(rawEncoding->gamut)) << "\","
      << "\"gamma\":\"" << jsonEscape(transferFunctionId(rawEncoding->gamma)) << "\"},";
  } else {
    o << "\"raw\":{},";
  }
}

bool saveInputSidecar(const std::string &imagePath, const PersistGui &gui,
                      const PersistChain &chain, const ColorEncoding *rawEncoding) {
  std::ostringstream o;
  appendSidecarHeader(o, "input", fs::path(imagePath).filename().string(), rawEncoding);
  appendGuiJson(o, gui, false);
  o << ',';
  appendChainJson(o, chain);
  o << '}';
  return writeFile(inputSidecarPath(imagePath), o.str());
}

namespace {
void appendEndpoint(std::ostream &out, const OutputRef &ref) {
  out << "{\"node\": " << jsonStringValue(ref.nodeId.value)
      << ", \"port\": " << jsonStringValue(ref.outputPortId.value) << '}';
}
void appendBindings(std::ostream &out, const InputBindings &bindings) {
  out << '{';
  size_t count = 0;
  for (const auto &binding : bindings) {
    if (count++) out << ", ";
    out << jsonStringValue(binding.first.value) << ": ";
    if (binding.second) appendEndpoint(out, *binding.second);
    else out << "null";
  }
  out << '}';
}
void appendGraphNode(std::ostream &out, const PersistNode &node, bool testOperations) {
  out << "        {\n          \"id\": " << jsonStringValue(node.id)
      << ",\n          \"kind\": " << jsonStringValue(persist_detail::operationName(node.operation, testOperations));
  if (node.operation == NodeOperation::Source) { out << "\n        }"; return; }
  if (node.operation == NodeOperation::ImageProcessor) {
    out << ",\n          \"backend\": " << jsonStringValue(node.backend)
        << ",\n          \"identifier\": " << jsonStringValue(node.identifier);
    if (!node.label.empty()) out << ",\n          \"label\": " << jsonStringValue(node.label);
    out << ",\n          \"params\": {";
    size_t count = 0;
    for (const auto &param : node.paramsJson) {
      persist_json::Value value;
      std::string error;
      if (!persist_json::read(param.second, value, error))
        throw std::runtime_error("Invalid parameter JSON: " + param.first + ": " + error);
      if (count++) out << ',';
      out << "\n            " << jsonStringValue(param.first) << ": " << value.raw;
    }
    if (count) out << "\n          ";
    out << '}';
  }
  out << ",\n          \"enabled\": " << (node.enabled ? "true" : "false") << ",\n          \"inputs\": ";
  appendBindings(out, node.inputs);
  if (!nodeInterface(node.operation).slotInputs.empty()) {
    out << ",\n          \"slots\": [";
    size_t count = 0;
    for (const auto &slot : node.slots) {
      if (count++) out << ',';
      out << "\n            {\"id\": " << jsonStringValue(std::to_string(slot.id.value))
          << ", \"settings\": {\"enabled\": " << (slot.settings.enabled ? "true" : "false") << "}, \"inputs\": ";
      appendBindings(out, slot.inputs);
      out << '}';
    }
    if (count) out << "\n          ";
    out << "],\n          \"nextSlotId\": " << jsonStringValue(std::to_string(node.nextSlotId));
  }
  out << "\n        }";
}
void appendColorPair(std::ostream &out, const std::string &space, const std::string &gamma) {
  out << "{\"colorSpace\": " << jsonStringValue(space) << ", \"gamma\": " << jsonStringValue(gamma) << '}';
}
}  // namespace

namespace persist_detail {
bool encodeGraphFile(const PersistFile &file, std::string &json, std::string &error,
                     std::vector<std::string> *warnings, bool testOperations) {
  error.clear();
  try {
    std::vector<std::string> collected;
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out.precision(std::numeric_limits<double>::max_digits10);
    out << "{\n  \"format\": \"rawnode\",\n  \"version\": 1";
    if (file.metadataJson) {
      persist_json::Value metadata;
      if (!persist_json::read(*file.metadataJson, metadata, error)) return false;
      out << ",\n  \"metadata\": " << metadata.raw;
    }
    if (file.grade) {
      const auto &grade = *file.grade;
      error = graphRecordError(grade.graph, testOperations);
      if (!error.empty()) return false;
      out << ",\n  \"grade\": {\n    \"color\": {\n      \"rawWorking\": ";
      appendColorPair(out, grade.color.rawColorSpace, grade.color.rawGamma);
      out << ",\n      \"output\": ";
      appendColorPair(out, grade.color.outputColorSpace, grade.color.outputGamma);
      out << "\n    },\n    \"graph\": {\n      \"primarySource\": " << jsonStringValue(grade.graph.primarySource.value)
          << ",\n      \"documentOutput\": ";
      appendEndpoint(out, grade.graph.documentOutput);
      out << ",\n      \"nodes\": [\n";
      for (size_t i = 0; i < grade.graph.nodes.size(); ++i) {
        if (i) out << ",\n";
        appendGraphNode(out, grade.graph.nodes[i], testOperations);
      }
      out << "\n      ]\n    }";
      if (!grade.positions.empty()) {
        out << ",\n    \"positions\": {";
        size_t count = 0;
        for (const auto &position : grade.positions) {
          if (!std::isfinite(position.second.x) || !std::isfinite(position.second.y))
            throw std::runtime_error("Non-finite node position: " + position.first);
          bool exists = false;
          for (const auto &node : grade.graph.nodes) if (node.id == position.first) exists = true;
          if (!exists) { collected.push_back("Dropped position for nonexistent node: " + position.first); continue; }
          if (count++) out << ',';
          out << "\n      " << jsonStringValue(position.first) << ": {\"x\": " << position.second.x
              << ", \"y\": " << position.second.y << '}';
        }
        out << "\n    }";
      }
      out << "\n  }";
    }
    out << "\n}\n";
    std::string candidate = out.str();
    // Use the one schema reader for writer validation as well. This catches
    // invalid UTF-8/colour/metadata and rejects malformed opaque fragments.
    PersistFile checked;
    if (!decodeGraphFile(candidate, checked, error, nullptr, testOperations)) return false;
    json = std::move(candidate);
    if (warnings) *warnings = std::move(collected);
    return true;
  } catch (const std::runtime_error &failure) {
    error = failure.what();
    return false;
  }
}
}  // namespace persist_detail

bool encodeGraphFile(const PersistFile &file, std::string &json, std::string &error,
                     std::vector<std::string> *warnings) {
  return persist_detail::encodeGraphFile(file, json, error, warnings, false);
}
