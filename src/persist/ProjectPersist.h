#pragma once

#include "color/ColorEncoding.h"
#include "GraphModel.h"

#include <map>
#include <optional>
#include <string>
#include <vector>
#include <variant>

// Historical V1/V2 fixture support only; never used by live preferences.
struct PersistGui {
  std::string outputColorSpace;
  std::string outputGamma;
};

struct PersistNode {
  std::string id;
  std::string backend = "ofx";
  std::string identifier;
  std::string label;
  bool enabled = true;
  double strength = 1.0;
  // Legacy V2 only; the graph-native codec excludes UI state.
  std::map<std::string, bool> groupOpen;

  NodeOperation operation = NodeOperation::ImageProcessor;
  InputBindings inputs;
  std::vector<Slot> slots;
  uint64_t nextSlotId = 1;

  // Raw JSON fragment per parameter value (number, bool, string, array, object).
  // Keeping the raw representation lets unavailable/unknown parameters survive
  // load-save cycles even when this build cannot interpret them.
  std::map<std::string, std::string> paramsJson;
};

struct PersistChain {
  // V2 uses the stable node ID. selectedNode is retained only for V1 migration.
  std::string selectedNodeId;
  int selectedNode = -1;
  std::vector<PersistNode> nodes;
};

struct PersistGradeColor {
  // Stable colour IDs. Legacy transfers may omit RAW for raster sources;
  // graph-native grades always require both complete pairs.
  std::string rawColorSpace;
  std::string rawGamma;
  std::string outputColorSpace;
  std::string outputGamma;
};

// Temporary records only. Node order is storage order; consumers own bindings.
struct PersistGraph {
  std::vector<PersistNode> nodes;
  NodeId primarySource;
  OutputRef documentOutput{NodeId{}, ports::image};
};

struct PersistPosition { double x = 0, y = 0; };
using PersistPositions = std::map<std::string, PersistPosition>;

struct PersistGrade {
  PersistGradeColor color;
  PersistGraph graph;
  PersistPositions positions;
};

struct PersistFile {
  std::optional<PersistGrade> grade;
  // An engaged empty object is distinct from absent metadata. Preserve its raw
  // JSON for later section updates; the codec validates the supported semantics.
  std::optional<std::string> metadataJson;
};

// Shared graph-native codec for photo grades and file-level validation.
// Failure leaves the destination unchanged; error describes the first failure.
bool decodeGraphFile(const std::string &json, PersistFile &out, std::string &error,
                     std::vector<std::string> *warnings = nullptr);
bool encodeGraphFile(const PersistFile &file, std::string &json, std::string &error,
                     std::vector<std::string> *warnings = nullptr);

enum class SidecarReadStatus { Absent, Loaded, Error };
enum class PhotoMark { Unmarked, Pick, Reject };

// Graph-native sidecar file operations used by the live editor.
// Paths name the destination sidecar (normally inputSidecarPath(photo)). A lone
// sibling .ofxrawhost.json protects an absent .rawnode.json from creation.
// Read failure/absence leaves out unchanged. Errors include the affected path.
SidecarReadStatus readGraphSidecar(const std::string &path, PersistFile &out, std::string &error,
                                  std::vector<std::string> *warnings = nullptr);
// Validate the existing file and the complete outgoing document, then replace
// atomically. Preserve the untouched section's raw JSON; never restore plugins.
// Failure preserves the destination. No directory creation or copy-over fallback.
// POSIX: require a writable existing file and preserve its permission bits;
// new sidecars default to owner-only 0600.
bool updateSidecarGrade(const std::string &path, const PersistGrade &grade, std::string &error);
bool updateSidecarMark(const std::string &path, PhotoMark mark, std::string &error);

struct PersistSidecar {
  std::string format;
  int version = 0;
  std::string kind;
  std::string sourcePath;
  // Read-only migration field for PR #16-era combined RAW working-space names.
  std::string legacyRawWorkingSpace;
  std::string rawColorSpace;
  std::string rawGamma;
  PersistGui gui;
  PersistChain chain;
};

std::string inputSidecarPath(const std::string &imagePath);
std::string legacyInputSidecarPath(const std::string &imagePath);

bool isSupportedImagePath(const std::string &path);
bool isHostMetadataPath(const std::string &path);
std::vector<std::string> openImageDialogFilters();
std::vector<std::string> listFolderImages(const std::string &folderDir);

// Encode/decode one JSON string value, including quotes. These helpers keep
// processor string parameters valid JSON and correctly handle escaped Unicode.
std::string jsonStringValue(const std::string &value);
bool parseJsonStringValue(const std::string &raw, std::string &out);

// Clipboard v2 is a thin wrapper around the shared grade/node codec. Fragments
// contain no Source and only internal connections; absent inputs stay detached.
struct PersistFragment {
  std::vector<PersistNode> nodes;
  PersistPositions positions;
};
using PersistTransfer = std::variant<PersistGrade, PersistFragment>;
bool encodeTransferPayload(const PersistTransfer &transfer, std::string &json, std::string &error);
bool decodeTransferPayload(const std::string &json, PersistTransfer &out, std::string &error);
// Presets are ordinary graph-native files. Loading requires a grade and ignores
// photo metadata; saving uses updateSidecarGrade after overwrite confirmation.
bool loadPresetFile(const std::string &path, PersistGrade &out, std::string &error);


bool loadSidecarFile(const std::string &path, PersistSidecar &out);
bool saveInputSidecar(const std::string &imagePath, const PersistGui &gui,
                      const PersistChain &chain, const ColorEncoding *rawEncoding = nullptr);
