#include "ui/UiContext.h"

#include "NodeGraph.h"
#include "ofx/OfxHost.h"
#include "imgui.h"
#include "portable-file-dialogs.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>

static bool icontains(const std::string &hay, const std::string &needle) {
  if (needle.empty()) return true;
  auto lower = [](unsigned char c) { return (char)std::tolower(c); };
  auto it = std::search(hay.begin(), hay.end(), needle.begin(), needle.end(),
                        [&](char a, char b) { return lower(a) == lower((unsigned char)b); });
  return it != hay.end();
}

static void ctlHeader(bool &headerShown) {
  if (headerShown) return;
  ImGui::SeparatorText("CTL");
  headerShown = true;
}

static void drawCtlScripts(App &app, const DirectoryFolder &folder, const std::filesystem::path &root,
                           const std::string &query, bool &headerShown) {
  for (const auto &child : folder.folders) {
    if (!query.empty()) {
      drawCtlScripts(app, child, root, query, headerShown);
    } else {
      const auto name = child.path.filename().u8string();
      if (ImGui::TreeNode(name.c_str())) {
        drawCtlScripts(app, child, root, query, headerShown);
        ImGui::TreePop();
      }
    }
  }

  for (const auto &path : folder.files) {
    // Search results show the relative path so matching basenames remain distinct.
    const auto label = query.empty() ? path.filename().u8string() : path.lexically_relative(root).generic_u8string();
    if (!icontains(label, query)) continue;
    ctlHeader(headerShown);
    if (ImGui::Selectable(label.c_str())) {
      addCtlNode(app, path.u8string());
      app.pluginFilter[0] = '\0';
      ImGui::CloseCurrentPopup();
    }
  }
}

static void fitExpandablePicker() {
  // A normal popup only picks its initial position. Keep room for the list to
  // grow when a folder expands, and let ImGui scroll when it reaches that limit.
  const auto *viewport = ImGui::GetWindowViewport();
  const auto buttonMin = ImGui::GetItemRectMin();
  const auto buttonMax = ImGui::GetItemRectMax();
  const auto padding = ImGui::GetStyle().DisplaySafeAreaPadding;
  const float above = buttonMin.y - viewport->WorkPos.y - padding.y;
  const float below = viewport->WorkPos.y + viewport->WorkSize.y - buttonMax.y - padding.y;
  const float left = buttonMax.x - viewport->WorkPos.x - padding.x;
  const float right = viewport->WorkPos.x + viewport->WorkSize.x - buttonMin.x - padding.x;
  const bool openAbove = above > below;
  const bool alignRight = left > right;
  ImGui::SetNextWindowPos(ImVec2(alignRight ? buttonMax.x : buttonMin.x,
                                openAbove ? buttonMin.y : buttonMax.y),
                          ImGuiCond_Always, ImVec2(alignRight ? 1.0f : 0.0f, openAbove ? 1.0f : 0.0f));
  ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(std::max(left, right), std::max(above, below)));
}

void drawProcessorPicker(App &app) {
  ImGui::BeginDisabled(app.isPhotoProtected());
  if (ImGui::Button("+ Add Processor")) {
    app.ctlScriptDiscovery = discoverDirectory(std::filesystem::u8path(app.ctlScriptsDirectory), ".ctl");
    ImGui::OpenPopup("##addPluginPopup");
  }
  const auto &discovery = app.ctlScriptDiscovery;
  fitExpandablePicker();
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

    bool showedCtlHeader = false;
    const bool showManualCtl = q.empty() || icontains("CTL", q) || icontains("script", q);
    if (showManualCtl) {
      ctlHeader(showedCtlHeader);
      if (ImGui::Selectable("Load CTL script…")) {
        auto file = pfd::open_file("Load CTL script", "", {"CTL script", "*.ctl"});
        const auto paths = file.result();
        if (!paths.empty()) addCtlNode(app, paths[0]);
        app.pluginFilter[0] = '\0';
        ImGui::CloseCurrentPopup();
      }
    }

    ImGui::PushID("Discovered CTL scripts");
    drawCtlScripts(app, discovery.contents, discovery.contents.path, q, showedCtlHeader);
    ImGui::PopID();
    if (showManualCtl && !discovery.contents.path.empty()) {
      const bool empty = discovery.contents.empty();
      if (discovery.scanError) {
        ImGui::TextDisabled("%s", empty ? "Could not scan CTL scripts directory"
                                       : "Some CTL scripts could not be listed");
      } else if (empty) {
        ImGui::TextDisabled("No CTL scripts found");
      }
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

    if (shown == 0 && !showedCtlHeader) ImGui::TextDisabled("No matches");
    ImGui::EndPopup();
  } else {
    app.ctlScriptDiscovery = {};
  }

  ImGui::EndDisabled();
}
