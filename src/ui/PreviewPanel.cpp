#include "ui/UiContext.h"
#include "ui/CommandTargets.h"
#include "DocumentMutation.h"
#include "NodeGraph.h"
#include "RenderPipeline.h"

#include "imgui.h"
#include "imgui_internal.h"

#if defined(__APPLE__)
#include "ui/MacPinch.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ui {
namespace {
bool heldPreviewInputBlocked(const App &app) {
  const auto &io = ImGui::GetIO();
  return app.path.empty() || app.isPhotoProtected() || !app.renderer.canEditParameters() ||
         app.exportDialog.open || app.previewInteraction.drag != PreviewDrag::None ||
         app.previewInteraction.editPending || app.graphCanvas.dragging || app.graphCanvas.positionEditPending ||
         io.AppFocusLost || io.WantTextInput || io.KeyMods != 0 || ImGui::IsAnyItemActive() ||
         ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup) || ImGui::IsAnyMouseDown() ||
         ImGui::IsKeyDown(ImGuiKey_Space);
}

void adjustHeldParameter(App &app, PreviewHeldAdjustment &held, const ScalarCommandOperand &operand,
                         int arrowSteps, double wheelSteps) {
  const auto &address = operand.address;
  if (held.target && held.target->value.empty()) return;
  const auto *node = held.target ? findNode(app, *held.target) : nullptr;
  if (!held.target) {
    if (arrowSteps == 0 && wheelSteps == 0) return;
    node = resolveProcessor(app, address.processor);
    held.target = node ? node->id : NodeId{};
    app.shortcutFailure.clear();
  }

  const auto param = node ? scalarParameter(*node, address.parameterId) : std::nullopt;
  const double step = operand.value ? *operand.value : param ? automaticStep(*param) : 0;
  const bool integer = param && param->type == ParameterType::Integer;
  std::string error;
  if (!node) error = "Processor unavailable: " + address.processor.identifier;
  else if (!param) error = "Editable scalar parameter unavailable: " + address.parameterId;
  else if (step <= 0) error = "No automatic step for " + param->id + ". Enter an explicit step.";
  else if (integer && std::trunc(step) != step) error = "An integer parameter requires a whole step.";
  if (!error.empty()) {
    held.target = NodeId{};
    // Keep owning the controls until release, even though this target is gone.
    if (app.parameterGesture && app.parameterGesture->heldAdjustment) finishParameterGesture(app, true);
    app.shortcutFailure = std::move(error);
    return;
  }

  if (integer) {
    const double accumulated = held.integerWheelRemainder + wheelSteps;
    wheelSteps = std::trunc(accumulated);
    held.integerWheelRemainder = accumulated - wheelSteps;
  }
  const double steps = arrowSteps + wheelSteps;
  if (steps == 0) return;
  const auto value = scalarResult(*param, ScalarEdit::Adjust, steps * step, error);
  if (!value) {
    app.shortcutFailure = std::move(error);
    return;
  }
  if (*value == param->value) return;
  if (!setNodeParameterValue(app, node->id, param->id, *value)) {
    app.shortcutFailure = "Could not adjust " + param->id;
    return;
  }
  if (!app.parameterGesture) {
    auto &gesture = app.parameterGesture.emplace();
    gesture.photoPath = app.path;
    gesture.node = node->id;
    gesture.parameter = param->id;
    gesture.heldAdjustment = true;
  }
}

