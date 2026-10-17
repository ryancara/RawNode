#pragma once

#include <string>

// Application startup, ImGui + GLFW loop, and ordered teardown that drains
// workers, saves the current photo and application preferences, and releases graphics.
// Panel and widget code lives in ui/. optionalPath may be empty.
int runApp(const std::string &optionalPath);
