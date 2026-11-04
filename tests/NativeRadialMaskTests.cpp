#include "NativeRadialMaskTests.h"

#include "Export.h"
#include "GraphCommands.h"
#include "GraphEvaluator.h"
#include "NodeGraph.h"
#include "RenderRuntimeTestAccess.h"
#include "RenderPipeline.h"
#include "persist/DocumentActions.h"
#include "processors/NativeRadialMaskProcessor.h"
#include "ui/UiContext.h"
#include "ui/ParamWidgets.h"
#include "imgui_internal.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <stdexcept>

#define CHECK(expression) do { if (!(expression)) { \
  std::fprintf(stderr, "native Radial Mask check failed at line %d: %s\n", __LINE__, #expression); return false; \
} } while (false)

namespace {
OutputRef image(const NodeId &id) { return {id, ports::image}; }
OutputRef mask(const NodeId &id) { return {id, ports::mask}; }
InputAddress input(const NodeId &id, const PortId &port) { return {id, std::nullopt, port}; }
Image raster(int w, int h) {
  Image result; result.w = w; result.h = h;
  for (int i = 0; i < w * h; ++i) result.px.insert(result.px.end(), {.25f, -.5f, 2.0f, .375f});
  return result;
}
bool idle(App &app) {
  return RenderRuntimeTestAccess::wait(app.renderer, [](auto s) {
    return !s.previewPending && !s.previewBusy && !s.displayPending;
  });
}
EvaluationResult evaluate(const App &app, const OutputRef &endpoint, int w, int h,
                          EvaluationPurpose purpose = EvaluationPurpose::Preview,
                          graph_test::Executor *executor = nullptr) {
  return evaluateGraph({app.nodes, app.primarySource, app.documentOutput}, endpoint,
      {purpose, purpose == EvaluationPurpose::Export ? w : w * 3,
                purpose == EvaluationPurpose::Export ? h : h * 3, w, h}, std::make_shared<Image>(raster(w, h)), {}, executor);
}
MaskValue coverage(const EvaluationResult &result) {
  const auto *value = std::get_if<MaskValue>(&result.value);
  return value ? *value : nullptr;
}
ImageValue pixels(const EvaluationResult &result) {
  const auto *value = std::get_if<ImageValue>(&result.value);
  return value ? *value : nullptr;
}
float at(const Mask &value, int x, int y) { return value.px[size_t(y) * value.w + x]; }
bool uniform(const MaskValue &value, float sample) {
  return value && std::all_of(value->px.begin(), value->px.end(), [&](float v) { return v == sample; });
}
bool warning(const EvaluationResult &value, const std::string &text) {
  return std::any_of(value.diagnostics.begin(), value.diagnostics.end(), [&](const auto &d) {
    return d.level == DiagnosticLevel::Warning && d.message.find(text) != std::string::npos;
  });
}
double parameter(const Node &node, const std::string &id) {
  for (const auto &p : node.processor->parameters()) if (p.id == id) return std::get<double>(p.value);
  throw std::runtime_error("Missing test parameter");
}
bool inverted(const Node &node) {
  for (const auto &p : node.processor->parameters()) if (p.id == "invert") return std::get<bool>(p.value);
  throw std::runtime_error("Missing Invert parameter");
}

bool numerical() {
  NativeRadialMaskProcessor radial;
  const auto defaults = radial.parameters();
  CHECK(defaults.size() == 6);
  CHECK(std::get<double>(defaults[0].value) == .5 && std::get<double>(defaults[1].value) == .5);
  CHECK(std::get<double>(defaults[2].value) == .25 && std::get<double>(defaults[3].value) == .25);
  CHECK(std::get<double>(defaults[4].value) == .5);
  CHECK(defaults[5].id == "invert" && defaults[5].label == "Invert" && defaults[5].type == ParameterType::Boolean);
  CHECK(!std::get<bool>(defaults[5].value) && !std::get<bool>(defaults[5].defaultValue));
  Mask value;
  CHECK(radial.setParameterValue("centreX", .5625) && radial.setParameterValue("centreY", .5625));
  CHECK(radial.setParameterValue("feather", 0.0) && radial.generateMask(8, 8, value, {}).ok);
  // Centre at pixel (4,4); each radius is exactly two pixels. The boundary is included.
  CHECK(at(value, 4, 4) == 1 && at(value, 6, 4) == 1 && at(value, 4, 2) == 1);
  CHECK(at(value, 7, 4) == 0 && at(value, 6, 6) == 0);
  CHECK(radial.setParameterValue("feather", 1.0) && radial.generateMask(8, 8, value, {}).ok);
  CHECK(at(value, 4, 4) == 1 && at(value, 5, 4) == .5f && at(value, 6, 4) == 0);

  CHECK(radial.setParameterValue("centreX", .515625) && radial.setParameterValue("centreY", .515625));
  CHECK(radial.setParameterValue("feather", .5) && radial.generateMask(32, 32, value, {}).ok);
  // Radius = 8 px. d=5/8 and 7/8 give t=3/4 and 1/4: exact cubic reference values.
  CHECK(at(value, 21, 16) == .84375f && at(value, 23, 16) == .15625f);
  // Off-axis (3,4) px gives d=sqrt(3*3+4*4)/8=5/8, not Manhattan distance 7/8.
  CHECK(at(value, 19, 20) == .84375f);
  CHECK(at(value, 22, 16) == .5f && at(value, 20, 16) == 1 && at(value, 24, 16) == 0);
  for (float v : value.px) CHECK(std::isfinite(v) && v >= 0 && v <= 1);

  // Rectangular Reference: radii are fractions of their own axes, not the short side.
  CHECK(radial.setParameterValue("centreX", .53125) && radial.setParameterValue("centreY", .5625));
  CHECK(radial.setParameterValue("feather", 1.0) && radial.generateMask(16, 8, value, {}).ok);
  CHECK(value.w == 16 && value.h == 8 && value.px.size() == 128);
  CHECK(at(value, 10, 4) == .5f && at(value, 8, 5) == .5f);
  CHECK(at(value, 12, 4) == 0 && at(value, 8, 6) == 0);
  CHECK(radial.setParameterValue("radiusY", .5) && radial.generateMask(16, 8, value, {}).ok);
  CHECK(at(value, 10, 4) == at(value, 8, 6)); // Equal pixel-space radii form a circle.

  CHECK(radial.setParameterValue("centreX", .2) && radial.setParameterValue("centreY", .8));
  CHECK(radial.setParameterValue("radiusX", .6) && radial.setParameterValue("radiusY", .7));
  CHECK(radial.setParameterValue("feather", .75));
  Mask preview, full;
  CHECK(radial.generateMask(7, 5, preview, {}).ok && radial.generateMask(21, 15, full, {}).ok);
  // With a 3x raster, (3*i+1) is the same normalised pixel centre as preview i.
  for (int y = 0; y < 5; ++y) for (int x = 0; x < 7; ++x)
    CHECK(at(preview, x, y) == at(full, 3 * x + 1, 3 * y + 1));
  CHECK(at(preview, 1, 4) > at(preview, 1, 0)); // Y increases from the bottom.

  CHECK(radial.setParameterValue("radiusX", 0.0) && radial.generateMask(7, 5, value, {}).ok);
  CHECK(std::all_of(value.px.begin(), value.px.end(), [](float v) { return v == 0; }));
  CHECK(radial.resetParameter("radiusX") && radial.setParameterValue("radiusY", 0.0));
  CHECK(radial.generateMask(1, 1, value, {}).ok && value.px == std::vector<float>{0});
  CHECK(radial.resetParameter("radiusY") && radial.resetParameter("centreY"));
  CHECK(radial.setParameterValue("centreX", -.125) && radial.setParameterValue("feather", 0.0));
  CHECK(radial.generateMask(4, 1, value, {}).ok && at(value, 0, 0) == 1 && at(value, 1, 0) == 0);
  CHECK(radial.setParameterValue("centreX", -10.0) && std::get<double>(radial.parameters()[0].value) == -1);
  CHECK(radial.setParameterValue("centreY", 10.0) && std::get<double>(radial.parameters()[1].value) == 2);
  CHECK(radial.setParameterValue("radiusX", -1.0) && std::get<double>(radial.parameters()[2].value) == 0);
  CHECK(radial.setParameterValue("radiusY", 5.0) && std::get<double>(radial.parameters()[3].value) == 2);
  CHECK(radial.setParameterValue("feather", 5.0) && std::get<double>(radial.parameters()[4].value) == 1);
  const auto before = radial.parameters();
  for (const auto &p : before) {
    CHECK(!radial.setParameterValue(p.id, std::numeric_limits<double>::quiet_NaN()));
    CHECK(!radial.setParameterValue(p.id, std::numeric_limits<double>::infinity()));
    CHECK(!radial.setParameterValue(p.id, 1));
  }
  for (size_t i = 0; i < before.size(); ++i) CHECK(radial.parameters()[i].value == before[i].value);
  CHECK(!radial.setParameterValue("unknown", .5) && !radial.resetParameter("unknown"));
  CHECK(!radial.generateMask(0, 5, value, {}).ok && !radial.generateMask(5, -1, value, {}).ok);
  for (const auto &p : before) CHECK(radial.resetParameter(p.id));
  CHECK(radial.setParameterValue("radiusX", std::numeric_limits<double>::denorm_min()));
  CHECK(radial.setParameterValue("feather", std::numeric_limits<double>::denorm_min()));
  CHECK(radial.generateMask(3, 3, value, {}).ok);
  CHECK(at(value, 1, 1) == 1 && at(value, 0, 1) == 0);
  return true;
}

bool inversion() {
  NativeRadialMaskProcessor radial;
  CHECK(radial.setParameterValue("centreX", .515625) && radial.setParameterValue("centreY", .515625));
  for (double feather : {0.0, .5, 1.0}) {
    Mask ordinary, inverse, restored;
    CHECK(radial.setParameterValue("feather", feather) && radial.generateMask(32, 32, ordinary, {}).ok);
    CHECK(radial.setParameterValue("invert", true) && radial.generateMask(32, 32, inverse, {}).ok);
    CHECK(inverse.w == ordinary.w && inverse.h == ordinary.h && inverse.px.size() == ordinary.px.size());
    CHECK(at(inverse, 16, 16) == 0 && at(inverse, 31, 31) == 1);
    // Literal cubic values: d=5/8,7/8 at feather 1/2 yield 1-27/32,1-5/32.
    if (feather == .5) CHECK(at(inverse, 21, 16) == .15625f && at(inverse, 23, 16) == .84375f);
    if (feather == 0) CHECK(at(inverse, 24, 16) == 0 && at(inverse, 25, 16) == 1);
    for (size_t i = 0; i < inverse.px.size(); ++i) {
      CHECK(std::isfinite(inverse.px[i]) && inverse.px[i] >= 0 && inverse.px[i] <= 1);
      // Each independent double result is rounded to float once (at most one float epsilon in the sum).
      CHECK(std::abs(double(ordinary.px[i]) + inverse.px[i] - 1) <= std::numeric_limits<float>::epsilon());
    }
    CHECK(!radial.setParameterValue("invert", 1.0) && !radial.setParameterValue("invert", 1));
    CHECK(!radial.setParameterValue("invert", std::string("true")));
    CHECK(radial.resetParameter("invert") && radial.generateMask(32, 32, restored, {}).ok);
    CHECK(restored.px == ordinary.px); // Default-off generation retains the exact original samples.
  }
  // At d=2^-14, ordinary coverage rounds to float 1, but its complement must
  // retain 3*d^2-2*d^3 rather than subtracting an already-rounded float.
  CHECK(radial.setParameterValue("centreX", .5-1.0/16384) && radial.setParameterValue("centreY", .5));
  CHECK(radial.setParameterValue("radiusX", 1.0) && radial.setParameterValue("feather", 1.0));
  Mask tiny;
  CHECK(radial.setParameterValue("invert", true) && radial.generateMask(1, 1, tiny, {}).ok);
  CHECK(tiny.px[0] == float(3.0/268435456 - 2.0/4398046511104));
  return true;
}

bool zeroRadiusAtPixelCentre() {
  App app;
  CHECK(addNativeRadialMaskNode(app)); const auto radial = *app.selectedNodeId();
  // Pixel (4,4) in an 8x8 raster is exactly (9/16,9/16). Each case has
  // one zero radius and one positive radius, including at that sampled centre.
  CHECK(setNodeParameterValue(app, radial, "centreX", .5625));
  CHECK(setNodeParameterValue(app, radial, "centreY", .5625));
  for (bool invert : {false, true}) for (const auto &zero : {"radiusX", "radiusY"}) {
    CHECK(setNodeParameterValue(app, radial, "invert", invert));
    CHECK(resetNodeParameter(app, radial, "radiusX") && resetNodeParameter(app, radial, "radiusY"));
    CHECK(setNodeParameterValue(app, radial, zero, 0.0));
    const auto result = evaluate(app, mask(radial), 8, 8);
    CHECK(result.state == EvaluationState::Available && result.diagnostics.empty());
    const auto value = coverage(result);
    CHECK(value && value->w == 8 && value->h == 8 && value->px.size() == 64);
    CHECK(uniform(value, invert ? 1.0f : 0.0f));
  }
  return true;
}

// Hand-authored production schema, independent of the graph writer. Only the
// processor identity varies for malformed-record cases; ports follow the kind.
std::string radialIdentityFile(const char *backend = "native",
                               const char *identifier = NativeRadialMaskProcessor::kIdentifier) {
  return R"({"format":"rawnode","version":1,"grade":{
    "color":{"rawWorking":{"colorSpace":"rec2020","gamma":"linear"},
             "output":{"colorSpace":"rec709","gamma":"srgb"}},
    "graph":{"primarySource":"camera","documentOutput":{"node":"camera","port":"image"},
      "nodes":[{"id":"camera","kind":"source"},
        {"id":"radial","kind":"native-radial-mask","backend":)" + jsonStringValue(backend) +
      R"(,"identifier":)" + jsonStringValue(identifier) + R"(,"enabled":true,
         "params":{"centreX":0.5,"centreY":0.5,"radiusX":0.25,"radiusY":0.25,"feather":0.5},
         "inputs":{"reference":{"node":"camera","port":"image"}}}]}}})";
}

