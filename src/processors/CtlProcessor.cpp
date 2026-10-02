#include "processors/CtlProcessor.h"

#include <CtlFunctionCall.h>
#include <CtlInterpreter.h>
#include <CtlMessage.h>
#include <CtlSimdInterpreter.h>
#include <CtlStdType.h>
#include <CtlType.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <locale>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace fs = std::filesystem;

namespace {

// RawNode's own entry-point checks; their messages are already specific.
struct ContractError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// CTL reports compile and import errors through a process-wide message
// function (stderr by default) and then throws a generic exception such as
// 'Failed to load CTL module "module.1a2b3c4d"'. One router is installed once,
// before any interpreter exists, so CTL's unsynchronised global pointer is never
// swapped while a render thread may be calling CTL print(). The router forwards
// everything to the previous function and also copies messages raised on a
// thread that is currently loading a script.
thread_local std::string *tCapturedMessages = nullptr;
Ctl::MessageOutputFunction gPreviousMessageOutput = nullptr;

void routeCtlMessage(const std::string &message) {
  if (tCapturedMessages) *tCapturedMessages += message;
  if (gPreviousMessageOutput) gPreviousMessageOutput(message);
}

class ScopedCtlMessageCapture {
 public:
  ScopedCtlMessageCapture() {
    static std::once_flag installed;
    std::call_once(installed, [] { gPreviousMessageOutput = Ctl::setMessageOutputFunction(routeCtlMessage); });
    previous_ = tCapturedMessages;
    tCapturedMessages = &text_;
  }
  ~ScopedCtlMessageCapture() { tCapturedMessages = previous_; }
  ScopedCtlMessageCapture(const ScopedCtlMessageCapture &) = delete;
  ScopedCtlMessageCapture &operator=(const ScopedCtlMessageCapture &) = delete;

  const std::string &text() const { return text_; }

 private:
  std::string text_;
  std::string *previous_ = nullptr;
};

std::string trimmed(const std::string &text) {
  const size_t begin = text.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) return {};
  const size_t end = text.find_last_not_of(" \t\r\n");
  return text.substr(begin, end - begin + 1);
}

bool isCaretLine(const std::string &line) {
  return line.find('^') != std::string::npos && line.find_first_not_of(" \t^") == std::string::npos;
}

// Reduces captured CTL output to its diagnostics ("Script.ctl:3: Syntax Error."),
// dropping source echoes, caret markers and "(@errorN)" codes, and showing paths
// in the script's directory by file name.
std::string summarizeCtlMessages(const std::string &text, const std::string &scriptDir) {
  std::vector<std::string> lines;
  std::istringstream in(text);
  for (std::string line; std::getline(in, line);) lines.push_back(line);

  std::vector<std::string> diagnostics;
  for (size_t i = 0; i < lines.size(); ++i) {
    // CTL echoes the offending source line followed by a "^" marker line.
    if (i + 1 < lines.size() && isCaretLine(lines[i + 1])) {
      ++i;
      continue;
    }
    std::string line = lines[i];
    const size_t code = line.rfind("(@error");
    if (code != std::string::npos) line.erase(code);
    line = trimmed(line);
    if (line.empty() || isCaretLine(line)) continue;

    if (!scriptDir.empty() && line.size() > scriptDir.size() && line.compare(0, scriptDir.size(), scriptDir) == 0 &&
        (line[scriptDir.size()] == '/' || line[scriptDir.size()] == '\\'))
      line.erase(0, scriptDir.size() + 1);
    if (std::find(diagnostics.begin(), diagnostics.end(), line) == diagnostics.end()) diagnostics.push_back(line);
  }

  if (diagnostics.empty()) return {};
  constexpr size_t kShown = 3;
  std::string summary = diagnostics[0];
  for (size_t i = 1; i < diagnostics.size() && i < kShown; ++i) summary += "; " + diagnostics[i];
  if (diagnostics.size() > kShown) summary += " (+" + std::to_string(diagnostics.size() - kShown) + " more)";
  return summary;
}

bool validVaryingFloat(const Ctl::FunctionArgPtr &arg) {
  return arg.refcount() != 0 && arg->isVarying() &&
         arg->type().cast<Ctl::FloatType>().refcount() != 0;
}

std::string canonicalScriptPath(const std::string &path) {
  std::error_code ec;
  const fs::path absolute = fs::absolute(fs::path(path), ec);
  if (ec) return path;
  const fs::path canonical = fs::weakly_canonical(absolute, ec);
  return ec ? absolute.string() : canonical.string();
}

// --- ART metadata ---------------------------------------------------------
// ART scripts describe each ART_main parameter in a comment line such as
//   // @ART-param: ["gain", "Gain", 0.0, 4.0, 1.0, 0.01]
// whose value is a JSON array. RawNode interprets the scalar presentation
// metadata used by ART: defaults, labels, numeric ranges/precision, groups,
// tooltips and integer choice menus. Curve metadata is handled separately.

struct JsonValue {
  enum class Kind { Null, Bool, Number, String, Array, Object } kind = Kind::Null;
  bool boolean = false;
  double number = 0.0;
  std::string string;
  std::vector<JsonValue> items;  // Array only
};

class JsonReader {
 public:
  explicit JsonReader(const std::string &text) : text_(text) {}

  // Parses exactly one JSON value spanning the whole text.
  bool parseDocument(JsonValue &out) {
    if (!parseValue(out, 0)) return false;
    skipWs();
    return pos_ == text_.size();
  }

 private:
  static constexpr int kMaxDepth = 32;

  void skipWs() {
    while (pos_ < text_.size() && std::isspace((unsigned char)text_[pos_])) ++pos_;
  }

  bool literal(const char *word) {
    const size_t n = std::char_traits<char>::length(word);
    if (text_.compare(pos_, n, word) != 0) return false;
    pos_ += n;
    return true;
  }

  bool parseValue(JsonValue &out, int depth) {
    if (depth > kMaxDepth) return false;
    skipWs();
    if (pos_ >= text_.size()) return false;
    const char c = text_[pos_];
    if (c == '"') {
      out.kind = JsonValue::Kind::String;
      return parseString(out.string);
    }
    if (c == '[' || c == '{') return parseContainer(out, depth);
    if (literal("true")) { out.kind = JsonValue::Kind::Bool; out.boolean = true; return true; }
    if (literal("false")) { out.kind = JsonValue::Kind::Bool; out.boolean = false; return true; }
    if (literal("null")) { out.kind = JsonValue::Kind::Null; return true; }
    return parseNumber(out);
  }

  // Locale-independent, unlike strtod under a non-"C" LC_NUMERIC.
  bool parseNumber(JsonValue &out) {
    const size_t start = pos_;
    while (pos_ < text_.size() && (std::isdigit((unsigned char)text_[pos_]) || text_[pos_] == '-' ||
                                   text_[pos_] == '+' || text_[pos_] == '.' || text_[pos_] == 'e' ||
                                   text_[pos_] == 'E'))
      ++pos_;
    if (pos_ == start) return false;
    std::istringstream in(text_.substr(start, pos_ - start));
    in.imbue(std::locale::classic());
    double value = 0.0;
    if (!(in >> value) || in.peek() != std::char_traits<char>::eof()) return false;
    out.kind = JsonValue::Kind::Number;
    out.number = value;
    return true;
  }

  static void appendUtf8(std::string &out, unsigned cp) {
    if (cp < 0x80) {
      out += (char)cp;
    } else if (cp < 0x800) {
      out += (char)(0xC0 | (cp >> 6));
      out += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      out += (char)(0xE0 | (cp >> 12));
      out += (char)(0x80 | ((cp >> 6) & 0x3F));
      out += (char)(0x80 | (cp & 0x3F));
    } else {
      out += (char)(0xF0 | (cp >> 18));
      out += (char)(0x80 | ((cp >> 12) & 0x3F));
      out += (char)(0x80 | ((cp >> 6) & 0x3F));
      out += (char)(0x80 | (cp & 0x3F));
    }
  }

