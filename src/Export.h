#pragma once

#include <string>

struct App;

// Full-resolution evaluation and file production. Both entry points capture
// inputs on the control thread under RenderRuntime ownership, preserving the
// accepted document state when Export is confirmed. Ownership is held through
// file writing and final status publication; each graph operation owns sizing.
// Both refuse destinations identifying app.path (including filesystem aliases)
// before file opening; other existing destinations remain valid export targets.
// Synchronous entry point, currently used by tests.
bool runExportJob(App &app, const std::string &outPath);
// Asynchronous entry point used by the production UI.
bool startExport(App &app, const std::string &outPath);
