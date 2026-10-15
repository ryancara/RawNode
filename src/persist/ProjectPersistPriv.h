#pragma once

#include "persist/ProjectPersist.h"

#include <filesystem>
#include <map>
#include <sstream>
#include <string>

std::string jsonEscape(const std::string &s);
void appendGuiJson(std::ostringstream &o, const PersistGui &g);
void appendChainJson(std::ostringstream &o, const PersistChain &chain);
bool writeFile(const std::filesystem::path &path, const std::string &body);
bool extractObject(const std::string &json, const char *key, std::string &objOut);
bool extractArray(const std::string &json, const char *key, std::string &arrOut);
bool extractStringField(const std::string &json, const char *key, std::string &out);
bool extractIntField(const std::string &json, const char *key, int &out);
bool extractFloatField(const std::string &json, const char *key, float &out);
bool extractBoolField(const std::string &json, const char *key, bool &out);
void loadGuiFromJson(const std::string &guiObj, PersistGui &g);
bool loadChainFromJson(const std::string &chainObj, PersistChain &chain);
bool readAllText(const std::string &path, std::string &out);

// Shared implementation seams. Test access admits existing graph_test kinds;
// production entry points always pass false.
namespace persist_detail {
const char *operationName(NodeOperation operation, bool testOperations);
NodeOperation operationFromName(const std::string &name, bool testOperations);
std::string graphRecordError(const PersistGraph &graph, bool testOperations);
bool decodeGraphFile(const std::string &json, PersistFile &out, std::string &error,
                     std::vector<std::string> *warnings, bool testOperations);
bool encodeGraphFile(const PersistFile &file, std::string &json, std::string &error,
                     std::vector<std::string> *warnings, bool testOperations);
// Throws a descriptive error on wrong types or unknown choices.
ParameterValue parameterValue(const ProcessorParameter &param, const std::string &raw);
}
