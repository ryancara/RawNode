#include "PresetClipboardTests.h"

#include "NodeGraph.h"
#include "GraphCommands.h"
#include "GraphEvaluator.h"
#include "GraphPersistenceTestAccess.h"
#include "RenderRuntimeTestAccess.h"
#include "persist/DocumentActionsPriv.h"
#include "ui/UiContext.h"
#include "imgui.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <stdexcept>

#define CHECK(expression) do { if (!(expression)) { \
  std::fprintf(stderr, "preset/clipboard check failed at line %d: %s\n", __LINE__, #expression); return false; \
} } while (false)

namespace {
namespace fs = std::filesystem;
struct Files {
  fs::path dir = fs::temp_directory_path() / ("rawnode-transfers-" +
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  Files() { fs::create_directories(dir); }
  ~Files() { std::error_code error; fs::remove_all(dir, error); }
  std::string path(const char *name) const { return (dir / name).string(); }
};
struct Clipboard {
  ImGuiContext *previous = ImGui::GetCurrentContext();
  ImGuiContext *context = ImGui::CreateContext();
  std::string text = "original clipboard";
  Clipboard() {
    auto &platform = ImGui::GetPlatformIO();
    platform.Platform_ClipboardUserData = &text;
    platform.Platform_GetClipboardTextFn = [](ImGuiContext *) {
      return static_cast<std::string *>(ImGui::GetPlatformIO().Platform_ClipboardUserData)->c_str();
    };
    platform.Platform_SetClipboardTextFn = [](ImGuiContext *, const char *value) {
      *static_cast<std::string *>(ImGui::GetPlatformIO().Platform_ClipboardUserData) = value;
    };
  }
  ~Clipboard() { ImGui::DestroyContext(context); ImGui::SetCurrentContext(previous); }
};
void write(const std::string &path, const std::string &text) {
  std::ofstream out(path, std::ios::binary); out << text;
  if (!out.good()) throw std::runtime_error("Could not write fixture");
}
std::string bytes(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), {});
}
OutputRef image(const std::string &id) { return {NodeId{id}, ports::image}; }
const std::string source = R"({"id":"camera","kind":"source"})";
const std::string color = R"("color":{"rawWorking":{"colorSpace":"rec2020","gamma":"linear"},"output":{"colorSpace":"display-p3","gamma":"srgb"}})";
std::string file(const std::string &nodes, const std::string &output = "camera", const std::string &extra = "") {
  return "{\"format\":\"rawnode\",\"version\":1,\"metadata\": { \"mark\" : \"pick\" },\"grade\":{" + color +
      ",\"graph\":{\"primarySource\":\"camera\",\"documentOutput\":{\"node\":\"" + output +
      "\",\"port\":\"image\"},\"nodes\":[" + nodes + "]}" + extra + "}}";
}
std::string processor(const std::string &id, const std::string &upstream, const std::string &exposure = "1") {
  return "{\"id\":\"" + id + "\",\"kind\":\"image-processor\",\"backend\":\"native\","
      "\"identifier\":\"rawnode.native.exposure\",\"enabled\":true,\"params\":{\"exposure\":" + exposure +
      "},\"inputs\":{\"image\":{\"node\":\"" + upstream + "\",\"port\":\"image\"}}}";
}
PersistGrade grade(const std::string &json, bool testOperations = false) {
  PersistFile parsed; std::string error;
  if (!(testOperations ? GraphPersistenceTestAccess::decode(json, parsed, error) : decodeGraphFile(json, parsed, error)) || !parsed.grade)
    throw std::runtime_error(error);
  return *parsed.grade;
}
const PersistNode &record(const PersistGrade &grade, const std::string &id) {
  for (const auto &node : grade.graph.nodes) if (node.id == id) return node;
  throw std::runtime_error("Missing expected record");
}
std::string payload(App &app) {
  PersistGrade current; std::string json, error;
  if (!captureGrade(app, current) || !encodeTransferPayload(current, json, error)) throw std::runtime_error(error);
  return json;
}
bool hasPosition(const App &app, const std::string &id, double x, double y) {
  const auto found = app.nodePositions.find(id);
  return found != app.nodePositions.end() && found->second.x == x && found->second.y == y;
}

