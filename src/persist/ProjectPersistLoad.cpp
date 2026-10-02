#include "persist/ProjectPersist.h"
#include "persist/ProjectPersistPriv.h"

#include <cctype>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#include <utility>

struct JsonCursor {
  const char *p = nullptr;
  const char *end = nullptr;
};

static void skipWs(JsonCursor &c) {
  while (c.p < c.end && std::isspace((unsigned char)*c.p)) ++c.p;
}

static bool match(JsonCursor &c, char ch) {
  skipWs(c);
  if (c.p >= c.end || *c.p != ch) return false;
  ++c.p;
  return true;
}

static bool parseString(JsonCursor &c, std::string &out) {
  skipWs(c);
  if (c.p >= c.end || *c.p != '"') return false;
  ++c.p;
  out.clear();
  while (c.p < c.end) {
    char ch = *c.p++;
    if (ch == '"') return true;
    if (ch == '\\' && c.p < c.end) {
      char e = *c.p++;
      switch (e) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        default: out += e; break;
      }
    } else {
      out += ch;
    }
  }
  return false;
}

static std::string trimRaw(const char *begin, const char *end) {
  while (begin < end && std::isspace((unsigned char)*begin)) ++begin;
  while (end > begin && std::isspace((unsigned char)*(end - 1))) --end;
  return std::string(begin, end);
}

// Captures one JSON value without interpreting it. This is deliberately used
// for parameter values so unknown future parameter shapes can survive a
// load-save cycle unchanged.
static bool captureJsonValue(JsonCursor &c, std::string &raw) {
  skipWs(c);
  if (c.p >= c.end) return false;
  const char *start = c.p;

  if (*c.p == '"') {
    std::string ignored;
    if (!parseString(c, ignored)) return false;
    raw = trimRaw(start, c.p);
    return true;
  }

  if (*c.p == '{' || *c.p == '[') {
    std::vector<char> closes;
    closes.push_back(*c.p == '{' ? '}' : ']');
    ++c.p;
    bool inString = false;
    bool escape = false;

    while (c.p < c.end && !closes.empty()) {
      const char ch = *c.p++;
      if (inString) {
        if (escape) {
          escape = false;
        } else if (ch == '\\') {
          escape = true;
        } else if (ch == '"') {
          inString = false;
        }
        continue;
      }

      if (ch == '"') {
        inString = true;
      } else if (ch == '{') {
        closes.push_back('}');
      } else if (ch == '[') {
        closes.push_back(']');
      } else if (!closes.empty() && ch == closes.back()) {
        closes.pop_back();
      }
    }

    if (!closes.empty()) return false;
    raw = trimRaw(start, c.p);
    return true;
  }

  while (c.p < c.end && *c.p != ',' && *c.p != '}' && *c.p != ']') ++c.p;
  raw = trimRaw(start, c.p);
  return !raw.empty();
}

static bool extractValueField(const std::string &json, const char *key, std::string &raw) {
  const std::string needle = std::string("\\\"") + key + "\\":";
  size_t pos = json.find(needle);
  if (pos == std::string::npos) return false;
  JsonCursor c{json.c_str() + pos + needle.size(), json.c_str() + json.size()};
  return captureJsonValue(c, raw);
}

bool extractObject(const std::string &json, const char *key, std::string &objOut) {
  std::string raw;
  if (!extractValueField(json, key, raw) || raw.empty() || raw.front() != '{') return false;
  objOut = std::move(raw);
  return true;
}

bool extractArray(const std::string &json, const char *key, std::string &arrOut) {
  std::string raw;
  if (!extractValueField(json, key, raw) || raw.empty() || raw.front() != '[') return false;
  arrOut = std::move(raw);
  return true;
}

bool extractStringField(const std::string &json, const char *key, std::string &out) {
  const std::string needle = std::string("\\\"") + key + "\\":";
  size_t pos = json.find(needle);
  if (pos == std::string::npos) return false;
  JsonCursor c{json.c_str() + pos + needle.size(), json.c_str() + json.size()};
  return parseString(c, out);
}

