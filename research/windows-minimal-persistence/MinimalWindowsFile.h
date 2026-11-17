#pragma once
// Research-only experimental writer. Not linked into RawNode.
#ifdef _WIN32
#include <string>

namespace minimal {
enum class Fault { None, Write, Flush, Close, Replace, Cleanup };

struct Options {
  // Production would fix this policy. Research runs the same writer on
  // FAT32/exFAT/SMB to observe behaviour with qualification disabled.
  bool localNtfsOnly = true;
  Fault fault = Fault::None;
};

// Strict UTF-8 path, as RawNode passes std::string paths.
bool replaceFile(const std::string &utf8Path, const std::string &body, std::string &error, Options options = {});
// Same writer for callers that already hold UTF-16 (research fixtures).
bool replaceFile(const std::wstring &path, const std::string &body, std::string &error, Options options = {});
}  // namespace minimal
#endif