bool completePresetsAndOverwrite() {
  Files files; Clipboard clipboard;
  App app;
  app.nodePositions[app.primarySource.value] = {-10.5, 8};
  const auto path = files.path("preset.rawnode.json");
  CHECK(savePreset(app, path) && copyGrade(app)); // Editable Source-only grade.
  CHECK(savePresetDialog(app, files.path("extension")) && fs::exists(files.path("extension.rawnode.json")));
  PersistFile saved; std::string error;
  CHECK(readGraphSidecar(path, saved, error) == SidecarReadStatus::Loaded && saved.grade && !saved.metadataJson);
  CHECK(saved.grade->graph.nodes.size() == 1 && saved.grade->positions.at(app.primarySource.value).x == -10.5);
  PersistTransfer transfer;
  CHECK(decodeTransferPayload(clipboard.text, transfer, error) && std::get<PersistGrade>(transfer).graph.nodes.size() == 1);
  const auto original = bytes(path);
  int confirmations = 0;
  CHECK(!savePreset(app, path, [&](const auto &destination) { ++confirmations; return destination != path; }));
  CHECK(confirmations == 1 && bytes(path) == original);
  const std::string missing = R"({"id":"opaque","kind":"image-processor","backend":"future","identifier":"future.node","label":"Opaque","enabled":false,"params":{"opaque":{"n":9007199254740993,"curve":[0,0.12345678901234567890123456789],"text":"\u03bb"}},"inputs":{"image":{"node":"a","port":"image"}}})";
  const auto incoming = grade(file(missing + ',' + processor("branch", "a", "-2") + ',' + source + ',' + processor("a", "camera"),
                                  "opaque", R"(,"positions":{"camera":{"x":1,"y":2},"opaque":{"x":3.25,"y":-4}})"));
  CHECK(applyGrade(app, incoming));
  auto destinationFile = file(source);
  destinationFile.replace(destinationFile.find("pick"), 4, "reject");
  write(path, destinationFile);
  const auto metadata = std::string("{ \"mark\" : \"reject\" }");
  CHECK(savePreset(app, path, [&](const auto &destination) { ++confirmations; return destination == path; }));
  CHECK(confirmations == 2 && readGraphSidecar(path, saved, error) == SidecarReadStatus::Loaded);
  CHECK(saved.metadataJson == metadata && saved.grade->graph.nodes.size() == 4);
  CHECK(record(*saved.grade, "opaque").paramsJson == record(incoming, "opaque").paramsJson);
  CHECK(!record(*saved.grade, "opaque").enabled && record(*saved.grade, "branch").inputs.at(ports::image) == image("a"));
  CHECK(saved.grade->positions.at("opaque").x == 3.25 && record(*saved.grade, "a").inputs.at(ports::image) == image("camera"));
  CHECK(copySelectedNode(app) == false); // Replace clears selection.
  CHECK(selectNode(app, NodeId{"branch"}) && copySelectedNode(app)); // Branch capture has no paste restriction.
  CHECK(copyGrade(app));
  CHECK(decodeTransferPayload(clipboard.text, transfer, error) && std::get<PersistGrade>(transfer).graph.nodes.size() == 4);

  const auto before = bytes(path);
  for (auto fault : {persist_detail::FileFault::TempCreate, persist_detail::FileFault::Write,
                    persist_detail::FileFault::Flush, persist_detail::FileFault::Close, persist_detail::FileFault::Replace}) {
    document_detail::ActionTestOptions options; options.beforeSave = [=] { return fault; };
    CHECK(!document_detail::savePreset(app, path, [](const auto &) { return true; }, options));
    CHECK(bytes(path) == before);
  }
  for (const auto &protectedBytes : {std::string("invalid"), std::string(R"({"format":"rawnode","version":999})")}) {
    write(path, protectedBytes);
    CHECK(!savePreset(app, path, [](const auto &) { return true; }) && bytes(path) == protectedBytes);
  }
  const auto legacyPhoto = files.path("legacy.nef");
  write(legacyInputSidecarPath(legacyPhoto), "legacy bytes");
  CHECK(!savePreset(app, inputSidecarPath(legacyPhoto), [](const auto &) { return true; }));
  CHECK(!fs::exists(inputSidecarPath(legacyPhoto)) && bytes(legacyInputSidecarPath(legacyPhoto)) == "legacy bytes");
  write(path, R"({"format":"rawnode","version":1,"metadata":{"mark":"reject"}})");
  PersistGrade untouched = incoming;
  CHECK(!loadPresetFile(path, untouched, error) && untouched.graph.nodes.size() == 4);
  return true;
}