bool persistedRadialIdentityValidation() {
  App app; PersistFile valid; std::string error;
  CHECK(decodeGraphFile(radialIdentityFile(), valid, error) && valid.grade);
  CHECK(applyGrade(app, *valid.grade));
  auto *original = findNode(app, NodeId{"radial"})->processor.get();
  CHECK(original);
  PersistGrade before; std::string beforeJson;
  CHECK(captureGrade(app, before) && encodeTransferPayload(before, beforeJson, error));
  for (bool wrongBackend : {true, false}) {
    const char *backend = wrongBackend ? "ctl" : "native";
    const char *identifier = wrongBackend ? NativeRadialMaskProcessor::kIdentifier : "rawnode.native.exposure";
    PersistFile decoded; decoded.metadataJson = R"({"mark":"pick"})";
    CHECK(!decodeGraphFile(radialIdentityFile(backend, identifier), decoded, error) && !error.empty());
    CHECK(!decoded.grade && decoded.metadataJson == R"({"mark":"pick"})");
    auto invalid = *valid.grade;
    invalid.graph.nodes[1].backend = backend;
    invalid.graph.nodes[1].identifier = identifier;
    CHECK(!applyGrade(app, invalid));
    PersistGrade after; std::string afterJson;
    CHECK(captureGrade(app, after) && encodeTransferPayload(after, afterJson, error));
    CHECK(afterJson == beforeJson && findNode(app, NodeId{"radial"})->processor.get() == original);
  }
  return true;
}

