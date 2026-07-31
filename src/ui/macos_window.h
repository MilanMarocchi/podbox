#pragma once

struct GLFWwindow;

namespace podbox {

// Merges the window's title bar into the content area, so the toolbar runs to
// the top of the window and the close/minimise/zoom buttons sit over its left
// edge — the way iTunes and every Mac media app of that era looked. A no-op
// off macOS.
//
// Callers must keep the leftmost kTrafficLightWidth of the toolbar clear, and
// must call dragWindowWithCurrentEvent() when the user drags empty toolbar
// space: with no title bar left to grab, that is the only way to move the
// window.
void useUnifiedTitlebar(GLFWwindow* window);

// Hands the in-flight mouse drag to the window server, which moves the window
// for the rest of the gesture. Safe to call repeatedly; only the first call of
// a drag does anything.
void dragWindowWithCurrentEvent(GLFWwindow* window);

}  // namespace podbox
