#pragma once

struct GLFWwindow;

namespace podbox {

// Commands delivered by macOS's system media-key routing. They are queued by
// the platform callback and consumed on the UI thread, so playback state is
// never mutated from a MediaPlayer callback.
enum class MediaKeyCommand { None, Play, Pause, TogglePlayPause };

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

// Registers PodBox as a macOS now-playing application so the keyboard's
// Play/Pause key works even while the window is in the background. A no-op on
// other platforms.
void installMediaKeyHandler();
void removeMediaKeyHandler();
MediaKeyCommand takeMediaKeyCommand();
void setMediaKeyPlaybackState(bool playing);

// True while the window is in macOS full screen, where its size is the
// screen's rather than one worth remembering.
bool windowIsFullScreen(GLFWwindow* window);

// A separate, freely resizable window showing the Now Playing cover, like
// iTunes' artwork viewer. AppKit remembers its frame between launches. Only
// macOS has one; elsewhere these do nothing and Supported() is false.
bool artworkWindowSupported();
// Opens the window or brings it forward. `activate` also makes it key; a
// window reopened at launch should not take focus from the main one.
void showArtworkWindow(bool activate);
void closeArtworkWindow();
// Open, including minimised to the Dock.
bool artworkWindowOpen();
// `rgba` is width * height * 4 bytes, copied; null shows "No Artwork".
void setArtworkWindowImage(const unsigned char* rgba, int width, int height,
                           const char* title);

}  // namespace podbox
