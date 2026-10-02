#include "ui/ParamWidgets.h"

#include "NodeGraph.h"
#include "RenderPipeline.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
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

static bool paramEditButton(double &value, bool asInt, double lo, double hi) {
  const float h = ImGui::GetFrameHeight();
  if (ImGui::Button("##edit", ImVec2(h, h))) ImGui::OpenPopup("##type");
  drawPencilIcon(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("Type value");

  if (!ImGui::BeginPopup("##type")) return false;
  ImGui::SetKeyboardFocusHere();
  bool commit = false;
  if (asInt) {
    int iv = (int)std::lround(value);
    if (ImGui::InputInt("##v", &iv, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
      value = std::clamp((double)iv, lo, hi);
      commit = true;
    }
  } else if (ImGui::InputDouble("##v", &value, 0, 0, "%.6g",
                                ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
    value = std::clamp(value, lo, hi);
    commit = true;
  }
  if (commit) ImGui::CloseCurrentPopup();
  ImGui::EndPopup();
  return commit;
}

static void finishParameterChange(App &app, bool changed) {
  if (!changed) return;
  syncOutputTag(app);
  scheduleRender(app);
}

static void drawParam(App &app, Node &node, ProcessorParameter param) {
  if (!node.processor) return;

  const std::string label = param.label.empty() ? param.id : param.label;
  const std::string idLabel = label + "##" + param.id;
  ImGui::PushID(param.id.c_str());
  if (!param.enabled) ImGui::BeginDisabled();

  const float btn = ImGui::GetFrameHeight();
  const float gap = ImGui::GetStyle().ItemInnerSpacing.x;
  const float labelW = ImGui::CalcTextSize(label.c_str()).x;
  auto valueWidth = [&] { return std::max(40.0f, ImGui::GetContentRegionAvail().x - gap - labelW); };

  bool changed = false;

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
    const double step = asInt ? 1.0 : std::max((hi - lo) / 100.0, 1e-6);

    auto commitNumeric = [&](double v) {
      if (asInt) {
        const int iv = (int)std::lround(v);
        if (node.processor->setParameterValue(param.id, iv)) {
          param.value = iv;
          changed = true;
        }
      } else if (node.processor->setParameterValue(param.id, v)) {
        param.value = v;
        changed = true;
      }
    };

    if (paramResetButton() && node.processor->resetParameter(param.id)) {
      param.value = param.defaultValue;
      changed = true;
      if (asInt) {
        if (const int *v = std::get_if<int>(&param.value)) value = *v;
      } else if (const double *v = std::get_if<double>(&param.value)) {
        value = *v;
      }
    }

    ImGui::SameLine(0, gap);
    double typed = value;
    if (paramEditButton(typed, asInt, hardLo, hardHi)) {
      commitNumeric(typed);
      value = typed;
    }

    ImGui::SameLine(0, gap);
    if (paramStepButton(false)) {
      value = std::clamp(value - step, lo, hi);
      commitNumeric(value);
    }
    ImGui::SameLine(0, gap);
    if (paramStepButton(true)) {
      value = std::clamp(value + step, lo, hi);
      commitNumeric(value);
    }

    ImGui::SameLine(0, gap);
    ImGui::SetNextItemWidth(valueWidth());
    float fv = (float)value;
    if (ImGui::SliderFloat(idLabel.c_str(), &fv, (float)lo, (float)hi)) commitNumeric(fv);

  } else if (param.type == ParameterType::Boolean) {
    bool value = false;
    if (const bool *v = std::get_if<bool>(&param.value)) value = *v;

    if (paramResetButton() && node.processor->resetParameter(param.id)) {
      if (const bool *v = std::get_if<bool>(&param.defaultValue)) value = *v;
      changed = true;
    }

    ImGui::SameLine(0, gap);
    if (ImGui::Checkbox(idLabel.c_str(), &value) && node.processor->setParameterValue(param.id, value)) changed = true;

  } else if (param.type == ParameterType::Choice) {
    int current = 0;
    if (const int *v = std::get_if<int>(&param.value)) current = *v;

    if (paramResetButton() && node.processor->resetParameter(param.id)) {
      if (const int *v = std::get_if<int>(&param.defaultValue)) current = *v;
      changed = true;
    }

    ImGui::SameLine(0, gap);
    std::vector<const char *> items;
    items.reserve(param.choices.size());
    for (const std::string &choice : param.choices) items.push_back(choice.c_str());
    ImGui::SetNextItemWidth(valueWidth());
    if (!items.empty() && ImGui::Combo(idLabel.c_str(), &current, items.data(), (int)items.size()) &&
        node.processor->setParameterValue(param.id, current))
      changed = true;

  } else if (param.type == ParameterType::PushButton) {
    if (ImGui::Button(idLabel.c_str()) && node.processor->activateParameter(param.id)) changed = true;

  } else if (param.type == ParameterType::String) {
    std::string value;
    if (const std::string *v = std::get_if<std::string>(&param.value)) value = *v;

    if (param.readOnly) {
      ImGui::Text("%s: %s", label.c_str(), value.c_str());
    } else {
      if (paramResetButton() && node.processor->resetParameter(param.id)) {
        if (const std::string *v = std::get_if<std::string>(&param.defaultValue)) value = *v;
        changed = true;
      }

      ImGui::SameLine(0, gap);
      char buf[512];
      std::snprintf(buf, sizeof buf, "%s", value.c_str());
      ImGui::SetNextItemWidth(valueWidth());
      if (ImGui::InputText(idLabel.c_str(), buf, sizeof buf) &&
          node.processor->setParameterValue(param.id, std::string(buf)))
        changed = true;
    }

  } else if (param.type == ParameterType::Vector) {
    std::vector<double> values;
    if (const auto *v = std::get_if<std::vector<double>>(&param.value)) values = *v;

    const float rowW = ImGui::CalcItemWidth();
    if (paramResetButton() && node.processor->resetParameter(param.id)) {
      if (const auto *v = std::get_if<std::vector<double>>(&param.defaultValue)) values = *v;
      changed = true;
    }

    ImGui::SameLine(0, gap);
    ImGui::TextUnformatted(label.c_str());
    ImGui::Indent();
    for (size_t i = 0; i < values.size(); ++i) {
      ImGui::PushID((int)i);
      double typed = values[i];
      if (paramEditButton(typed, param.vectorIsInteger, -1e7, 1e7)) {
        values[i] = param.vectorIsInteger ? std::round(typed) : typed;
        if (node.processor->setParameterValue(param.id, values)) changed = true;
      }
      ImGui::SameLine(0, gap);
      ImGui::SetNextItemWidth(std::max(40.0f, rowW - btn - gap));
      float fv = (float)values[i];
      if (ImGui::DragFloat("##v", &fv, param.vectorIsInteger ? 1.0f : 0.01f)) {
        values[i] = param.vectorIsInteger ? std::round(fv) : fv;
        if (node.processor->setParameterValue(param.id, values)) changed = true;
      }
      ImGui::PopID();
    }
    ImGui::Unindent();
  }

  if (!param.enabled) ImGui::EndDisabled();
  ImGui::PopID();
  finishParameterChange(app, changed);
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
