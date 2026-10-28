#include "ui/UiContext.h"

#include "NodeGraph.h"
#include "ofx/OfxHost.h"
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

void drawProcessorPicker(App &app) {
  ImGui::BeginDisabled(app.isPhotoProtected());
  if (ImGui::Button("+ Add Processor")) ImGui::OpenPopup("##addPluginPopup");
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

    if (q.empty() || icontains("Radial Mask", q) || icontains("Native", q)) {
      nativeHeader();
      if (ImGui::Selectable("Radial Mask")) {
        addNativeRadialMaskNode(app);
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
