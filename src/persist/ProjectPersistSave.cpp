#include "persist/ProjectPersist.h"
#include "persist/ProjectPersistPriv.h"
#include "persist/Json.h"

#include <cmath>
#include <cerrno>
#include <cstdlib>
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

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

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

// Historical V2 fixture writer, never used by live settings/photo saves.
void appendGuiJson(std::ostringstream &o, const PersistGui &g) {
  o << "\"gui\":{\"outputColorSpace\":" << jsonStringValue(g.outputColorSpace)
    << ",\"outputGamma\":" << jsonStringValue(g.outputGamma) << '}';
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

std::vector<std::string> listFolderImages(const std::string &folderDir) {
  std::vector<std::string> out;
  std::error_code ec;
  if (!fs::is_directory(folderDir, ec)) return out;
  for (const auto &ent : fs::directory_iterator(folderDir, fs::directory_options::skip_permission_denied, ec)) {
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
  appendGuiJson(o, gui);
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
  if (isProcessorOperation(node.operation)) {
    if (node.operation == NodeOperation::ImageProcessor) out << ",\n          \"strength\": " << node.strength;
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
void appendPositions(std::ostream &out, const PersistPositions &positions, const std::vector<PersistNode> &nodes,
                     std::vector<std::string> *warnings = nullptr) {
  if (!positions.empty()) {
    out << ",\n    \"positions\": {";
    size_t count = 0;
    for (const auto &position : positions) {
      if (!std::isfinite(position.second.x) || !std::isfinite(position.second.y))
        throw std::runtime_error("Non-finite node position: " + position.first);
      bool exists = false;
      for (const auto &node : nodes) if (node.id == position.first) exists = true;
      if (!exists) {
        if (warnings) warnings->push_back("Dropped position for nonexistent node: " + position.first);
        continue;
      }
      if (count++) out << ',';
      out << "\n      " << jsonStringValue(position.first) << ": {\"x\": " << position.second.x
          << ", \"y\": " << position.second.y << '}';
    }
    out << "\n    }";
  }
}
void appendGrade(std::ostream &out, const PersistGrade &grade, bool testOperations,
                 std::vector<std::string> *warnings = nullptr) {
  const auto error = persist_detail::graphRecordError(grade.graph, testOperations);
  if (!error.empty()) throw std::runtime_error(error);
  out << "{\n    \"color\": {\n      \"rawWorking\": ";
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
  appendPositions(out, grade.positions, grade.graph.nodes, warnings);
  out << "\n  }";
}
}  // namespace

namespace persist_detail {
bool encodeTransferPayload(const PersistTransfer &transfer, std::string &json, std::string &error,
                           bool testOperations) {
  try {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out.precision(std::numeric_limits<double>::max_digits10);
    out << "{\n  \"format\": \"rawnode-transfer\",\n  \"version\": 2,\n  \"kind\": ";
    if (const auto *grade = std::get_if<PersistGrade>(&transfer)) {
      out << "\"grade\",\n  \"grade\": ";
      appendGrade(out, *grade, testOperations);
    } else {
      const auto &fragment = std::get<PersistFragment>(transfer);
      error = fragmentRecordError(fragment, testOperations);
      if (!error.empty()) return false;
      out << "\"fragment\",\n  \"nodes\": [";
      for (size_t i = 0; i < fragment.nodes.size(); ++i) {
        if (i) out << ',';
        appendGraphNode(out, fragment.nodes[i], testOperations);
      }
      out << ']';
      appendPositions(out, fragment.positions, fragment.nodes);
    }
    out << "\n}\n";
    std::string candidate = out.str();
    PersistTransfer checked;
    if (!decodeTransferPayload(candidate, checked, error, testOperations)) return false;
    json = std::move(candidate);
    return true;
  } catch (const std::runtime_error &failure) {
    error = failure.what();
    return false;
  }
}
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
      out << ",\n  \"grade\": ";
      appendGrade(out, *file.grade, testOperations, &collected);
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

namespace {
#ifndef _WIN32
struct SiblingTemporaryFile {
  std::filesystem::path path;
  std::FILE *stream = nullptr;
  ~SiblingTemporaryFile() {
    if (stream) std::fclose(stream);
    if (!path.empty()) { std::error_code ignored; std::filesystem::remove(path, ignored); }
  }
};
#endif

}  // namespace

bool persist_detail::replaceFileAtomically(const std::string &path, const std::string &body, std::string &error,
                      persist_detail::FileFault fault) {
#ifdef _WIN32
  error = path + ": Atomic file replacement is not yet supported on Windows";
  return false;
#else
  using Fault = persist_detail::FileFault;
  const auto fail = [&](const char *operation, int code) {
    error = path + ": " + operation + ": " + std::strerror(code ? code : EIO);
    return false;
  };
  struct stat destination{};
  const int inspected = fault == Fault::PermissionStat ? -1 : lstat(path.c_str(), &destination);
  const int inspectError = fault == Fault::PermissionStat ? EIO : errno;
  if (inspected != 0 && inspectError != ENOENT)
    return fail("Cannot read destination permissions", inspectError);
  const bool present = inspected == 0;
  if (present) {
    if (!S_ISREG(destination.st_mode)) return fail("Destination is not a regular file", EINVAL);
    // Rename needs directory permission alone; retain the ordinary V2 refusal
    // to overwrite a file that the caller cannot write.
    const int writable = fault == Fault::PermissionCheck ? -1 : access(path.c_str(), W_OK);
    if (writable != 0)
      return fail("Destination is not writable", fault == Fault::PermissionCheck ? EACCES : errno);
  }
  // New sidecars retain mkstemp's owner-only 0600 default.
  std::string pattern = path + ".tmp-XXXXXX";
  std::vector<char> name(pattern.begin(), pattern.end());
  name.push_back('\0');
  const int fd = fault == Fault::TempCreate ? -1 : mkstemp(name.data());
  if (fd < 0) return fail("Cannot create sibling temporary file", fault == Fault::TempCreate ? EIO : errno);
  SiblingTemporaryFile temp{fs::path(name.data())};
  const auto abortWrite = [&](const char *operation, int code) {
    fail(operation, code);
    if (temp.stream) { std::fclose(temp.stream); temp.stream = nullptr; }
    std::error_code ec;
    fs::remove(temp.path, ec);
    if (ec) error += "; cannot remove temporary file " + temp.path.string() + ": " + ec.message();
    else temp.path.clear();
    return false;
  };
  temp.stream = fdopen(fd, "wb");
  if (!temp.stream) { const int code = errno; close(fd); return abortWrite("Cannot open temporary stream", code); }
  // A write fault leaves real partial data in our temporary file, so cleanup
  // and preservation are exercised after writing, not just at an early return.
  const size_t size = fault == Fault::Write ? body.size() / 2 : body.size();
  const size_t written = std::fwrite(body.data(), 1, size, temp.stream);
  if (written != body.size())
    return abortWrite("Cannot write temporary file", fault == Fault::Write ? EIO : errno);
  const int flushed = fault == Fault::Flush ? EOF : std::fflush(temp.stream);
  if (flushed != 0)
    return abortWrite("Cannot flush temporary file", fault == Fault::Flush ? EIO : errno);
  if (present) {
    // Copy after writing, since writing can clear set-user/group-ID bits.
    const int copied = fault == Fault::PermissionCopy ? -1 : fchmod(fd, destination.st_mode & 07777);
    if (copied != 0)
      return abortWrite("Cannot copy destination permissions", fault == Fault::PermissionCopy ? EIO : errno);
  }
  int closed = std::fclose(temp.stream);
  temp.stream = nullptr; // fclose consumes the stream even when it reports failure.
  if (fault == Fault::Close) closed = EOF;
  if (closed != 0)
    return abortWrite("Cannot close temporary file", fault == Fault::Close ? EIO : errno);
  // POSIX rename replaces an existing destination atomically on the same
  // filesystem. No unlink/copy fallback and no power-loss durability promise.
  const int replaced = fault == Fault::Replace ? -1 : std::rename(temp.path.c_str(), path.c_str());
  if (replaced != 0)
    return abortWrite("Cannot replace destination", fault == Fault::Replace ? EIO : errno);
  temp.path.clear();
  return true;
#endif
}
namespace persist_detail {
bool updateGraphSidecar(const std::string &path, const PersistGrade *grade, PhotoMark mark,
                        std::string &error, FileFault fault, bool testOperations) {
  std::string json;
  const auto status = readGraphFileText(path, json, error, fault);
  if (status == SidecarReadStatus::Error) return false;
  const auto fail = [&] { error = path + ": " + error; return false; };
  persist_json::Value root;
  PersistFile existing;
  if (status == SidecarReadStatus::Loaded &&
      (!persist_json::read(json, root, error) || !decodeGraphFile(root, existing, error, nullptr, testOperations)))
    return fail();
  std::string outgoing;
  if (grade) {
    existing.grade = *grade;
    if (!encodeGraphFile(existing, outgoing, error, nullptr, testOperations)) return fail();
  } else {
    const char *metadata;
    switch (mark) {
      case PhotoMark::Unmarked: metadata = "{}"; break;
      case PhotoMark::Pick: metadata = R"({"mark":"pick"})"; break;
      case PhotoMark::Reject: metadata = R"({"mark":"reject"})"; break;
      default: error = "Unsupported photo mark"; return fail();
    }
    outgoing = "{\n  \"format\": \"rawnode\",\n  \"version\": 1,\n  \"metadata\": " + std::string(metadata);
    const auto savedGrade = root.object.find("grade");
    if (savedGrade != root.object.end()) outgoing += ",\n  \"grade\": " + savedGrade->second.raw;
    outgoing += "\n}\n";
    PersistFile checked;
    if (!decodeGraphFile(outgoing, checked, error, nullptr, testOperations)) return fail();
  }
  return replaceFileAtomically(path, outgoing, error, fault);
}
}  // namespace persist_detail

bool updateSidecarGrade(const std::string &path, const PersistGrade &grade, std::string &error) {
  return persist_detail::updateGraphSidecar(path, &grade, PhotoMark::Unmarked, error,
                                          persist_detail::FileFault::None, false);
}
bool updateSidecarMark(const std::string &path, PhotoMark mark, std::string &error) {
  return persist_detail::updateGraphSidecar(path, nullptr, mark, error, persist_detail::FileFault::None, false);
}

bool encodeTransferPayload(const PersistTransfer &transfer, std::string &json, std::string &error) {
  return persist_detail::encodeTransferPayload(transfer, json, error, false);
}
