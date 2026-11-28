#pragma once

#ifdef __cplusplus
extern "C" {
#endif

struct GLFWwindow;

// Call on the main thread with a newly created window, before rendering.
void MacWindow_SetSRGBColorSpace(struct GLFWwindow *window);

#ifdef __cplusplus
}
#endif