  bool parseHex4(unsigned &value) {
    if (pos_ + 4 > text_.size()) return false;
    value = 0;
    for (int i = 0; i < 4; ++i) {
      const char h = text_[pos_++];
      value <<= 4;
      if (h >= '0' && h <= '9') value |= (unsigned)(h - '0');
      else if (h >= 'a' && h <= 'f') value |= (unsigned)(h - 'a' + 10);
      else if (h >= 'A' && h <= 'F') value |= (unsigned)(h - 'A' + 10);
      else return false;
    }
    return true;
  }

  bool parseString(std::string &out) {
    ++pos_;  // opening quote
    out.clear();
    while (pos_ < text_.size()) {
      const char c = text_[pos_++];
      if (c == '"') return true;
      if (c != '\\') {
        out += c;
        continue;
      }
      if (pos_ >= text_.size()) return false;
      const char e = text_[pos_++];
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
          unsigned cp = 0;
          if (!parseHex4(cp)) return false;
          if (cp >= 0xD800 && cp <= 0xDBFF) {
            unsigned low = 0;
            if (pos_ + 2 > text_.size() || text_[pos_] != '\\' || text_[pos_ + 1] != 'u') return false;
            pos_ += 2;
            if (!parseHex4(low) || low < 0xDC00 || low > 0xDFFF) return false;
            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
          } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            return false;
          }
          appendUtf8(out, cp);
          break;
        }
        default:
          return false;
      }
    }
    return false;
  }

  // Arrays keep their items; objects are validated and skipped (unused).
  bool parseContainer(JsonValue &out, int depth) {
    const bool isArray = text_[pos_] == '[';
    const char close = isArray ? ']' : '}';
    out.kind = isArray ? JsonValue::Kind::Array : JsonValue::Kind::Object;
    ++pos_;
    skipWs();
    if (pos_ < text_.size() && text_[pos_] == close) {
      ++pos_;
      return true;
    }
    for (;;) {
      if (!isArray) {
        skipWs();
        std::string key;
        if (pos_ >= text_.size() || text_[pos_] != '"' || !parseString(key)) return false;
        skipWs();
        if (pos_ >= text_.size() || text_[pos_++] != ':') return false;
      }
      JsonValue item;
      if (!parseValue(item, depth + 1)) return false;
      if (isArray) out.items.push_back(std::move(item));
      skipWs();
      if (pos_ >= text_.size()) return false;
      const char c = text_[pos_++];
      if (c == close) return true;
      if (c != ',') return false;
    }
  }

  const std::string &text_;
  size_t pos_ = 0;
};

struct ArtParamDefinition {
  int line = 0;
  JsonValue spec;  // the JSON array; spec.items[0] is the parameter name
};

// Collects "@ART-param:" lines (optionally behind "//"), keyed by parameter
// name, matching how ART scans scripts. Malformed definitions are errors, as
// they are in ART.
std::map<std::string, ArtParamDefinition> readArtParamDefinitions(const std::string &path) {
  std::map<std::string, ArtParamDefinition> defs;
  std::ifstream in(path, std::ios::binary);
  const std::string file = fs::path(path).filename().string();
  std::string line;
  for (int number = 1; std::getline(in, line); ++number) {
    size_t s = 0;
    while (s < line.size() && std::isspace((unsigned char)line[s])) ++s;
    if (line.compare(s, 2, "//") == 0) s += 2;
    while (s < line.size() && std::isspace((unsigned char)line[s])) ++s;
    static const std::string kTag = "@ART-param:";
    if (line.compare(s, kTag.size(), kTag) != 0) continue;

    const std::string where = file + ":" + std::to_string(number) + ": ";
    ArtParamDefinition def;
    def.line = number;
    const std::string json = line.substr(s + kTag.size());
    if (!JsonReader(json).parseDocument(def.spec) || def.spec.kind != JsonValue::Kind::Array ||
        def.spec.items.size() < 2 || def.spec.items[0].kind != JsonValue::Kind::String)
      throw ContractError(where + "invalid @ART-param definition");
    const std::string name = def.spec.items[0].string;
    if (!defs.emplace(name, std::move(def)).second)
      throw ContractError(where + "duplicate @ART-param definition for " + name);
  }
  return defs;
}

bool jsonInteger(const JsonValue &value, int &out) {
  if (value.kind != JsonValue::Kind::Number || !std::isfinite(value.number) ||
      std::fabs(value.number) > (double)std::numeric_limits<int>::max() || value.number != std::trunc(value.number))
    return false;
  out = (int)value.number;
  return true;
}

// Reads the default declared by an @ART-param definition, using ART's
// positional layouts:
//   bool   [name, label, default?, group?, tooltip?]
//   float  [name, label, min, max, default?, step?, group?, tooltip?]
//   int    [name, label, min, max, default?, group?, tooltip?]
//   choice [name, label, [options...], default?, group?, tooltip?]  (int)
// Returns false when the definition gives no default; throws when it is
// malformed for the parameter's type.
bool artMetadataDefault(const ArtParamDefinition &def, ParameterType type, const std::string &file,
                        ParameterValue &out) {
  const auto &items = def.spec.items;
  const auto bad = [&]() {
    return ContractError(file + ":" + std::to_string(def.line) + ": invalid @ART-param definition for " +
                         items[0].string);
  };
  switch (type) {
    case ParameterType::Boolean:
      if (items.size() >= 3 && items[2].kind == JsonValue::Kind::Bool) {
        out = items[2].boolean;
        return true;
      }
      return false;
    case ParameterType::Double:
      if (items.size() < 4 || items[2].kind != JsonValue::Kind::Number || items[3].kind != JsonValue::Kind::Number)
        throw bad();
      if (items.size() < 5) return false;
      if (items[4].kind != JsonValue::Kind::Number || !std::isfinite(items[4].number) ||
          std::fabs(items[4].number) > std::numeric_limits<float>::max())
        throw bad();
      out = (double)(float)items[4].number;  // CTL float storage
      return true;
    case ParameterType::Integer: {
      const bool choice = items.size() >= 3 && items[2].kind == JsonValue::Kind::Array;
      const size_t at = choice ? 3 : 4;
      if (!choice && (items.size() < 4 || items[2].kind != JsonValue::Kind::Number ||
                      items[3].kind != JsonValue::Kind::Number))
        throw bad();
      if (items.size() <= at) return false;
      int v = 0;
      if (!jsonInteger(items[at], v)) throw bad();
      out = v;
      return true;
    }
    default:
      return false;
  }
}


std::string artDisplayText(const std::string &text) {
  if (text.empty() || text[0] != char(36)) return text;
  const size_t semi = text.find(';');
  if (semi != std::string::npos) return text.substr(semi + 1);
  return text.substr(1);
}

struct ArtPresentation {
  ParameterType type = ParameterType::Unsupported;
  std::string label;
  std::string groupId;
  std::string groupLabel;
  std::string hint;
  bool hasRange = false;
  double min = 0.0;
  double max = 1.0;
  double step = 0.0;
  std::vector<std::string> choices;
  std::vector<int> choiceValues;
};

