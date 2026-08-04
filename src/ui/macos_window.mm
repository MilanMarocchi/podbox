#include "ui/macos_window.h"

#import <Cocoa/Cocoa.h>
#import <MediaPlayer/MediaPlayer.h>

#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include <deque>
#include <mutex>
#include <optional>

namespace podbox {
namespace {

std::mutex mediaKeyMutex;
std::deque<MediaKeyCommand> mediaKeyCommands;
id playTarget = nil;
id pauseTarget = nil;
id toggleTarget = nil;
std::optional<bool> publishedPlaying;

void enqueueMediaKeyCommand(MediaKeyCommand command) {
    {
        std::lock_guard lock(mediaKeyMutex);
        mediaKeyCommands.push_back(command);
    }
    // The main loop may be sleeping because the window is in the background.
    // GLFW documents this as callable from any thread.
    glfwPostEmptyEvent();
}

}  // namespace

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

void installMediaKeyHandler() {
    MPRemoteCommandCenter* commands =
        [MPRemoteCommandCenter sharedCommandCenter];
    commands.playCommand.enabled = YES;
    commands.pauseCommand.enabled = YES;
    commands.togglePlayPauseCommand.enabled = YES;

    playTarget = [commands.playCommand
        addTargetWithHandler:^MPRemoteCommandHandlerStatus(
            MPRemoteCommandEvent*) {
          enqueueMediaKeyCommand(MediaKeyCommand::Play);
          return MPRemoteCommandHandlerStatusSuccess;
        }];
    pauseTarget = [commands.pauseCommand
        addTargetWithHandler:^MPRemoteCommandHandlerStatus(
            MPRemoteCommandEvent*) {
          enqueueMediaKeyCommand(MediaKeyCommand::Pause);
          return MPRemoteCommandHandlerStatusSuccess;
        }];
    toggleTarget = [commands.togglePlayPauseCommand
        addTargetWithHandler:^MPRemoteCommandHandlerStatus(
            MPRemoteCommandEvent*) {
          enqueueMediaKeyCommand(MediaKeyCommand::TogglePlayPause);
          return MPRemoteCommandHandlerStatusSuccess;
        }];

    // A paused session makes PodBox eligible for the keyboard command before
    // the first track has played. The generic title is replaced by the app's
    // own playback UI; it also gives Control Centre a useful label.
    MPNowPlayingInfoCenter* info = [MPNowPlayingInfoCenter defaultCenter];
    info.nowPlayingInfo = @{MPMediaItemPropertyTitle : @"PodBox"};
    info.playbackState = MPNowPlayingPlaybackStatePaused;
    publishedPlaying = false;
}

void removeMediaKeyHandler() {
    MPRemoteCommandCenter* commands =
        [MPRemoteCommandCenter sharedCommandCenter];
    if (playTarget) [commands.playCommand removeTarget:playTarget];
    if (pauseTarget) [commands.pauseCommand removeTarget:pauseTarget];
    if (toggleTarget)
        [commands.togglePlayPauseCommand removeTarget:toggleTarget];
    playTarget = pauseTarget = toggleTarget = nil;

    commands.playCommand.enabled = NO;
    commands.pauseCommand.enabled = NO;
    commands.togglePlayPauseCommand.enabled = NO;
    MPNowPlayingInfoCenter* info = [MPNowPlayingInfoCenter defaultCenter];
    info.playbackState = MPNowPlayingPlaybackStateStopped;
    info.nowPlayingInfo = nil;
    publishedPlaying.reset();

    std::lock_guard lock(mediaKeyMutex);
    mediaKeyCommands.clear();
}

MediaKeyCommand takeMediaKeyCommand() {
    std::lock_guard lock(mediaKeyMutex);
    if (mediaKeyCommands.empty()) return MediaKeyCommand::None;
    const MediaKeyCommand command = mediaKeyCommands.front();
    mediaKeyCommands.pop_front();
    return command;
}

void setMediaKeyPlaybackState(bool playing) {
    if (publishedPlaying && *publishedPlaying == playing) return;
    [MPNowPlayingInfoCenter defaultCenter].playbackState =
        playing ? MPNowPlayingPlaybackStatePlaying
                : MPNowPlayingPlaybackStatePaused;
    publishedPlaying = playing;
}

}  // namespace podbox
