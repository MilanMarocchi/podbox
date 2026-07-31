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
inline constexpr float kToolbarHeight = 56.0f;
inline constexpr float kStatusBarHeight = 24.0f;
inline constexpr float kLcdWidth = 360.0f;
inline constexpr float kLcdHeight = 40.0f;
inline constexpr float kSearchWidth = 170.0f;

// --- drawing ---------------------------------------------------------------

inline ImVec4 v4(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

void addTextCentered(ImDrawList* dl, ImFont* font, float size, ImVec2 center,
                     ImU32 color, const char* text);

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
