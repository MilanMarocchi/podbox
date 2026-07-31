#pragma once

#include "ui/theme.h"

#include <imgui.h>

// Drawing primitives for the Mac OS X "Aqua" look of the iTunes 7-9 era.
//
// ImGui's stock widgets are flat and rectangular; Aqua controls are glossy,
// rounded, and lit from above. These helpers draw the handful of shapes the
// app needs so dialogs and chrome read as period-correct rather than as a
// debug UI wearing a grey palette.
namespace podbox::aqua {

// The basic Aqua control body: a vertical gradient, a hairline border, and a
// bright top edge standing in for the gloss.
void gradientRect(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 top, ImU32 bottom,
                  ImU32 border, float rounding, bool gloss = true);

// A push button. `isDefault` gives it the blue treatment Mac OS X used for
// the action bound to Return.
bool button(const char* label, ImVec2 size = ImVec2(0, 0),
            bool isDefault = false);

// A round transport button (play, previous, next), drawn as a bezelled disc.
// Returns true when clicked. `glyph` is drawn by the caller inside the disc.
bool roundButton(const char* id, ImVec2 center, float radius, bool pressed);

// Begins a sheet: the modal Mac OS X dropped from a window's title bar. No
// chrome of its own, a soft grey face, and a shadow so it floats.
//
// Must be paired with endSheet() when it returns true. `width` is fixed
// because sheets do not resize.
bool beginSheet(const char* id, float width);
void endSheet();

// The standard sheet text block: a bold headline, then smaller grey prose.
void heading(const Fonts& f, const char* text);
void body(const Fonts& f, const char* fmt, ...) IM_FMTARGS(2);

// A hairline divider with the inset look Aqua used inside sheets.
void divider();

// The blue gradient a selected row wears in the source list and the column
// browser, with the lighter hairline along its top edge. Painted over an
// already-drawn flat Selectable fill.
void selectionGradient(ImDrawList* dl, ImVec2 a, ImVec2 b);

// Moves the cursor so a row of `count` buttons `width` wide (plus spacing)
// ends flush with the sheet's right edge, the way Mac dialogs align them.
void rightAlignButtons(int count, float width);

}  // namespace podbox::aqua