// Returns whether the held binding owns the Preview's vertical wheel.
bool updateHeldPreviewAdjustment(App &app, bool canvasHovered) {
  if ((!canvasHovered && !ImGui::IsWindowFocused()) || heldPreviewInputBlocked(app)) {
    finishHeldPreviewAdjustment(app);
    return false;
  }
  if (app.previewHeldAdjustment && !ImGui::IsKeyDown(app.previewHeldAdjustment->key))
    finishHeldPreviewAdjustment(app);
  const auto &bindings = effectiveKeyboardBindings(app);
  const auto binding = std::find_if(bindings.begin(), bindings.end(), [&](const KeyboardBinding &entry) {
    if (!entry.invokes(ApplicationCommand::AdjustWhileHeld)) return false;
    if (app.previewHeldAdjustment) return entry.trigger == app.previewHeldAdjustment->key;
    return !app.parameterGesture &&
           ImGui::IsKeyChordPressed(entry.trigger, ImGuiInputFlags_None, ImGuiKeyOwner_NoOwner);
  });
  if (binding == bindings.end()) {
    finishHeldPreviewAdjustment(app);
    return false;
  }
  if (!app.previewHeldAdjustment)
    app.previewHeldAdjustment = PreviewHeldAdjustment{static_cast<ImGuiKey>(binding->trigger), std::nullopt};

  // Preview runs before ordinary shortcut dispatch. Keyboard navigation is
  // disabled, and earlier panels leave these arrows free in the idle context.
  const auto arrowStep = [](ImGuiKey key) {
    const bool pressed = ImGui::IsKeyPressed(key, ImGuiInputFlags_Repeat, ImGuiKeyOwner_NoOwner);
    // A frame-only lock leaves the key free for controls on the next frame.
    if (pressed) ImGui::SetKeyOwner(key, ImGuiKeyOwner_NoOwner, ImGuiInputFlags_LockThisFrame);
    return int(pressed);
  };
  const int up = arrowStep(ImGuiKey_UpArrow);
  const int down = arrowStep(ImGuiKey_DownArrow);
  const double wheel = canvasHovered ? ImGui::GetIO().MouseWheel : 0;
  adjustHeldParameter(app, *app.previewHeldAdjustment,
                      std::get<ScalarCommandOperand>(binding->commands.front().operand),
                      up - down, wheel);
  return canvasHovered;
}
}  // namespace

void finishHeldPreviewAdjustment(App &app) {
  app.previewHeldAdjustment.reset();
  if (app.parameterGesture && app.parameterGesture->heldAdjustment) finishParameterGesture(app, true);
}
}  // namespace ui