bool replaceAndProtection() {
  Files files; Clipboard clipboard; App app;
  app.path = files.path("destination.tif");
  app.full = {{0.25f, -0.5f, 2, 0.75f}, 1, 1};
  app.preview = std::make_shared<Image>(app.full);
  // Raster pixels have an actual encoding independent of the stored RAW pair.
  const ColorEncoding rasterEncoding{RgbGamut::ACES_AP1, TransferFunction::Linear};
  app.inputIsRaw = false;
  app.inputEncoding = rasterEncoding;
  CHECK(addNativeExposureNode(app));
  app.nodePositions[app.nodes[1].id.value] = {100,200};
  app.rawWorkingEncoding = {RgbGamut::ACES_AP0, TransferFunction::Linear};
  app.defaultOutputEncoding = {RgbGamut::ACES_AP1, TransferFunction::Linear};
  const auto defaults = app.rawWorkingEncoding, outputDefault = app.defaultOutputEncoding;
  write(inputSidecarPath(app.path), file(source));
  const auto metadataBytes = bytes(inputSidecarPath(app.path));
  auto incoming = grade(file(processor("b", "a", "2") + ',' + source + ',' + processor("a", "camera"), "b",
                             R"(,"positions":{"a":{"x":4,"y":8}})"));
  const auto preview = app.preview; const auto pixels = app.full.px.data();
  static int rasterDecodeCalls;
  rasterDecodeCalls = 0;
  document_detail::ActionTestOptions options;
  options.decode = [](const std::string &, Image &, ColorEncoding &, bool &, ColorEncoding) {
    ++rasterDecodeCalls;
    return false;
  };
  CHECK(document_detail::applyGrade(app, incoming, options) && app.primarySource == NodeId{"camera"} && app.documentOutput == image("b"));
  CHECK(app.preview == preview && app.full.px.data() == pixels && app.path == files.path("destination.tif"));
  CHECK(rasterDecodeCalls == 0 && !app.inputIsRaw && app.inputEncoding == rasterEncoding);
  CHECK(app.nodePositions.size() == 1 && hasPosition(app, "a", 4, 8));
  CHECK(app.photoRawWorkingEncoding == (ColorEncoding{RgbGamut::Rec2020, TransferFunction::Linear}));
  CHECK(app.outputEncoding == (ColorEncoding{RgbGamut::DisplayP3, TransferFunction::SRGB}));
  CHECK(app.rawWorkingEncoding == defaults && app.defaultOutputEncoding == outputDefault && bytes(inputSidecarPath(app.path)) == metadataBytes);
  const auto before = payload(app); const auto processor = findNode(app, NodeId{"a"})->processor.get();
  incoming.graph.nodes[0].inputs.at(ports::image) = image("missing");
  CHECK(!applyGrade(app, incoming) && payload(app) == before && findNode(app, NodeId{"a"})->processor.get() == processor);
  CHECK(applyGrade(app, grade(file(source))) && app.nodePositions.empty() && app.nodes.size() == 1);
  CHECK(copyGrade(app));
  app.sidecarWriteBlockedPath = app.path;
  const auto clipboardBefore = clipboard.text;
  PersistGrade capture;
  CHECK(!captureGrade(app, capture) && !copyGrade(app) && !pasteFromClipboard(app));
  CHECK(!savePreset(app, files.path("forbidden.rawnode.json")) && !applyGrade(app, grade(file(source))));
  CHECK(!appendGrade(app, grade(file(source))) && clipboard.text == clipboardBefore && app.nodes.size() == 1);
  return true;
}