bool processorOperationIdentityPairing() {
  auto json = radialIdentityFile();
  const std::string kind = "native-radial-mask", reference = "\"reference\"";
  json.replace(json.find(kind), kind.size(), "image-processor");
  json.replace(json.find(reference), reference.size(), "\"image\"");
  PersistFile file; std::string error; App app;
  CHECK(decodeGraphFile(json, file, error) && file.grade && applyGrade(app, *file.grade));
  const NodeId id{"radial"};
  const auto *node = findNode(app, id);
  CHECK(node && node->operation == NodeOperation::ImageProcessor && !node->processor && node->isMissingProcessor());
  CHECK(node->storedBackend == "native" && node->storedIdentifier == NativeRadialMaskProcessor::kIdentifier);
  CHECK(node->preservedParamsJson == file.grade->graph.nodes[1].paramsJson);
  CHECK(findOutput(app, image(id)) && findOutput(app, image(id))->type == ValueType::Image);
  CHECK(!findOutput(app, mask(id)));
  const auto result = evaluate(app, image(id), 8, 8);
  CHECK(result.state == EvaluationState::Available && result.valueType == ValueType::Image);
  CHECK(pixels(result) && pixels(result)->px == raster(8, 8).px && warning(result, "Missing Image processor"));
  CHECK(evaluate(app, mask(id), 8, 8).state == EvaluationState::Error);
  return true;
}

