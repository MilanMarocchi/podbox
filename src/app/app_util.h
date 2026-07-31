#pragma once

// Small helpers shared between the three translation units that make up the
// UI (app.cpp, app_chrome.cpp, app_modals.cpp). Internal to src/app — nothing
// outside it should include this.

#include "ui/theme.h"

#include <imgui.h>

#include <cstdint>
#include <filesystem>
#include <string>

namespace podbox {

// Layout. The window is laid out by absolute cursor positioning rather than
// docking, so these are the only source of truth for where things sit.
inline constexpr float kSidebarWidth = 200.0f;
// Space kept clear at the right of a source-list row, so a long name stops
// before the eject button rather than running under it.
inline constexpr float kSidebarRightMargin = 34.0f;
inline constexpr float kToolbarHeight = 56.0f;
// The window has no separate title bar: the toolbar runs to the top of the
// window and macOS draws its close/minimise/zoom buttons over the left of it,
// the way iTunes did. Everything in the toolbar starts clear of them.
inline constexpr float kTrafficLightWidth = 78.0f;
inline constexpr float kTransportStartX = kTrafficLightWidth + 8.0f;
// Where the toolbar's left cluster ends: traffic lights, the three transport
// discs and the volume slider. The LCD may not encroach on it.
inline constexpr float kToolbarLeftEnd = kTransportStartX + 26.0f + 5.0f +
                                         30.0f + 5.0f + 26.0f + 12.0f + 112.0f +
                                         16.0f;
inline constexpr float kStatusBarHeight = 24.0f;
inline constexpr float kLcdWidth = 360.0f;
inline constexpr float kLcdHeight = 40.0f;
// How far in from the LCD's edges text may run. Wide enough to clear the
// elapsed/remaining times on the bottom row and leave a margin at the sides.
inline constexpr float kLcdTextInset = 12.0f;
inline constexpr float kSearchWidth = 170.0f;

// --- drawing ---------------------------------------------------------------

inline ImVec4 v4(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

void addTextCentered(ImDrawList* dl, ImFont* font, float size, ImVec2 center,
                     ImU32 color, const char* text);

// `text` shortened to fit `maxWidth`, with an ellipsis when it had to give.
// Song and album titles are arbitrarily long and the chrome that shows them
// is not, so anything drawn with ImDrawList — which does no clipping of its
// own — needs this or it runs over whatever is beside it.
std::string truncateToWidth(ImFont* font, float size, const std::string& text,
                            float maxWidth);

// truncateToWidth plus addTextCentered, centred on the middle of [x0, x1] and
// fitted to it. The pairing is what most of the chrome actually wants.
void addTextCenteredFit(ImDrawList* dl, ImFont* font, float size, float x0,
                        float x1, float y, ImU32 color,
                        const std::string& text);

// Five stars, iTunes-style. Returns the rating (0-100) the user clicked, or
// -1 when they did not. Drawn rather than using a widget so it matches the
// rest of the hand-drawn chrome.
int drawStars(ImDrawList* dl, ImVec2 p, std::uint8_t rating, bool hovered,
              ImVec2 mouse, bool clicked);

// Small iPod glyph drawn with primitives, iTunes-sidebar style.
void drawIpodIcon(ImDrawList* dl, ImVec2 p);

// --- text ------------------------------------------------------------------

bool containsCi(const std::string& haystack, const std::string& lowerNeedle);
int cmpCi(const std::string& a, const std::string& b);

// ':'-separated iTunesDB location -> absolute path on the mounted device.
std::filesystem::path locationToPath(const std::filesystem::path& mount,
                                     const std::string& location);

std::string plural(int n, const char* one, const char* many);

// What the status bar says once an import batch drains.
std::string importSummary(int added, int skipped);

std::string formatTotalDuration(std::uint64_t ms);

}  // namespace podbox
