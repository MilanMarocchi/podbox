// Tests for the drawing helpers in src/app/app_util.
//
// These need a live ImGui context because measuring text needs a font atlas,
// which is why they are their own target rather than part of dedupe_test.
// No window and no renderer: the atlas is built in memory and thrown away.

#include "app/app_util.h"

#include <imgui.h>

#include <cstdio>
#include <string>

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("  FAIL: %s\n", what.c_str());
        ++failures;
    }
}

void checkEq(const std::string& got, const std::string& want,
             const std::string& what) {
    if (got != want) {
        std::printf("  FAIL: %s\n    got:  \"%s\"\n    want: \"%s\"\n",
                    what.c_str(), got.c_str(), want.c_str());
        ++failures;
    }
}

// True when every byte sequence in `s` is a complete UTF-8 code point. This is
// the property truncation must not break: half of a multi-byte character
// renders as a replacement glyph.
bool wellFormedUtf8(const std::string& s) {
    for (std::size_t i = 0; i < s.size();) {
        const unsigned char c = s[i];
        std::size_t len = 0;
        if (c < 0x80) len = 1;
        else if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        else return false;  // a continuation byte where one may not start
        if (i + len > s.size()) return false;
        for (std::size_t k = 1; k < len; ++k)
            if ((s[i + k] & 0xC0) != 0x80) return false;
        i += len;
    }
    return true;
}

float widthOf(ImFont* f, float size, const std::string& s) {
    return f->CalcTextSizeA(size, FLT_MAX, 0.0f, s.c_str()).x;
}

void testTruncate(ImFont* font, float size) {
    using podbox::truncateToWidth;
    std::printf("truncateToWidth\n");

    // Anything that already fits comes back untouched — no stray ellipsis on
    // a title that was never too long.
    const std::string shortStr = "Help!";
    checkEq(truncateToWidth(font, size, shortStr, 500.0f), shortStr,
            "text that fits is returned unchanged");
    checkEq(truncateToWidth(font, size, "", 100.0f), "",
            "empty stays empty");

    // The whole point: the result must fit the budget it was given.
    const std::string longStr =
        "Wraith Pinned To The Mist And Other Games (Deluxe Remaster)";
    for (float budget : {30.0f, 60.0f, 120.0f, 200.0f}) {
        const std::string cut = truncateToWidth(font, size, longStr, budget);
        check(widthOf(font, size, cut) <= budget,
              "result fits the budget at " + std::to_string(int(budget)) + "px");
        check(cut.size() < longStr.size(),
              "long text was actually shortened at " +
                  std::to_string(int(budget)) + "px");
    }

    // A budget too small even for the ellipsis yields nothing, rather than an
    // ellipsis that itself overflows.
    checkEq(truncateToWidth(font, size, longStr, 1.0f), "",
            "an impossible budget draws nothing");

    // Real metadata is full of accents and em-dashes. Cutting mid-sequence is
    // the bug this guards against.
    // Almost every character here is a two-byte sequence, so a byte-wise cut
    // lands mid-character constantly. A sparser string can pass by luck.
    const std::string accented =
        "ÁÉÍÓÚÑÜÀÈÌÒÙÂÊÎÔÛÄËÏÖåæøçñüÿ — Ágætis byrjun — Viðrar vel";
    check(wellFormedUtf8(accented), "the fixture itself is valid UTF-8");
    for (float budget = 2.0f; budget < 320.0f; budget += 1.0f) {
        const std::string cut = truncateToWidth(font, size, accented, budget);
        check(wellFormedUtf8(cut),
              "never splits a code point at " + std::to_string(int(budget)) +
                  "px");
        check(widthOf(font, size, cut) <= budget,
              "accented result fits at " + std::to_string(int(budget)) + "px");
    }
}

}  // namespace

int main() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->AddFontDefault();
    // A renderer would normally do this; without it CalcTextSizeA has no
    // glyphs to measure.
    unsigned char* pixels = nullptr;
    int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);

    ImFont* font = io.Fonts->Fonts[0];
    testTruncate(font, 13.0f);

    ImGui::DestroyContext();
    std::printf("\n%s\n", failures ? "FAILED" : "OK");
    return failures ? 1 : 0;
}