bool extractIntField(const std::string &json, const char *key, int &out) {
  std::string raw;
  if (!extractValueField(json, key, raw)) return false;
  char *end = nullptr;
  long value = std::strtol(raw.c_str(), &end, 10);
  if (end == raw.c_str()) return false;
  out = (int)value;
  return true;
}

bool extractFloatField(const std::string &json, const char *key, float &out) {
  std::string raw;
  if (!extractValueField(json, key, raw)) return false;
  char *end = nullptr;
  double value = std::strtod(raw.c_str(), &end);
  if (end == raw.c_str()) return false;
  out = (float)value;
  return true;
}

bool extractBoolField(const std::string &json, const char *key, bool &out) {
  std::string raw;
  if (!extractValueField(json, key, raw)) return false;
  if (raw == "true") {
    out = true;
    return true;
  }
  if (raw == "false") {
    out = false;
    return true;
  }
  return false;
}

void loadGuiFromJson(const std::string &guiObj, PersistGui &g) {
  extractIntField(guiObj, "outputIndex", g.outputIndex);
  extractIntField(guiObj, "exportFormat", g.exportFormat);
  extractIntField(guiObj, "jpegQuality", g.jpegQuality);
  extractIntField(guiObj, "previewRes", g.previewRes);
  extractIntField(guiObj, "themeIndex", g.themeIndex);
  extractBoolField(guiObj, "showLeft", g.showLeft);
  extractBoolField(guiObj, "showRight", g.showRight);
  extractFloatField(guiObj, "leftW", g.leftW);
  extractFloatField(guiObj, "rightW", g.rightW);
  extractBoolField(guiObj, "showFilmstrip", g.showFilmstrip);
  extractFloatField(guiObj, "filmstripH", g.filmstripH);
}

static void parseParamsObject(const std::string &paramsObj, std::map<std::string, std::string> &params) {
  JsonCursor c{paramsObj.c_str(), paramsObj.c_str() + paramsObj.size()};
  if (!match(c, '{')) return;

  for (;;) {
    skipWs(c);
    if (c.p < c.end && *c.p == '}') return;

    std::string key;
    if (!parseString(c, key)) return;
    if (!match(c, ':')) return;

    std::string raw;
    if (!captureJsonValue(c, raw)) return;
    params[key] = std::move(raw);

    skipWs(c);
    if (c.p < c.end && *c.p == ',') {
      ++c.p;
      continue;
    }
    if (c.p < c.end && *c.p == '}') return;
    return;
  }
}

static void parseGroupOpen(const std::string &obj, std::map<std::string, bool> &groupOpen) {
  JsonCursor c{obj.c_str(), obj.c_str() + obj.size()};
  if (!match(c, '{')) return;

  for (;;) {
    skipWs(c);
    if (c.p < c.end && *c.p == '}') return;

    std::string key;
    if (!parseString(c, key)) return;
    if (!match(c, ':')) return;

    std::string raw;
    if (!captureJsonValue(c, raw)) return;
    if (raw == "true") groupOpen[key] = true;
    else if (raw == "false") groupOpen[key] = false;

    skipWs(c);
    if (c.p < c.end && *c.p == ',') {
      ++c.p;
      continue;
    }
    if (c.p < c.end && *c.p == '}') return;
    return;
  }
}

