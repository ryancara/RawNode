#import <AppKit/AppKit.h>

#include "ui/MacWindow.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3native.h>

void MacWindow_SetSRGBColorSpace(GLFWwindow *window) {
  NSWindow *nativeWindow = glfwGetCocoaWindow(window);
  [nativeWindow setColorSpace:[NSColorSpace sRGBColorSpace]];
}
