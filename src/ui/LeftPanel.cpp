#include "ui/UiContext.h"

#include "persist/DocumentActions.h"
#include "persist/ProjectPersist.h"
#include "NodeGraph.h"
#include "GraphCommands.h"
#include "color/LinearColorTransform.h"
#include "color/TransferFunction.h"
#include "DocumentMutation.h"
#include "ofx/OfxHost.h"  // gPlugins: external plugin discovery is still OFX-specific.
#include "ui/Widgets.h"
#include "ui/NodeList.h"

#include "IconsFontAwesome6.h"
#include "imgui.h"
#include "portable-file-dialogs.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

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

  ColorEncoding inputEncoding;
  ColorEncoding rawDefaultEncoding;
  ColorEncoding outputEncoding;
  bool inputIsRaw = false;
  {
    std::lock_guard<std::mutex> lock(app.colorMutex);
    inputEncoding = app.inputEncoding;
    rawDefaultEncoding = app.rawWorkingEncoding;
    outputEncoding = app.outputEncoding;
    inputIsRaw = app.inputIsRaw;
  }

  if (app.path.empty())
    ImGui::TextUnformatted("Input: —");
  else
    ImGui::Text("Input: %s", colorEncodingName(inputEncoding).c_str());

  const bool currentIsRaw = !app.path.empty() && inputIsRaw;
  const ColorEncoding rawShown = currentIsRaw ? inputEncoding : rawDefaultEncoding;

  std::vector<const char *> gamutItems;
  gamutItems.reserve((size_t)rgbGamutCount());
  for (int i = 0; i < rgbGamutCount(); ++i)
    gamutItems.push_back(rgbGamutDefinition(i).name);

  std::vector<const char *> gammaItems;
  gammaItems.reserve((size_t)transferFunctionCount());
  for (int i = 0; i < transferFunctionCount(); ++i)
    gammaItems.push_back(transferFunctionDefinition(i).name);

  int rawGamutIndex = std::max(0, rgbGamutIndex(rawShown.gamut));
  if (ImGui::Combo("RAW colour space", &rawGamutIndex, gamutItems.data(), (int)gamutItems.size())) {
    setRawWorkingEncoding(app, rgbGamutDefinition(rawGamutIndex).value, rawShown.gamma);
  }

  int rawGammaIndex = std::max(0, transferFunctionIndex(rawShown.gamma));
  if (ImGui::Combo("RAW gamma", &rawGammaIndex, gammaItems.data(), (int)gammaItems.size())) {
    ColorEncoding currentRaw;
    {
      std::lock_guard<std::mutex> lock(app.colorMutex);
      currentRaw = app.inputIsRaw ? app.inputEncoding : app.rawWorkingEncoding;
    }
    setRawWorkingEncoding(app, currentRaw.gamut, transferFunctionDefinition(rawGammaIndex).value);
  }

  int outputGamutIndex = std::max(0, rgbGamutIndex(outputEncoding.gamut));
  if (ImGui::Combo("Output colour space", &outputGamutIndex, gamutItems.data(), (int)gamutItems.size())) {
    {
      std::lock_guard<std::mutex> lock(app.colorMutex);
      app.outputEncoding.gamut = rgbGamutDefinition(outputGamutIndex).value;
    }
    app.renderer.requestDisplayRefresh();
  }

  int outputGammaIndex = std::max(0, transferFunctionIndex(outputEncoding.gamma));
  if (ImGui::Combo("Output gamma", &outputGammaIndex, gammaItems.data(), (int)gammaItems.size())) {
    {
      std::lock_guard<std::mutex> lock(app.colorMutex);
      app.outputEncoding.gamma = transferFunctionDefinition(outputGammaIndex).value;
    }
    app.renderer.requestDisplayRefresh();
  }

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
  if (!app.path.empty() && app.sidecarWriteBlockedPath == app.path)
    ImGui::TextWrapped("Sidecar is read-only — changes to this photo will not be saved.");
  if (app.workspaceWriteBlocked)
    ImGui::TextWrapped("Workspace metadata is read-only — workspace changes will not be saved.");
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
  const auto rows = ui::projectNodeList({app.nodes, app.primarySource, app.documentOutput});
  if (const Node *output = findNode(app, app.documentOutput.nodeId))
    ImGui::TextWrapped("Document output: %s", nodeDisplayName(*output).c_str());
  if (!rows.spineError.empty())
    ImGui::TextWrapped("Reordering unavailable: %s. All graph nodes are shown below.", rows.spineError.c_str());
  if (rows.spine.empty() && rows.other.empty())
    ImGui::TextDisabled("No nodes yet.\nAdd a processor to build a chain.");
  const float btnH = ImGui::GetFrameHeight();
  const float btnGap = ImGui::GetStyle().ItemSpacing.x;
  const float btnsW = 4.0f * btnH + 3.0f * btnGap;
  std::optional<NodeId> remove, moveFrom, moveTo, enable;
  bool enabled = false;
  const auto drawRows = [&](const std::vector<NodeId> &ids, bool mainSpine) {
    const bool reorderable = mainSpine && rows.spineError.empty();
    for (size_t row = 0; row < ids.size(); ++row) {
      const Node *node = findNode(app, ids[row]);
      if (!node) continue;
      ImGui::PushID(node->id.value.data(), node->id.value.data() + node->id.value.size());
      const std::string label = nodeDisplayName(*node);
      if (!node->enabled) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.4f);
      if (ImGui::Selectable(label.c_str(), app.selectedNodeId == node->id, 0,
                            ImVec2(std::max(btnH, ImGui::GetContentRegionAvail().x - btnsW - btnGap), btnH)))
        selectNode(app, node->id);
      if (!node->enabled) ImGui::PopStyleVar();
      if (reorderable && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
        ImGui::SetDragDropPayload("NODE_ID", node->id.value.c_str(), node->id.value.size() + 1);
        ImGui::Text("%s", label.c_str());
        ImGui::EndDragDropSource();
      }
      if (reorderable && ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload *payload = ImGui::AcceptDragDropPayload("NODE_ID")) {
          const auto *data = static_cast<const char *>(payload->Data);
          if (payload->DataSize > 1 && data[payload->DataSize - 1] == '\0') {
            moveFrom = NodeId{std::string(data, payload->DataSize - 1)};
            moveTo = node->id;
          }
        }
        ImGui::EndDragDropTarget();
      }
      ImGui::SameLine(0.0f, btnGap);
      if (iconBtn("##en", node->enabled ? ICON_FA_EYE : ICON_FA_EYE_SLASH)) {
        enable = node->id;
        enabled = !node->enabled;
      }
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        ImGui::SetTooltip(node->enabled ? "Disable processing" : "Enable processing");

      const auto arrow = [&](const char *id, const char *icon, bool available, const char *hint) {
        ImGui::SameLine(0.0f, btnGap);
        ImGui::BeginDisabled(!available);
        const bool clicked = iconBtn(id, icon);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayNormal))
          ImGui::SetTooltip("%s", !mainSpine ? "Only main Image spine nodes can be moved" :
                            !reorderable ? "Moves require a complete main Image spine" : hint);
        return clicked;
      };
      if (arrow("##up", ICON_FA_ARROW_UP, reorderable && row > 0,
                row > 0 ? "Move up" : "Already first on the main Image spine")) {
        moveFrom = node->id;
        moveTo = ids[row - 1];
      }
      if (arrow("##dn", ICON_FA_ARROW_DOWN, reorderable && row + 1 < ids.size(),
                row + 1 < ids.size() ? "Move down" : "Already last on the main Image spine")) {
        moveFrom = node->id;
        moveTo = ids[row + 1];
      }
      ImGui::SameLine(0.0f, btnGap);
      if (iconBtn("##rm", ICON_FA_XMARK)) remove = node->id;
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        ImGui::SetTooltip("Remove node");
      ImGui::PopID();
    }
  };
  if (!rows.spine.empty()) {
    ImGui::SeparatorText(rows.spineError.empty() ? "Main Image spine" : "Main Image spine (partial)");
    drawRows(rows.spine, true);
  }
  if (!rows.other.empty()) {
    ImGui::SeparatorText("Other graph nodes");
    ImGui::TextDisabled("Outside the displayed Image spine;\nselect and edit without list reordering.");
    drawRows(rows.other, false);
  }
  ImGui::EndChild();
  // Apply one row-control intent after all references into the node list have
  // expired. Intents and drag/drop retain identity, never storage positions.
  if (remove) destroyNode(app, *remove);
  else if (moveFrom && moveTo) moveNode(app, *moveFrom, *moveTo);
  else if (enable) setNodeEnabled(app, *enable, enabled);
}
