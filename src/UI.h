#pragma once

#include <string>

// Application startup, ImGui + GLFW loop, and ordered teardown that drains
// workers, persists the current document/workspace state, and releases graphics.
// Panel and widget code lives in ui/. optionalPath may be empty.
int runApp(const std::string &optionalPath);
