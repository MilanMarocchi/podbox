// The album grid: iTunes' Grid view. It shows the same rows the track table
// would — search, column browser and source all apply — grouped into albums
// with their covers. Clicking a cover opens the album's songs in a panel
// below its row, the way iTunes 11 did it.

#include "app/app.h"
#include "app/app_util.h"

#include "library/artwork.h"
#include "library/dedupe.h"
#include "ui/aqua.h"
#include "ui/theme.h"

#include <imgui.h>

#define GL_SILENCE_DEPRECATION
#if defined(__APPLE__)
#include <OpenGL/gl3.h>
#else
#include <GL/gl.h>
#endif

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_map>

namespace fs = std::filesystem;

namespace podbox {

namespace {

constexpr float kCover = 150.0f;
constexpr float kLabelHeight = 36.0f;
constexpr float kRowGap = 14.0f;
constexpr float kMargin = 22.0f;
constexpr float kMinGap = 16.0f;
constexpr float kMaxGap = 40.0f;
constexpr float kPanelCover = 180.0f;
constexpr float kPanelPad = 18.0f;
constexpr float kPanelHeader = 46.0f;
constexpr float kSongRow = 20.0f;
// Pixels on the longest edge: a cover is drawn at 150 points, which is 300
// pixels on a Retina display.
constexpr int kThumbEdge = 300;
// Decodes per worker run, so the first covers arrive quickly and a scroll
// does not wait behind a long queue for covers no longer on screen.
constexpr std::size_t kCoverBatch = 8;
// Past this many textures, ones not drawn recently are released.
constexpr std::size_t kCoverBudget = 300;
constexpr double kCoverIdleSeconds = 10.0;

// Compilations have many artists and one album, so the album name alone is
// the key. Songs with no album at all are grouped per artist instead of
// collapsing every loose single into one tile.
std::string albumKey(const Track& t) {
    return t.album.empty() ? "\x01" + toLower(t.artist) : toLower(t.album);
}

void drawNoteGlyph(ImDrawList* dl, ImVec2 c, float s, ImU32 col) {
    // Two beamed quavers, scaled to the well.
    const float r = s * 0.11f;
    const ImVec2 h1(c.x - s * 0.16f, c.y + s * 0.17f);
    const ImVec2 h2(c.x + s * 0.16f, c.y + s * 0.11f);
    dl->AddCircleFilled(h1, r, col, 20);
    dl->AddCircleFilled(h2, r, col, 20);
    const float stem = s * 0.04f;
    const float top1 = c.y - s * 0.2f, top2 = c.y - s * 0.26f;
    dl->AddRectFilled(ImVec2(h1.x + r - stem, top1), ImVec2(h1.x + r, h1.y), col);
    dl->AddRectFilled(ImVec2(h2.x + r - stem, top2), ImVec2(h2.x + r, h2.y), col);
    dl->AddQuadFilled(ImVec2(h1.x + r - stem, top1), ImVec2(h2.x + r - stem, top2),
                      ImVec2(h2.x + r, top2 + s * 0.08f),
                      ImVec2(h1.x + r, top1 + s * 0.08f), col);
}

}  // namespace

void App::groupVisibleIntoAlbums() {
    grid_.albums.clear();
    const Library* lib = shownLibrary();
    if (!lib) return;

    std::unordered_map<std::string, int> byKey;
    std::vector<std::vector<std::pair<int, int>>> rows;
    for (const auto& row : visible_) {
        const Track& t = lib->tracks[row.second];
        std::string key = albumKey(t);
        const auto [it, fresh] = byKey.emplace(key, int(rows.size()));
        if (fresh) {
            rows.emplace_back();
            AlbumTile a;
            a.key = std::move(key);
            a.album = t.album.empty() ? "Unknown Album" : t.album;
            a.artist = t.artist;
            grid_.albums.push_back(std::move(a));
        }
        AlbumTile& a = grid_.albums[it->second];
        rows[it->second].push_back(row);
        if (a.artist.empty())
            a.artist = t.artist;
        else if (!t.artist.empty() && cmpCi(a.artist, t.artist) != 0)
            a.artist = "Various Artists";
        a.year = std::max(a.year, t.year);
        a.totalMs += t.lengthMs;
    }
    for (AlbumTile& a : grid_.albums)
        if (a.artist.empty()) a.artist = "Unknown Artist";

    // Albums by artist then title; songs in disc and track order.
    std::vector<int> order(grid_.albums.size());
    for (int i = 0; i < int(order.size()); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](int x, int y) {
        const AlbumTile& a = grid_.albums[x];
        const AlbumTile& b = grid_.albums[y];
        int c = cmpCi(a.artist, b.artist);
        if (c == 0) c = cmpCi(a.album, b.album);
        return c < 0;
    });
    const auto& tracks = lib->tracks;
    std::vector<AlbumTile> sorted;
    sorted.reserve(order.size());
    visible_.clear();
    for (const int i : order) {
        auto& songs = rows[i];
        std::stable_sort(songs.begin(), songs.end(),
                         [&](const auto& x, const auto& y) {
                             const Track& a = tracks[x.second];
                             const Track& b = tracks[y.second];
                             if (a.discNumber != b.discNumber)
                                 return a.discNumber < b.discNumber;
                             if (a.trackNumber != b.trackNumber)
                                 return a.trackNumber < b.trackNumber;
                             return cmpCi(a.title, b.title) < 0;
                         });
        AlbumTile a = std::move(grid_.albums[i]);
        a.begin = int(visible_.size());
        visible_.insert(visible_.end(), songs.begin(), songs.end());
        a.end = int(visible_.size());
        a.coverFile = trackFilePath(tracks[songs.front().second]);
        sorted.push_back(std::move(a));
    }
    grid_.albums = std::move(sorted);
}

