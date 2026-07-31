#include "ui/macos_window.h"

#import <Cocoa/Cocoa.h>

#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

namespace podbox {

void useUnifiedTitlebar(GLFWwindow* window) {
    NSWindow* w = glfwGetCocoaWindow(window);
    if (!w) return;
    // FullSizeContentView lets the content view extend behind the title bar;
    // making the bar transparent and hiding its title leaves only the traffic
    // lights drawn over our toolbar.
    w.styleMask |= NSWindowStyleMaskFullSizeContentView;
    w.titlebarAppearsTransparent = YES;
    w.titleVisibility = NSWindowTitleHidden;
    // Not movableByWindowBackground: that would let a drag anywhere in the
    // track list shove the window around. The toolbar asks for a drag
    // explicitly instead.
    w.movableByWindowBackground = NO;
}

void dragWindowWithCurrentEvent(GLFWwindow* window) {
    NSWindow* w = glfwGetCocoaWindow(window);
    NSEvent* e = NSApp.currentEvent;
    if (!w || !e) return;
    // AppKit takes over for the remainder of the gesture, so this matches the
    // system's own title-bar drag, snapping and all.
    [w performWindowDragWithEvent:e];
}

}  // namespace podbox
