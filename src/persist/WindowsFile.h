#pragma once
#ifdef _WIN32
#include <filesystem>
#include <string>
namespace persist_detail {
enum class FileFault;
bool windowsUtf8Environment(std::string &error);
// Persistence paths are UTF-8, independent of the ANSI code page.
bool windowsPath(const std::string &text, std::filesystem::path &path, std::string &error);
bool windowsUtf8(const wchar_t *text, std::string &out, std::string &error);
bool windowsCanSaveFile(const std::string &path, std::string &error);
bool replaceWindowsFile(const std::string &path, const std::string &body, std::string &error, FileFault fault);
}
#endif