ArtPresentation artPresentation(const ArtParamDefinition &def, ParameterType baseType, const std::string &file) {
  const auto &items = def.spec.items;
  const auto bad = [&]() {
    return ContractError(file + ":" + std::to_string(def.line) + ": invalid @ART-param definition for " +
                         items[0].string);
  };
  if (items.size() < 2 || items[1].kind != JsonValue::Kind::String) throw bad();

  ArtPresentation out;
  out.type = baseType;
  out.label = artDisplayText(items[1].string);

  auto setGroupTooltip = [&](size_t at) {
    if (items.size() <= at) return;
    if (items[at].kind != JsonValue::Kind::String) throw bad();
    if (!items[at].string.empty()) {
      out.groupId = "__art_group__:" + items[at].string;
      out.groupLabel = artDisplayText(items[at].string);
    }
    if (items.size() > at + 1) {
      if (items[at + 1].kind != JsonValue::Kind::String) throw bad();
      out.hint = artDisplayText(items[at + 1].string);
    }
  };

  switch (baseType) {
    case ParameterType::Boolean:
      if (items.size() < 2 || items.size() > 5) throw bad();
      if (items.size() >= 3 && items[2].kind != JsonValue::Kind::Bool) throw bad();
      if (items.size() >= 4) setGroupTooltip(3);
      break;

    case ParameterType::Double:
      if (items.size() < 4 || items.size() > 8 ||
          items[2].kind != JsonValue::Kind::Number || items[3].kind != JsonValue::Kind::Number ||
          !std::isfinite(items[2].number) || !std::isfinite(items[3].number))
        throw bad();
      out.hasRange = true;
      out.min = items[2].number;
      out.max = items[3].number;
      if (items.size() >= 5 && items[4].kind != JsonValue::Kind::Number) throw bad();
      if (items.size() >= 6) {
        if (items[5].kind != JsonValue::Kind::Number || !std::isfinite(items[5].number)) throw bad();
        out.step = items[5].number;
      } else if (items.size() >= 5) {
        out.step = (out.max - out.min) / 100.0;
      }
      if (items.size() >= 7) setGroupTooltip(6);
      break;

    case ParameterType::Integer:
      if (items.size() < 3 || items.size() > 7) throw bad();
      if (items[2].kind == JsonValue::Kind::Array) {
        out.type = ParameterType::Choice;
        bool strings = true;
        for (const JsonValue &choice : items[2].items) {
          if (choice.kind != JsonValue::Kind::String) {
            strings = false;
            break;
          }
        }
        if (strings) {
          for (size_t i = 0; i < items[2].items.size(); ++i) {
            out.choices.push_back(artDisplayText(items[2].items[i].string));
            out.choiceValues.push_back((int)i);
          }
        } else {
          for (const JsonValue &choice : items[2].items) {
            if (choice.kind != JsonValue::Kind::Array || choice.items.size() != 2 ||
                choice.items[0].kind != JsonValue::Kind::String)
              throw bad();
            int value = 0;
            if (!jsonInteger(choice.items[1], value) || value < 0) throw bad();
            out.choices.push_back(artDisplayText(choice.items[0].string));
            out.choiceValues.push_back(value);
          }
        }
        if (items.size() >= 4) {
          int ignored = 0;
          if (!jsonInteger(items[3], ignored)) throw bad();
        }
        if (items.size() >= 5) setGroupTooltip(4);
      } else {
        if (items.size() < 4) throw bad();
        int lo = 0, hi = 0;
        if (!jsonInteger(items[2], lo) || !jsonInteger(items[3], hi)) throw bad();
        out.hasRange = true;
        out.min = lo;
        out.max = hi;
        out.step = 1.0;
        if (items.size() >= 5) {
          int ignored = 0;
          if (!jsonInteger(items[4], ignored)) throw bad();
        }
        if (items.size() >= 6) setGroupTooltip(5);
      }
      break;

    default:
      throw bad();
  }
  return out;
}

std::string readArtLabel(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  std::string line;
  for (; std::getline(in, line);) {
    size_t s = 0;
    while (s < line.size() && std::isspace((unsigned char)line[s])) ++s;
    if (line.compare(s, 2, "//") == 0) s += 2;
    while (s < line.size() && std::isspace((unsigned char)line[s])) ++s;
    static const std::string kTag = "@ART-label:";
    if (line.compare(s, kTag.size(), kTag) != 0) continue;
    JsonValue value;
    if (JsonReader(line.substr(s + kTag.size())).parseDocument(value) &&
        value.kind == JsonValue::Kind::String)
      return artDisplayText(value.string);
  }
  return {};
}

}  // namespace

struct CtlProcessor::Impl {
  // Fixed after load(): which CTL inputs are exposed and how.
  struct ParameterBinding {
    std::string id;
    std::string label;
    std::string parent;
    std::string hint;
    Ctl::FunctionArgPtr arg;
    ParameterType type = ParameterType::Unsupported;
    ParameterValue defaultValue;
    bool hasRange = false;
    double min = 0.0;
    double max = 1.0;
    double step = 0.0;
    std::vector<std::string> choices;
    std::vector<int> choiceValues;
  };

  Ctl::SimdInterpreter interpreter;
  Ctl::FunctionCallPtr function;
  Ctl::FunctionArgPtr rIn;
  Ctl::FunctionArgPtr gIn;
  Ctl::FunctionArgPtr bIn;
  Ctl::FunctionArgPtr aIn;
  Ctl::FunctionArgPtr rOut;
  Ctl::FunctionArgPtr gOut;
  Ctl::FunctionArgPtr bOut;
  Ctl::FunctionArgPtr aOut;
  std::vector<ParameterBinding> exposedParameters;
  std::vector<std::pair<std::string, std::string>> artGroups;
  bool artDialect = false;
  std::string artLabel;

  // Two locks, never held together:
  // - parameterMutex guards parameterValues, the UI-facing state (parallel to
  //   exposedParameters). It is held only briefly, so parameter reads and edits
  //   never wait for a CTL render.
  // - renderMutex guards the interpreter and its argument registers for the
  //   whole of a render.
  std::mutex parameterMutex;
  std::vector<ParameterValue> parameterValues;
  std::mutex renderMutex;

  static ParameterType exposedParameterType(const Ctl::FunctionArgPtr &arg) {
    if (!arg.refcount() || arg->isVarying()) return ParameterType::Unsupported;
    if (arg->type().cast<Ctl::FloatType>().refcount() != 0) return ParameterType::Double;
    if (arg->type().cast<Ctl::IntType>().refcount() != 0) return ParameterType::Integer;
    if (arg->type().cast<Ctl::BoolType>().refcount() != 0) return ParameterType::Boolean;
    return ParameterType::Unsupported;
  }

  static ParameterValue readValue(const Ctl::FunctionArgPtr &arg, ParameterType type) {
    switch (type) {
      case ParameterType::Double:
        return (double)*reinterpret_cast<const float *>(arg->data());
      case ParameterType::Integer:
      case ParameterType::Choice:
        return *reinterpret_cast<const int *>(arg->data());
      case ParameterType::Boolean:
        return *reinterpret_cast<const bool *>(arg->data());
      default:
        return {};
    }
  }

  static bool normaliseValue(ParameterType type, const ParameterValue &value, ParameterValue &out) {
    switch (type) {
      case ParameterType::Double: {
        // CTL float inputs are 32-bit. Reject NaN/inf and values that would
        // overflow (converting an out-of-range double to float is undefined).
        const double *v = std::get_if<double>(&value);
        if (!v || !std::isfinite(*v) || std::fabs(*v) > std::numeric_limits<float>::max()) return false;
        out = (double)(float)*v;
        return true;
      }
      case ParameterType::Integer:
      case ParameterType::Choice: {
        const int *v = std::get_if<int>(&value);
        if (!v) return false;
        out = *v;
        return true;
      }
      case ParameterType::Boolean: {
        const bool *v = std::get_if<bool>(&value);
        if (!v) return false;
        out = *v;
        return true;
      }
      default:
        return false;
    }
  }

