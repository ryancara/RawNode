#include "ui/UiContext.h"

#include "NodeGraph.h"
#include "ui/ParamWidgets.h"
#include "ui/Widgets.h"
#include "color/LinearColorTransform.h"
#include "color/TransferFunction.h"


#include "IconsFontAwesome6.h"
#include "imgui.h"

#include <algorithm>
#include <vector>

static void drawProcessorParameters(App &app) {
  Node *node = selectedNode(app);
  if (!node) {
    ImGui::TextWrapped("Select Source, a processor or Output to view its settings.");
    return;
  }
  ImGui::AlignTextToFramePadding();
  const std::string label = nodeDisplayName(*node);
  ImGui::TextUnformatted(label.c_str());
  ImGui::Separator();
  if (node->operation != NodeOperation::ImageProcessor) {
    ImGui::TextDisabled("No parameter controls are available for this graph operation.");
    return;
  }
  if (!node->processor) {
    ImGui::TextDisabled("Processor unavailable. Its saved state is preserved and the node is bypassed.");
    return;
  }
  // ImGui edit/focus state belongs to the logical node, even when two nodes
  // have the same processor and parameter names.
  ImGui::PushID(node->id.value.data(), node->id.value.data() + node->id.value.size());
  if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_F) || ImGui::IsKeyChordPressed(ImGuiMod_Super | ImGuiKey_F))
    ImGui::SetKeyboardFocusHere();
  const bool hasFilter = app.paramFilter[0] != '\0';
  if (hasFilter) {
    const float clearW = ImGui::GetFrameHeight();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - clearW - ImGui::GetStyle().ItemSpacing.x);
  } else {
    ImGui::SetNextItemWidth(-1);
  }
  ImGui::InputTextWithHint("##paramFilter", "Search parameters...", app.paramFilter, sizeof app.paramFilter);
  if (hasFilter) {
    ImGui::SameLine();
    if (iconBtn("##clearFilter", ICON_FA_XMARK)) app.paramFilter[0] = '\0';
  }
  ImGui::Separator();
  drawParams(app, *node, "");
  ImGui::PopID();
}

namespace {
void drawColourControls(App &app, ColorEncoding encoding, bool raw) {
  std::vector<const char *> gamuts, gammas;
  for (int i = 0; i < rgbGamutCount(); ++i) gamuts.push_back(rgbGamutDefinition(i).name);
  for (int i = 0; i < transferFunctionCount(); ++i) gammas.push_back(transferFunctionDefinition(i).name);
  ImGui::BeginDisabled(app.path.empty() || app.isPhotoProtected());
  int gamut = std::max(0, rgbGamutIndex(encoding.gamut));
  if (ImGui::Combo(raw ? "RAW colour space" : "Output colour space", &gamut, gamuts.data(), (int)gamuts.size())) {
    encoding.gamut = rgbGamutDefinition(gamut).value;
    if (raw) setRawWorkingEncoding(app, encoding.gamut, encoding.gamma);
    else setOutputEncoding(app, encoding);
  }
  int gamma = std::max(0, transferFunctionIndex(encoding.gamma));
  if (ImGui::Combo(raw ? "RAW gamma" : "Output gamma", &gamma, gammas.data(), (int)gammas.size())) {
    if (raw) setRawWorkingEncoding(app, encoding.gamut, transferFunctionDefinition(gamma).value);
    else setOutputEncoding(app, {encoding.gamut, transferFunctionDefinition(gamma).value});
  }
  ImGui::EndDisabled();
}

void drawSourceSettings(App &app) {
  ImGui::TextUnformatted("Source");
  ImGui::Separator();
  if (app.path.empty()) { ImGui::TextDisabled("Open an image to view input settings."); return; }
  ImGui::TextWrapped("%s", app.path.c_str());
  ImGui::Text("%d x %d", app.full.w, app.full.h);
  ColorEncoding input;
  bool raw;
  {
    std::lock_guard<std::mutex> lock(app.colorMutex);
    input = app.inputEncoding;
    raw = app.inputIsRaw;
  }
  ImGui::TextWrapped("Input pixels: %s", colorEncodingName(input).c_str());
  if (raw) drawColourControls(app, input, true);
}

void drawOutputSettings(App &app) {
  ImGui::TextUnformatted("Output");
  ImGui::Separator();
  ColorEncoding output;
  {
    std::lock_guard<std::mutex> lock(app.colorMutex);
    output = app.outputEncoding;
  }
  drawColourControls(app, output, false);
  ImGui::Separator();
  ImGui::Combo("Export format", &app.exportFormat, "PNG (8-bit)\0JPEG\0");
  if (app.exportFormat == 1) ImGui::SliderInt("JPEG quality", &app.jpegQuality, 1, 100);
}
}  // namespace

void drawInspector(App &app) {
  if (app.outputSelected()) drawOutputSettings(app);
  else if (app.selectedNodeId() == app.primarySource) drawSourceSettings(app);
  else {
    ImGui::BeginDisabled(app.isPhotoProtected());
    drawProcessorParameters(app);
    ImGui::EndDisabled();
  }
}
