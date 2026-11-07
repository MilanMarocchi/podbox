#include "ui/macos_window.h"

#import <Cocoa/Cocoa.h>
#import <MediaPlayer/MediaPlayer.h>

#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include <cstring>
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

// Esc and Cmd-W close it: GLFW's menu bar has no Close item to carry the
// shortcut, and a viewer with no keyboard way out feels broken.
@interface PBArtworkWindow : NSWindow
@end

@implementation PBArtworkWindow
- (void)keyDown:(NSEvent*)event {
    if (event.keyCode == 53 /* Esc */)
        [self performClose:nil];
    else
        [super keyDown:event];
}
- (BOOL)performKeyEquivalent:(NSEvent*)event {
    const NSEventModifierFlags mods =
        event.modifierFlags & NSEventModifierFlagDeviceIndependentFlagsMask;
    if (mods == NSEventModifierFlagCommand &&
        [event.charactersIgnoringModifiers isEqualToString:@"w"]) {
        [self performClose:nil];
        return YES;
    }
    return [super performKeyEquivalent:event];
}
@end

namespace podbox {
namespace {

PBArtworkWindow* artworkWindow = nil;
NSImageView* artworkView = nil;
NSTextField* artworkPlaceholder = nil;

PBArtworkWindow* ensureArtworkWindow() {
    if (artworkWindow) return artworkWindow;
    const NSRect frame = NSMakeRect(0, 0, 400, 400);
    PBArtworkWindow* w = [[PBArtworkWindow alloc]
        initWithContentRect:frame
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                            NSWindowStyleMaskMiniaturizable |
                            NSWindowStyleMaskResizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    // Closing only hides it, so reopening keeps the frame and the image.
    w.releasedWhenClosed = NO;
    w.title = @"Artwork";
    w.backgroundColor = NSColor.blackColor;
    w.contentMinSize = NSMakeSize(160, 160);
    w.collectionBehavior |= NSWindowCollectionBehaviorFullScreenPrimary;

    NSView* content = [[NSView alloc] initWithFrame:frame];
    NSImageView* image = [[NSImageView alloc] initWithFrame:frame];
    image.imageScaling = NSImageScaleProportionallyUpOrDown;
    image.imageFrameStyle = NSImageFrameNone;
    image.editable = NO;
    image.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [content addSubview:image];

    NSTextField* label = [NSTextField labelWithString:@"No Artwork"];
    label.textColor = [NSColor colorWithWhite:0.6 alpha:1.0];
    label.font = [NSFont systemFontOfSize:15];
    label.alignment = NSTextAlignmentCenter;
    [label sizeToFit];
    label.frame = NSMakeRect(0, (frame.size.height - label.frame.size.height) / 2,
                             frame.size.width, label.frame.size.height);
    label.autoresizingMask = NSViewWidthSizable | NSViewMinYMargin |
                             NSViewMaxYMargin;
    [content addSubview:label];
    w.contentView = content;

    // First launch: centred. After that AppKit puts it back where it was,
    // at the size it was, on whichever display it was on.
    [w center];
    [w setFrameAutosaveName:@"PodBoxArtworkWindow"];
    [w setFrameUsingName:@"PodBoxArtworkWindow"];

    artworkWindow = w;
    artworkView = image;
    artworkPlaceholder = label;
    return w;
}

}  // namespace

bool windowIsFullScreen(GLFWwindow* window) {
    NSWindow* w = glfwGetCocoaWindow(window);
    return w && (w.styleMask & NSWindowStyleMaskFullScreen);
}

bool artworkWindowSupported() { return true; }

void showArtworkWindow(bool activate) {
    PBArtworkWindow* w = ensureArtworkWindow();
    if (w.miniaturized) [w deminiaturize:nil];
    if (activate)
        [w makeKeyAndOrderFront:nil];
    else
        [w orderFront:nil];
}

void closeArtworkWindow() {
    if (artworkWindow) [artworkWindow orderOut:nil];
}

bool artworkWindowOpen() {
    return artworkWindow &&
           (artworkWindow.visible || artworkWindow.miniaturized);
}

void setArtworkWindowImage(const unsigned char* rgba, int width, int height,
                           const char* title) {
    PBArtworkWindow* w = ensureArtworkWindow();
    w.title = title && *title ? [NSString stringWithUTF8String:title]
                              : @"Artwork";
    if (!rgba || width <= 0 || height <= 0) {
        artworkView.image = nil;
        artworkPlaceholder.hidden = NO;
        return;
    }
    NSBitmapImageRep* rep = [[NSBitmapImageRep alloc]
        initWithBitmapDataPlanes:nullptr
                      pixelsWide:width
                      pixelsHigh:height
                   bitsPerSample:8
                 samplesPerPixel:4
                        hasAlpha:YES
                        isPlanar:NO
                  colorSpaceName:NSDeviceRGBColorSpace
                     bytesPerRow:NSInteger(width) * 4
                    bitsPerPixel:32];
    if (!rep) return;
    std::memcpy(rep.bitmapData, rgba, std::size_t(width) * height * 4);
    NSImage* image = [[NSImage alloc] initWithSize:NSMakeSize(width, height)];
    [image addRepresentation:rep];
    artworkView.image = image;
    artworkPlaceholder.hidden = YES;
}

}  // namespace podbox