bool linearAppendAndRejection() {
  Files files; App app;
  const auto destination = grade(file(processor("end", "start", "-1") + ',' + processor("branch", "start", "3") + ',' +
                                    source + ',' + processor("start", "camera", "1"), "end",
                                    R"(,"positions":{"start":{"x":10,"y":20},"branch":{"x":30,"y":40}})"));
  CHECK(applyGrade(app, destination));
  app.path = files.path("destination.nef");
  write(inputSidecarPath(app.path), file(source));
  const auto sidecar = bytes(inputSidecarPath(app.path));
  const auto raw = app.photoRawWorkingEncoding, output = app.outputEncoding;
  auto incoming = grade(file(processor("tail", "entry", "2") + ',' + source + ',' + processor("entry", "camera", "-2"), "tail",
                             R"(,"positions":{"camera":{"x":999,"y":999},"entry":{"x":-2,"y":-3},"tail":{"x":7,"y":9}})"));
  // Incoming colours are intentionally different and must not change destination.
  incoming.color = {"aces-ap1", "srgb", "rec709", "linear"};
  CHECK(appendGrade(app, incoming, NodeId{"start"}) && app.nodes.size() == 6 && app.documentOutput == image("end"));
  const auto tail = findNode(app, NodeId{"end"})->fixedInputs.at(ports::image)->nodeId;
  const auto entry = findNode(app, tail)->fixedInputs.at(ports::image)->nodeId;
  CHECK(tail != NodeId{"tail"} && entry != NodeId{"entry"} && tail != entry);
  CHECK(findNode(app, entry)->fixedInputs.at(ports::image) == image("start"));
  CHECK(findNode(app, NodeId{"branch"})->fixedInputs.at(ports::image) == image("start"));
  CHECK(findNode(app, NodeId{"start"})->fixedInputs.at(ports::image) == image("camera"));
  CHECK(app.nodePositions.size() == 4 && hasPosition(app, entry.value, -2,-3) && hasPosition(app, tail.value, 7,9));
  CHECK(hasPosition(app, "start", 10,20) && hasPosition(app, "branch", 30,40));
  CHECK(app.photoRawWorkingEncoding == raw && app.outputEncoding == output && bytes(inputSidecarPath(app.path)) == sidecar);
  const auto before = payload(app); const auto epoch = RenderRuntimeTestAccess::snapshot(app.renderer).epoch;
  CHECK(appendGrade(app, grade(file(source))));
  CHECK(RenderRuntimeTestAccess::snapshot(app.renderer).epoch == epoch && payload(app) == before);
  CHECK(!appendGrade(app, incoming, NodeId{"stale"}) && payload(app) == before);
  CHECK(!appendGrade(app, incoming, NodeId{"branch"}) && payload(app) == before);
  CHECK(!appendGrade(app, destination) && payload(app) == before); // Incoming branch.
  auto invalid = incoming;
  invalid.positions["entry"].x = std::numeric_limits<double>::infinity();
  CHECK(!appendGrade(app, invalid) && payload(app) == before);
  invalid = incoming; invalid.graph.nodes[0].inputs.at(ports::image) = image("stale");
  CHECK(!appendGrade(app, invalid) && payload(app) == before);
  invalid = incoming;
  invalid.graph.nodes[0].paramsJson["exposure"] = "\"wrong parameter type\"";
  // Storage is shuffled: entry is constructed successfully, then tail fails.
  CHECK(!appendGrade(app, invalid) && payload(app) == before);
  PersistFragment failedBatch{{invalid.graph.nodes[2], invalid.graph.nodes[0]}, {}};
  failedBatch.nodes[0].inputs.at(ports::image).reset();
  CHECK(!pasteFragment(app, failedBatch, NodeId{"start"}) && payload(app) == before);
  CHECK(appendGrade(app, incoming) && app.nodes.size() == 8);
  const auto appendedTail = app.documentOutput.nodeId;
  const auto appendedEntry = findNode(app, appendedTail)->fixedInputs.at(ports::image)->nodeId;
  CHECK(findNode(app, appendedEntry)->fixedInputs.at(ports::image) == image("end"));
  CHECK(appendedTail != tail && appendedEntry != entry && app.nodePositions.size() == 6);
  CHECK(destroyNode(app, appendedTail) && !app.nodePositions.count(appendedTail.value));
  // A disconnected destination spine has no insertion point.
  CHECK(graph::disconnect(app, {NodeId{"end"}, std::nullopt, ports::image}).ok());
  const auto disconnected = payload(app);
  CHECK(!appendGrade(app, incoming) && payload(app) == disconnected);
  return true;
}