unsigned int App::gridCoverTexture(const fs::path& file, bool* loaded) {
    const auto it = grid_.covers.find(file.string());
    if (it == grid_.covers.end() || !it->second.loaded) {
        if (std::find(grid_.wanted.begin(), grid_.wanted.end(), file) ==
            grid_.wanted.end())
            grid_.wanted.push_back(file);
        *loaded = false;
        return 0;
    }
    it->second.lastUsed = ImGui::GetTime();
    *loaded = true;
    return it->second.texture;
}

void App::updateGridCovers() {
    const double now = ImGui::GetTime();
    if (grid_.job.ready()) {
        std::vector<ArtImage> images;
        try { images = std::move(*grid_.job.take()); } catch (...) {}
        for (std::size_t i = 0; i < grid_.jobKeys.size(); ++i) {
            AlbumGrid::Cover& c = grid_.covers[grid_.jobKeys[i]];
            c.loaded = true;
            c.lastUsed = now;
            if (i >= images.size() || !images[i].ok()) continue;
            const ArtImage& img = images[i];
            if (c.texture == 0) glGenTextures(1, &c.texture);
            glBindTexture(GL_TEXTURE_2D, c.texture);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                            GL_LINEAR_MIPMAP_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, img.width, img.height, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, img.rgba.data());
            glGenerateMipmap(GL_TEXTURE_2D);
        }
        grid_.jobKeys.clear();
    }

    // Reads come off the player too, so they stop for anything that needs
    // it to themselves.
    const bool mayRead = !closeRequested_ && ejectingMount_.empty() &&
                         ejectRequestedMount_.empty() && !deviceJob_.busy();
    if (!grid_.job.busy() && mayRead && !grid_.wanted.empty()) {
        std::vector<fs::path> batch;
        for (const fs::path& p : grid_.wanted) {
            if (batch.size() == kCoverBatch) break;
            batch.push_back(p);
            grid_.jobKeys.push_back(p.string());
        }
        grid_.job.start([batch] {
            std::vector<ArtImage> out;
            out.reserve(batch.size());
            for (const fs::path& p : batch)
                out.push_back(loadArtworkThumbnail(p, kThumbEdge));
            return out;
        });
    }
    // Rebuilt by the next draw, so a cover scrolled past is not decoded.
    grid_.wanted.clear();

    if (grid_.covers.size() > kCoverBudget) {
        for (auto it = grid_.covers.begin(); it != grid_.covers.end();) {
            if (it->second.loaded && now - it->second.lastUsed > kCoverIdleSeconds) {
                if (it->second.texture) glDeleteTextures(1, &it->second.texture);
                it = grid_.covers.erase(it);
            } else {
                ++it;
            }
        }
    }
}