struct ReferenceExecutor final : graph_test::Executor {
  int calls = 0;
  bool unavailable = false;
  graph_test::OperationResult execute(const Node &, const EvaluationContext &, const graph_test::InputValues &,
                                     const RenderCancellation &) override {
    ++calls;
    if (unavailable) return {EvaluationState::Unavailable, {}, "Reference unavailable"};
    return {EvaluationState::Available, {{ports::image, std::make_shared<Image>(raster(3, 7))}}, {}};
  }
};

bool graphSemantics(bool invert) {
  App app;
  CHECK(addNativeExposureNode(app)); const auto exposure = *app.selectedNodeId();
  CHECK(setNodeParameterValue(app, exposure, "exposure", 1.0));
  const auto output = app.documentOutput;
  CHECK(addNativeRadialMaskNode(app)); const auto radial = *app.selectedNodeId();
  CHECK(setNodeParameterValue(app, radial, "invert", invert));
  CHECK(app.documentOutput == output && findNode(app, radial)->operation == NodeOperation::NativeRadialMask);
  CHECK(!*findInput(app, input(radial, ports::reference)));
  CHECK(!setNodeStrength(app, radial, .5));
  CHECK(graph::connect(app, mask(radial), input(exposure, ports::mask)).ok());
  CHECK(!graph::connect(app, image(app.primarySource), input(exposure, ports::mask)).ok());
  CHECK(!graph::connect(app, mask(radial), input(radial, ports::reference)).ok());
  CHECK(!graph::setDocumentOutput(app, mask(radial)).ok());
  auto generated = evaluate(app, mask(radial), 8, 8);
  CHECK(generated.state == EvaluationState::Available && coverage(generated)->w == 8 && coverage(generated)->h == 8);
  auto result = evaluate(app, output, 8, 8);
  CHECK(result.state == EvaluationState::Available && result.diagnostics.empty());
  const auto local = pixels(result);
  for (size_t i = 0; i < 64; ++i) {
    const float amount = coverage(generated)->px[i];
    CHECK(std::abs(local->px[i * 4] - .25f * (1 + amount)) < 1e-7f);
    CHECK(std::abs(local->px[i * 4 + 1] - (-.5f * (1 + amount))) < 1e-7f);
    CHECK(std::abs(local->px[i * 4 + 2] - 2.0f * (1 + amount)) < 1e-6f);
    CHECK(local->px[i * 4 + 3] == .375f);
  }
  CHECK(evaluate(app, output, 8, 8, EvaluationPurpose::Export).value.index() == result.value.index());
  CHECK(pixels(evaluate(app, output, 8, 8, EvaluationPurpose::Export))->px == local->px);

  // Shared fan-out: two successive EV +1 operations each use the same coverage.
  selectOutput(app); CHECK(addNativeExposureNode(app)); const auto second = *app.selectedNodeId();
  CHECK(setNodeParameterValue(app, second, "exposure", 1.0));
  CHECK(graph::connect(app, mask(radial), input(second, ports::mask)).ok());
  const auto twice = pixels(evaluate(app, app.documentOutput, 8, 8));
  for (size_t i = 0; i < 64; ++i) {
    const float gain = 1 + coverage(generated)->px[i];
    CHECK(std::abs(twice->px[i * 4] - .25f * gain * gain) < 2e-7f);
  }
  const auto preview = coverage(evaluate(app, mask(radial), 7, 5));
  const auto full = coverage(evaluate(app, mask(radial), 21, 15, EvaluationPurpose::Export));
  const auto previewImage = pixels(evaluate(app, app.documentOutput, 7, 5));
  const auto exportImage = pixels(evaluate(app, app.documentOutput, 21, 15, EvaluationPurpose::Export));
  for (int y = 0; y < 5; ++y) for (int x = 0; x < 7; ++x) {
    CHECK(at(*preview, x, y) == at(*full, 3 * x + 1, 3 * y + 1));
    for (int c = 0; c < 4; ++c)
      CHECK(previewImage->px[(y * 7 + x) * 4 + c] == exportImage->px[((3 * y + 1) * 21 + 3 * x + 1) * 4 + c]);
  }
  setNodeEnabled(app, radial, false);
  const auto inactive = evaluate(app, mask(radial), 8, 8);
  CHECK(inactive.state == EvaluationState::Inactive && std::holds_alternative<std::monostate>(inactive.value));
  CHECK(inactive.diagnostics.empty());
  result = evaluate(app, app.documentOutput, 8, 8);
  CHECK(result.state == EvaluationState::Available && result.diagnostics.empty() && pixels(result)->px[0] == 1);
  setNodeEnabled(app, radial, true);
  CHECK(setNodeParameterValue(app, radial, "radiusX", 0.0));
  CHECK(uniform(coverage(evaluate(app, mask(radial), 8, 8)), invert ? 1.0f : 0.0f));
  auto expected = raster(8, 8);
  if (invert) for (size_t i = 0; i < 64; ++i) for (int c = 0; c < 3; ++c) expected.px[i*4+c] *= 4;
  CHECK(pixels(evaluate(app, app.documentOutput, 8, 8))->px == expected.px);
  CHECK(resetNodeParameter(app, radial, "radiusX"));

  ReferenceExecutor executor;
  Node reference(NodeOperation::TestUnavailableImage); reference.id = NodeId{"reference-fixture"};
  CHECK(graph::addNode(app, std::move(reference)).ok());
  CHECK(graph::connect(app, image(NodeId{"reference-fixture"}), input(radial, ports::reference)).ok());
  generated = evaluate(app, mask(radial), 8, 8, EvaluationPurpose::Preview, &executor);
  CHECK(generated.state == EvaluationState::Available && coverage(generated)->w == 3 && coverage(generated)->h == 7);
  CHECK(executor.calls == 1);
  CHECK(evaluate(app, output, 8, 8, EvaluationPurpose::Preview, &executor).state == EvaluationState::Error); // Real dimension mismatch.
  executor.unavailable = true;
  CHECK(evaluate(app, mask(radial), 8, 8, EvaluationPurpose::Preview, &executor).state == EvaluationState::Unavailable);
  result = evaluate(app, app.documentOutput, 8, 8, EvaluationPurpose::Preview, &executor);
  CHECK(result.state == EvaluationState::Available && warning(result, "Optional Mask unavailable"));
  CHECK(pixels(result)->px[0] == 1);
  const int calls = executor.calls;
  setNodeEnabled(app, radial, false);
  result = evaluate(app, app.documentOutput, 8, 8, EvaluationPurpose::Preview, &executor);
  CHECK(executor.calls == calls && result.state == EvaluationState::Available && result.diagnostics.empty());
  // Generation-only dependency is also skipped if it is incomplete, while stored cycles still reject.
  Node incomplete(NodeOperation::TestImage); incomplete.id = NodeId{"incomplete-reference"};
  CHECK(graph::addNode(app, std::move(incomplete)).ok());
  CHECK(graph::reconnect(app, image(NodeId{"incomplete-reference"}), input(radial, ports::reference)).ok());
  CHECK(evaluate(app, app.documentOutput, 8, 8).state == EvaluationState::Available);
  CHECK(!graph::reconnect(app, image(exposure), input(radial, ports::reference)).ok());
  setNodeEnabled(app, radial, true);
  CHECK(evaluate(app, mask(radial), 8, 8).state == EvaluationState::Error);
  CHECK(graph::disconnect(app, input(radial, ports::reference)).ok());
  auto *node = findNode(app, radial); node->processor.reset();
  CHECK(evaluate(app, mask(radial), 8, 8).state == EvaluationState::Unavailable);
  setNodeEnabled(app, radial, false);
  CHECK(evaluate(app, mask(radial), 8, 8).state == EvaluationState::Inactive);
  // A broken explicit endpoint remains a structural error, including during bypass.
  findNode(app, radial)->fixedInputs.at(ports::reference) = image(NodeId{"missing-reference"});
  CHECK(evaluate(app, mask(radial), 8, 8).state == EvaluationState::Error);
  findNode(app, radial)->fixedInputs.at(ports::reference).reset();
  CHECK(destroyNode(app, radial) && !*findInput(app, input(exposure, ports::mask)) && !*findInput(app, input(second, ports::mask)));
  return true;
}

