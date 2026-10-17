#pragma once

#include "persist/ProjectPersist.h"

#include <filesystem>
#include <map>
#include <sstream>
#include <string>

namespace persist_json { struct Value; }

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
// Narrow fault seam for deterministic file-I/O tests. Production passes None;
// no global hook or replacement filesystem is installed.
enum class FileFault {
  None, Read, PermissionStat, PermissionCheck, TempCreate, Write, Flush, PermissionCopy, Close, Replace
};
SidecarReadStatus readGraphFileText(const std::string &path, std::string &json, std::string &error,
                                  FileFault fault);
bool updateGraphSidecar(const std::string &path, const PersistGrade *grade, PhotoMark mark,
                        std::string &error, FileFault fault, bool testOperations);
const char *operationName(NodeOperation operation, bool testOperations);
NodeOperation operationFromName(const std::string &name, bool testOperations);
std::string graphRecordError(const PersistGraph &graph, bool testOperations);
bool decodeGraphFile(const std::string &json, PersistFile &out, std::string &error,
                     std::vector<std::string> *warnings, bool testOperations);
bool decodeGraphFile(const persist_json::Value &root, PersistFile &out, std::string &error,
                     std::vector<std::string> *warnings, bool testOperations);
bool encodeGraphFile(const PersistFile &file, std::string &json, std::string &error,
                     std::vector<std::string> *warnings, bool testOperations);
std::string fragmentRecordError(const PersistFragment &fragment, bool testOperations);
bool encodeTransferPayload(const PersistTransfer &transfer, std::string &json, std::string &error,
                           bool testOperations);
bool decodeTransferPayload(const std::string &json, PersistTransfer &out, std::string &error,
                           bool testOperations);
// Throws a descriptive error on wrong types or unknown choices.
ParameterValue parameterValue(const ProcessorParameter &param, const std::string &raw);
}