static bool parseNodeArray(const std::string &nodesArr, PersistChain &chain, bool v2) {
  JsonCursor c{nodesArr.c_str(), nodesArr.c_str() + nodesArr.size()};
  if (!match(c, '[')) return false;

  chain.nodes.clear();
  for (;;) {
    skipWs(c);
    if (c.p < c.end && *c.p == ']') return true;

    std::string nodeObj;
    if (!captureJsonValue(c, nodeObj) || nodeObj.empty() || nodeObj.front() != '{') return false;

    PersistNode node;
    if (v2) {
      extractStringField(nodeObj, "id", node.id);
      extractStringField(nodeObj, "backend", node.backend);
      extractStringField(nodeObj, "identifier", node.identifier);
      extractStringField(nodeObj, "label", node.label);
      extractBoolField(nodeObj, "enabled", node.enabled);

      std::string uiObj;
      std::string groupObj;
      if (extractObject(nodeObj, "ui", uiObj) && extractObject(uiObj, "groupOpen", groupObj))
        parseGroupOpen(groupObj, node.groupOpen);
    } else {
      node.backend = "ofx";
      extractStringField(nodeObj, "pluginIdentifier", node.identifier);
      extractStringField(nodeObj, "pluginLabel", node.label);
      extractBoolField(nodeObj, "enabled", node.enabled);

      std::string groupObj;
      if (extractObject(nodeObj, "groupOpen", groupObj)) parseGroupOpen(groupObj, node.groupOpen);
    }

    std::string paramsObj;
    if (extractObject(nodeObj, "params", paramsObj)) parseParamsObject(paramsObj, node.paramsJson);
    chain.nodes.push_back(std::move(node));

    skipWs(c);
    if (c.p < c.end && *c.p == ',') {
      ++c.p;
      continue;
    }
    if (c.p < c.end && *c.p == ']') return true;
    return false;
  }
}

bool loadChainFromJson(const std::string &chainObj, PersistChain &chain) {
  // V1 migration path.
  extractIntField(chainObj, "selectedNode", chain.selectedNode);
  std::string nodesArr;
  if (!extractArray(chainObj, "nodes", nodesArr)) return false;
  return parseNodeArray(nodesArr, chain, false);
}

static bool loadGraphV2FromJson(const std::string &graphObj, PersistChain &chain) {
  extractStringField(graphObj, "selectedNodeId", chain.selectedNodeId);
  std::string nodesArr;
  if (!extractArray(graphObj, "nodes", nodesArr)) return false;
  return parseNodeArray(nodesArr, chain, true);
}

bool readAllText(const std::string &path, std::string &out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  out.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  return true;
}

bool loadWorkspaceProject(const std::string &workspaceDir, PersistGui &gui, std::string &activeImageRel) {
  std::string json;
  if (!readAllText(workspaceProjectPath(workspaceDir), json)) return false;
  std::string guiObj;
  if (extractObject(json, "gui", guiObj)) loadGuiFromJson(guiObj, gui);
  extractStringField(json, "activeImage", activeImageRel);
  return true;
}

bool loadSidecarFile(const std::string &path, PersistSidecar &out) {
  std::string json;
  if (!readAllText(path, json)) return false;

  out = PersistSidecar{};
  extractStringField(json, "format", out.format);
  extractIntField(json, "version", out.version);
  extractStringField(json, "kind", out.kind);
  extractStringField(json, "inputColorSpace", out.inputColorSpace);
  extractStringField(json, "exportedAt", out.exportedAt);

  std::string guiObj;
  if (extractObject(json, "gui", guiObj)) loadGuiFromJson(guiObj, out.gui);

  const bool v2 = out.format == "rawnode-sidecar" || out.version >= 2;
  if (v2) {
    extractStringField(json, "source", out.sourcePath);
    extractStringField(json, "workingSpace", out.workingSpace);
    std::string graphObj;
    if (!extractObject(json, "graph", graphObj)) return false;
    return loadGraphV2FromJson(graphObj, out.chain);
  }

  // V1 ofxrawhost sidecars are accepted and upgraded to V2 when next saved.
  extractStringField(json, "sourcePath", out.sourcePath);
  std::string chainObj;
  if (!extractObject(json, "chain", chainObj)) return false;
  return loadChainFromJson(chainObj, out.chain);
}