struct Files {
  std::filesystem::path dir = std::filesystem::temp_directory_path() / ("rawnode-radial-" +
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  Files() { std::filesystem::create_directory(dir); }
  ~Files() { std::error_code ignored; std::filesystem::remove_all(dir, ignored); }
  std::string path(const char *name) const { return (dir / name).string(); }
};
struct Clipboard {
  ImGuiContext *previous = ImGui::GetCurrentContext(), *context = ImGui::CreateContext();
  std::string text;
  Clipboard() {
    auto &platform = ImGui::GetPlatformIO(); platform.Platform_ClipboardUserData = this;
    platform.Platform_GetClipboardTextFn = [](ImGuiContext *) {
      return static_cast<Clipboard *>(ImGui::GetPlatformIO().Platform_ClipboardUserData)->text.c_str();
    };
    platform.Platform_SetClipboardTextFn = [](ImGuiContext *, const char *text) {
      static_cast<Clipboard *>(ImGui::GetPlatformIO().Platform_ClipboardUserData)->text = text;
    };
  }
  ~Clipboard() { ImGui::DestroyContext(context); ImGui::SetCurrentContext(previous); }
};

bool inspectorBoolean() {
  Files files; Clipboard ui; App app;
  const auto photo = files.path("radial-inspector.png");
  CHECK(writeImage(raster(8, 8), photo, {RgbGamut::Rec709, TransferFunction::Linear}) && openPath(app, photo));
  CHECK(addNativeRadialMaskNode(app)); const auto radial = *app.selectedNodeId();
  auto &io = ImGui::GetIO(); io.IniFilename = nullptr; io.ConfigInputTrickleEventQueue = false;
  unsigned char *font; int w, h; io.Fonts->GetTexDataAsRGBA32(&font, &w, &h);
  ImRect checkbox;
  const auto tick = [&] {
    io.DisplaySize = {900, 800}; io.DeltaTime = 1.0f / 60;
    ImGui::NewFrame();
    ImGui::SetNextWindowPos({20, 20}); ImGui::SetNextWindowSize({650, 700});
    ImGui::Begin("Radial Inspector", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove);
    drawInspector(app); checkbox = ui.context->LastItemData.Rect;
    ImGui::End(); ImGui::EndFrame();
  };
  const auto click = [&](ImVec2 point) {
    io.AddMousePosEvent(point.x, point.y); tick();
    io.AddMouseButtonEvent(0, true); tick();
    io.AddMouseButtonEvent(0, false); tick();
  };
  tick(); tick();
  const auto history = app.history.size();
  click(checkbox.GetCenter());
  CHECK(inverted(*findNode(app, radial)) && app.history.size() == history + 1);
  CHECK(app.history.undo(app) && !inverted(*findNode(app, radial)));
  CHECK(app.history.redo(app) && inverted(*findNode(app, radial)));
  tick();
  click({checkbox.Min.x - ImGui::GetStyle().ItemInnerSpacing.x - ImGui::GetFrameHeight()/2, checkbox.GetCenter().y});
  CHECK(!inverted(*findNode(app, radial)) && app.history.size() == history + 2);
  CHECK(app.history.undo(app) && inverted(*findNode(app, radial)));
  CHECK(app.history.redo(app) && !inverted(*findNode(app, radial)));
  return true;
}

bool maskPresentation() {
  App app;
  CHECK(addNativeExposureNode(app)); const auto exposure = *app.selectedNodeId();
  CHECK(addNativeRadialMaskNode(app)); const auto radial = *app.selectedNodeId();
  CHECK(graph::connect(app, mask(radial), input(exposure, ports::mask)).ok());
  CHECK(setNodeParameterValue(app, radial, "centreX", .515625));
  CHECK(setNodeParameterValue(app, radial, "centreY", .515625));
  CHECK(setNodeParameterValue(app, radial, "invert", true));
  const auto generated = evaluate(app, mask(radial), 32, 32);
  const auto photo = evaluate(app, app.documentOutput, 32, 32);
  const auto gray = display_detail::preparePresentation(app, generated);
  const auto background = display_detail::preparePresentation(app, photo);
  const auto overlay = display_detail::preparePresentation(app, photo, generated);
  // Along the centre row: inverted centre=0, cubic feather=5/32, outside=1.
  for (const auto &sample : {std::pair<int, float>{16, 0}, {21, .15625f}, {31, 1}}) {
    const size_t i = (size_t(31-16)*32+sample.first)*4;
    for (int c = 0; c < 3; ++c) {
      CHECK(gray.rgba[i+c] == std::lround(255*sample.second));
      const float target = c == 0 ? 255.0f : 0.0f;
      CHECK(overlay.rgba[i+c] == static_cast<unsigned char>(background.rgba[i+c] +
          (target-background.rgba[i+c])*(.4f*sample.second)+.5f));
    }
    CHECK(gray.rgba[i+3] == 255 && overlay.rgba[i+3] == background.rgba[i+3]);
  }
  return true;
}

bool editingAndPersistence() {
  Files files; Clipboard clipboard; App app;
  const auto photo = files.path("photo.png");
  CHECK(writeImage(raster(8, 8), photo, {RgbGamut::Rec709, TransferFunction::Linear}));
  CHECK(openPath(app, photo));
  CHECK(addNativeExposureNode(app)); const auto exposure = *app.selectedNodeId();
  CHECK(setNodeParameterValue(app, exposure, "exposure", 1.0) && app.history.recordCompletedEdit(app));
  CHECK(addNativeRadialMaskNode(app)); const auto radial = *app.selectedNodeId();
  CHECK(app.history.undo(app) && !findNode(app, radial));
  CHECK(app.history.redo(app) && findNode(app, radial));
  CHECK(!inverted(*findNode(app, radial)));
  CHECK(graph::connect(app, mask(radial), input(exposure, ports::mask)).ok());
  CHECK(graph::connect(app, image(app.primarySource), input(radial, ports::reference)).ok());
  CHECK(setNodeParameterValue(app, radial, "centreX", .25));
  CHECK(setNodeParameterValue(app, radial, "radiusY", .4));
  CHECK(setNodeParameterValue(app, radial, "feather", .75));
  CHECK(setNodeParameterValue(app, radial, "invert", true));
  CHECK(app.history.recordCompletedEdit(app));
  CHECK(app.history.undo(app) && parameter(*findNode(app, radial), "centreX") == .5 && !inverted(*findNode(app, radial)));
  CHECK(app.history.redo(app) && parameter(*findNode(app, radial), "centreX") == .25 && inverted(*findNode(app, radial)));
  setNodeEnabled(app, radial, false);
  CHECK(!findNode(app, radial)->enabled && app.history.undo(app) && findNode(app, radial)->enabled);
  CHECK(app.history.redo(app) && !findNode(app, radial)->enabled);
  setNodeEnabled(app, radial, true);
  CHECK(saveCurrentInputSidecar(app));
  CHECK(openPath(app, photo) && findNode(app, radial) && findNode(app, radial)->enabled);
  CHECK(inverted(*findNode(app, radial)));
  CHECK(parameter(*findNode(app, radial), "radiusY") == .4 && parameter(*findNode(app, radial), "feather") == .75);
  CHECK(*findInput(app, input(exposure, ports::mask)) == mask(radial));
  CHECK(*findInput(app, input(radial, ports::reference)) == image(app.primarySource));
  setNodeEnabled(app, radial, false);
  CHECK(saveCurrentInputSidecar(app) && openPath(app, photo) && !findNode(app, radial)->enabled);
  CHECK(evaluate(app, mask(radial), 8, 8).state == EvaluationState::Inactive);
  setNodeEnabled(app, radial, true);
  const auto preset = files.path("preset.rawnode.json");
  CHECK(savePreset(app, preset)); PersistGrade grade; std::string error;
  CHECK(loadPresetFile(preset, grade, error));
  for (const auto &saved : {std::string("1"), std::string("\"true\""), std::string("null"), std::string("{}")}) {
    auto badBoolean = grade;
    for (auto &record : badBoolean.graph.nodes) if (record.id == radial.value) record.paramsJson["invert"] = saved;
    CHECK(!applyGrade(app, badBoolean) && inverted(*findNode(app, radial)));
  }
  auto older = grade;
  for (auto &record : older.graph.nodes) if (record.id == radial.value) record.paramsJson.erase("invert");
  CHECK(applyGrade(app, older) && !inverted(*findNode(app, radial)));
  CHECK(applyGrade(app, grade) && inverted(*findNode(app, radial)));
  // Preserve Boolean JSON verbatim when the processor is unavailable, using the existing missing-node path.
  auto *missing = findNode(app, radial);
  for (const auto &record : grade.graph.nodes) if (record.id == radial.value) missing->preservedParamsJson = record.paramsJson;
  missing->processor.reset();
  PersistGrade unavailable;
  CHECK(captureGrade(app, unavailable));
  for (const auto &record : unavailable.graph.nodes) if (record.id == radial.value)
    CHECK(record.paramsJson.at("invert") == "true");
  std::string savedMissing; PersistTransfer reopenedMissing;
  CHECK(encodeTransferPayload(unavailable, savedMissing, error) && decodeTransferPayload(savedMissing, reopenedMissing, error));
  CHECK(applyGrade(app, std::get<PersistGrade>(reopenedMissing)) && inverted(*findNode(app, radial)));
  // Unknown parameters survive the one existing codec; understood invalid values fail before publication.
  auto invalid = grade;
  for (auto &record : invalid.graph.nodes) if (record.id == radial.value) record.paramsJson["radiusX"] = "-1";
  CHECK(!applyGrade(app, invalid) && parameter(*findNode(app, radial), "radiusX") == .25);
  for (auto &record : grade.graph.nodes) if (record.id == radial.value) record.paramsJson["future"] = "{\"v\":7}";
  CHECK(applyGrade(app, grade) && findNode(app, radial)->preservedParamsJson.at("future") == "{\"v\":7}");
  CHECK(selectNode(app, radial) && canCopySelectedNode(app) && copySelectedNode(app));
  CHECK(clipboard.text.find("native-radial-mask") != std::string::npos && clipboard.text.find("strength") == std::string::npos);
  const auto output = app.documentOutput;
  CHECK(pasteFromClipboard(app)); const auto pasted = *app.selectedNodeId();
  CHECK(pasted != radial && app.documentOutput == output && !*findInput(app, input(pasted, ports::reference)));
  CHECK(parameter(*findNode(app, pasted), "centreX") == .25);
  CHECK(inverted(*findNode(app, pasted)));
  CHECK(app.nodePositions.at(pasted.value).x == app.nodePositions.at(radial.value).x &&
        app.nodePositions.at(pasted.value).y == app.nodePositions.at(radial.value).y);
  CHECK(app.history.undo(app) && !findNode(app, pasted));
  CHECK(app.history.redo(app) && findNode(app, pasted));
  CHECK(copyGrade(app)); clearNodes(app); CHECK(pasteFromClipboard(app));
  CHECK(findNode(app, radial) && *findInput(app, input(exposure, ports::mask)) == mask(radial));
  CHECK(inverted(*findNode(app, radial)));
  CHECK(destroyNode(app, radial) && !*findInput(app, input(exposure, ports::mask)));
  CHECK(app.history.undo(app) && *findInput(app, input(exposure, ports::mask)) == mask(radial));
  CHECK(loadPresetDialog(app, false, preset) && findNode(app, radial));
  CHECK(inverted(*findNode(app, radial)));

  // Real worker publication follows parameter edits; selection leaves the processed photograph as default.
  app.renderer.start(); app.renderer.requestPreview(); CHECK(idle(app));
  std::vector<float> before;
  { std::lock_guard<std::mutex> lock(app.displayMutex);
    CHECK(app.processed.result.requested == app.documentOutput && app.processed.result.state == EvaluationState::Available);
    before = pixels(app.processed.result)->px;
  }
  for (int i = 0; i < 40; ++i)
    CHECK(setNodeParameterValue(app, radial, "centreX", (i % 2) ? .8 : .2));
  CHECK(idle(app));
  std::vector<float> edited;
  { std::lock_guard<std::mutex> lock(app.displayMutex); edited = pixels(app.processed.result)->px; }
  CHECK(before != edited && app.history.recordCompletedEdit(app));
  CHECK(app.history.undo(app) && idle(app));
  { std::lock_guard<std::mutex> lock(app.displayMutex); CHECK(pixels(app.processed.result)->px == before); }
  CHECK(app.history.redo(app) && idle(app));
  { std::lock_guard<std::mutex> lock(app.displayMutex); CHECK(pixels(app.processed.result)->px == edited); }
  {
    auto exporting = app.renderer.acquireExport(); CHECK(bool(exporting));
    CHECK(!setNodeParameterValue(app, radial, "centreX", .1) && !resetNodeParameter(app, radial, "feather"));
    CHECK(parameter(*findNode(app, radial), "centreX") == .8);
  }
  const auto exported = files.path("export.png"), expected = files.path("expected.png");
  CHECK(writeImage(Image{edited, 8, 8}, expected, app.outputEncoding));
  CHECK(runExportJob(app, exported) && idle(app));
  Image actual, baseline;
  ColorEncoding detected; bool raw;
  CHECK(loadImage(exported, actual, detected, raw) && loadImage(expected, baseline, detected, raw));
  CHECK(actual.w == 8 && actual.h == 8 && actual.px == baseline.px);
  CHECK(!std::filesystem::exists(inputSidecarPath(exported)));
  app.renderer.shutdown();
  return true;
}
}

bool testNativeRadialMask() {
  if (!numerical() || !inversion() || !zeroRadiusAtPixelCentre() || !persistedRadialIdentityValidation() ||
      !processorOperationIdentityPairing() || !graphSemantics(false) || !graphSemantics(true) ||
      !inspectorBoolean() || !maskPresentation() || !editingAndPersistence()) return false;
  std::printf("ok  native Radial Mask geometry, typed evaluation, editing, persistence and preview/export\n");
  return true;
}
