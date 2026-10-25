#include "ui/ParamWidgets.h"
#include "AppState.h"
#include "NodeGraph.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

static void drawPencilIcon(ImVec2 a, ImVec2 b) {
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);
  const float pad = (b.x - a.x) * 0.22f;
  const ImVec2 p0(a.x + pad, b.y - pad);
  const ImVec2 p1(b.x - pad, a.y + pad);
  const ImVec2 dir(p1.x - p0.x, p1.y - p0.y);
  const float len = std::sqrt(dir.x * dir.x + dir.y * dir.y);
  if (len < 1.0f) return;
  const ImVec2 n(-dir.y / len * 1.6f, dir.x / len * 1.6f);
  dl->AddLine(ImVec2(p0.x + n.x, p0.y + n.y), ImVec2(p1.x + n.x, p1.y + n.y), col, 1.2f);
  dl->AddLine(ImVec2(p0.x - n.x, p0.y - n.y), ImVec2(p1.x - n.x, p1.y - n.y), col, 1.2f);
  dl->AddLine(p0, ImVec2(p0.x + dir.x * 0.2f, p0.y + dir.y * 0.2f), col, 1.2f);
  dl->AddLine(ImVec2(p1.x + n.x, p1.y + n.y), ImVec2(p1.x - n.x, p1.y - n.y), col, 1.2f);
}

static void drawResetIcon(ImVec2 a, ImVec2 b) {
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);
  const ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
  const float r = (b.x - a.x) * 0.28f;
  constexpr float kPi = 3.14159265f;
  dl->PathClear();
  dl->PathArcTo(c, r, kPi * 0.15f, kPi * 1.75f, 16);
  dl->PathStroke(col, 0, 1.4f);
  const float ang = kPi * 0.15f;
  const ImVec2 tip(c.x + std::cos(ang) * r, c.y + std::sin(ang) * r);
  const ImVec2 t1(tip.x - 3.2f, tip.y - 1.2f);
  const ImVec2 t2(tip.x - 1.2f, tip.y + 3.2f);
  dl->AddTriangleFilled(tip, t1, t2, col);
}

static void drawMinusIcon(ImVec2 a, ImVec2 b) {
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);
  const float cy = (a.y + b.y) * 0.5f;
  const float pad = (b.x - a.x) * 0.28f;
  dl->AddLine(ImVec2(a.x + pad, cy), ImVec2(b.x - pad, cy), col, 1.4f);
}

static void drawPlusIcon(ImVec2 a, ImVec2 b) {
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);
  const float cx = (a.x + b.x) * 0.5f;
  const float cy = (a.y + b.y) * 0.5f;
  const float pad = (b.x - a.x) * 0.28f;
  dl->AddLine(ImVec2(a.x + pad, cy), ImVec2(b.x - pad, cy), col, 1.4f);
  dl->AddLine(ImVec2(cx, a.y + pad), ImVec2(cx, b.y - pad), col, 1.4f);
}

