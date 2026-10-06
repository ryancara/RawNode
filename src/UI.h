#pragma once

#include <string>

// Application startup, ImGui + GLFW loop, and ordered worker/graphics teardown.
// Panel and widget code lives in ui/. optionalPath may be empty.
int runApp(const std::string &optionalPath);