bool insertAfterSelectedFinalNode() {
  Clipboard clipboard;
  const auto destination = grade(file(processor("end", "start", "-1") + ',' + processor("branch", "start", "3") + ',' +
                                     source + ',' + processor("start", "camera", "1"), "end"));
  const auto incoming = grade(file(processor("tail", "entry", "-1") + ',' + source + ',' + processor("entry", "camera", "2"), "tail"));
  for (bool paste : {false, true}) {
    App app;
    CHECK(applyGrade(app, destination) && selectNode(app, NodeId{"end"}));
    auto pixels = std::make_shared<Image>();
    pixels->w = pixels->h = 1;
    pixels->px = {0.25f, -0.5f, 2.0f, 0.75f};
    std::map<std::string, const Processor *> processors;
    for (const auto &node : app.nodes) processors.emplace(node.id.value, node.processor.get());
    if (paste) {
      PersistFragment fragment{{record(incoming, "entry"), record(incoming, "tail")}, {}};
      fragment.nodes.front().inputs.at(ports::image).reset();
      std::string error;
      CHECK(encodeTransferPayload(fragment, clipboard.text, error) && pasteFromClipboard(app));
    } else CHECK(appendGrade(app, incoming, app.selectedNodeId));
    CHECK(app.nodes.size() == 6 && app.selectedNodeId && app.documentOutput == (OutputRef{*app.selectedNodeId, ports::image}));
    const auto tail = app.documentOutput.nodeId;
    CHECK(tail != NodeId{"end"});
    const auto *tailNode = findNode(app, tail);
    CHECK(tailNode && tailNode->fixedInputs.at(ports::image));
    const auto entry = tailNode->fixedInputs.at(ports::image)->nodeId;
    CHECK(entry != NodeId{"end"} && findNode(app, entry)->fixedInputs.at(ports::image) == image("end"));
    PersistGrade after;
    CHECK(captureGrade(app, after));
    for (const auto &saved : destination.graph.nodes) {
      const auto &retained = record(after, saved.id);
      CHECK(retained.operation == saved.operation && retained.inputs == saved.inputs && retained.enabled == saved.enabled);
      CHECK(retained.paramsJson == saved.paramsJson && retained.identifier == saved.identifier);
      CHECK(findNode(app, NodeId{saved.id})->processor.get() == processors.at(saved.id));
    }
    const auto result = evaluateGraph({app.nodes, app.primarySource, app.documentOutput}, app.documentOutput,
                                      {EvaluationPurpose::Preview, 1, 1, 1, 1}, pixels);
    CHECK(result.state == EvaluationState::Available);
    const auto rendered = std::get<ImageValue>(result.value);
    // Destination +1/-1 EV cancels. Both incoming +2/-1 EV processors
    // must contribute: skipping either yields a different power-of-two result.
    CHECK(rendered && rendered->px == std::vector<float>({0.5f, -1.0f, 4.0f, 0.75f}));
  }
  return true;
}