void App::drawLayoutSwitch(float x) {
    constexpr float kSegW = 30.0f, kSegH = 22.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos();
    const float y = (kToolbarHeight - kSegH) * 0.5f;
    const ImVec2 a(wp.x + x, wp.y + y);
    const ImVec2 b(a.x + 2.0f * kSegW, a.y + kSegH);
    aqua::gradientRect(dl, a, b, pal::ControlTop, pal::ControlBottom,
                       pal::ControlBorder, 4.0f);

    const Layout layouts[2] = {Layout::List, Layout::Grid};
    const char* tips[2] = {"List", "Grid"};
    for (int i = 0; i < 2; ++i) {
        const ImVec2 s0(a.x + kSegW * i, a.y), s1(s0.x + kSegW, b.y);
        ImGui::SetCursorPos(ImVec2(x + kSegW * i, y));
        ImGui::PushID(i);
        const bool clicked = ImGui::InvisibleButton("##layout", ImVec2(kSegW, kSegH));
        ImGui::PopID();
        const bool hot = ImGui::IsItemHovered();
        if (hot) ImGui::SetTooltip("%s", tips[i]);
        const bool on = layout_ == layouts[i];
        if (on || ImGui::IsItemActive()) {
            // Pressed in: the darker inset face, square on the inner edge.
            const ImDrawFlags corners = i == 0 ? ImDrawFlags_RoundCornersLeft
                                               : ImDrawFlags_RoundCornersRight;
            dl->AddRectFilled(ImVec2(s0.x + 1, s0.y + 1), ImVec2(s1.x - 1, s1.y - 1),
                              pal::ControlOnBottom, 3.0f, corners);
            dl->AddRectFilledMultiColor(
                ImVec2(s0.x + (i == 0 ? 3 : 1), s0.y + 1),
                ImVec2(s1.x - (i == 0 ? 1 : 3), s0.y + kSegH * 0.5f),
                pal::ControlOnTop, pal::ControlOnTop, pal::ControlOnBottom,
                pal::ControlOnBottom);
        }
        const ImU32 col = on ? pal::GlyphHot : hot ? pal::GlyphHot : pal::Glyph;
        const ImVec2 c((s0.x + s1.x) * 0.5f, (s0.y + s1.y) * 0.5f);
        if (layouts[i] == Layout::List) {
            for (int r = -1; r <= 1; ++r) {
                const float ly = c.y + r * 4.0f;
                dl->AddRectFilled(ImVec2(c.x - 7, ly - 1), ImVec2(c.x - 5, ly + 1), col);
                dl->AddRectFilled(ImVec2(c.x - 3, ly - 1), ImVec2(c.x + 7, ly + 1), col);
            }
        } else {
            for (int gy = 0; gy < 2; ++gy)
                for (int gx = 0; gx < 2; ++gx) {
                    const ImVec2 q(c.x - 6.5f + gx * 7.0f, c.y - 6.5f + gy * 7.0f);
                    dl->AddRectFilled(q, ImVec2(q.x + 6, q.y + 6), col, 1.0f);
                }
        }
        if (clicked && layout_ != layouts[i]) {
            layout_ = layouts[i];
            visibleDirty_ = true;
            // From the device pane there is nothing to lay out yet.
            if (view_ == View::Device)
                switchSource(library_ ? View::Music : View::Library);
        }
    }
    dl->AddLine(ImVec2(a.x + kSegW, a.y + 1), ImVec2(a.x + kSegW, b.y - 1),
                pal::ControlBorder);
}

