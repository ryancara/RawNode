#pragma once

#include <map>
#include <string>
#include <vector>

// Internal, read-only JSON value. Numbers and opaque sections retain their
// original tokens; conversion happens only for fields whose type is understood.
namespace persist_json {
enum class Type { Null, Boolean, Number, String, Array, Object };
struct Value {
  Type type = Type::Null;
  std::string raw;
  std::string string;
  std::vector<Value> array;
  std::map<std::string, Value> object;
};

// Legacy V1/V2 string parameters historically permitted literal controls. Only
// those callers opt in; the graph-native schema always uses strict mode.
bool read(const std::string &json, Value &out, std::string &error, bool legacyStrings = false);
bool finiteNumber(const Value &value, double &out);
bool integer(const Value &value, int &out);
}  // namespace persist_json
