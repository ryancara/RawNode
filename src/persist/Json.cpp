#include "persist/Json.h"

#include <charconv>
#include <cmath>
#include <locale>
#include <sstream>
#include <stdexcept>

namespace persist_json {
namespace {
class Reader {
 public:
  Reader(const std::string &text, bool legacyStrings) : text_(text), legacyStrings_(legacyStrings) {}
  Value document() {
    Value value = readValue(0);
    whitespace();
    if (pos_ != text_.size()) fail("Trailing content");
    return value;
  }

 private:
  const std::string &text_;
  bool legacyStrings_;
  size_t pos_ = 0;

  [[noreturn]] void fail(const char *message) const {
    throw std::runtime_error(std::string(message) + " at byte " + std::to_string(pos_));
  }
  void whitespace() {
    while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\n' ||
           text_[pos_] == '\r' || text_[pos_] == '\t')) ++pos_;
  }
  bool take(char ch) {
    if (pos_ == text_.size() || text_[pos_] != ch) return false;
    ++pos_;
    return true;
  }
  void require(char ch) { if (!take(ch)) fail("Unexpected JSON character"); }
  static bool digit(char ch) { return ch >= '0' && ch <= '9'; }
  unsigned hex4() {
    unsigned value = 0;
    for (int i = 0; i < 4; ++i) {
      if (pos_ == text_.size()) fail("Truncated Unicode escape");
      const char ch = text_[pos_++];
      unsigned h;
      if (digit(ch)) h = ch - '0';
      else if (ch >= 'a' && ch <= 'f') h = ch - 'a' + 10;
      else if (ch >= 'A' && ch <= 'F') h = ch - 'A' + 10;
      else fail("Invalid Unicode escape");
      value = value * 16 + h;
    }
    return value;
  }
  static void appendUtf8(std::string &out, unsigned cp) {
    if (cp <= 0x7f) out += char(cp);
    else if (cp <= 0x7ff) {
      out += char(0xc0 | (cp >> 6)); out += char(0x80 | (cp & 63));
    } else if (cp <= 0xffff) {
      out += char(0xe0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 63)); out += char(0x80 | (cp & 63));
    } else {
      out += char(0xf0 | (cp >> 18)); out += char(0x80 | ((cp >> 12) & 63));
      out += char(0x80 | ((cp >> 6) & 63)); out += char(0x80 | (cp & 63));
    }
  }
  void rawUtf8(std::string &out, unsigned char lead) {
    const size_t start = pos_ - 1;
    int count;
    unsigned cp, minimum;
    if (lead >= 0xc2 && lead <= 0xdf) { count = 1; cp = lead & 31; minimum = 0x80; }
    else if (lead >= 0xe0 && lead <= 0xef) { count = 2; cp = lead & 15; minimum = 0x800; }
    else if (lead >= 0xf0 && lead <= 0xf4) { count = 3; cp = lead & 7; minimum = 0x10000; }
    else fail("Invalid UTF-8 lead byte");
    for (int i = 0; i < count; ++i) {
      if (pos_ == text_.size()) fail("Truncated UTF-8");
      const unsigned char ch = text_[pos_++];
      if ((ch & 0xc0) != 0x80) fail("Invalid UTF-8 continuation");
      cp = (cp << 6) | (ch & 63);
    }
    if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) fail("Invalid UTF-8 code point");
    out.append(text_, start, pos_ - start);
  }
  std::string readString() {
    require('"');
    std::string out;
    while (pos_ < text_.size()) {
      const unsigned char ch = text_[pos_++];
      if (ch == '"') return out;
      if (ch < 0x20 && !legacyStrings_) fail("Unescaped control character");
      if (ch >= 0x80 && legacyStrings_) {
        out += char(ch);
        continue;
      }
      if (ch >= 0x80) { rawUtf8(out, ch); continue; }
      if (ch != '\\') { out += char(ch); continue; }
      if (pos_ == text_.size()) fail("Truncated escape");
      switch (text_[pos_++]) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
          unsigned cp = hex4();
          if (cp >= 0xd800 && cp <= 0xdbff) {
            require('\\'); require('u');
            const unsigned low = hex4();
            if (low < 0xdc00 || low > 0xdfff) fail("Invalid surrogate pair");
            cp = 0x10000 + ((cp - 0xd800) << 10) + low - 0xdc00;
          } else if (cp >= 0xdc00 && cp <= 0xdfff) fail("Unpaired surrogate");
          appendUtf8(out, cp);
          break;
        }
        default: fail("Invalid JSON escape");
      }
    }
    fail("Unterminated string");
  }
  void number() {
    take('-');
    if (!take('0')) {
      if (pos_ == text_.size() || text_[pos_] < '1' || text_[pos_] > '9') fail("Invalid JSON number");
      while (pos_ < text_.size() && digit(text_[pos_])) ++pos_;
    }
    if (take('.')) {
      if (pos_ == text_.size() || !digit(text_[pos_])) fail("Missing fractional digits");
      while (pos_ < text_.size() && digit(text_[pos_])) ++pos_;
    }
    if (take('e') || take('E')) {
      if (!take('+')) take('-');
      if (pos_ == text_.size() || !digit(text_[pos_])) fail("Missing exponent digits");
      while (pos_ < text_.size() && digit(text_[pos_])) ++pos_;
    }
  }
  Value readValue(unsigned depth) {
    // Bound recursive parsing, including opaque parameters, before stack use
    // can depend on untrusted input depth.
    if (depth > 128) fail("JSON nesting limit exceeded");
    whitespace();
    if (pos_ == text_.size()) fail("Truncated JSON");
    const size_t start = pos_;
    Value value;
    const char ch = text_[pos_];
    if (ch == '"') { value.type = Type::String; value.string = readString(); }
    else if (ch == '{') {
      value.type = Type::Object;
      ++pos_; whitespace();
      if (!take('}')) {
        do {
          whitespace();
          std::string key = readString();
          whitespace(); require(':');
          Value child = readValue(depth + 1);
          if (!value.object.emplace(std::move(key), std::move(child)).second) fail("Duplicate object key");
          whitespace();
          if (take('}')) break;
          require(',');
        } while (true);
      }
    } else if (ch == '[') {
      value.type = Type::Array;
      ++pos_; whitespace();
      if (!take(']')) {
        do {
          value.array.push_back(readValue(depth + 1));
          whitespace();
          if (take(']')) break;
          require(',');
        } while (true);
      }
    } else if (ch == 't' || ch == 'f' || ch == 'n') {
      const std::string literal = ch == 't' ? "true" : ch == 'f' ? "false" : "null";
      if (text_.compare(pos_, literal.size(), literal) != 0) fail("Invalid JSON literal");
      pos_ += literal.size();
      value.type = ch == 'n' ? Type::Null : Type::Boolean;
    } else {
      value.type = Type::Number;
      number();
    }
    value.raw = text_.substr(start, pos_ - start);
    return value;
  }
};
}  // namespace

bool read(const std::string &json, Value &out, std::string &error, bool legacyStrings) {
  error.clear();
  try {
    Value value = Reader(json, legacyStrings).document();
    out = std::move(value);
    return true;
  } catch (const std::runtime_error &failure) {
    error = failure.what();
    return false;
  }
}

bool finiteNumber(const Value &value, double &out) {
  if (value.type != Type::Number) return false;
  std::istringstream stream(value.raw);
  stream.imbue(std::locale::classic());
  stream >> out;
  if (!stream || !stream.eof() || !std::isfinite(out)) return false;
  // Ordinary double conversion may underflow silently. Opaque numbers never
  // convert at all; understood numerical fields must not become zero this way.
  if (out == 0) {
    const auto exponent = value.raw.find_first_of("eE");
    const auto mantissa = value.raw.substr(0, exponent);
    if (mantissa.find_first_of("123456789") != std::string::npos) return false;
  }
  return true;
}

bool integer(const Value &value, int &out) {
  if (value.type != Type::Number) return false;
  const auto end = value.raw.data() + value.raw.size();
  const auto parsed = std::from_chars(value.raw.data(), end, out);
  return parsed.ec == std::errc{} && parsed.ptr == end;
}
}  // namespace persist_json