  static void writeValue(const ParameterBinding &binding, const ParameterValue &value) {
    switch (binding.type) {
      case ParameterType::Double:
        *reinterpret_cast<float *>(binding.arg->data()) = (float)std::get<double>(value);
        break;
      case ParameterType::Integer:
      case ParameterType::Choice:
        *reinterpret_cast<int *>(binding.arg->data()) = std::get<int>(value);
        break;
      case ParameterType::Boolean:
        *reinterpret_cast<bool *>(binding.arg->data()) = std::get<bool>(value);
        break;
      default:
        break;
    }
  }

  int findParameter(const std::string &id) const {
    for (size_t i = 0; i < exposedParameters.size(); ++i)
      if (exposedParameters[i].id == id) return (int)i;
    return -1;
  }

  void load(const std::string &path) {
    std::vector<std::string> modulePaths = Ctl::Interpreter::modulePaths();
    const std::string parent = fs::path(path).parent_path().string();
    if (!parent.empty() && std::find(modulePaths.begin(), modulePaths.end(), parent) == modulePaths.end())
      modulePaths.insert(modulePaths.begin(), parent);
    interpreter.setUserModulePath(modulePaths, true);

    interpreter.loadFile(path);

    // Standard CTL remains the primary contract. If the module does not define
    // main(), fall back to ART's documented ART_main entry point. Other errors
    // creating main() are not hidden by the compatibility fallback.
    try {
      function = interpreter.newFunctionCall("main");
    } catch (const std::exception &e) {
      if (std::string(e.what()) != "Cannot find CTL function main.") throw;
      try {
        function = interpreter.newFunctionCall("ART_main");
      } catch (const std::exception &artError) {
        if (std::string(artError.what()) != "Cannot find CTL function ART_main.") throw;
        // Not a ContractError: when compilation or an import failed, CTL's own
        // diagnostics explain why no entry point exists and are preferred.
        throw std::runtime_error("CTL script defines neither main() nor ART_main()");
      }
      artDialect = true;
    }

    if (function->returnValue()->type().cast<Ctl::VoidType>().refcount() == 0)
      throw ContractError(artDialect ? "ART_main() must return void" : "CTL main() must return void");

    if (artDialect) {
      // ART defines the first three inputs positionally as varying float RGB
      // channels, followed by script-defined parameters, and requires exactly
      // three varying float RGB outputs.
      if (function->numInputArgs() < 3)
        throw ContractError("ART_main() must take three varying float RGB inputs");
      if (function->numOutputArgs() != 3)
        throw ContractError("ART_main() must have exactly three varying float RGB outputs (found " +
                            std::to_string(function->numOutputArgs()) + ")");

      rIn = function->inputArg(0);
      gIn = function->inputArg(1);
      bIn = function->inputArg(2);
      rOut = function->outputArg(0);
      gOut = function->outputArg(1);
      bOut = function->outputArg(2);

      if (!validVaryingFloat(rIn) || !validVaryingFloat(gIn) || !validVaryingFloat(bIn) ||
          !validVaryingFloat(rOut) || !validVaryingFloat(gOut) || !validVaryingFloat(bOut))
        throw ContractError("ART_main() RGB inputs and outputs must be varying float");

      // Defaults follow ART's documented precedence: the @ART-param default,
      // then the CTL default in ART_main, then zero/false. The same metadata
      // also supplies presentation: labels, slider ranges/precision, groups
      // and choice menus.
      const std::string file = fs::path(path).filename().string();
      artLabel = readArtLabel(path);
      std::map<std::string, ArtParamDefinition> metadata = readArtParamDefinitions(path);
      for (size_t i = 3; i < function->numInputArgs(); ++i) {
        Ctl::FunctionArgPtr arg = function->inputArg(i);
        const std::string &name = arg->name();
        const ParameterType type = exposedParameterType(arg);
        if (type == ParameterType::Unsupported) {
          if (arg->isVarying())
            throw ContractError("ART CTL parameter " + name + " must be uniform, not varying");
          const Ctl::ArrayTypePtr array = arg->type().cast<Ctl::ArrayType>();
          if (array.refcount() != 0 && array->elementType().cast<Ctl::FloatType>().refcount() != 0)
            throw ContractError("ART CTL parameter " + name +
                                " is a curve (float array); ART curve parameters are not supported yet");
          if (array.refcount() != 0)
            throw ContractError("ART CTL parameter " + name + " is an array; array parameters are not supported");
          throw ContractError("ART CTL parameter " + name +
                              " has an unsupported type; ART parameters must be float, int or bool");
        }

        ParameterBinding binding;
        binding.id = name;
        binding.label = name;
        binding.arg = arg;
        binding.type = type;

        ParameterValue metadataDefault;
        const auto def = metadata.find(name);
        if (def != metadata.end()) {
          const ArtPresentation presentation = artPresentation(def->second, type, file);
          binding.type = presentation.type;
          binding.label = presentation.label.empty() ? name : presentation.label;
          binding.parent = presentation.groupId;
          binding.hint = presentation.hint;
          binding.hasRange = presentation.hasRange;
          binding.min = presentation.min;
          binding.max = presentation.max;
          binding.step = presentation.step;
          binding.choices = presentation.choices;
          binding.choiceValues = presentation.choiceValues;
          if (!presentation.groupId.empty() &&
              std::find_if(artGroups.begin(), artGroups.end(), [&](const auto &g) {
                return g.first == presentation.groupId;
              }) == artGroups.end())
            artGroups.emplace_back(presentation.groupId, presentation.groupLabel);

          if (artMetadataDefault(def->second, type, file, metadataDefault))
            binding.defaultValue = metadataDefault;
        }
        if (std::holds_alternative<std::monostate>(binding.defaultValue) && arg->hasDefaultValue()) {
          arg->setDefaultValue();
          binding.defaultValue = readValue(arg, type);
        } else if (std::holds_alternative<std::monostate>(binding.defaultValue)) {
          switch (type) {
            case ParameterType::Double: binding.defaultValue = 0.0; break;
            case ParameterType::Integer: binding.defaultValue = 0; break;
            case ParameterType::Boolean: binding.defaultValue = false; break;
            default: break;
          }
        }
        if (def != metadata.end()) metadata.erase(def);

        parameterValues.push_back(binding.defaultValue);
        exposedParameters.push_back(std::move(binding));
      }

      // As in ART, metadata must describe parameters that exist.
      if (!metadata.empty())
        throw ContractError(file + ":" + std::to_string(metadata.begin()->second.line) +
                            ": @ART-param refers to unknown ART_main parameter " + metadata.begin()->first);
    } else {
      rIn = function->findInputArg("rIn");
      gIn = function->findInputArg("gIn");
      bIn = function->findInputArg("bIn");
      aIn = function->findInputArg("aIn");
      rOut = function->findOutputArg("rOut");
      gOut = function->findOutputArg("gOut");
      bOut = function->findOutputArg("bOut");
      aOut = function->findOutputArg("aOut");

      if (!validVaryingFloat(rIn) || !validVaryingFloat(gIn) || !validVaryingFloat(bIn))
        throw ContractError("CTL main() must provide varying float rIn, gIn and bIn inputs");
      if (!validVaryingFloat(rOut) || !validVaryingFloat(gOut) || !validVaryingFloat(bOut))
        throw ContractError("CTL main() must provide varying float rOut, gOut and bOut outputs");
      if (aIn.refcount() != 0 && !validVaryingFloat(aIn))
        throw ContractError("CTL aIn must be a varying float when present");
      if (aOut.refcount() != 0 && !validVaryingFloat(aOut))
        throw ContractError("CTL aOut must be a varying float when present");

      for (size_t i = 0; i < function->numInputArgs(); ++i) {
        Ctl::FunctionArgPtr arg = function->inputArg(i);
        const std::string &name = arg->name();
        if (name == "rIn" || name == "gIn" || name == "bIn" || name == "aIn") continue;
        if (!arg->hasDefaultValue())
          throw ContractError("Unsupported required CTL input parameter: " + name);

        // Plain CTL provides a type/name/default but no UI range metadata.
        // Defaulted scalar uniform float/int/bool inputs are therefore exposed
        // through RawNode's generic parameter API as unbounded controls.
        arg->setDefaultValue();
        const ParameterType type = exposedParameterType(arg);
        if (type == ParameterType::Unsupported) continue;

        ParameterBinding binding;
        binding.id = name;
        binding.label = name;
        binding.arg = arg;
        binding.type = type;
        binding.defaultValue = readValue(arg, type);
        parameterValues.push_back(binding.defaultValue);
        exposedParameters.push_back(std::move(binding));
      }

      if (aIn.refcount() != 0 && aIn->hasDefaultValue()) aIn->setDefaultValue();
    }
  }
};

