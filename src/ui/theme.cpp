#include "ui/theme.h"

#include <cstring>
#include <filesystem>
#include <fstream>

namespace podbox {
namespace {

ImVec4 v4(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

void applyStyle() {
    ImGui::StyleColorsLight();
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 0.0f;
    s.ChildRounding = 0.0f;
    s.PopupRounding = 4.0f;
    s.FrameRounding = 3.0f;
    s.ScrollbarRounding = 8.0f;
    s.GrabRounding = 3.0f;
    s.WindowBorderSize = 0.0f;
    s.ChildBorderSize = 0.0f;
    s.FramePadding = ImVec2(6, 3);
    s.ItemSpacing = ImVec2(8, 5);
    s.CellPadding = ImVec2(6, 3);
    s.ScrollbarSize = 15.0f;

    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = v4(pal::rgb(30, 30, 30));
    c[ImGuiCol_TextDisabled] = v4(pal::rgb(128, 136, 146));
    c[ImGuiCol_WindowBg] = v4(pal::rgb(255, 255, 255));
    c[ImGuiCol_PopupBg] = v4(pal::rgb(246, 246, 246));
    c[ImGuiCol_Border] = v4(pal::rgb(170, 175, 182));
    c[ImGuiCol_FrameBg] = v4(pal::rgb(255, 255, 255));
    c[ImGuiCol_FrameBgHovered] = v4(pal::rgb(242, 246, 251));
    c[ImGuiCol_FrameBgActive] = v4(pal::rgb(229, 238, 248));
    c[ImGuiCol_Button] = v4(pal::rgb(250, 250, 250));
    c[ImGuiCol_ButtonHovered] = v4(pal::rgb(235, 241, 249));
    c[ImGuiCol_ButtonActive] = v4(pal::rgb(216, 227, 240));
    c[ImGuiCol_Header] = v4(pal::Selection);
    c[ImGuiCol_HeaderHovered] = v4(pal::rgb(76, 132, 220));
    c[ImGuiCol_HeaderActive] = v4(pal::rgb(46, 102, 197));
    c[ImGuiCol_ScrollbarBg] = v4(pal::rgb(247, 247, 247));
    c[ImGuiCol_ScrollbarGrab] = v4(pal::rgb(195, 195, 195));
    c[ImGuiCol_ScrollbarGrabHovered] = v4(pal::rgb(170, 170, 170));
    c[ImGuiCol_ScrollbarGrabActive] = v4(pal::rgb(150, 150, 150));
    c[ImGuiCol_Separator] = v4(pal::rgb(205, 209, 214));
    c[ImGuiCol_TableHeaderBg] = v4(pal::rgb(245, 245, 245));
    c[ImGuiCol_TableBorderStrong] = v4(pal::rgb(190, 194, 200));
    c[ImGuiCol_TableBorderLight] = v4(pal::rgb(222, 226, 231));
    c[ImGuiCol_TableRowBg] = v4(pal::rgb(255, 255, 255));
    c[ImGuiCol_TableRowBgAlt] = v4(pal::RowAlt);
    c[ImGuiCol_NavHighlight] = v4(pal::Selection);
}

// Number of faces in a TrueType file; > 1 for .ttc collections. Used to
// validate a face index before handing it to the font loader.
int ttcFaceCount(const char* path) {
    std::ifstream in(path, std::ios::binary);
    unsigned char h[12] = {};
    if (!in.read(reinterpret_cast<char*>(h), sizeof h)) return 0;
    if (std::memcmp(h, "ttcf", 4) != 0) return 1;
    return int((h[8] << 24) | (h[9] << 16) | (h[10] << 8) | h[11]);
}

}  // namespace

Fonts setupTheme(float contentScale) {
    applyStyle();

    ImGuiIO& io = ImGui::GetIO();
    Fonts f;

    // Lucida Grande was the Mac system font of the iTunes 7-9 era; its .ttc
    // holds the regular face at index 0 and bold at index 1.
    struct Candidate {
        const char* path;
        int boldNo;
    };
    constexpr Candidate kCandidates[] = {
        {"/System/Library/Fonts/LucidaGrande.ttc", 1},
        {"/System/Library/Fonts/Helvetica.ttc", 1},
        {"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", -1},
    };

    for (const Candidate& cand : kCandidates) {
        std::error_code ec;
        if (!std::filesystem::exists(cand.path, ec)) continue;
        const int faces = ttcFaceCount(cand.path);
        if (faces < 1) continue;
        const int boldNo =
            (cand.boldNo >= 0 && cand.boldNo < faces) ? cand.boldNo : 0;
        // Latin plus general punctuation (em-dashes, curly quotes, ellipsis)
        // — song metadata uses these constantly.
        static const ImWchar kRanges[] = {
            0x0020, 0x00FF, 0x0100, 0x017F, 0x2000, 0x206F, 0x2122, 0x2122, 0,
        };
        auto add = [&](float size, int fontNo) {
            ImFontConfig cfg;
            cfg.FontNo = fontNo;
            cfg.GlyphRanges = kRanges;
            return io.Fonts->AddFontFromFileTTF(cand.path, size * contentScale,
                                                &cfg);
        };
        f.ui = add(f.uiSize, 0);
        f.uiBold = add(f.uiSize, boldNo);
        f.label = add(f.labelSize, 0);
        f.labelBold = add(f.labelSize, boldNo);
        break;
    }

    if (f.ui) {
        io.FontGlobalScale = 1.0f / contentScale;
    } else {
        f.ui = f.uiBold = f.label = f.labelBold = io.Fonts->AddFontDefault();
        io.FontGlobalScale = 1.0f;
    }
    io.FontDefault = f.ui;
    return f;
}

}  // namespace podbox
