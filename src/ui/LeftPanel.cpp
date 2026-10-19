#include "ui/UiContext.h"

#include "persist/DocumentActions.h"
#include "persist/ProjectPersist.h"
#include "NodeGraph.h"
#include "DocumentMutation.h"
#include "color/LinearColorTransform.h"
#include "color/TransferFunction.h"
#include "ofx/OfxHost.h"  // gPlugins: external plugin discovery is still OFX-specific.

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
    if (!r.empty()) openPath(app, r[0], promptSaveFailure);
  }
  ImGui::SameLine();
  if (ImGui::Button("Open Folder")) {
    auto f = pfd::select_folder("Open folder");
    auto r = f.result();
    if (!r.empty()) app.pendingFolderDir = r;
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

  ImGui::BeginDisabled(!currentIsRaw || app.isPhotoProtected());
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

  ImGui::EndDisabled();
  ImGui::BeginDisabled(app.path.empty() || app.isPhotoProtected());
  int outputGamutIndex = std::max(0, rgbGamutIndex(outputEncoding.gamut));
  if (ImGui::Combo("Output colour space", &outputGamutIndex, gamutItems.data(), (int)gamutItems.size())) {
    setOutputEncoding(app, {rgbGamutDefinition(outputGamutIndex).value, outputEncoding.gamma});
  }

  int outputGammaIndex = std::max(0, transferFunctionIndex(outputEncoding.gamma));
  if (ImGui::Combo("Output gamma", &outputGammaIndex, gammaItems.data(), (int)gammaItems.size())) {
    setOutputEncoding(app, {outputEncoding.gamut, transferFunctionDefinition(outputGammaIndex).value});
  }
  ImGui::EndDisabled();

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
  if (!app.persistenceWarning.empty())
    ImGui::TextWrapped("%s", app.persistenceWarning.c_str());
  ImGui::Separator();

  ImGui::TextUnformatted("Processing Nodes");
  ImGui::BeginDisabled(app.isPhotoProtected());
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

  ImGui::EndDisabled();
}