CtlProcessor::CtlProcessor(std::string path, std::string name, std::unique_ptr<Impl> impl)
    : path_(std::move(path)), name_(std::move(name)), impl_(std::move(impl)) {}

CtlProcessor::~CtlProcessor() = default;

std::unique_ptr<CtlProcessor> CtlProcessor::create(const std::string &path, std::string *error) {
  const std::string canonical = canonicalScriptPath(path);
  std::error_code ec;
  if (!fs::is_regular_file(canonical, ec)) {
    if (error) *error = "CTL file not found";
    return nullptr;
  }

  // Prefer CTL's own diagnostics over its generic load/lookup exception text.
  ScopedCtlMessageCapture capture;
  const auto ctlError = [&](const char *fallback) {
    const std::string summary = summarizeCtlMessages(capture.text(), fs::path(canonical).parent_path().string());
    return summary.empty() ? std::string(fallback) : summary;
  };

  try {
    auto impl = std::make_unique<Impl>();
    impl->load(canonical);
    std::string name = fs::path(canonical).stem().string();
    if (impl->artDialect && !impl->artLabel.empty()) name = impl->artLabel;
    if (name.empty()) name = "CTL";
    return std::unique_ptr<CtlProcessor>(
        new CtlProcessor(canonical, std::move(name), std::move(impl)));
  } catch (const ContractError &e) {
    if (error) *error = e.what();
    return nullptr;
  } catch (const std::exception &e) {
    if (error) *error = ctlError(e.what());
    return nullptr;
  } catch (...) {
    if (error) *error = ctlError("Unknown CTL interpreter error");
    return nullptr;
  }
}

std::string CtlProcessor::identifier() const { return path_; }

std::string CtlProcessor::displayName() const { return name_; }

std::vector<ProcessorParameter> CtlProcessor::parameters() const {
  std::vector<ProcessorParameter> out;
  if (!impl_) return out;

  std::lock_guard<std::mutex> lock(impl_->parameterMutex);
  out.reserve(impl_->artGroups.size() + impl_->exposedParameters.size());

  for (const auto &group : impl_->artGroups) {
    ProcessorParameter param;
    param.id = group.first;
    param.label = group.second.empty() ? group.first : group.second;
    param.type = ParameterType::Group;
    param.groupInitiallyOpen = true;
    out.push_back(std::move(param));
  }

  for (size_t i = 0; i < impl_->exposedParameters.size(); ++i) {
    const auto &binding = impl_->exposedParameters[i];
    ProcessorParameter param;
    param.id = binding.id;
    param.label = binding.label.empty() ? binding.id : binding.label;
    param.parent = binding.parent;
    param.hint = binding.hint.empty()
                     ? (impl_->artDialect ? "ART CTL input parameter" : "Standard CTL input parameter")
                     : binding.hint;
    param.type = binding.type;
    param.value = impl_->parameterValues[i];
    param.defaultValue = binding.defaultValue;
    param.hasRange = binding.hasRange;
    param.min = binding.min;
    param.max = binding.max;
    param.displayMin = binding.min;
    param.displayMax = binding.max;
    param.step = binding.step;
    param.choices = binding.choices;
    param.choiceValues = binding.choiceValues;
    out.push_back(std::move(param));
  }
  return out;
}

bool CtlProcessor::setParameterValue(const std::string &id, const ParameterValue &value, bool notify) {
  (void)notify;
  if (!impl_) return false;

  const int index = impl_->findParameter(id);
  if (index < 0) return false;
  ParameterValue normalised;
  if (!Impl::normaliseValue(impl_->exposedParameters[index].type, value, normalised)) return false;

  // Takes effect from the next render; never waits for one in progress.
  std::lock_guard<std::mutex> lock(impl_->parameterMutex);
  impl_->parameterValues[index] = std::move(normalised);
  return true;
}

bool CtlProcessor::resetParameter(const std::string &id, bool notify) {
  (void)notify;
  if (!impl_) return false;

  const int index = impl_->findParameter(id);
  if (index < 0) return false;

  std::lock_guard<std::mutex> lock(impl_->parameterMutex);
  impl_->parameterValues[index] = impl_->exposedParameters[index].defaultValue;
  return true;
}

bool CtlProcessor::activateParameter(const std::string &id) {
  (void)id;
  return false;
}

void CtlProcessor::setRenderSize(int width, int height) {
  (void)width;
  (void)height;
}

ProcessorResult CtlProcessor::render(const Image &input, Image &output, int generation) {
  (void)generation;
  if (!impl_) return ProcessorResult::failure(-1, "Invalid CTL processor");

  std::vector<ParameterValue> values;
  {
    std::lock_guard<std::mutex> lock(impl_->parameterMutex);
    values = impl_->parameterValues;
  }

  std::lock_guard<std::mutex> lock(impl_->renderMutex);
  try {
    // One snapshot per render keeps every chunk of the frame consistent. CTL
    // rejects assignments to input parameters, so the script cannot change
    // these registers and they need no per-chunk re-application.
    for (size_t i = 0; i < impl_->exposedParameters.size(); ++i)
      Impl::writeValue(impl_->exposedParameters[i], values[i]);

    output.w = input.w;
    output.h = input.h;
    output.px.resize(input.px.size());

    const size_t pixels = (size_t)input.w * input.h;
    size_t offset = 0;
    while (offset < pixels) {
      const size_t count = std::min(impl_->interpreter.maxSamples(), pixels - offset);

      float *rIn = reinterpret_cast<float *>(impl_->rIn->data());
      float *gIn = reinterpret_cast<float *>(impl_->gIn->data());
      float *bIn = reinterpret_cast<float *>(impl_->bIn->data());
      float *aIn = impl_->aIn.refcount() ? reinterpret_cast<float *>(impl_->aIn->data()) : nullptr;

      for (size_t i = 0; i < count; ++i) {
        const size_t p = (offset + i) * 4;
        rIn[i] = input.px[p + 0];
        gIn[i] = input.px[p + 1];
        bIn[i] = input.px[p + 2];
        if (aIn) aIn[i] = input.px[p + 3];
      }

      impl_->function->callFunction(count);

      const float *rOut = reinterpret_cast<const float *>(impl_->rOut->data());
      const float *gOut = reinterpret_cast<const float *>(impl_->gOut->data());
      const float *bOut = reinterpret_cast<const float *>(impl_->bOut->data());
      const float *aOut = impl_->aOut.refcount()
                              ? reinterpret_cast<const float *>(impl_->aOut->data())
                              : nullptr;

      for (size_t i = 0; i < count; ++i) {
        const size_t p = (offset + i) * 4;
        output.px[p + 0] = rOut[i];
        output.px[p + 1] = gOut[i];
        output.px[p + 2] = bOut[i];
        output.px[p + 3] = aOut ? aOut[i] : input.px[p + 3];
      }

      offset += count;
    }

    return ProcessorResult::success();
  } catch (const std::exception &e) {
    return ProcessorResult::failure(-1, e.what());
  } catch (...) {
    return ProcessorResult::failure(-1, "Unknown CTL render error");
  }
}
) return text;
  const size_t semi = text.find(';');
  if (semi != std::string::npos) return text.substr(semi + 1);
  return text.substr(1);
}

