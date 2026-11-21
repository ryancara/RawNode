#pragma once

#include <filesystem>

// Directory containing the running executable. Returns empty if unavailable.
std::filesystem::path executableDirectory();