bool rejectIncomingOffSpineConsumer() {
  App app;
  const auto destination = grade(file(processor("end", "start", "-1") + ',' + processor("branch", "start", "3") + ',' +
                                     source + ',' + processor("start", "camera", "1"), "end",
                                     R"(,"positions":{"start":{"x":10,"y":20},"branch":{"x":30,"y":40}})"));
  CHECK(applyGrade(app, destination) && selectNode(app, NodeId{"end"}));
  app.paramFilter[0] = 'e';
  const auto before = payload(app);
  const auto selection = app.selectedNodeId;
  const auto inputEncoding = app.inputEncoding;
  const auto rawDefault = app.rawWorkingEncoding, outputDefault = app.defaultOutputEncoding;
  const auto epoch = RenderRuntimeTestAccess::snapshot(app.renderer).epoch;
  std::map<std::string, const Processor *> processors;
  for (const auto &node : app.nodes) processors.emplace(node.id.value, node.processor.get());
  // b consumes a, but Output is a: dropping the full-node-coverage check
  // would incorrectly append both nodes as if b were part of the saved spine.
  const auto incoming = grade(R"({"format":"rawnode","version":1,"grade":{
    "color":{"rawWorking":{"colorSpace":"aces-ap1","gamma":"srgb"},"output":{"colorSpace":"rec709","gamma":"linear"}},
    "graph":{"primarySource":"source","documentOutput":{"node":"a","port":"image"},"nodes":[
      {"id":"source","kind":"source"},
      {"id":"a","kind":"image-processor","backend":"native","identifier":"rawnode.native.exposure","enabled":true,"params":{"exposure":2},"inputs":{"image":{"node":"source","port":"image"}}},
      {"id":"b","kind":"image-processor","backend":"native","identifier":"rawnode.native.exposure","enabled":true,"params":{"exposure":-1},"inputs":{"image":{"node":"a","port":"image"}}}
    ]},"positions":{"a":{"x":-5,"y":-6},"b":{"x":7,"y":8}}}})");
  CHECK(!appendGrade(app, incoming, app.selectedNodeId));
  CHECK(RenderRuntimeTestAccess::snapshot(app.renderer).epoch == epoch);
  CHECK(app.nodes.size() == 4 && app.documentOutput == image("end") && payload(app) == before);
  CHECK(app.selectedNodeId == selection && app.paramFilter[0] == 'e' && app.inputEncoding == inputEncoding);
  CHECK(app.rawWorkingEncoding == rawDefault && app.defaultOutputEncoding == outputDefault);
  CHECK(!findNode(app, NodeId{"a"}) && !findNode(app, NodeId{"b"}));
  for (const auto &node : app.nodes) CHECK(node.processor.get() == processors.at(node.id.value));
  return true;
}

