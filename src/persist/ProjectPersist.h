#pragma once

#include "color/ColorEncoding.h"
#include "GraphModel.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

struct PersistGui {
  // Read-only migration value for older V2/workspace JSON.
  int legacyOutputIndex = 0;
  std::string outputColorSpace;
  std::string outputGamma;
  std::string rawDefaultColorSpace;
  std::string rawDefaultGamma;
  int exportFormat = 1;
  int jpegQuality = 92;
  int previewRes = 1;
  int themeIndex = 2;
  bool showLeft = true;
  bool showRight = true;
  float leftW = 280.0f;
  float rightW = 420.0f;
  bool showFilmstrip = true;
  float filmstripH = 96.0f;
};

struct PersistNode {
  std::string id;
  std::string backend = "ofx";
  std::string identifier;
  std::string label;
  bool enabled = true;
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

// In-memory codec. Live editor callers still use V2.
// Failure leaves the destination unchanged; error describes the first failure.
bool decodeGraphFile(const std::string &json, PersistFile &out, std::string &error,
                     std::vector<std::string> *warnings = nullptr);
bool encodeGraphFile(const PersistFile &file, std::string &json, std::string &error,
                     std::vector<std::string> *warnings = nullptr);

enum class SidecarReadStatus { Absent, Loaded, Error };
enum class PhotoMark { Unmarked, Pick, Reject };

// Graph-native file operations only; live editor callers still use V2.
// Paths name the destination sidecar (normally inputSidecarPath(photo)). A lone
// sibling .ofxrawhost.json protects an absent .rawnode.json from creation.
// Read failure/absence leaves out unchanged. Errors include the affected path.
SidecarReadStatus readGraphSidecar(const std::string &path, PersistFile &out, std::string &error,
                                  std::vector<std::string> *warnings = nullptr);
// Validate the existing file and the complete outgoing document, then replace
// atomically. Preserve the untouched section's raw JSON; never restore plugins.
// Failure preserves the destination. No directory creation or copy-over fallback.
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

std::string workspaceProjectPath(const std::string &workspaceDir);
std::string inputSidecarPath(const std::string &imagePath);
std::string legacyInputSidecarPath(const std::string &imagePath);

bool isSupportedImagePath(const std::string &path);
bool isHostMetadataPath(const std::string &path);
std::vector<std::string> openImageDialogFilters();
std::vector<std::string> listWorkspaceImages(const std::string &workspaceDir);

bool loadWorkspaceProject(const std::string &workspaceDir, PersistGui &gui, std::string &activeImageRel);
bool saveWorkspaceProject(const std::string &workspaceDir, const PersistGui &gui, const std::string &activeImageRel);

// Encode/decode one JSON string value, including quotes. These helpers keep
// processor string parameters valid JSON and correctly handle escaped Unicode.
std::string jsonStringValue(const std::string &value);
bool parseJsonStringValue(const std::string &raw, std::string &out);

// Versioned processor-transfer payload used by copy/paste and presets.
// kind is currently "node" or "grade"; the graph itself stays Sidecar V2-shaped.
// Additive optional fields may remain within a version only when older readers
// can safely ignore them. Any field that changes the meaning of existing data
// or must not be ignored requires a format-version bump.
std::string serializeTransferPayload(const std::string &kind, const PersistChain &chain,
                                     const PersistGradeColor *color = nullptr);
bool parseTransferPayload(const std::string &json, std::string &kind, PersistChain &chain,
                          PersistGradeColor *color = nullptr);

// Portable node/full-grade presets use the same graph representation as
// transfer payloads, with an independent file-format version.
bool savePresetFile(const std::string &path, const std::string &kind, const PersistChain &chain,
                    const PersistGradeColor *color = nullptr);
bool loadPresetFile(const std::string &path, std::string &kind, PersistChain &chain,
                    PersistGradeColor *color = nullptr);


bool loadSidecarFile(const std::string &path, PersistSidecar &out);
bool saveInputSidecar(const std::string &imagePath, const PersistGui &gui,
                      const PersistChain &chain, const ColorEncoding *rawEncoding = nullptr);

std::string relativeToWorkspace(const std::string &workspaceDir, const std::string &absPath);
