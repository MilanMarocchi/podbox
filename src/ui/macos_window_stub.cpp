#include "ui/macos_window.h"

namespace podbox {

// Other platforms keep their ordinary title bar, and with it the usual way
// of moving the window.
void useUnifiedTitlebar(GLFWwindow*) {}
void dragWindowWithCurrentEvent(GLFWwindow*) {}
void installMediaKeyHandler() {}
void removeMediaKeyHandler() {}
MediaKeyCommand takeMediaKeyCommand() { return MediaKeyCommand::None; }
void setMediaKeyPlaybackState(bool) {}
bool windowIsFullScreen(GLFWwindow*) { return false; }
bool artworkWindowSupported() { return false; }
void showArtworkWindow(bool) {}
void closeArtworkWindow() {}
bool artworkWindowOpen() { return false; }
void setArtworkWindowImage(const unsigned char*, int, int, const char*) {}

}  // namespace podbox