struct ArtPresentation {
  ParameterType type = ParameterType::Unsupported;
  std::string label;
  std::string groupId;
  std::string groupLabel;
  std::string hint;
  bool hasRange = false;
  double min = 0.0;
  double max = 1.0;
  double step = 0.0;
  std::vector<std::string> choices;
  std::vector<int> choiceValues;
};

ArtPresentation artPresentation(const ArtParamDefinition &def, ParameterType baseType, const std::string &file) {
  const auto &items = def.spec.items;
  const auto bad = [&]() {
    return ContractError(file + ":" + std::to_string(def.line) + ": invalid @ART-param definition for " +
                         items[0].string);
  };
  if (items.size() < 2 || items[1].kind != JsonValue::Kind::String) throw bad();

  ArtPresentation out;
  out.type = baseType;
  out.label = artDisplayText(items[1].string);

  auto setGroupTooltip = [&](size_t at) {
    if (items.size() <= at) return;
    if (items[at].kind != JsonValue::Kind::String) throw bad();
    if (!items[at].string.empty()) {
      out.groupId = "__art_group__:" + items[at].string;
      out.groupLabel = artDisplayText(items[at].string);
    }
    if (items.size() > at + 1) {
      if (items[at + 1].kind != JsonValue::Kind::String) throw bad();
      out.hint = artDisplayText(items[at + 1].string);
    }
  };

  switch (baseType) {
    case ParameterType::Boolean:
      if (items.size() < 2 || items.size() > 5) throw bad();
      if (items.size() >= 3 && items[2].kind != JsonValue::Kind::Bool) throw bad();
      if (items.size() >= 4) setGroupTooltip(3);
      break;

    case ParameterType::Double:
      if (items.size() < 4 || items.size() > 8 ||
          items[2].kind != JsonValue::Kind::Number || items[3].kind != JsonValue::Kind::Number ||
          !std::isfinite(items[2].number) || !std::isfinite(items[3].number))
        throw bad();
      out.hasRange = true;
      out.min = items[2].number;
      out.max = items[3].number;
      if (items.size() >= 5 && items[4].kind != JsonValue::Kind::Number) throw bad();
      if (items.size() >= 6) {
        if (items[5].kind != JsonValue::Kind::Number || !std::isfinite(items[5].number)) throw bad();
        out.step = items[5].number;
      } else if (items.size() >= 5) {
        out.step = (out.max - out.min) / 100.0;
      }
      if (items.size() >= 7) setGroupTooltip(6);
      break;

    case ParameterType::Integer:
      if (items.size() < 3 || items.size() > 7) throw bad();
      if (items[2].kind == JsonValue::Kind::Array) {
        out.type = ParameterType::Choice;
        bool strings = true;
        for (const JsonValue &choice : items[2].items) {
          if (choice.kind != JsonValue::Kind::String) {
            strings = false;
            break;
          }
        }
        if (strings) {
          for (size_t i = 0; i < items[2].items.size(); ++i) {
            out.choices.push_back(artDisplayText(items[2].items[i].string));
            out.choiceValues.push_back((int)i);
          }
        } else {
          for (const JsonValue &choice : items[2].items) {
            if (choice.kind != JsonValue::Kind::Array || choice.items.size() != 2 ||
                choice.items[0].kind != JsonValue::Kind::String)
              throw bad();
            int value = 0;
            if (!jsonInteger(choice.items[1], value) || value < 0) throw bad();
            out.choices.push_back(artDisplayText(choice.items[0].string));
            out.choiceValues.push_back(value);
          }
        }
        if (items.size() >= 4) {
          int ignored = 0;
          if (!jsonInteger(items[3], ignored)) throw bad();
        }
        if (items.size() >= 5) setGroupTooltip(4);
      } else {
        if (items.size() < 4) throw bad();
        int lo = 0, hi = 0;
        if (!jsonInteger(items[2], lo) || !jsonInteger(items[3], hi)) throw bad();
        out.hasRange = true;
        out.min = lo;
        out.max = hi;
        out.step = 1.0;
        if (items.size() >= 5) {
          int ignored = 0;
          if (!jsonInteger(items[4], ignored)) throw bad();
        }
        if (items.size() >= 6) setGroupTooltip(5);
      }
      break;

    default:
      throw bad();
  }
  return out;
}

std::string readArtLabel(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  std::string line;
  for (; std::getline(in, line);) {
    size_t s = 0;
    while (s < line.size() && std::isspace((unsigned char)line[s])) ++s;
    if (line.compare(s, 2, "//") == 0) s += 2;
    while (s < line.size() && std::isspace((unsigned char)line[s])) ++s;
    static const std::string kTag = "@ART-label:";
    if (line.compare(s, kTag.size(), kTag) != 0) continue;
    JsonValue value;
    if (JsonReader(line.substr(s + kTag.size())).parseDocument(value) &&
        value.kind == JsonValue::Kind::String)
      return artDisplayText(value.string);
  }
  return {};
}

}  // namespace

struct CtlProcessor::Impl {
  // Fixed after load(): which CTL inputs are exposed and how.
  struct ParameterBinding {
    std::string id;
    Ctl::FunctionArgPtr arg;
    ParameterType type = ParameterType::Unsupported;
    ParameterValue defaultValue;
  };

  Ctl::SimdInterpreter interpreter;
  Ctl::FunctionCallPtr function;
  Ctl::FunctionArgPtr rIn;
  Ctl::FunctionArgPtr gIn;
  Ctl::FunctionArgPtr bIn;
  Ctl::FunctionArgPtr aIn;
  Ctl::FunctionArgPtr rOut;
  Ctl::FunctionArgPtr gOut;
  Ctl::FunctionArgPtr bOut;
  Ctl::FunctionArgPtr aOut;
  std::vector<ParameterBinding> exposedParameters;
  bool artDialect = false;

  // Two locks, never held together:
  // - parameterMutex guards parameterValues, the UI-facing state (parallel to
  //   exposedParameters). It is held only briefly, so parameter reads and edits
  //   never wait for a CTL render.
  // - renderMutex guards the interpreter and its argument registers for the
  //   whole of a render.
  std::mutex parameterMutex;
  std::vector<ParameterValue> parameterValues;
  std::mutex renderMutex;

  static ParameterType exposedParameterType(const Ctl::FunctionArgPtr &arg) {
    if (!arg.refcount() || arg->isVarying()) return ParameterType::Unsupported;
    if (arg->type().cast<Ctl::FloatType>().refcount() != 0) return ParameterType::Double;
    if (arg->type().cast<Ctl::IntType>().refcount() != 0) return ParameterType::Integer;
    if (arg->type().cast<Ctl::BoolType>().refcount() != 0) return ParameterType::Boolean;
    return ParameterType::Unsupported;
  }

