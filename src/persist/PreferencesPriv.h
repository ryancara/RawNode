#pragma once
#include "persist/Preferences.h"
#include "persist/ProjectPersistPriv.h"

namespace preferences_detail {
// Explicit environment inputs keep platform path tests independent of the host.
ConfigPaths configPaths(const std::string &home, const std::string &configHome,
                        std::string &warning);
bool save(const std::string &path, const Preferences &preferences, std::string &error,
          persist_detail::FileFault fault);
}