void App::drawAlbumGrid() {
    const Library* shown = shownLibrary();
    if (!shown) return;
    // A source that became showable without a rebuild has rows but no albums.
    if (grid_.albums.empty() && !visible_.empty()) groupVisibleIntoAlbums();

    ImGui::PushStyleColor(ImGuiCol_ChildBg, v4(pal::BrowserBg));
    ImGui::BeginChild("albumgrid", ImVec2(0, 0));
    ImGui::PopStyleColor();

    if (grid_.albums.empty()) {
        const char* msg = search_[0] ? "No albums match your search" : "No albums";
        const ImVec2 avail = ImGui::GetWindowSize();
        const ImVec2 ts = ImGui::CalcTextSize(msg);
        ImGui::SetCursorPos(ImVec2((avail.x - ts.x) * 0.5f, (avail.y - ts.y) * 0.5f));
        ImGui::TextDisabled("%s", msg);
        ImGui::EndChild();
        return;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float viewH = ImGui::GetWindowHeight();
    const float scrollY = ImGui::GetScrollY();

    const int cols = std::max(1, int((width - 2.0f * kMargin + kMinGap) /
                                     (kCover + kMinGap)));
    const float gap =
        cols > 1 ? std::clamp((width - 2.0f * kMargin - cols * kCover) / (cols - 1),
                              kMinGap, kMaxGap)
                 : 0.0f;
    const float left = std::max(
        kMargin * 0.5f, (width - cols * kCover - (cols - 1) * gap) * 0.5f);
    const float rowH = kCover + kLabelHeight + kRowGap;

    int openIndex = -1;
    for (int i = 0; i < int(grid_.albums.size()); ++i)
        if (grid_.albums[i].key == grid_.openKey) openIndex = i;
    if (openIndex < 0) grid_.openKey.clear();

    // How tall the open album's panel is, at this width.
    float panelH = 0.0f;
    if (openIndex >= 0) {
        const AlbumTile& a = grid_.albums[openIndex];
        const int n = a.end - a.begin;
        // The same split drawAlbumTracks makes.
        const float listW = width - 2.0f * kPanelPad - kPanelCover - kPanelPad;
        const int songCols = n > 10 && listW > 560.0f ? 2 : 1;
        const int perCol = (n + songCols - 1) / songCols;
        panelH = std::max(kPanelCover + 2.0f * kPanelPad,
                          kPanelHeader + perCol * kSongRow + 2.0f * kPanelPad);
    }

    float y = kMargin;
    std::string toggled;
    const int rows = (int(grid_.albums.size()) + cols - 1) / cols;
    for (int r = 0; r < rows; ++r) {
        const float rowTop = y;
        bool panelHere = false;
        for (int c = 0; c < cols; ++c) {
            const int i = r * cols + c;
            if (i >= int(grid_.albums.size())) break;
            const AlbumTile& a = grid_.albums[i];
            const bool open = i == openIndex;
            panelHere |= open;
            const ImVec2 p0(origin.x + left + c * (kCover + gap), origin.y + y);
            const ImVec2 p1(p0.x + kCover, p0.y + kCover);
            if (!ImGui::IsRectVisible(p0, ImVec2(p1.x, p1.y + kLabelHeight)))
                continue;

            bool loaded = false;
            const unsigned int tex = gridCoverTexture(a.coverFile, &loaded);
            dl->AddRectFilled(ImVec2(p0.x + 1, p0.y + 2), ImVec2(p1.x + 2, p1.y + 3),
                              pal::CoverShadow, 2.0f);
            if (tex) {
                dl->AddImage((ImTextureID)(intptr_t)tex, p0, p1);
            } else {
                dl->AddRectFilledMultiColor(p0, p1, pal::CoverPlaceholderTop,
                                            pal::CoverPlaceholderTop,
                                            pal::CoverPlaceholderBottom,
                                            pal::CoverPlaceholderBottom);
                if (loaded)
                    drawNoteGlyph(dl, ImVec2((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f),
                                  kCover, pal::CoverPlaceholderGlyph);
            }
            dl->AddRect(p0, p1, open ? pal::Selection : pal::ArtworkBorder, 0.0f, 0,
                        open ? 3.0f : 1.0f);

            const std::string title =
                truncateToWidth(fonts_.ui, fonts_.uiSize, a.album, kCover);
            const std::string artist =
                truncateToWidth(fonts_.label, fonts_.labelSize, a.artist, kCover);
            dl->AddText(fonts_.ui, fonts_.uiSize, ImVec2(p0.x, p1.y + 5.0f),
                        open ? pal::Selection : pal::Text, title.c_str());
            dl->AddText(fonts_.label, fonts_.labelSize, ImVec2(p0.x, p1.y + 21.0f),
                        pal::TextDim, artist.c_str());

            // A click only opens the album; its songs are picked in the
            // panel. Right-clicking the cover is how to act on all of them,
            // so that selects the whole album for the context menu.
            auto selectAlbum = [&] {
                selection_.clear();
                for (int row = a.begin; row < a.end; ++row)
                    selection_.push_back(shown->tracks[visible_[row].second].id);
                selectedTrackId_ = selection_.front();
                selectionAnchor_ = selectedTrackId_;
            };
            ImGui::SetCursorScreenPos(p0);
            ImGui::PushID(a.key.c_str());
            if (ImGui::InvisibleButton("##tile", ImVec2(kCover, kCover + kLabelHeight))) {
                selection_.clear();
                selectedTrackId_ = selectionAnchor_ = 0;
                toggled = a.key;
            }
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) selectAlbum();
            if (ImGui::IsItemHovered() &&
                ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                // The first click already opened it; keep it open.
                toggled.clear();
                grid_.openKey = a.key;
                playTrackId(shown->tracks[visible_[a.begin].second].id);
            }
            trackContextMenu(shown->tracks[visible_[a.begin].second]);
            ImGui::PopID();
        }
        y += rowH;

        if (panelHere) {
            const AlbumTile& a = grid_.albums[openIndex];
            // A notch under the open cover, pointing into its panel.
            const int c = openIndex % cols;
            const float nx = origin.x + left + c * (kCover + gap) + kCover * 0.5f;
            const float top = origin.y + y - kRowGap * 0.5f;
            const ImVec2 q0(origin.x, top), q1(origin.x + width, top + panelH);
            dl->AddRectFilledMultiColor(q0, q1, pal::AlbumPanelTop, pal::AlbumPanelTop,
                                        pal::AlbumPanelBottom, pal::AlbumPanelBottom);
            dl->AddLine(ImVec2(q0.x, q0.y + 0.5f), ImVec2(q1.x, q0.y + 0.5f),
                        pal::AlbumPanelBorder);
            dl->AddLine(ImVec2(q0.x, q1.y - 0.5f), ImVec2(q1.x, q1.y - 0.5f),
                        pal::AlbumPanelBorder);
            dl->AddTriangleFilled(ImVec2(nx - 10, top + 1), ImVec2(nx + 10, top + 1),
                                  ImVec2(nx, top - 9), pal::AlbumPanelTop);
            dl->AddLine(ImVec2(nx - 10, top + 0.5f), ImVec2(nx, top - 9.5f),
                        pal::AlbumPanelBorder);
            dl->AddLine(ImVec2(nx, top - 9.5f), ImVec2(nx + 10, top + 0.5f),
                        pal::AlbumPanelBorder);
            dl->AddLine(ImVec2(nx - 9, top + 0.5f), ImVec2(nx + 9, top + 0.5f),
                        pal::AlbumPanelTop);
            drawAlbumTracks(a, q0, width);

            if (grid_.scrollToOpen >= 0.0f) {
                // Bring the whole panel into view, but never scroll the
                // clicked cover itself off the top.
                const float panelBottom = y - kRowGap * 0.5f + panelH + kRowGap;
                if (panelBottom > scrollY + viewH)
                    ImGui::SetScrollY(std::min(rowTop - kRowGap, panelBottom - viewH));
                grid_.scrollToOpen = -1.0f;
            }
            y += panelH + kRowGap;
        }
    }
    y += kMargin;

    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(ImVec2(width, y));

    if (!toggled.empty()) {
        grid_.openKey = grid_.openKey == toggled ? std::string() : toggled;
        if (!grid_.openKey.empty()) grid_.scrollToOpen = 0.0f;
    }
    handleTrackTableKeys();
    ImGui::EndChild();
}

void App::drawAlbumTracks(const AlbumTile& a, ImVec2 o, float width) {
    const Library* shown = shownLibrary();
    if (!shown) return;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // The cover, larger.
    const ImVec2 c0(o.x + kPanelPad, o.y + kPanelPad);
    const ImVec2 c1(c0.x + kPanelCover, c0.y + kPanelCover);
    bool loaded = false;
    const unsigned int tex = gridCoverTexture(a.coverFile, &loaded);
    dl->AddRectFilled(ImVec2(c0.x + 1, c0.y + 2), ImVec2(c1.x + 2, c1.y + 3),
                      pal::CoverShadow);
    if (tex) {
        dl->AddImage((ImTextureID)(intptr_t)tex, c0, c1);
    } else {
        dl->AddRectFilledMultiColor(c0, c1, pal::CoverPlaceholderTop,
                                    pal::CoverPlaceholderTop,
                                    pal::CoverPlaceholderBottom,
                                    pal::CoverPlaceholderBottom);
        if (loaded)
            drawNoteGlyph(dl, ImVec2((c0.x + c1.x) * 0.5f, (c0.y + c1.y) * 0.5f),
                          kPanelCover, pal::CoverPlaceholderGlyph);
    }
    dl->AddRect(c0, c1, pal::ArtworkBorder);

    // Title, then artist · year · count and length.
    const float lx = c1.x + kPanelPad;
    const float lw = std::max(60.0f, o.x + width - kPanelPad - lx);
    const int n = a.end - a.begin;
    dl->AddText(fonts_.uiBold, fonts_.uiSize * 1.25f, ImVec2(lx, c0.y),
                pal::Text,
                truncateToWidth(fonts_.uiBold, fonts_.uiSize * 1.25f, a.album, lw)
                    .c_str());
    std::string sub = a.artist;
    if (a.year) sub += "  ·  " + std::to_string(a.year);
    sub += "  ·  " + plural(n, "song", "songs") + ", " +
           formatTotalDuration(a.totalMs);
    dl->AddText(fonts_.label, fonts_.labelSize, ImVec2(lx, c0.y + 21.0f),
                pal::TextDim,
                truncateToWidth(fonts_.label, fonts_.labelSize, sub, lw).c_str());

    // The songs, in one column or two.
    const int songCols = n > 10 && lw > 560.0f ? 2 : 1;
    const int perCol = (n + songCols - 1) / songCols;
    const float colGap = 24.0f;
    const float colW = (lw - (songCols - 1) * colGap) / songCols;
    const float listTop = o.y + kPanelPad + kPanelHeader;
    const ImGuiIO& io = ImGui::GetIO();
    // Disc numbers only matter when there is more than one disc.
    bool multiDisc = false;
    for (int row = a.begin + 1; row < a.end; ++row)
        if (shown->tracks[visible_[row].second].discNumber !=
            shown->tracks[visible_[a.begin].second].discNumber)
            multiDisc = true;

    for (int k = 0; k < n; ++k) {
        const int row = a.begin + k;
        const Track& t = shown->tracks[visible_[row].second];
        const int col = k / perCol, line = k % perCol;
        const ImVec2 r0(lx + col * (colW + colGap), listTop + line * kSongRow);
        const ImVec2 r1(r0.x + colW, r0.y + kSongRow);
        const bool selected = isSelected(t.id);
        if (selected)
            aqua::selectionGradient(dl, r0, r1);
        else if (line % 2 == 1)
            dl->AddRectFilled(r0, r1, pal::alpha(pal::RowAlt, 150));
        const ImU32 text = selected ? IM_COL32_WHITE : pal::Text;
        const ImU32 dim = selected ? IM_COL32_WHITE : pal::TextDim;
        const float ty = r0.y + (kSongRow - fonts_.uiSize) * 0.5f;

        // Track number, or a speaker-ish play mark on the one playing.
        if (t.id == playingTrackId_) {
            const float cy = r0.y + kSongRow * 0.5f;
            dl->AddTriangleFilled(ImVec2(r0.x + 8, cy - 4.5f), ImVec2(r0.x + 8, cy + 4.5f),
                                  ImVec2(r0.x + 15, cy), selected ? text : pal::Selection);
        } else {
            char num[16] = "";
            if (t.trackNumber && multiDisc && t.discNumber)
                std::snprintf(num, sizeof(num), "%u-%u", t.discNumber, t.trackNumber);
            else if (t.trackNumber)
                std::snprintf(num, sizeof(num), "%u", t.trackNumber);
            const float nw = fonts_.label->CalcTextSizeA(fonts_.labelSize, FLT_MAX, 0, num).x;
            dl->AddText(fonts_.label, fonts_.labelSize,
                        ImVec2(r0.x + 26.0f - nw, ty + 1.0f), dim, num);
        }
        const std::string time = formatDuration(t.lengthMs);
        const float tw = fonts_.label->CalcTextSizeA(fonts_.labelSize, FLT_MAX, 0,
                                                     time.c_str()).x;
        dl->AddText(fonts_.label, fonts_.labelSize, ImVec2(r1.x - 6.0f - tw, ty + 1.0f),
                    dim, time.c_str());
        // A compilation names each song's artist; an album by one artist
        // already says it in the header.
        std::string name = t.title;
        if (a.artist == "Various Artists" && !t.artist.empty())
            name += "  —  " + t.artist;
        dl->AddText(fonts_.ui, fonts_.uiSize, ImVec2(r0.x + 34.0f, ty), text,
                    truncateToWidth(fonts_.ui, fonts_.uiSize, name,
                                    colW - 34.0f - tw - 14.0f).c_str());

        ImGui::SetCursorScreenPos(r0);
        ImGui::PushID(int(t.id));
        if (ImGui::InvisibleButton("##song", ImVec2(colW, kSongRow)))
            selectRow(row, t.id, io.KeyShift, io.KeySuper || io.KeyCtrl);
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            playTrackId(t.id);
        trackContextMenu(t);
        ImGui::PopID();
    }
}

}  // namespace podbox
