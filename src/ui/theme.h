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

// Palette approximating iTunes 10 on Mac OS X. It is the era's flatter,
// lighter chrome: the brushed-metal contrast of iTunes 7-9 is gone, the LCD
// went pale grey-green, and the bezels got thinner.
//
// This is meant to be the single source of truth. If you find yourself
// writing pal::rgb(...) at a call site, add a name here instead — the one
// exception being a pressed or hovered shade derived from a colour that is
// already named, which is local to the control that draws it.
namespace pal {
constexpr ImU32 rgb(int r, int g, int b) { return IM_COL32(r, g, b, 255); }
constexpr ImU32 alpha(ImU32 c, int a) {
    return (c & 0x00FFFFFFu) | (ImU32(a) << IM_COL32_A_SHIFT);
}

inline constexpr ImU32 Text = rgb(30, 30, 30);
inline constexpr ImU32 TextDim = rgb(116, 122, 130);
inline constexpr ImU32 Warning = rgb(160, 60, 20);
inline constexpr ImU32 Danger = rgb(160, 40, 40);
inline constexpr ImU32 Success = rgb(20, 110, 30);

inline constexpr ImU32 ToolbarTop = rgb(246, 246, 246);
inline constexpr ImU32 ToolbarBottom = rgb(214, 214, 214);
inline constexpr ImU32 ToolbarBorder = rgb(148, 148, 148);

// The centre readout. iTunes 10's is flat and faintly green, with none of
// the glossy top wash the earlier versions had.
inline constexpr ImU32 LcdBg = rgb(226, 231, 222);
inline constexpr ImU32 LcdBgTop = rgb(232, 236, 228);
inline constexpr ImU32 LcdBgBottom = rgb(216, 222, 212);
inline constexpr ImU32 LcdBorder = rgb(142, 148, 138);
inline constexpr ImU32 LcdText = rgb(38, 44, 38);
inline constexpr ImU32 LcdTextDim = rgb(96, 104, 94);
inline constexpr ImU32 LcdProgressBg = rgb(190, 196, 186);
inline constexpr ImU32 LcdProgressFill = rgb(74, 124, 208);
inline constexpr ImU32 LcdProgressKnob = rgb(60, 104, 186);

// Generic light control face: the transport capsule, aqua::button, the
// view-mode segments.
inline constexpr ImU32 ControlTop = rgb(253, 253, 253);
inline constexpr ImU32 ControlBottom = rgb(226, 226, 228);
inline constexpr ImU32 ControlBorder = rgb(154, 156, 160);
inline constexpr ImU32 ControlDivider = rgb(196, 199, 203);
inline constexpr ImU32 ControlOnTop = rgb(150, 156, 164);  // pressed: inset
inline constexpr ImU32 ControlOnBottom = rgb(190, 195, 202);
inline constexpr ImU32 ControlHotBottom = rgb(236, 236, 238);
inline constexpr ImU32 DefaultBtnTop = rgb(126, 174, 235);
inline constexpr ImU32 DefaultBtnBottom = rgb(56, 116, 205);
inline constexpr ImU32 DefaultBtnBorder = rgb(52, 100, 170);

// Hand-drawn glyphs: transport arrows, eject, speaker, stars, the iPod.
inline constexpr ImU32 Glyph = rgb(74, 80, 88);
inline constexpr ImU32 GlyphDim = rgb(168, 172, 178);
inline constexpr ImU32 GlyphHot = rgb(50, 56, 64);
inline constexpr ImU32 GlyphOn = rgb(48, 106, 200);  // shuffle/repeat engaged

inline constexpr ImU32 SidebarBg = rgb(224, 229, 236);
inline constexpr ImU32 SidebarBorder = rgb(160, 166, 175);
inline constexpr ImU32 SidebarHeader = rgb(120, 130, 144);
inline constexpr ImU32 SidebarText = rgb(38, 42, 48);

inline constexpr ImU32 Selection = rgb(56, 117, 215);  // #3875D7
inline constexpr ImU32 SelectionTop = rgb(122, 166, 228);
inline constexpr ImU32 SelectionBottom = rgb(48, 106, 200);
inline constexpr ImU32 SelectionEdge = rgb(155, 190, 238);
inline constexpr ImU32 RowAlt = rgb(242, 246, 252);

// Column browser (Genres | Artists | Albums).
inline constexpr ImU32 BrowserBg = rgb(255, 255, 255);
inline constexpr ImU32 BrowserRowAlt = rgb(243, 246, 251);
inline constexpr ImU32 BrowserHeadTop = rgb(246, 247, 248);
inline constexpr ImU32 BrowserHeadBottom = rgb(223, 225, 229);
inline constexpr ImU32 BrowserHeadText = rgb(48, 52, 58);
inline constexpr ImU32 BrowserBorder = rgb(168, 172, 179);

// Track table.
inline constexpr ImU32 HeaderTop = rgb(250, 250, 251);
inline constexpr ImU32 HeaderBottom = rgb(228, 229, 233);
inline constexpr ImU32 HeaderBorder = rgb(170, 174, 180);
inline constexpr ImU32 HeaderSortTop = rgb(214, 226, 243);
inline constexpr ImU32 HeaderSortBottom = rgb(188, 206, 234);
// Semi-transparent so the row striping still reads through the sorted column.
inline constexpr ImU32 SortColumnTint = IM_COL32(216, 229, 246, 110);
inline constexpr ImU32 GridLine = rgb(224, 227, 232);
inline constexpr ImU32 StarOn = rgb(70, 110, 190);
inline constexpr ImU32 StarOff = rgb(200, 202, 206);

inline constexpr ImU32 CapacityBg = rgb(240, 240, 241);
inline constexpr ImU32 CapacityFill = rgb(74, 124, 208);
inline constexpr ImU32 CapacityBorder = rgb(158, 163, 170);

inline constexpr ImU32 StatusTop = rgb(246, 246, 246);
inline constexpr ImU32 StatusBottom = rgb(216, 216, 216);
inline constexpr ImU32 StatusBorder = rgb(150, 150, 150);
inline constexpr ImU32 StatusText = rgb(88, 92, 98);

inline constexpr ImU32 ArtworkWell = rgb(255, 255, 255);
inline constexpr ImU32 ArtworkBorder = rgb(168, 174, 182);

inline constexpr ImU32 SheetBg = rgb(237, 237, 237);
inline constexpr ImU32 SheetBorder = rgb(126, 129, 133);
}  // namespace pal

// Applies the iTunes-like light style and loads era-appropriate fonts
// (Lucida Grande, with fallbacks). `contentScale` is the monitor content
// scale (2.0 on retina) so glyphs rasterize crisply.
Fonts setupTheme(float contentScale);

}  // namespace podbox
