#include "app/app_util.h"

#include "library/dedupe.h"  // toLower

#include <algorithm>
#include <cfloat>
#include <cstdio>

#include <strings.h>  // strcasecmp

namespace fs = std::filesystem;

namespace podbox {

void addTextCentered(ImDrawList* dl, ImFont* font, float size, ImVec2 center,
                     ImU32 color, const char* text) {
    const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    dl->AddText(font, size,
                ImVec2(center.x - ts.x * 0.5f, center.y - ts.y * 0.5f), color,
                text);
}

std::string truncateToWidth(ImFont* font, float size, const std::string& text,
                            float maxWidth) {
    if (text.empty() || maxWidth <= 0.0f) return {};
    if (font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str()).x <= maxWidth)
        return text;

    // U+2026, which the loaded glyph ranges cover.
    static const char* kEllipsis = "\xE2\x80\xA6";
    const float dots =
        font->CalcTextSizeA(size, FLT_MAX, 0.0f, kEllipsis).x;
    if (dots > maxWidth) return {};

    // Step whole UTF-8 code points. ImGui charges a fallback glyph for a
    // partial sequence, so measurement alone happens to reject a mid-character
    // cut too — but that is an accident of how it counts, not a guarantee, and
    // half a character renders as a replacement glyph if it ever changes.
    // Linear rather than binary because this runs on a handful of strings a
    // frame, and a binary search would need the same boundary care anyway.
    std::size_t fit = 0;
    for (std::size_t i = 1; i <= text.size(); ++i) {
        if ((text[i] & 0xC0) == 0x80) continue;  // mid-sequence byte
        const float w =
            font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str(),
                                text.c_str() + i)
                .x;
        if (w + dots > maxWidth) break;
        fit = i;
    }
    return text.substr(0, fit) + kEllipsis;
}

void addTextCenteredFit(ImDrawList* dl, ImFont* font, float size, float x0,
                        float x1, float y, ImU32 color,
                        const std::string& text) {
    const std::string fitted = truncateToWidth(font, size, text, x1 - x0);
    if (fitted.empty()) return;
    addTextCentered(dl, font, size, ImVec2((x0 + x1) * 0.5f, y), color,
                    fitted.c_str());
}

int drawStars(ImDrawList* dl, ImVec2 p, std::uint8_t rating, bool hovered,
              ImVec2 mouse, bool clicked) {
    constexpr float kStep = 13.0f, kR = 5.0f;
    const int filled = rating / 20;
    int result = -1;
    for (int i = 0; i < 5; ++i) {
        const ImVec2 c(p.x + kStep * i + kR, p.y + kR);
        const bool on = i < filled;
        // A hovered row previews the rating the click would set.
        const bool preview =
            hovered && mouse.x >= p.x && mouse.x < p.x + kStep * 5 &&
            mouse.x >= c.x - kR;
        const ImU32 col = (on || preview) ? pal::StarOn
                                          : pal::StarOff;
        dl->AddCircleFilled(c, on || preview ? kR * 0.62f : kR * 0.34f, col, 8);
        if (clicked && hovered && mouse.x >= c.x - kR && mouse.x < c.x + kR)
            result = (i + 1) * 20;
    }
    // Clicking left of the first star clears the rating.
    if (clicked && hovered && mouse.x >= p.x - kStep * 0.6f && mouse.x < p.x)
        result = 0;
    return result;
}

void drawIpodIcon(ImDrawList* dl, ImVec2 p) {
    const float w = 11.0f, h = 16.0f;
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), pal::ControlTop,
                      2.5f);
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), pal::Glyph, 2.5f);
    dl->AddRectFilled(ImVec2(p.x + 2, p.y + 2), ImVec2(p.x + w - 2, p.y + 7),
                      pal::CapacityFill, 1.0f);
    dl->AddCircle(ImVec2(p.x + w * 0.5f, p.y + 11.5f), 2.6f,
                  pal::ControlBorder);
}

bool containsCi(const std::string& haystack, const std::string& lowerNeedle) {
    if (lowerNeedle.empty()) return true;
    return toLower(haystack).find(lowerNeedle) != std::string::npos;
}

int cmpCi(const std::string& a, const std::string& b) {
    return strcasecmp(a.c_str(), b.c_str());
}

fs::path locationToPath(const fs::path& mount, const std::string& location) {
    std::string rel = location;
    if (!rel.empty() && rel[0] == ':') rel.erase(0, 1);
    std::replace(rel.begin(), rel.end(), ':', '/');
    return mount / rel;
}

std::string plural(int n, const char* one, const char* many) {
    return std::to_string(n) + ' ' + (n == 1 ? one : many);
}

std::string importSummary(int added, int skipped) {
    if (added == 0 && skipped > 0)
        return skipped == 1
                   ? "That song is already on this iPod"
                   : "All " + plural(skipped, "song is", "songs are") +
                         " already on this iPod";
    std::string msg = "Added " + plural(added, "song", "songs");
    if (skipped > 0)
        msg += "  ·  skipped " + plural(skipped, "duplicate", "duplicates");
    return msg;
}

std::string formatTotalDuration(std::uint64_t ms) {
    const double minutes = double(ms) / 60000.0;
    char buf[32];
    if (minutes < 60.0)
        std::snprintf(buf, sizeof(buf), "%.0f minutes", minutes);
    else if (minutes < 60.0 * 24.0)
        std::snprintf(buf, sizeof(buf), "%.1f hours", minutes / 60.0);
    else
        std::snprintf(buf, sizeof(buf), "%.1f days", minutes / (60.0 * 24.0));
    return buf;
}

}  // namespace podbox