bool fragmentsAndClipboard() {
  Clipboard clipboard; App app;
  CHECK(applyGrade(app, grade(file(processor("b", "a") + ',' + processor("branch", "a") + ',' + source + ',' + processor("a", "camera"), "b"))));
  app.nodePositions["a"] = {1,2}; app.nodePositions["b"] = {3,4};
  std::vector<std::string> warnings;
  auto fragment = captureFragment(app, {NodeId{"b"}, NodeId{"a"}}, warnings);
  CHECK(warnings.empty() && fragment.nodes.size() == 2 && fragment.nodes[1].inputs.at(ports::image) == std::nullopt);
  CHECK(fragment.nodes[0].inputs.at(ports::image) == image("a") && fragment.positions.size() == 2);
  std::string error;
  CHECK(encodeTransferPayload(fragment, clipboard.text, error));
  CHECK(selectNode(app, NodeId{"a"}) && pasteFromClipboard(app));
  const auto tail = *app.selectedNodeId;
  const auto entry = findNode(app, tail)->fixedInputs.at(ports::image)->nodeId;
  CHECK(tail != NodeId{"b"} && entry != NodeId{"a"} && findNode(app, entry)->fixedInputs.at(ports::image) == image("a"));
  CHECK(findNode(app, NodeId{"b"})->fixedInputs.at(ports::image) == (OutputRef{tail, ports::image}));
  CHECK(findNode(app, NodeId{"branch"})->fixedInputs.at(ports::image) == image("a"));
  CHECK(hasPosition(app, entry.value, 1,2) && hasPosition(app, tail.value, 3,4));
  // Capture may represent a branch, even though attachment deliberately cannot.
  fragment = captureFragment(app, {NodeId{"a"}, NodeId{"b"}, NodeId{"branch"}, entry, tail}, warnings);
  CHECK(encodeTransferPayload(fragment, clipboard.text, error));
  const auto before = payload(app), clipboardBefore = clipboard.text;
  CHECK(!pasteFromClipboard(app) && payload(app) == before && clipboard.text == clipboardBefore);
  for (const auto &invalid : {std::string(R"({"format":"rawnode-transfer","version":1,"kind":"node","graph":{"nodes":[]}})"),
                            std::string(R"({"format":"rawnode-transfer","version":2,"kind":"fragment","nodes":[{"id":"camera","kind":"source"}]})"),
                            std::string(R"({"format":"rawnode-transfer","version":2,"kind":"fragment","nodes":[],"nodes":[]})")}) {
    clipboard.text = invalid;
    CHECK(!pasteFromClipboard(app) && clipboard.text == invalid && payload(app) == before);
  }
  // Foreign IDs matching destination IDs cannot reconnect an omitted Image input.
  const std::string handFragment = R"({"format":"rawnode-transfer","version":2,"kind":"fragment","nodes":[{"id":"a","kind":"image-processor","backend":"future","identifier":"opaque","enabled":false,"params":{"big":9007199254740993},"inputs":{"image":null}}]})";
  clipboard.text = handFragment; app.selectedNodeId.reset();
  const auto predecessor = app.documentOutput;
  CHECK(pasteFromClipboard(app));
  const auto *pasted = selectedNode(app);
  CHECK(pasted && pasted->id != NodeId{"a"} && pasted->fixedInputs.at(ports::image) == predecessor && !pasted->enabled);
  CHECK(!pasted->processor && pasted->preservedParamsJson.at("big") == "9007199254740993");
  return true;
}

