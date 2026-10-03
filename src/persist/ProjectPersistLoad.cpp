#include "persist/ProjectPersist.h"
#include "persist/ProjectPersistPriv.h"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <utility>
#include <vector>

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

static int hexValue(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return 10 + ch - 'a';
  if (ch >= 'A' && ch <= 'F') return 10 + ch - 'A';
  return -1;
}

static bool parseHex4(JsonCursor &c, unsigned &value) {
  value = 0;
  for (int i = 0; i < 4; ++i) {
    if (c.p >= c.end) return false;
    const int h = hexValue(*c.p++);
    if (h < 0) return false;
    value = (value << 4) | (unsigned)h;
  }
  return true;
}

static bool appendUtf8(std::string &out, unsigned cp) {
  if (cp <= 0x7F) {
    out += (char)cp;
  } else if (cp <= 0x7FF) {
    out += (char)(0xC0 | (cp >> 6));
    out += (char)(0x80 | (cp & 0x3F));
  } else if (cp <= 0xFFFF) {
    if (cp >= 0xD800 && cp <= 0xDFFF) return false;
    out += (char)(0xE0 | (cp >> 12));
    out += (char)(0x80 | ((cp >> 6) & 0x3F));
    out += (char)(0x80 | (cp & 0x3F));
  } else if (cp <= 0x10FFFF) {
    out += (char)(0xF0 | (cp >> 18));
    out += (char)(0x80 | ((cp >> 12) & 0x3F));
    out += (char)(0x80 | ((cp >> 6) & 0x3F));
    out += (char)(0x80 | (cp & 0x3F));
  } else {
    return false;
  }
  return true;
}

static bool parseString(JsonCursor &c, std::string &out) {
  skipWs(c);
  if (c.p >= c.end || *c.p != '"') return false;
  ++c.p;
  out.clear();

  while (c.p < c.end) {
    const unsigned char ch = (unsigned char)*c.p++;
    if (ch == '"') return true;
    // Raw control characters are accepted on read: string parameters were
    // written with only '"' and '\\' escaped before Sidecar V2, so legacy
    // multi-line values contain literal newlines/tabs. Writes escape them.

    if (ch != '\\') {
      out += (char)ch;
      continue;
    }

    if (c.p >= c.end) return false;
    const char e = *c.p++;
    switch (e) {
      case '"': out += '"'; break;
      case '\\': out += '\\'; break;
      case '/': out += '/'; break;
      case 'b': out += '\b'; break;
      case 'f': out += '\f'; break;
      case 'n': out += '\n'; break;
      case 'r': out += '\r'; break;
      case 't': out += '\t'; break;
      case 'u': {
        unsigned first = 0;
        if (!parseHex4(c, first)) return false;

        unsigned cp = first;
        if (first >= 0xD800 && first <= 0xDBFF) {
          if (c.end - c.p < 2 || c.p[0] != '\\' || c.p[1] != 'u') return false;
          c.p += 2;
          unsigned second = 0;
          if (!parseHex4(c, second) || second < 0xDC00 || second > 0xDFFF) return false;
          cp = 0x10000 + ((first - 0xD800) << 10) + (second - 0xDC00);
        } else if (first >= 0xDC00 && first <= 0xDFFF) {
          return false;
        }

        if (!appendUtf8(out, cp)) return false;
        break;
      }
      default:
        return false;
    }
  }

  return false;
}

bool parseJsonStringValue(const std::string &raw, std::string &out) {
  JsonCursor c{raw.c_str(), raw.c_str() + raw.size()};
  if (!parseString(c, out)) return false;
  skipWs(c);
  return c.p == c.end;
}

static std::string trimRaw(const char *begin, const char *end) {
  while (begin < end && std::isspace((unsigned char)*begin)) ++begin;
  while (end > begin && std::isspace((unsigned char)*(end - 1))) --end;
  return std::string(begin, end);
}

// Captures one JSON value without interpreting it. Parameter values use this
// so unknown future shapes can survive a load-save cycle unchanged.
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
      } else if (ch == '}' || ch == ']') {
        return false;
      }
    }

    if (!closes.empty() || inString) return false;
    raw = trimRaw(start, c.p);
    return true;
  }

  // Scalars (numbers, true/false/null) end at whitespace too, so a missing
  // comma is reported by the caller instead of being absorbed into the value.
  while (c.p < c.end && *c.p != ',' && *c.p != '}' && *c.p != ']' && !std::isspace((unsigned char)*c.p)) ++c.p;
  raw = trimRaw(start, c.p);
  return !raw.empty();
}