static bool paramResetButton() {
  const float h = ImGui::GetFrameHeight();
  const bool clicked = ImGui::Button("##reset", ImVec2(h, h));
  drawResetIcon(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("Reset to default");
  return clicked;
}

static bool paramStepButton(bool plus) {
  const float h = ImGui::GetFrameHeight();
  const bool clicked = ImGui::Button(plus ? "##stepup" : "##stepdown", ImVec2(h, h));
  if (plus) drawPlusIcon(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
  else drawMinusIcon(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip(plus ? "Increase" : "Decrease");
  return clicked;
}

void finishParameterGesture(App &app, bool force) {
  if (!app.parameterGesture) return;
  if (app.parameterGesture->photoPath != app.path || app.isPhotoProtected()) {
    app.parameterGesture.reset();
    return;
  }
  const auto &context = *ImGui::GetCurrentContext();
  if (!force && !ImGui::GetIO().AppFocusLost && context.ActiveId == app.parameterGesture->item &&
      context.ActiveIdIsAlive == app.parameterGesture->item) return;
  // Export can start while a widget is losing focus. Complete once it releases
  // execution, without repeatedly attempting capture or changing export status.
  if (!app.renderer.canCaptureSnapshot()) return;
  auto gesture = std::move(*app.parameterGesture);
  app.parameterGesture.reset();
  if (gesture.pendingValue) {
    Node *node = findNode(app, gesture.node);
    if (node && node->processor) {
      const auto params = node->processor->parameters();
      for (const auto &param : params)
        if (param.id == gesture.parameter && param.enabled && !param.readOnly && param.value != *gesture.pendingValue)
          setNodeParameterValue(app, gesture.node, gesture.parameter, *gesture.pendingValue);
    }
  }
  if (app.history.hasUnrecordedEdit()) app.history.recordCompletedEdit(app);
}

static void observeParameterItem(App &app, const Node &node, const ProcessorParameter &param, bool enabled) {
  const auto item = ImGui::GetItemID();
  if (enabled && ImGui::IsItemActive() && (!app.parameterGesture || app.parameterGesture->item != item)) {
    finishParameterGesture(app, true);
    app.parameterGesture = ui::ParameterGesture{item, app.path, node.id, param.id, std::nullopt};
  }
  if (app.parameterGesture && app.parameterGesture->item == item && (!enabled || ImGui::IsItemDeactivated()))
    finishParameterGesture(app, true);
}

// Numeric text retains its existing deferred-write behaviour. The single active
// item owns the pending value, so disappearing controls can complete it too.
static bool numericInput(App &app, Node &node, const ProcessorParameter &param, const char *label,
                         double &value, bool asInt, double lo, double hi, int component,
                         ImGuiInputTextFlags flags) {
  const auto item = ImGui::GetID(label);
  double edit = value;
  if (app.parameterGesture && app.parameterGesture->item == item && app.parameterGesture->pendingValue) {
    const auto &pending = *app.parameterGesture->pendingValue;
    if (component >= 0) edit = std::get<std::vector<double>>(pending)[size_t(component)];
    else edit = asInt ? double(std::get<int>(pending)) : std::get<double>(pending);
  }
  bool edited;
  if (asInt) {
    int integer = int(std::lround(edit));
    edited = ImGui::InputInt(label, &integer, 0, 0, flags);
    edit = integer;
  } else edited = ImGui::InputDouble(label, &edit, 0, 0, "%.6g", flags);
  const bool enabled = param.enabled && app.renderer.canEditParameters();
  if (enabled && ImGui::IsItemActive() && (!app.parameterGesture || app.parameterGesture->item != item)) {
    finishParameterGesture(app, true);
    app.parameterGesture = ui::ParameterGesture{item, app.path, node.id, param.id, std::nullopt};
  }
  if (enabled && app.parameterGesture && app.parameterGesture->item == item &&
      (edited || app.parameterGesture->pendingValue)) {
    edit = std::clamp(edit, lo, hi);
    if (component >= 0) {
      auto values = std::get<std::vector<double>>(param.value);
      values[size_t(component)] = asInt ? std::round(edit) : edit;
      app.parameterGesture->pendingValue = std::move(values);
    } else if (asInt) app.parameterGesture->pendingValue = int(std::lround(edit));
    else app.parameterGesture->pendingValue = edit;
  }
  const bool completed = ImGui::IsItemDeactivated();
  observeParameterItem(app, node, param, enabled);
  if (completed) value = edit;
  return completed;
}

static bool paramEditButton(App &app, Node &node, const ProcessorParameter &param,
                            double &value, bool asInt, double lo, double hi, int component = -1) {
  const float h = ImGui::GetFrameHeight();
  if (ImGui::Button("##edit", ImVec2(h, h))) ImGui::OpenPopup("##type");
  drawPencilIcon(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("Type value");
  if (!ImGui::BeginPopup("##type")) return false;
  if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
  const bool completed = numericInput(app, node, param, "##v", value, asInt, lo, hi, component,
                                      ImGuiInputTextFlags_AutoSelectAll);
  if (completed) ImGui::CloseCurrentPopup();
  ImGui::EndPopup();
  return completed;
}

static void drawParam(App &app, Node &node, ProcessorParameter param) {
  if (!node.processor) return;

  const std::string label = param.label.empty() ? param.id : param.label;
  const std::string idLabel = label + "##" + param.id;
  ImGui::PushID(param.id.c_str());
  const bool editable = app.renderer.canEditParameters();
  ImGui::BeginDisabled(!param.enabled || !editable);

  // Disabled ImGui items can still report a pending edit on deactivation.
  // Guard every processor mutation independently of the widget return value.
  // Export cannot start mid-call on the same UI/control thread; it may only
  // finish, in which case these controls become editable on their next draw.
  auto setValue = [&](const ParameterValue &value) {
    if (app.parameterGesture && app.parameterGesture->item != ImGui::GetItemID() && ImGui::IsItemActive())
      finishParameterGesture(app, true);
    return editable && setNodeParameterValue(app, node.id, param.id, value);
  };
  auto complete = [&] {
    if (app.history.hasUnrecordedEdit()) app.history.recordCompletedEdit(app);
  };
  auto reset = [&] {
    finishParameterGesture(app, true);
    const bool changed = editable && resetNodeParameter(app, node.id, param.id);
    if (changed) complete();
    return changed;
  };
  auto activate = [&] {
    finishParameterGesture(app, true);
    if (editable && activateNodeParameter(app, node.id, param.id)) complete();
  };

  const float btn = ImGui::GetFrameHeight();
  const float gap = ImGui::GetStyle().ItemInnerSpacing.x;
  const float labelW = ImGui::CalcTextSize(label.c_str()).x;
  auto valueWidth = [&] { return std::max(40.0f, ImGui::GetContentRegionAvail().x - gap - labelW); };

  if (param.type == ParameterType::Double || param.type == ParameterType::Integer) {
    const bool asInt = param.type == ParameterType::Integer;
    double value = 0.0;
    if (asInt) {
      if (const int *v = std::get_if<int>(&param.value)) value = *v;
    } else {
      if (const double *v = std::get_if<double>(&param.value)) value = *v;
    }

    const double lo = param.displayMin;
    const double hi = param.displayMax;
    const double hardLo = param.min;
    const double hardHi = param.max;
    const double step = asInt ? 1.0
                              : (param.step > 0.0 ? param.step : std::max((hi - lo) / 100.0, 1e-6));

    auto commitNumeric = [&](double v) {
      if (asInt) {
        const int iv = (int)std::lround(v);
        if (setValue(iv)) {
          param.value = iv;
        }
      } else if (setValue(v)) {
        param.value = v;
      }
    };

    if (paramResetButton() && reset()) {
      param.value = param.defaultValue;
      if (asInt) {
        if (const int *v = std::get_if<int>(&param.value)) value = *v;
      } else if (const double *v = std::get_if<double>(&param.value)) {
        value = *v;
      }
    }

    if (!param.hasRange) {
      ImGui::SameLine(0, gap);
      ImGui::SetNextItemWidth(valueWidth());
      const double low = asInt ? double(std::numeric_limits<int>::lowest()) : std::numeric_limits<double>::lowest();
      const double high = asInt ? double(std::numeric_limits<int>::max()) : std::numeric_limits<double>::max();
      numericInput(app, node, param, idLabel.c_str(), value, asInt, low, high, -1, 0);
      ImGui::EndDisabled();
      ImGui::PopID();
      return;
    }

    ImGui::SameLine(0, gap);
    double typed = value;
    if (paramEditButton(app, node, param, typed, asInt, hardLo, hardHi)) {
      value = typed;
    }

    ImGui::SameLine(0, gap);
    if (paramStepButton(false)) {
      finishParameterGesture(app, true);
      value = std::clamp(value - step, lo, hi);
      commitNumeric(value);
      complete();
    }
    ImGui::SameLine(0, gap);
    if (paramStepButton(true)) {
      finishParameterGesture(app, true);
      value = std::clamp(value + step, lo, hi);
      commitNumeric(value);
      complete();
    }

    ImGui::SameLine(0, gap);
    ImGui::SetNextItemWidth(valueWidth());
    float fv = (float)value;
    char sliderFormat[16] = "%.3f";
    if (asInt) {
      std::snprintf(sliderFormat, sizeof sliderFormat, "%%.0f");
    } else if (param.step > 0.0) {
      int decimals = 0;
      double scaled = param.step;
      while (decimals < 6 && std::fabs(scaled - std::round(scaled)) > 1e-9) {
        scaled *= 10.0;
        ++decimals;
      }
      std::snprintf(sliderFormat, sizeof sliderFormat, "%%.%df", decimals);
    }
    if (ImGui::SliderFloat(idLabel.c_str(), &fv, (float)lo, (float)hi, sliderFormat)) commitNumeric(fv);
    observeParameterItem(app, node, param, param.enabled && editable);

  } else if (param.type == ParameterType::Boolean) {
    bool value = false;
    if (const bool *v = std::get_if<bool>(&param.value)) value = *v;

    if (paramResetButton() && reset()) {
      if (const bool *v = std::get_if<bool>(&param.defaultValue)) value = *v;
    }

    ImGui::SameLine(0, gap);
    if (ImGui::Checkbox(idLabel.c_str(), &value)) {
      finishParameterGesture(app, true);
      if (setValue(value)) complete();
    }

  } else if (param.type == ParameterType::Choice) {
    int currentValue = 0;
    if (const int *v = std::get_if<int>(&param.value)) currentValue = *v;

    if (paramResetButton() && reset()) {
      if (const int *v = std::get_if<int>(&param.defaultValue)) currentValue = *v;
    }

    const bool explicitValues = param.choiceValues.size() == param.choices.size();
    int currentIndex = currentValue;
    if (explicitValues) {
      currentIndex = 0;
      for (size_t i = 0; i < param.choiceValues.size(); ++i) {
        if (param.choiceValues[i] == currentValue) {
          currentIndex = (int)i;
          break;
        }
      }
    }

    ImGui::SameLine(0, gap);
    std::vector<const char *> items;
    items.reserve(param.choices.size());
    for (const std::string &choice : param.choices) items.push_back(choice.c_str());
    ImGui::SetNextItemWidth(valueWidth());
    if (!items.empty() && ImGui::Combo(idLabel.c_str(), &currentIndex, items.data(), (int)items.size())) {
      const int value = explicitValues ? param.choiceValues[(size_t)currentIndex] : currentIndex;
      finishParameterGesture(app, true);
      if (setValue(value)) complete();
    }

  } else if (param.type == ParameterType::PushButton) {
    if (ImGui::Button(idLabel.c_str())) activate();

  } else if (param.type == ParameterType::String) {
    std::string value;
    if (const std::string *v = std::get_if<std::string>(&param.value)) value = *v;

    if (param.readOnly) {
      ImGui::Text("%s: %s", label.c_str(), value.c_str());
    } else {
      if (paramResetButton() && reset()) {
        if (const std::string *v = std::get_if<std::string>(&param.defaultValue)) value = *v;
      }

      ImGui::SameLine(0, gap);
      char buf[512];
      std::snprintf(buf, sizeof buf, "%s", value.c_str());
      ImGui::SetNextItemWidth(valueWidth());
      if (ImGui::InputText(idLabel.c_str(), buf, sizeof buf)) setValue(std::string(buf));
      observeParameterItem(app, node, param, param.enabled && editable);
    }

  } else if (param.type == ParameterType::Vector) {
    std::vector<double> values;
    if (const auto *v = std::get_if<std::vector<double>>(&param.value)) values = *v;

    const float rowW = ImGui::CalcItemWidth();
    if (paramResetButton() && reset()) {
      if (const auto *v = std::get_if<std::vector<double>>(&param.defaultValue)) values = *v;
    }

    ImGui::SameLine(0, gap);
    ImGui::TextUnformatted(label.c_str());
    ImGui::Indent();
    for (size_t i = 0; i < values.size(); ++i) {
      ImGui::PushID((int)i);
      double typed = values[i];
      if (paramEditButton(app, node, param, typed, param.vectorIsInteger, -1e7, 1e7, int(i))) {
        values[i] = param.vectorIsInteger ? std::round(typed) : typed;
      }
      ImGui::SameLine(0, gap);
      ImGui::SetNextItemWidth(std::max(40.0f, rowW - btn - gap));
      float fv = (float)values[i];
      if (ImGui::DragFloat("##v", &fv, param.vectorIsInteger ? 1.0f : 0.01f)) {
        values[i] = param.vectorIsInteger ? std::round(fv) : fv;
        setValue(values);
      }
      observeParameterItem(app, node, param, param.enabled && editable);
      ImGui::PopID();
    }
    ImGui::Unindent();
  }

  ImGui::EndDisabled();
  ImGui::PopID();
}

static bool icontains(const std::string &haystack, const std::string &needle) {
  if (needle.empty()) return true;
  auto lower = [](unsigned char c) { return (char)std::tolower(c); };
  auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
                        [&](char a, char b) { return lower((unsigned char)a) == lower((unsigned char)b); });
  return it != haystack.end();
}

static bool paramMatches(const ProcessorParameter &param, const std::string &query) {
  return icontains(param.label, query) || icontains(param.id, query) || icontains(param.hint, query);
}

static const ProcessorParameter *findParam(const std::vector<ProcessorParameter> &params, const std::string &id) {
  for (const ProcessorParameter &param : params)
    if (param.id == id) return &param;
  return nullptr;
}

static bool ancestorsOpen(Node &node, const std::vector<ProcessorParameter> &params, const std::string &group) {
  if (group.empty()) return true;
  const ProcessorParameter *param = findParam(params, group);
  return param && node.groupOpen[group] && ancestorsOpen(node, params, param->parent);
}

static bool subtreeMatches(const std::vector<ProcessorParameter> &params, const std::string &parent,
                           const std::string &query) {
  for (const ProcessorParameter &param : params) {
    if (param.parent != parent || param.type == ParameterType::Page || param.secret) continue;
    if (param.type == ParameterType::Group) {
      if (paramMatches(param, query) || subtreeMatches(params, param.id, query)) return true;
    } else if (paramMatches(param, query)) {
      return true;
    }
  }
  return false;
}

static void drawParamsImpl(App &app, Node &node, const std::vector<ProcessorParameter> &params,
                           const std::string &parent) {
  const std::string filter = app.paramFilter;
  const bool filtering = !filter.empty();

  for (const ProcessorParameter &param : params) {
    if (param.parent != parent || param.type == ParameterType::Page || param.secret) continue;

    if (param.type == ParameterType::Group) {
      if (filtering && !subtreeMatches(params, param.id, filter) && !paramMatches(param, filter)) continue;
      if (!filtering && !ancestorsOpen(node, params, parent) && !parent.empty()) continue;

      const std::string groupLabel = (param.label.empty() ? param.id : param.label) + "##" + param.id;
      if (filtering) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
      else ImGui::SetNextItemOpen(node.groupOpen[param.id], ImGuiCond_Once);

      if (ImGui::CollapsingHeader(groupLabel.c_str())) {
        if (!filtering) node.groupOpen[param.id] = true;
        ImGui::Indent();
        drawParamsImpl(app, node, params, param.id);
        ImGui::Unindent();
      } else if (!filtering) {
        node.groupOpen[param.id] = false;
      }
      continue;
    }

    if (param.type == ParameterType::Custom || param.type == ParameterType::Unsupported) continue;

    if (filtering) {
      if (!paramMatches(param, filter)) continue;
    } else if (!ancestorsOpen(node, params, parent)) {
      continue;
    }

    drawParam(app, node, param);
  }
}

void drawParams(App &app, Node &node, const std::string &parent) {
  if (!node.processor) return;
  const auto params = node.processor->parameters();
  drawParamsImpl(app, node, params, parent);
}