bool fragmentSlotsAndExternalLinks() {
  const std::string mask = R"({"id":"mask","kind":"test-reference-mask","enabled":true,"inputs":{"reference":{"node":"camera","port":"image"}}})";
  const std::string effect = R"({"id":"effect","kind":"test-masked-image","enabled":false,"inputs":{"image":{"node":"camera","port":"image"},"mask":{"node":"mask","port":"mask"}}})";
  const std::string slots = R"({"id":"slots","kind":"test-slotted","enabled":true,"inputs":{"reference":{"node":"camera","port":"image"}},"slots":[{"id":"3","settings":{"enabled":false},"inputs":{"image":{"node":"effect","port":"image"},"mask":{"node":"mask","port":"mask"}}},{"id":"1","settings":{},"inputs":{"image":{"node":"camera","port":"image"}}}],"nextSlotId":"5"})";
  auto parsed = grade(file(source + ',' + mask + ',' + effect + ',' + slots, "slots",
                          R"(,"positions":{"slots":{"x":4,"y":-8}})"), true);
  auto restored = document_detail::restoreGraph(parsed.graph);
  App app; app.nodes.swap(restored.nodes); app.primarySource = restored.primarySource;
  app.documentOutput = restored.output; app.nodePositions = parsed.positions;
  std::vector<std::string> warnings;
  auto fragment = captureFragment(app, {NodeId{"effect"}, NodeId{"slots"}, NodeId{"mask"}}, warnings);
  CHECK(warnings.size() == 2); // Source Reference on mask and on slots, both reported.
  CHECK(!fragment.nodes[0].enabled && !fragment.nodes[0].inputs.at(ports::image));
  CHECK(fragment.nodes[0].inputs.at(ports::mask) == (OutputRef{NodeId{"mask"}, ports::mask}));
  CHECK(fragment.nodes[1].slots[0].id == SlotId{3} && !fragment.nodes[1].slots[0].settings.enabled);
  CHECK(fragment.nodes[1].slots[1].id == SlotId{1} && fragment.nodes[1].slots[1].settings.enabled);
  CHECK(fragment.nodes[1].nextSlotId == 5 && fragment.nodes[1].slots[0].inputs.at(ports::image) == image("effect"));
  CHECK(fragment.nodes[1].slots[0].inputs.at(ports::mask) == (OutputRef{NodeId{"mask"}, ports::mask}));
  CHECK(!fragment.nodes[1].slots[1].inputs.at(ports::image) && !fragment.nodes[2].inputs.at(ports::reference));
  std::string json, error; PersistTransfer decoded;
  CHECK(persist_detail::encodeTransferPayload(fragment, json, error, true));
  CHECK(persist_detail::decodeTransferPayload(json, decoded, error, true));
  CHECK(std::get<PersistFragment>(decoded).nodes[1].nextSlotId == 5 && json.find("groupOpen") == std::string::npos);
  CHECK(persist_detail::encodeTransferPayload(parsed, json, error, true));
  CHECK(persist_detail::decodeTransferPayload(json, decoded, error, true));
  const auto &gradeTransfer = std::get<PersistGrade>(decoded);
  CHECK(record(gradeTransfer, "slots").slots[0].id == SlotId{3});
  CHECK(record(gradeTransfer, "slots").nextSlotId == 5 && gradeTransfer.positions.at("slots").y == -8);
  App destination; document_detail::ActionTestOptions options; options.testOperations = true;
  CHECK(document_detail::applyGrade(destination, gradeTransfer, options));
  CHECK(findNode(destination, NodeId{"slots"})->nextSlotIdForPersistence() == 5);
  CHECK(findNode(destination, NodeId{"slots"})->slots[0].inputs.at(ports::mask) == (OutputRef{NodeId{"mask"}, ports::mask}));
  CHECK(!encodeTransferPayload(fragment, json, error)); // Test kinds never escape into production formats.
  fragment = captureFragment(app, {NodeId{"effect"}}, warnings);
  CHECK(warnings.size() == 1 && warnings[0].find("mask") != std::string::npos && !fragment.nodes[0].inputs.at(ports::mask));
  const auto sizes = app.nodes.size();
  CHECK(!pasteFragment(app, fragment) && app.nodes.size() == sizes);
  return true;
}
} // namespace

bool testPresetsAndClipboard() {
  for (const auto &test : {
      std::make_pair("complete Source-only/branched presets, metadata, overwrite and atomic failures", completePresetsAndOverwrite),
      std::make_pair("Replace preserves pixels, metadata/defaults, replaces positions and enforces protection", replaceAndProtection),
      std::make_pair("linear middle/end Append preserves branches, bindings/positions and rejects atomically", linearAppendAndRejection),
      std::make_pair("selected final-node Append/Paste updates Output and evaluates the complete chain", insertAfterSelectedFinalNode),
      std::make_pair("incoming off-spine consumer rejects without changing document state", rejectIncomingOffSpineConsumer),
      std::make_pair("independent fragment capture, fresh IDs, detached links and real clipboard refusal", fragmentsAndClipboard),
      std::make_pair("internal Mask/Reference/Slot state and external-link diagnostics", fragmentSlotsAndExternalLinks)}) {
    if (!test.second()) { std::fprintf(stderr, "preset/clipboard group failed: %s\n", test.first); return false; }
    std::printf("ok  Step 7 presets/clipboard: %s\n", test.first);
  }
  return true;
}