  static ParameterValue readValue(const Ctl::FunctionArgPtr &arg, ParameterType type) {
    switch (type) {
      case ParameterType::Double:
        return (double)*reinterpret_cast<const float *>(arg->data());
      case ParameterType::Integer:
        return *reinterpret_cast<const int *>(arg->data());
      case ParameterType::Boolean:
        return *reinterpret_cast<const bool *>(arg->data());
      default:
        return {};
    }
  }

  static bool normaliseValue(ParameterType type, const ParameterValue &value, ParameterValue &out) {
    switch (type) {
      case ParameterType::Double: {
        // CTL float inputs are 32-bit. Reject NaN/inf and values that would
        // overflow (converting an out-of-range double to float is undefined).
        const double *v = std::get_if<double>(&value);
        if (!v || !std::isfinite(*v) || std::fabs(*v) > std::numeric_limits<float>::max()) return false;
        out = (double)(float)*v;
        return true;
      }
      case ParameterType::Integer: {
        const int *v = std::get_if<int>(&value);
        if (!v) return false;
        out = *v;
        return true;
      }
      case ParameterType::Boolean: {
        const bool *v = std::get_if<bool>(&value);
        if (!v) return false;
        out = *v;
        return true;
      }
      default:
        return false;
    }
  }

  static void writeValue(const ParameterBinding &binding, const ParameterValue &value) {
    switch (binding.type) {
      case ParameterType::Double:
        *reinterpret_cast<float *>(binding.arg->data()) = (float)std::get<double>(value);
        break;
      case ParameterType::Integer:
        *reinterpret_cast<int *>(binding.arg->data()) = std::get<int>(value);
        break;
      case ParameterType::Boolean:
        *reinterpret_cast<bool *>(binding.arg->data()) = std::get<bool>(value);
        break;
      default:
        break;
    }
  }

  int findParameter(const std::string &id) const {
    for (size_t i = 0; i < exposedParameters.size(); ++i)
      if (exposedParameters[i].id == id) return (int)i;
    return -1;
  }

  void load(const std::string &path) {
    std::vector<std::string> modulePaths = Ctl::Interpreter::modulePaths();
    const std::string parent = fs::path(path).parent_path().string();
    if (!parent.empty() && std::find(modulePaths.begin(), modulePaths.end(), parent) == modulePaths.end())
      modulePaths.insert(modulePaths.begin(), parent);
    interpreter.setUserModulePath(modulePaths, true);

    interpreter.loadFile(path);

    // Standard CTL remains the primary contract. If the module does not define
    // main(), fall back to ART's documented ART_main entry point. Other errors
    // creating main() are not hidden by the compatibility fallback.
    try {
      function = interpreter.newFunctionCall("main");
    } catch (const std::exception &e) {
      if (std::string(e.what()) != "Cannot find CTL function main.") throw;
      try {
        function = interpreter.newFunctionCall("ART_main");
      } catch (const std::exception &artError) {
        if (std::string(artError.what()) != "Cannot find CTL function ART_main.") throw;
        // Not a ContractError: when compilation or an import failed, CTL's own
        // diagnostics explain why no entry point exists and are preferred.
        throw std::runtime_error("CTL script defines neither main() nor ART_main()");
      }
      artDialect = true;
    }

    if (function->returnValue()->type().cast<Ctl::VoidType>().refcount() == 0)
      throw ContractError(artDialect ? "ART_main() must return void" : "CTL main() must return void");

    if (artDialect) {
      // ART defines the first three inputs positionally as varying float RGB
      // channels, followed by script-defined parameters, and requires exactly
      // three varying float RGB outputs.
      if (function->numInputArgs() < 3)
        throw ContractError("ART_main() must take three varying float RGB inputs");
      if (function->numOutputArgs() != 3)
        throw ContractError("ART_main() must have exactly three varying float RGB outputs (found " +
                            std::to_string(function->numOutputArgs()) + ")");

      rIn = function->inputArg(0);
      gIn = function->inputArg(1);
      bIn = function->inputArg(2);
      rOut = function->outputArg(0);
      gOut = function->outputArg(1);
      bOut = function->outputArg(2);

      if (!validVaryingFloat(rIn) || !validVaryingFloat(gIn) || !validVaryingFloat(bIn) ||
          !validVaryingFloat(rOut) || !validVaryingFloat(gOut) || !validVaryingFloat(bOut))
        throw ContractError("ART_main() RGB inputs and outputs must be varying float");

      // Defaults follow ART's documented precedence: the @ART-param default,
      // then the CTL default in ART_main, then zero/false. The resolved value
      // is the parameter's default and initial value, so Sidecar V2 records
      // ART's starting values for untouched parameters. Labels, ranges, groups
      // and choices from the same metadata are left for the presentation step.
      const std::string file = fs::path(path).filename().string();
      std::map<std::string, ArtParamDefinition> metadata = readArtParamDefinitions(path);
      for (size_t i = 3; i < function->numInputArgs(); ++i) {
        Ctl::FunctionArgPtr arg = function->inputArg(i);
        const std::string &name = arg->name();
        const ParameterType type = exposedParameterType(arg);
        if (type == ParameterType::Unsupported) {
          if (arg->isVarying())
            throw ContractError("ART CTL parameter " + name + " must be uniform, not varying");
          const Ctl::ArrayTypePtr array = arg->type().cast<Ctl::ArrayType>();
          if (array.refcount() != 0 && array->elementType().cast<Ctl::FloatType>().refcount() != 0)
            throw ContractError("ART CTL parameter " + name +
                                " is a curve (float array); ART curve parameters are not supported yet");
          if (array.refcount() != 0)
            throw ContractError("ART CTL parameter " + name + " is an array; array parameters are not supported");
          throw ContractError("ART CTL parameter " + name +
                              " has an unsupported type; ART parameters must be float, int or bool");
        }

        ParameterBinding binding;
        binding.id = name;
        binding.arg = arg;
        binding.type = type;

        ParameterValue metadataDefault;
        const auto def = metadata.find(name);
        if (def != metadata.end() && artMetadataDefault(def->second, type, file, metadataDefault)) {
          binding.defaultValue = metadataDefault;
        } else if (arg->hasDefaultValue()) {
          arg->setDefaultValue();
          binding.defaultValue = readValue(arg, type);
        } else {
          switch (type) {
            case ParameterType::Double: binding.defaultValue = 0.0; break;
            case ParameterType::Integer: binding.defaultValue = 0; break;
            case ParameterType::Boolean: binding.defaultValue = false; break;
            default: break;
          }
        }
        if (def != metadata.end()) metadata.erase(def);

        parameterValues.push_back(binding.defaultValue);
        exposedParameters.push_back(std::move(binding));
      }

      // As in ART, metadata must describe parameters that exist.
      if (!metadata.empty())
        throw ContractError(file + ":" + std::to_string(metadata.begin()->second.line) +
                            ": @ART-param refers to unknown ART_main parameter " + metadata.begin()->first);
    } else {
      rIn = function->findInputArg("rIn");
      gIn = function->findInputArg("gIn");
      bIn = function->findInputArg("bIn");
      aIn = function->findInputArg("aIn");
      rOut = function->findOutputArg("rOut");
      gOut = function->findOutputArg("gOut");
      bOut = function->findOutputArg("bOut");
      aOut = function->findOutputArg("aOut");

      if (!validVaryingFloat(rIn) || !validVaryingFloat(gIn) || !validVaryingFloat(bIn))
        throw ContractError("CTL main() must provide varying float rIn, gIn and bIn inputs");
      if (!validVaryingFloat(rOut) || !validVaryingFloat(gOut) || !validVaryingFloat(bOut))
        throw ContractError("CTL main() must provide varying float rOut, gOut and bOut outputs");
      if (aIn.refcount() != 0 && !validVaryingFloat(aIn))
        throw ContractError("CTL aIn must be a varying float when present");
      if (aOut.refcount() != 0 && !validVaryingFloat(aOut))
        throw ContractError("CTL aOut must be a varying float when present");

      for (size_t i = 0; i < function->numInputArgs(); ++i) {
        Ctl::FunctionArgPtr arg = function->inputArg(i);
        const std::string &name = arg->name();
        if (name == "rIn" || name == "gIn" || name == "bIn" || name == "aIn") continue;
        if (!arg->hasDefaultValue())
          throw ContractError("Unsupported required CTL input parameter: " + name);

        // Plain CTL provides a type/name/default but no UI range metadata.
        // Defaulted scalar uniform float/int/bool inputs are therefore exposed
        // through RawNode's generic parameter API as unbounded controls.
        arg->setDefaultValue();
        const ParameterType type = exposedParameterType(arg);
        if (type == ParameterType::Unsupported) continue;

        ParameterBinding binding;
        binding.id = name;
        binding.arg = arg;
        binding.type = type;
        binding.defaultValue = readValue(arg, type);
        parameterValues.push_back(binding.defaultValue);
        exposedParameters.push_back(std::move(binding));
      }

      if (aIn.refcount() != 0 && aIn->hasDefaultValue()) aIn->setDefaultValue();
    }
  }
};

