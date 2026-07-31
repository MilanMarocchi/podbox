#include "ui/aqua.h"

#include <cstdarg>
#include <cstdio>

namespace podbox::aqua {
namespace {

ImVec4 v4(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

ImU32 withAlpha(ImU32 c, int a) {
    return (c & 0x00FFFFFFu) | (ImU32(a) << IM_COL32_A_SHIFT);
}

constexpr float kPi = 3.14159265358979323846f;

}  // namespace

void gradientRect(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 top, ImU32 bottom,
                  ImU32 border, float rounding, bool gloss) {
    dl->AddRectFilledMultiColor(a, b, top, top, bottom, bottom);
    if (rounding > 0.0f) {
        // AddRectFilledMultiColor cannot round, so lay a rounded fill of the
        // mid tone underneath and let the gradient show through the middle.
        dl->AddRectFilled(a, b, top, rounding);
        dl->AddRectFilledMultiColor(ImVec2(a.x, a.y + rounding),
                                    ImVec2(b.x, b.y - rounding), top, top,
                                    bottom, bottom);
        dl->AddRectFilled(ImVec2(a.x, b.y - rounding * 2.0f), b, bottom,
                          rounding);
    }
    if (gloss) {
        // The bright hairline just inside the top edge that gives Aqua its
        // lit-from-above feel.
        dl->AddLine(ImVec2(a.x + rounding, a.y + 1.0f),
                    ImVec2(b.x - rounding, a.y + 1.0f),
                    withAlpha(IM_COL32_WHITE, 190));
    }
    dl->AddRect(a, b, border, rounding);
}

bool button(const char* label, ImVec2 size, bool isDefault) {
    ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2 text = ImGui::CalcTextSize(label, nullptr, true);
    if (size.x <= 0.0f) size.x = text.x + style.FramePadding.x * 2.0f + 18.0f;
    if (size.y <= 0.0f) size.y = 21.0f;

    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(label, size);
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 a = p, b(p.x + size.x, p.y + size.y);

    ImU32 top, bottom, border, fg;
    if (isDefault) {
        // Aqua's default button: saturated blue, white label.
        top = held ? pal::rgb(60, 108, 190) : pal::DefaultBtnTop;
        bottom = held ? pal::rgb(38, 78, 155) : pal::DefaultBtnBottom;
        border = pal::DefaultBtnBorder;
        fg = IM_COL32_WHITE;
    } else {
        top = held ? pal::rgb(222, 222, 222) : IM_COL32_WHITE;
        bottom = held ? pal::rgb(198, 198, 198) : pal::ControlBottom;
        border = pal::ControlBorder;
        fg = pal::Text;
    }
    if (hovered && !held && !isDefault) bottom = pal::ControlHotBottom;

    // Small push buttons of this era were almost capsules.
    const float r = size.y * 0.5f;
    gradientRect(dl, a, b, top, bottom, border, r);

    dl->AddText(ImVec2(p.x + (size.x - text.x) * 0.5f,
                       p.y + (size.y - text.y) * 0.5f),
                fg, label);
    return clicked;
}

bool roundButton(const char* id, ImVec2 center, float radius, bool pressed) {
    ImGui::SetCursorScreenPos(ImVec2(center.x - radius, center.y - radius));
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(radius * 2, radius * 2));
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive() || pressed;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    // Bezel, then face, then the gloss arc across the top half.
    dl->AddCircleFilled(center, radius, pal::ControlBorder, 32);
    const ImU32 face = held ? pal::ControlOnBottom
                            : (hovered ? pal::ControlTop
                                       : pal::ControlBottom);
    dl->AddCircleFilled(center, radius - 1.0f, face, 32);
    dl->PathArcTo(ImVec2(center.x, center.y + 1.0f), radius - 2.5f, kPi,
                  kPi * 2.0f, 16);
    dl->PathStroke(withAlpha(IM_COL32_WHITE, held ? 60 : 170), 0, 1.5f);
    return clicked;
}

bool beginSheet(const char* id, float width) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    // Sheets drop from the top of the window, centred horizontally.
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f,
                                   vp->WorkPos.y + 34.0f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(width, 0.0f));

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20, 18));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, v4(pal::SheetBg));
    ImGui::PushStyleColor(ImGuiCol_Border, v4(pal::SheetBorder));

    const bool open = ImGui::BeginPopupModal(
        id, nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoSavedSettings);
    if (!open) {
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(3);
        return false;
    }
    return true;
}

void endSheet() {
    ImGui::EndPopup();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
}

void heading(const Fonts& f, const char* text) {
    ImGui::PushFont(f.uiBold);
    ImGui::PushStyleColor(ImGuiCol_Text, v4(pal::Text));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

void body(const Fonts& f, const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    ImGui::PushFont(f.label);
    ImGui::PushStyleColor(ImGuiCol_Text, v4(pal::StatusText));
    ImGui::TextWrapped("%s", buf);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

void selectionGradient(ImDrawList* dl, ImVec2 a, ImVec2 b) {
    dl->AddRectFilledMultiColor(a, b, pal::SelectionTop, pal::SelectionTop,
                                pal::SelectionBottom, pal::SelectionBottom);
    dl->AddLine(a, ImVec2(b.x, a.y), pal::SelectionEdge);
}

void divider() {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    // Two hairlines: the shadow above, the highlight below.
    dl->AddLine(ImVec2(p.x, p.y + 4.0f), ImVec2(p.x + w, p.y + 4.0f),
                pal::GridLine);
    dl->AddLine(ImVec2(p.x, p.y + 5.0f), ImVec2(p.x + w, p.y + 5.0f),
                withAlpha(IM_COL32_WHITE, 200));
    ImGui::Dummy(ImVec2(0, 10));
}

void rightAlignButtons(int count, float width) {
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float total = count * width + (count - 1) * spacing;
    const float avail = ImGui::GetContentRegionAvail().x;
    if (avail > total)
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - total));
}

}  // namespace podbox::aqua
