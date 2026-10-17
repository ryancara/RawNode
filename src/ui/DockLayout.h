#pragma once

#include "AppState.h"

namespace DockLayout {

inline constexpr const char *kLeft = "OFX Plugins";
inline constexpr const char *kPreview = "Preview";
inline constexpr const char *kParams = "OFX Parameters";
inline constexpr const char *kFilmstrip = "Filmstrip";

// Host dockspace for the frame. Builds a default layout when no .ini exists
// using preference fallback sizes.
void BeginMainDockSpace(App &app);

}  // namespace DockLayout
