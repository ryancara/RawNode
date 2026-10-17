#pragma once

#include <string>

struct GLFWwindow;

bool ImGuiBackend_Init(GLFWwindow *window, int themeIndex, const std::string &layoutPath);
void ImGuiBackend_NewFrame();
void ImGuiBackend_Render();
void ImGuiBackend_Shutdown();
// Configure immediately after context creation, before the first frame.
// layoutPath must outlive the context; empty disables disk persistence.
void ImGuiBackend_ConfigureLayout(const std::string &layoutPath);