CtlProcessor::CtlProcessor(std::string path, std::string name, std::unique_ptr<Impl> impl)
    : path_(std::move(path)), name_(std::move(name)), impl_(std::move(impl)) {}

CtlProcessor::~CtlProcessor() = default;

std::unique_ptr<CtlProcessor> CtlProcessor::create(const std::string &path, std::string *error) {
  const std::string canonical = canonicalScriptPath(path);
  std::error_code ec;
  if (!fs::is_regular_file(canonical, ec)) {
    if (error) *error = "CTL file not found";
    return nullptr;
  }

  // Prefer CTL's own diagnostics over its generic load/lookup exception text.
  ScopedCtlMessageCapture capture;
  const auto ctlError = [&](const char *fallback) {
    const std::string summary = summarizeCtlMessages(capture.text(), fs::path(canonical).parent_path().string());
    return summary.empty() ? std::string(fallback) : summary;
  };

  try {
    auto impl = std::make_unique<Impl>();
    impl->load(canonical);
    std::string name = fs::path(canonical).stem().string();
    if (name.empty()) name = "CTL";
    return std::unique_ptr<CtlProcessor>(
        new CtlProcessor(canonical, std::move(name), std::move(impl)));
  } catch (const ContractError &e) {
    if (error) *error = e.what();
    return nullptr;
  } catch (const std::exception &e) {
    if (error) *error = ctlError(e.what());
    return nullptr;
  } catch (...) {
    if (error) *error = ctlError("Unknown CTL interpreter error");
    return nullptr;
  }
}

std::string CtlProcessor::identifier() const { return path_; }

std::string CtlProcessor::displayName() const { return name_; }

std::vector<ProcessorParameter> CtlProcessor::parameters() const {
  std::vector<ProcessorParameter> out;
  if (!impl_) return out;

  std::lock_guard<std::mutex> lock(impl_->parameterMutex);
  out.reserve(impl_->exposedParameters.size());
  for (size_t i = 0; i < impl_->exposedParameters.size(); ++i) {
    const auto &binding = impl_->exposedParameters[i];
    ProcessorParameter param;
    param.id = binding.id;
    param.label = binding.id;
    param.hint = impl_->artDialect ? "ART CTL input parameter (ART labels and ranges not yet applied)"
                                   : "Standard CTL input parameter";
    param.type = binding.type;
    param.value = impl_->parameterValues[i];
    param.defaultValue = binding.defaultValue;
    param.hasRange = false;
    out.push_back(std::move(param));
  }
  return out;
}

bool CtlProcessor::setParameterValue(const std::string &id, const ParameterValue &value, bool notify) {
  (void)notify;
  if (!impl_) return false;

  const int index = impl_->findParameter(id);
  if (index < 0) return false;
  ParameterValue normalised;
  if (!Impl::normaliseValue(impl_->exposedParameters[index].type, value, normalised)) return false;

  // Takes effect from the next render; never waits for one in progress.
  std::lock_guard<std::mutex> lock(impl_->parameterMutex);
  impl_->parameterValues[index] = std::move(normalised);
  return true;
}

bool CtlProcessor::resetParameter(const std::string &id, bool notify) {
  (void)notify;
  if (!impl_) return false;

  const int index = impl_->findParameter(id);
  if (index < 0) return false;

  std::lock_guard<std::mutex> lock(impl_->parameterMutex);
  impl_->parameterValues[index] = impl_->exposedParameters[index].defaultValue;
  return true;
}

bool CtlProcessor::activateParameter(const std::string &id) {
  (void)id;
  return false;
}

void CtlProcessor::setRenderSize(int width, int height) {
  (void)width;
  (void)height;
}

ProcessorResult CtlProcessor::render(const Image &input, Image &output, int generation) {
  (void)generation;
  if (!impl_) return ProcessorResult::failure(-1, "Invalid CTL processor");

  std::vector<ParameterValue> values;
  {
    std::lock_guard<std::mutex> lock(impl_->parameterMutex);
    values = impl_->parameterValues;
  }

  std::lock_guard<std::mutex> lock(impl_->renderMutex);
  try {
    // One snapshot per render keeps every chunk of the frame consistent. CTL
    // rejects assignments to input parameters, so the script cannot change
    // these registers and they need no per-chunk re-application.
    for (size_t i = 0; i < impl_->exposedParameters.size(); ++i)
      Impl::writeValue(impl_->exposedParameters[i], values[i]);

    output.w = input.w;
    output.h = input.h;
    output.px.resize(input.px.size());

    const size_t pixels = (size_t)input.w * input.h;
    size_t offset = 0;
    while (offset < pixels) {
      const size_t count = std::min(impl_->interpreter.maxSamples(), pixels - offset);

      float *rIn = reinterpret_cast<float *>(impl_->rIn->data());
      float *gIn = reinterpret_cast<float *>(impl_->gIn->data());
      float *bIn = reinterpret_cast<float *>(impl_->bIn->data());
      float *aIn = impl_->aIn.refcount() ? reinterpret_cast<float *>(impl_->aIn->data()) : nullptr;

      for (size_t i = 0; i < count; ++i) {
        const size_t p = (offset + i) * 4;
        rIn[i] = input.px[p + 0];
        gIn[i] = input.px[p + 1];
        bIn[i] = input.px[p + 2];
        if (aIn) aIn[i] = input.px[p + 3];
      }

      impl_->function->callFunction(count);

      const float *rOut = reinterpret_cast<const float *>(impl_->rOut->data());
      const float *gOut = reinterpret_cast<const float *>(impl_->gOut->data());
      const float *bOut = reinterpret_cast<const float *>(impl_->bOut->data());
      const float *aOut = impl_->aOut.refcount()
                              ? reinterpret_cast<const float *>(impl_->aOut->data())
                              : nullptr;

      for (size_t i = 0; i < count; ++i) {
        const size_t p = (offset + i) * 4;
        output.px[p + 0] = rOut[i];
        output.px[p + 1] = gOut[i];
        output.px[p + 2] = bOut[i];
        output.px[p + 3] = aOut ? aOut[i] : input.px[p + 3];
      }

      offset += count;
    }

    return ProcessorResult::success();
  } catch (const std::exception &e) {
    return ProcessorResult::failure(-1, e.what());
  } catch (...) {
    return ProcessorResult::failure(-1, "Unknown CTL render error");
  }
}