void drawPreviewPanel(App &app, void (*upload)(App &)) {
  const char *items[kPreviewResCount];
  for (int i = 0; i < kPreviewResCount; ++i) items[i] = kPreviewRes[i].label;
  ImGui::SetNextItemWidth(std::min(140.0f, ImGui::GetContentRegionAvail().x));
  if (ImGui::Combo("##previewResolution", &app.previewRes, items, kPreviewResCount)) rebuildPreview(app);
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Preview resolution");
  if (ImGui::GetContentRegionAvail().x >= 140.0f + 115.0f + ImGui::GetStyle().ItemSpacing.x)
    ImGui::SameLine();
  const auto mode = previewMode(app);
  const char *modeLabel = mode == PreviewMode::Photo ? "Photo" :
                          mode == PreviewMode::MaskOnly ? "Mask Only" : "Mask Overlay";
  ImGui::SetNextItemWidth(std::min(115.0f, ImGui::GetContentRegionAvail().x));
  if (ImGui::BeginCombo("##previewMode", modeLabel)) {
    if (ImGui::Selectable("Photo", mode == PreviewMode::Photo) && mode != PreviewMode::Photo)
      setPreviewMode(app, PreviewMode::Photo);
    ImGui::BeginDisabled(!selectedMaskOutput(app));
    if (ImGui::Selectable("Mask Only", mode == PreviewMode::MaskOnly) && mode != PreviewMode::MaskOnly)
      setPreviewMode(app, PreviewMode::MaskOnly);
    if (ImGui::Selectable("Mask Overlay", mode == PreviewMode::MaskOverlay) && mode != PreviewMode::MaskOverlay)
      setPreviewMode(app, PreviewMode::MaskOverlay);
    ImGui::EndDisabled();
    ImGui::EndCombo();
  }
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Preview mode");
  // Selection in Nodes can invalidate pixels after the main-loop upload.
  if (upload) upload(app);
  else pumpDisplayUpload(app);
  const ImVec2 canvasPos = ImGui::GetCursorScreenPos();
  const ImVec2 canvasSize = ImGui::GetContentRegionAvail();
  if (canvasSize.x < 1.0f || canvasSize.y < 1.0f) {
    ui::finishHeldPreviewAdjustment(app);
    return;
  }

  ImDrawList *dl = ImGui::GetWindowDrawList();
  const ImVec2 canvasMax(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y);
  dl->AddRectFilled(canvasPos, canvasMax, ImGui::GetColorU32(ImGuiCol_WindowBg));

  const bool textInputWasActive = ImGui::GetIO().WantTextInput ||
      ImGui::GetInputTextState(ImGui::GetActiveID()) ||
      (ImGui::GetActiveID() && ImGui::GetActiveID() != ImGui::GetID("##previewCanvas"));
  ImGui::InvisibleButton(
      "##previewCanvas", canvasSize,
      ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
  const bool canvasHovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
  const bool active = ImGui::IsItemActive();

  if (!app.tex) {
    ui::finishHeldPreviewAdjustment(app);
    dl->AddText(ImVec2(canvasPos.x + 8.0f, canvasPos.y + 8.0f), ImGui::GetColorU32(ImGuiCol_Text),
                app.full.px.empty() ? "Open an image to preview." : "No preview image.");
    return;
  }

  const bool heldOwnsWheel = ui::updateHeldPreviewAdjustment(app, canvasHovered);
  const float fit = std::min(canvasSize.x / (float)app.texW, canvasSize.y / (float)app.texH);
  const float dispW = app.texW * fit * app.previewZoom;
  const float dispH = app.texH * fit * app.previewZoom;
  const ImVec2 p0(canvasPos.x + canvasSize.x * 0.5f + app.previewPanX - dispW * 0.5f,
                  canvasPos.y + canvasSize.y * 0.5f + app.previewPanY - dispH * 0.5f);
  const ImVec2 p1(p0.x + dispW, p0.y + dispH);
  dl->PushClipRect(canvasPos, canvasMax, true);
  dl->AddImage((ImTextureID)(intptr_t)app.tex, p0, p1);
  const bool gestureOwnsDoubleClick = ui::drawPreviewInteraction(
      app, {p0, {dispW, dispH}}, canvasHovered, active, textInputWasActive);
  dl->PopClipRect();
  char zoomLbl[32];
  std::snprintf(zoomLbl, sizeof zoomLbl, "%.0f%%", app.previewZoom * 100.0f);
  dl->AddText(ImVec2(canvasPos.x + 8.0f, canvasPos.y + canvasSize.y - ImGui::GetTextLineHeight() - 8.0f),
              ImGui::GetColorU32(ImGuiCol_TextDisabled), zoomLbl);

#if defined(__APPLE__)
  const float pinch = MacPinch_Consume();
#else
  const float pinch = 0.0f;
#endif
  if (canvasHovered) {
    float zoomFactor = 1.0f;
    const float wheel = heldOwnsWheel ? 0.0f : ImGui::GetIO().MouseWheel;
    if (wheel != 0.0f) zoomFactor *= std::pow(1.1f, wheel);
#if defined(__APPLE__)
    if (pinch != 0.0f) zoomFactor *= std::max(0.01f, 1.0f + pinch);
#endif
    if (zoomFactor != 1.0f) {
      const float oldZoom = app.previewZoom;
      app.previewZoom = std::clamp(app.previewZoom * zoomFactor, 0.05f, 64.0f);
      const ImVec2 mouse = ImGui::GetIO().MousePos;
      const float ox = canvasPos.x + canvasSize.x * 0.5f + app.previewPanX;
      const float oy = canvasPos.y + canvasSize.y * 0.5f + app.previewPanY;
      const float oldW = app.texW * fit * oldZoom, oldH = app.texH * fit * oldZoom;
      const float newW = app.texW * fit * app.previewZoom, newH = app.texH * fit * app.previewZoom;
      const float u = oldW > 0.0f ? (mouse.x - (ox - oldW * 0.5f)) / oldW : 0.5f;
      const float v = oldH > 0.0f ? (mouse.y - (oy - oldH * 0.5f)) / oldH : 0.5f;
      app.previewPanX += (u - 0.5f) * (oldW - newW);
      app.previewPanY += (v - 0.5f) * (oldH - newH);
    }
    if (!gestureOwnsDoubleClick && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
      app.previewZoom = 1.0f;
      app.previewPanX = 0.0f;
      app.previewPanY = 0.0f;
    }
  }
}
