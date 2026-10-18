// Minimal still-image processor host: decode RAW/raster, process, preview, export.

#include "UI.h"
#include "SelfTest.h"
#include "ExportSafetyTests.h"
#include "PhotoDocumentTests.h"
#include "PreferencesTests.h"
#include "PresetClipboardTests.h"
#include "GraphRuntimeTests.h"
#include "GraphDepthTests.h"
#include "GraphModelTests.h"
#include "GraphCommandsTests.h"
#include "GraphEvaluatorTests.h"
#include "GraphPersistenceTests.h"

#include <cstring>
#include <string>

int main(int argc, char **argv) {
  if (argc > 1 && std::string(argv[1]) == "--selftest-graph-depth") return testGraphDepth() ? 0 : 1;
  if (argc > 1 && std::string(argv[1]) == "--selftest-graph")
    return testGraphModel() && testGraphCommands() && testGraphEvaluator() && testGraphPersistence() && testGraphDepth() ? 0 : 1;
  if (argc > 1 && std::string(argv[1]) == "--selftest-export-safety") return testExportSafety() ? 0 : 1;
  if (argc > 1 && std::string(argv[1]) == "--selftest-graph-runtime") return testGraphRuntime() ? 0 : 1;
  if (argc > 1 && std::string(argv[1]) == "--selftest-transfers") return testPresetsAndClipboard() ? 0 : 1;
  if (argc > 1 && std::string(argv[1]) == "--selftest-preferences") return testPreferences() ? 0 : 1;
  if (argc > 1 && std::string(argv[1]) == "--selftest-photo") return testPhotoDocuments() ? 0 : 1;
  if (argc > 1 && !strcmp(argv[1], "--selftest")) return runSelfTests();
  const std::string path = (argc > 1 && argv[1][0] != '-') ? argv[1] : "";
  return runApp(path);
}
