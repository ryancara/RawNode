#pragma once

#include <string>

struct App;

// Full-resolution evaluation and file production. Both entry points acquire
// RenderRuntime ownership, capture inputs on the control thread, and retain
// ownership through preview-size restoration and final status publication.
bool runExportJob(App &app, const std::string &outPath);
bool startExport(App &app, const std::string &outPath);
