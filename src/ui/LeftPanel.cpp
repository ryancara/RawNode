#include "ui/UiContext.h"

#include "persist/DocumentActions.h"
#include "persist/ProjectPersist.h"
#include "NodeGraph.h"
#include "color/LinearColorTransform.h"
#include "color/TransferFunction.h"
#include "RenderPipeline.h"
#include "ofx/OfxHost.h"  // gPlugins: external plugin discovery is still OFX-specific.
#include "ui/Widgets.h"

#include "IconsFontAwesome6.h"
#include "imgui.h"
#include "portable-file-dialogs.h"

#include <algorithm>
#include <cctype>
#include <string>

static bool icontains(const std::string &hay, const std::string &needle) {
  if (needle.empty()) return true;
  auto lower = [](unsigned char c) { return (char)std::tolower(c); };
  auto it = std::search(hay.begin(), hay.end(), needle.begin(), needle.end(),
                        [&](char a, char b) { return lower(a) == lower((unsigned char)b); });
  return it != hay.end();
}

void drawLeftPanel(App &app) {
  if (ImGui::Button("Open image")) {
    auto f = pfd::open_file("Open image", "", openImageDialogFilters());
    auto r = f.result();
    if (!r.empty()) openPath(app, r[0]);
  }
  ImGui::SameLine();
  if (ImGui::Button("Open Workspace")) {
    auto f = pfd::select_folder("Open workspace folder");
    auto r = f.result();
    if (!r.empty()) app.pendingWorkspaceDir = r;
  }
  ImGui::SameLine();
  if (ImGui::Button("Export")) doExport(app);

  if (app.path.empty()) {
    ImGui::TextUnformatted("Input: —");
  } else if (app.inputUsesRawEncoding) {
    ImGui::Text("Input: %s / %s", rgbGamutName(app.inputGamut), transferFunctionName(app.inputGamma));
  } else {
    ImGui::Text("Input: %s", colorSpaceName(app.inputSpace));
  }

  const bool currentIsRaw = !app.path.empty() && isRawImagePath(app.path);
  int rawGamutIndex = (int)(currentIsRaw ? app.inputGamut : app.rawWorkingGamut);
  int rawGammaIndex = (int)(currentIsRaw ? app.inputGamma : app.rawWorkingGamma);

  const char *rawGamutItems[] = {
      rgbGamutName(RgbGamut::Rec709),
      rgbGamutName(RgbGamut::Rec2020),
      rgbGamutName(RgbGamut::ACES_AP0),
      rgbGamutName(RgbGamut::ACES_AP1),
      rgbGamutName(RgbGamut::DaVinciWideGamut),
  };
  if (ImGui::Combo("RAW colour space", &rawGamutIndex, rawGamutItems, 5))
    setRawWorkingEncoding(app, (RgbGamut)rawGamutIndex,
                          currentIsRaw ? app.inputGamma : app.rawWorkingGamma);

  const char *rawGammaItems[] = {
      transferFunctionName(TransferFunction::Linear),
      transferFunctionName(TransferFunction::SRGB),
      transferFunctionName(TransferFunction::Rec709),
      transferFunctionName(TransferFunction::DaVinciIntermediate),
  };
  if (ImGui::Combo("RAW gamma", &rawGammaIndex, rawGammaItems, 4))
    setRawWorkingEncoding(app, currentIsRaw ? app.inputGamut : app.rawWorkingGamut,
                          (TransferFunction)rawGammaIndex);
  ImGui::Combo("Output tag", &app.outputIndex, kOutputSpaces, kOutputSpaceCount);
  if (ImGui::IsItemDeactivatedAfterEdit() || ImGui::IsItemEdited()) scheduleDisplayRecolor(app);
  {
    const char *items[kPreviewResCount];
    for (int i = 0; i < kPreviewResCount; ++i) items[i] = kPreviewRes[i].label;
    if (ImGui::Combo("Preview", &app.previewRes, items, kPreviewResCount)) rebuildPreview(app);
  }
  ImGui::Combo("Export format", &app.exportFormat, "PNG (8-bit)\0JPEG\0");
  if (app.exportFormat == 1) ImGui::SliderInt("JPEG quality", &app.jpegQuality, 1, 100);
  ImGui::Separator();
  const std::string status = app.getStatus();
  ImGui::TextWrapped("%s", status.c_str());
  ImGui::Separator();

  ImGui::TextUnformatted("Processing Nodes");
  if (ImGui::Button("Add processor…", ImVec2(-1, 0))) ImGui::OpenPopup("##addPluginPopup");
  if (ImGui::BeginPopup("##addPluginPopup")) {
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##pluginFilter", "Search…", app.pluginFilter, sizeof app.pluginFilter);
    ImGui::Separator();

    const std::string q = app.pluginFilter;
    int shown = 0;

    bool showedNativeHeader = false;
    const auto nativeHeader = [&]() {
      if (!showedNativeHeader) {
        ImGui::SeparatorText("Native");
        showedNativeHeader = true;
      }
    };

    if (q.empty() || icontains("Exposure", q) || icontains("Native", q)) {
      nativeHeader();
      if (ImGui::Selectable("Exposure")) {
        addNativeExposureNode(app);
        app.pluginFilter[0] = '\0';
        ImGui::CloseCurrentPopup();
      }
      ++shown;
    }

    if (q.empty() || icontains("CST", q) || icontains("Colour Space Transform", q) ||
        icontains("Color Space Transform", q) || icontains("Native", q)) {
      nativeHeader();
      if (ImGui::Selectable("CST")) {
        addNativeCstNode(app);
        app.pluginFilter[0] = '\0';
        ImGui::CloseCurrentPopup();
      }
      ++shown;
    }

    if (q.empty() || icontains("CTL", q) || icontains("script", q)) {
      ImGui::SeparatorText("CTL");
      if (ImGui::Selectable("Load CTL script…")) {
        auto file = pfd::open_file("Load CTL script", "", {"CTL script", "*.ctl"});
        const auto paths = file.result();
        if (!paths.empty()) addCtlNode(app, paths[0]);
        app.pluginFilter[0] = '\0';
        ImGui::CloseCurrentPopup();
      }
      ++shown;
    }

    std::string curAuthor;
    bool showedOfxHeader = false;
    for (int i = 0; i < (int)gPlugins.size(); ++i) {
      const auto &pe = gPlugins[i];
      if (!q.empty() && !icontains(pe.label, q) && !icontains(pe.author, q) &&
          !(pe.plugin && pe.plugin->pluginIdentifier && icontains(pe.plugin->pluginIdentifier, q)))
        continue;

      if (!showedOfxHeader) {
        ImGui::SeparatorText("OFX");
        showedOfxHeader = true;
      }
      if (pe.author != curAuthor) {
        curAuthor = pe.author;
        ImGui::TextDisabled("%s", curAuthor.c_str());
      }
      if (ImGui::Selectable(pe.label.c_str())) {
        addNode(app, i);
        app.pluginFilter[0] = '\0';
        ImGui::CloseCurrentPopup();
      }
      ++shown;
    }

    if (shown == 0) ImGui::TextDisabled("No matches");
    ImGui::EndPopup();
  }

  ImGui::BeginChild("nodeList", ImVec2(0, 0), ImGuiChildFlags_Borders);
  if (app.nodes.empty()) ImGui::TextDisabled("No nodes yet.\nAdd a processor to build a chain.");
  const float btnH = ImGui::GetFrameHeight();
  const float btnGap = ImGui::GetStyle().ItemSpacing.x;
  const float btnsW = 4.0f * btnH + 3.0f * btnGap;
  int removeAt = -1;
  for (int i = 0; i < (int)app.nodes.size(); ++i) {
    ImGui::PushID(i);
    Node &node = app.nodes[i];
    const bool selected = app.selectedNode == i;
    const std::string label = nodeDisplayName(node);
    if (!node.enabled) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.4f);
    if (ImGui::Selectable(label.c_str(), selected, 0, ImVec2(ImGui::GetContentRegionAvail().x - btnsW - btnGap, btnH))) {
      app.selectedNode = i;
      app.paramFilter[0] = '\0';
    }
    if (!node.enabled) ImGui::PopStyleVar();
    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
      ImGui::SetDragDropPayload("NODE_IDX", &i, sizeof(i));
      ImGui::Text("%s", label.c_str());
      ImGui::EndDragDropSource();
    }
    if (ImGui::BeginDragDropTarget()) {
      if (const ImGuiPayload *payload = ImGui::AcceptDragDropPayload("NODE_IDX")) {
        const int from = *(const int *)payload->Data;
        moveNode(app, from, i);
      }
      ImGui::EndDragDropTarget();
    }
    ImGui::SameLine(0.0f, btnGap);
    if (iconBtn("##en", node.enabled ? ICON_FA_EYE : ICON_FA_EYE_SLASH)) {
      node.enabled = !node.enabled;
      scheduleRender(app);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
      ImGui::SetTooltip(node.enabled ? "Disable processing" : "Enable processing");
    ImGui::SameLine(0.0f, btnGap);
    if (iconBtn("##up", ICON_FA_ARROW_UP) && i > 0) moveNode(app, i, i - 1);
    ImGui::SameLine(0.0f, btnGap);
    if (iconBtn("##dn", ICON_FA_ARROW_DOWN) && i + 1 < (int)app.nodes.size()) moveNode(app, i, i + 1);
    ImGui::SameLine(0.0f, btnGap);
    if (iconBtn("##rm", ICON_FA_XMARK)) removeAt = i;
    ImGui::PopID();
  }
  if (removeAt >= 0) destroyNode(app, removeAt);
  ImGui::EndChild();
}
