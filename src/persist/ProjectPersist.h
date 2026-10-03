#pragma once

#include "color/ColorEncoding.h"

#include <map>
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
  std::map<std::string, bool> groupOpen;

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
  // Stable colour IDs. RAW fields are empty when the copied source is not RAW.
  std::string rawColorSpace;
  std::string rawGamma;
  std::string outputColorSpace;
  std::string outputGamma;
};

struct PersistSidecar {
  std::string format;
  int version = 0;
  std::string kind;
  std::string sourcePath;
  // Read-only migration field for PR #16-era combined RAW working-space names.
  std::string legacyRawWorkingSpace;
  std::string rawColorSpace;
  std::string rawGamma;
  std::string exportedAt;
  PersistGui gui;
  PersistChain chain;
};

std::string workspaceProjectPath(const std::string &workspaceDir);
std::string inputSidecarPath(const std::string &imagePath);
std::string legacyInputSidecarPath(const std::string &imagePath);
std::string exportSidecarPath(const std::string &exportPath);

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
bool saveExportSidecar(const std::string &exportPath, const std::string &sourceImagePath,
                       const PersistGui &gui, const PersistChain &chain,
                       const ColorEncoding *rawEncoding = nullptr);

std::string relativeToWorkspace(const std::string &workspaceDir, const std::string &absPath);
