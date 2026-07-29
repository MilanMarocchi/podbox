#pragma once

#include <imgui.h>

namespace podbox {

// Fonts for the classic-iTunes look. `ui` is the general 13px text face,
// `label` the 11px face used for sidebar headers, legends and status text.
struct Fonts {
    ImFont* ui = nullptr;
    ImFont* uiBold = nullptr;
    ImFont* label = nullptr;
    ImFont* labelBold = nullptr;
    float uiSize = 13.0f;     // logical sizes, for ImDrawList::AddText
    float labelSize = 11.0f;
};

// Palette approximating iTunes 7-9 on Mac OS X.
namespace pal {
constexpr ImU32 rgb(int r, int g, int b) { return IM_COL32(r, g, b, 255); }

inline constexpr ImU32 ToolbarTop = rgb(236, 236, 236);
inline constexpr ImU32 ToolbarBottom = rgb(199, 199, 199);
inline constexpr ImU32 ToolbarBorder = rgb(128, 128, 128);

inline constexpr ImU32 LcdBg = rgb(222, 229, 234);
inline constexpr ImU32 LcdBorder = rgb(148, 157, 166);
inline constexpr ImU32 LcdText = rgb(55, 65, 75);
inline constexpr ImU32 LcdTextDim = rgb(105, 115, 125);

inline constexpr ImU32 SidebarBg = rgb(214, 221, 229);
inline constexpr ImU32 SidebarBorder = rgb(156, 164, 174);
inline constexpr ImU32 SidebarHeader = rgb(101, 115, 131);

inline constexpr ImU32 Selection = rgb(56, 117, 215);   // #3875D7
inline constexpr ImU32 RowAlt = rgb(240, 245, 253);

inline constexpr ImU32 CapacityBg = rgb(238, 238, 238);
inline constexpr ImU32 CapacityFill = rgb(84, 132, 214);
inline constexpr ImU32 CapacityBorder = rgb(147, 155, 164);

inline constexpr ImU32 StatusTop = rgb(247, 247, 247);
inline constexpr ImU32 StatusBottom = rgb(220, 220, 220);
inline constexpr ImU32 StatusBorder = rgb(160, 160, 160);
inline constexpr ImU32 StatusText = rgb(90, 90, 90);

inline constexpr ImU32 TextDim = rgb(110, 118, 128);
}  // namespace pal

// Applies the iTunes-like light style and loads era-appropriate fonts
// (Lucida Grande, with fallbacks). `contentScale` is the monitor content
// scale (2.0 on retina) so glyphs rasterize crisply.
Fonts setupTheme(float contentScale);

}  // namespace podbox
