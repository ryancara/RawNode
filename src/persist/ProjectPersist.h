#pragma once

#include "color/ColorEncoding.h"

#include <map>
#include <string>
#include <vector>

enum class ColorSpace;

struct PersistGui {
  // outputIndex is retained only for migration from older V2/workspace JSON.
  int outputIndex = 0;
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

struct PersistSidecar {
  std::string format;
  int version = 0;
  std::string kind;
  std::string sourcePath;
  std::string inputColorSpace;
  std::string workingSpace;
  // RAW encoding is additive within Sidecar V2. rawWorkingSpace is retained
  // for PR #16 sidecars and older builds; new sidecars also persist gamut and
  // transfer function independently.
  std::string rawWorkingSpace;
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

bool loadSidecarFile(const std::string &path, PersistSidecar &out);
bool saveInputSidecar(const std::string &imagePath, ColorEncoding inputEncoding, const PersistGui &gui,
                      const PersistChain &chain, const ColorEncoding *rawEncoding = nullptr);
bool saveExportSidecar(const std::string &exportPath, const std::string &sourceImagePath,
                       ColorEncoding inputEncoding, const PersistGui &gui,
                       const PersistChain &chain, const ColorEncoding *rawEncoding = nullptr);

// Compatibility overloads for older tests/call sites.
bool saveInputSidecar(const std::string &imagePath, ColorSpace inputSpace, const PersistGui &gui,
                      const PersistChain &chain, const ColorEncoding *rawEncoding);
bool saveExportSidecar(const std::string &exportPath, const std::string &sourceImagePath,
                       ColorSpace inputSpace, const PersistGui &gui,
                       const PersistChain &chain, const ColorEncoding *rawEncoding);

std::string relativeToWorkspace(const std::string &workspaceDir, const std::string &absPath);