// Find a direct member of a JSON object. This deliberately walks the object
// instead of text-searching so nested keys cannot shadow top-level fields.
static bool extractValueField(const std::string &json, const char *wantedKey, std::string &raw) {
  JsonCursor c{json.c_str(), json.c_str() + json.size()};
  if (!match(c, '{')) return false;

  for (;;) {
    skipWs(c);
    if (c.p >= c.end || *c.p == '}') return false;

    std::string key;
    if (!parseString(c, key) || !match(c, ':')) return false;

    std::string value;
    if (!captureJsonValue(c, value)) return false;
    if (key == wantedKey) {
      raw = std::move(value);
      return true;
    }

    skipWs(c);
    if (c.p < c.end && *c.p == ',') {
      ++c.p;
      continue;
    }
    if (c.p < c.end && *c.p == '}') return false;
    return false;
  }
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
  std::string raw;
  return extractValueField(json, key, raw) && parseJsonStringValue(raw, out);
}

bool extractIntField(const std::string &json, const char *key, int &out) {
  std::string raw;
  if (!extractValueField(json, key, raw)) return false;
  char *end = nullptr;
  long value = std::strtol(raw.c_str(), &end, 10);
  if (end == raw.c_str()) return false;
  while (*end && std::isspace((unsigned char)*end)) ++end;
  if (*end) return false;
  out = (int)value;
  return true;
}

bool extractFloatField(const std::string &json, const char *key, float &out) {
  std::string raw;
  if (!extractValueField(json, key, raw)) return false;
  char *end = nullptr;
  double value = std::strtod(raw.c_str(), &end);
  if (end == raw.c_str()) return false;
  while (*end && std::isspace((unsigned char)*end)) ++end;
  if (*end) return false;
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
  extractIntField(guiObj, "outputIndex", g.legacyOutputIndex);
  extractStringField(guiObj, "outputColorSpace", g.outputColorSpace);
  extractStringField(guiObj, "outputGamma", g.outputGamma);
  extractStringField(guiObj, "rawDefaultColorSpace", g.rawDefaultColorSpace);
  extractStringField(guiObj, "rawDefaultGamma", g.rawDefaultGamma);
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

// Returns false on malformed input so the caller rejects the whole sidecar
// (and write-protects it) instead of silently restoring a partial node.
static bool parseParamsObject(const std::string &paramsObj, std::map<std::string, std::string> &params) {
  JsonCursor c{paramsObj.c_str(), paramsObj.c_str() + paramsObj.size()};
  if (!match(c, '{')) return false;

  for (;;) {
    skipWs(c);
    if (c.p < c.end && *c.p == '}') return true;

    std::string key;
    if (!parseString(c, key) || !match(c, ':')) return false;

    std::string raw;
    if (!captureJsonValue(c, raw)) return false;
    params[key] = std::move(raw);

    skipWs(c);
    if (c.p < c.end && *c.p == ',') {
      ++c.p;
      continue;
    }
    return c.p < c.end && *c.p == '}';
  }
}

static void parseGroupOpen(const std::string &obj, std::map<std::string, bool> &groupOpen) {
  JsonCursor c{obj.c_str(), obj.c_str() + obj.size()};
  if (!match(c, '{')) return;

  for (;;) {
    skipWs(c);
    if (c.p < c.end && *c.p == '}') return;

    std::string key;
    if (!parseString(c, key) || !match(c, ':')) return;

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
    if (extractValueField(nodeObj, "params", paramsObj) && !parseParamsObject(paramsObj, node.paramsJson))
      return false;
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

bool parseTransferPayload(const std::string &json, std::string &kind, PersistChain &chain) {
  std::string format;
  int version = 0;
  if (!extractStringField(json, "format", format) || format != "rawnode-transfer" ||
      !extractIntField(json, "version", version) || version != 1 ||
      !extractStringField(json, "kind", kind))
    return false;

  std::string graphObj;
  if (!extractObject(json, "graph", graphObj)) return false;
  return loadGraphV2FromJson(graphObj, chain);
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
  extractStringField(json, "exportedAt", out.exportedAt);

  // Never rewrite a future RawNode schema as V2. The caller can use the
  // parsed version to explain why loading was refused.
  if (out.format == "rawnode-sidecar" && out.version != 2) return false;

  std::string guiObj;
  if (extractObject(json, "gui", guiObj)) loadGuiFromJson(guiObj, out.gui);

  if (out.format == "rawnode-sidecar") {
    extractStringField(json, "source", out.sourcePath);
    std::string rawObj;
    if (extractObject(json, "raw", rawObj)) {
      extractStringField(rawObj, "workingSpace", out.legacyRawWorkingSpace);
      extractStringField(rawObj, "colorSpace", out.rawColorSpace);
      extractStringField(rawObj, "gamma", out.rawGamma);
    }
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
