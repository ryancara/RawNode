#pragma once

#include <GLFW/glfw3.h>

// The Windows SDK's GL 1.1 header omits this GL 1.2 texture wrap enum.
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
