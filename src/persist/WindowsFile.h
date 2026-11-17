#pragma once

#ifdef _WIN32
#include <filesystem>
#include <string>

namespace persist_detail {
enum class FileFault;
// Persistence paths are UTF-8; these helpers never use the ANSI code page.
bool windowsPath(const std::string &text, std::filesystem::path &path, std::string &error);
bool windowsUtf8(const wchar_t *text, std::string &out, std::string &error);
// Refuse redirected/cloud parents and unqualified volumes, including mapped drives.
bool windowsParents(const std::filesystem::path &path, std::string &error);
bool replaceWindowsFile(const std::string &path, const std::string &body,
                        std::string &error, FileFault fault);
}
#endif
