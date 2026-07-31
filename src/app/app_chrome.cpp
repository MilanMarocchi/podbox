// The window chrome: toolbar, transport, source list, track table, device
// pane and status bar. Everything here draws; the state it reads and the
// mutations it triggers live in app.cpp.

#include "app/app.h"
#include "app/app_util.h"

#include "device/ipod_device.h"
#include "itdb/itunesdb.h"
#include "library/artwork.h"
#include "library/transcode.h"
#include "ui/aqua.h"
#include "ui/macos_window.h"
#include "ui/theme.h"

#include <imgui.h>

#define GL_SILENCE_DEPRECATION
#if defined(__APPLE__)
#include <OpenGL/gl3.h>
#else
#include <GL/gl.h>
#endif

#include <algorithm>
#include <cmath>
#include <cfloat>
#include <cstdio>
#include <string>

namespace fs = std::filesystem;

namespace podbox {

void App::updateArtwork() {
    // This is the Now Playing well, so it follows what is playing and falls
    // back to the selection when nothing is — it used to track the selection
    // alone, so starting a song left the previous cover on screen.
    const Track* track = playingTrack();
    std::uint32_t want = playingTrackId_;
    if (!track) {
        const Library* shown = shownLibrary();
        const auto* index = shownIndex();
        if (shown && index && selectedTrackId_) {
            const auto it = index->find(selectedTrackId_);
            if (it != index->end()) {
                track = &shown->tracks[it->second];
                want = selectedTrackId_;
            }
        }
    }
    if (!track) {
        art_.hasImage = false;
        art_.trackId = 0;
        return;
    }
    if (want == art_.trackId) return;  // already current
    art_.trackId = want;
    art_.hasImage = false;

    const ArtImage img = loadEmbeddedArtwork(trackFilePath(*track));
    if (!img.ok()) return;

    if (art_.texture == 0) glGenTextures(1, &art_.texture);
    glBindTexture(GL_TEXTURE_2D, art_.texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, img.width, img.height, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, img.rgba.data());
    art_.hasImage = true;
}

void App::drawArtworkPane(float sidebarHeight) {
    constexpr float kPane = 148.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos();
    const float top = wp.y + sidebarHeight - kPane;
    dl->AddLine(ImVec2(wp.x, top), ImVec2(wp.x + kSidebarWidth, top),
                pal::SidebarBorder);

    const float box = 120.0f;
    const ImVec2 p(wp.x + (kSidebarWidth - box) * 0.5f, top + 14.0f);
    dl->AddRectFilled(p, ImVec2(p.x + box, p.y + box), pal::ArtworkWell,
                      2.0f);
    if (art_.hasImage && art_.texture) {
        dl->AddImage((ImTextureID)(intptr_t)art_.texture, p,
                     ImVec2(p.x + box, p.y + box));
    } else {
        // Empty "no artwork" well, like iTunes' placeholder note glyph.
        addTextCentered(dl, fonts_.label, fonts_.labelSize,
                        ImVec2(p.x + box * 0.5f, p.y + box * 0.5f),
                        pal::TextDim,
                        art_.trackId ? "No artwork" : "");
    }
    dl->AddRect(p, ImVec2(p.x + box, p.y + box), pal::ArtworkBorder, 2.0f);
}

void App::drawTransport() {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float cy = kToolbarHeight * 0.5f;
    const PlaybackState st = player_ ? player_->state() : PlaybackState::Stopped;
    // Whatever list is on screen, not the iPod's — playback works just as
    // well from the Mac library, and gating on library_ left the transport
    // greyed out and prev/next dead whenever no iPod was attached.
    const Library* shownLib = shownLibrary();
    const bool haveList = shownLib && !visible_.empty();
    const ImU32 glyph = haveList ? pal::Glyph : pal::GlyphDim;

    // Three separate discs, not one capsule. iTunes drew each transport button
    // as its own circle running white at the top to mid-grey at the bottom;
    // a single pale capsule behind all three reads as a white slab sitting on
    // the toolbar.
    float x = kTransportStartX;

    auto disc = [&](const char* id, float r) -> ImVec2 {
        ImGui::SetCursorPos(ImVec2(x, cy - r));
        ImGui::InvisibleButton(id, ImVec2(r * 2.0f, r * 2.0f));
        const bool hot = ImGui::IsItemHovered();
        const bool held = ImGui::IsItemActive();
        const ImVec2 mn = ImGui::GetItemRectMin();
        const ImVec2 c(mn.x + r, mn.y + r);

        // A vertical gradient inside a circle, done as horizontal slices: the
        // draw list can fill a circle or gradient a rect, but not both.
        constexpr int kSlices = 14;
        for (int i = 0; i < kSlices; ++i) {
            const float t0 = float(i) / kSlices, t1 = float(i + 1) / kSlices;
            const float y0 = c.y - r + 2.0f * r * t0;
            const float y1 = c.y - r + 2.0f * r * t1;
            // Half-chord at the slice midpoint, so the fill follows the rim.
            const float m = (t0 + t1) * 0.5f * 2.0f - 1.0f;
            const float half = r * std::sqrt(std::max(0.0f, 1.0f - m * m));
            const float k = held ? 0.72f : (hot ? 1.04f : 1.0f);
            auto lerp = [&](int a, int b) {
                return int(std::clamp((a + (b - a) * (t0 + t1) * 0.5f) * k,
                                      0.0f, 255.0f));
            };
            dl->AddRectFilled(ImVec2(c.x - half, y0), ImVec2(c.x + half, y1),
                              pal::rgb(lerp(253, 172), lerp(253, 173),
                                       lerp(253, 174)));
        }
        dl->AddCircle(c, r, pal::ControlBorder, 32, 1.0f);
        x += r * 2.0f + kTransportGap;
        return c;
    };

    // A double triangle, pointing left or right: the rewind and fast-forward
    // glyphs, which are two chevrons rather than a triangle against a bar.
    // Two chevrons pointing `dir` (-1 left, +1 right), together spanning
    // exactly [c.x - kW, c.x + kW] so the pair is centred on the disc. The
    // apex leads; the flat edge trails.
    auto doubleTriangle = [&](ImVec2 c, int dir) {
        constexpr float kW = 5.5f, kHalfH = 5.0f;
        for (int k = 0; k < 2; ++k) {
            const float flat = c.x + float(dir) * (k * kW - kW);
            const float apex = flat + float(dir) * kW;
            dl->AddTriangleFilled(ImVec2(flat, c.y - kHalfH),
                                  ImVec2(flat, c.y + kHalfH),
                                  ImVec2(apex, c.y), glyph);
        }
    };

    {
        const ImVec2 c = disc("##prev", kPrevRadius);
        const bool clicked = ImGui::IsItemClicked();
        doubleTriangle(c, -1);
        if (clicked && haveList) playRelative(-1);
    }
    {
        const ImVec2 c = disc("##playpause", kPlayRadius);
        const bool clicked = ImGui::IsItemClicked();
        if (st == PlaybackState::Playing) {
            dl->AddRectFilled(ImVec2(c.x - 5, c.y - 6), ImVec2(c.x - 2, c.y + 6),
                              glyph);
            dl->AddRectFilled(ImVec2(c.x + 2, c.y - 6), ImVec2(c.x + 5, c.y + 6),
                              glyph);
        } else {
            // Centred on the disc: 11 wide, so 5.5 either side.
            dl->AddTriangleFilled(ImVec2(c.x - 5.5f, c.y - 7.0f),
                                  ImVec2(c.x - 5.5f, c.y + 7.0f),
                                  ImVec2(c.x + 5.5f, c.y), glyph);
        }
        if (clicked) {
            if (st == PlaybackState::Playing)
                player_->pause();
            else if (st == PlaybackState::Paused)
                player_->play();
            else if (playingTrackId_)
                playTrackId(playingTrackId_);
            else if (selectedTrackId_)
                playTrackId(selectedTrackId_);
            else if (haveList)
                playTrackId(shownLib->tracks[visible_[0].second].id);
        }
    }
    {
        const ImVec2 c = disc("##next", kNextRadius);
        const bool clicked = ImGui::IsItemClicked();
        doubleTriangle(c, +1);
        if (clicked && haveList) playRelative(+1);
    }

    // Volume: a quiet speaker, a recessed groove, a loud speaker. The groove
    // is light where it is filled and dark beyond the knob, which is what
    // makes it read as a slot cut into the toolbar rather than a line.
    x += kVolumeGap;
    ImGui::SetCursorPos(ImVec2(x, cy - 8.0f));
    ImGui::InvisibleButton("##volume", ImVec2(kVolumeWidth, 16.0f));
    const ImVec2 vmn = ImGui::GetItemRectMin();
    const ImVec2 vmx = ImGui::GetItemRectMax();
    const float ty = (vmn.y + vmx.y) * 0.5f;
    const float tx0 = vmn.x + 15.0f, tx1 = vmx.x - 19.0f;

    // A speaker: a small rectangular body with a cone opening toward `dir`,
    // plus `waves` arcs for the loud end.
    auto speaker = [&](float sx, int waves) {
        dl->AddRectFilled(ImVec2(sx, ty - 2.5f), ImVec2(sx + 3.0f, ty + 2.5f),
                          pal::Glyph);
        // The cone flares away from the body: narrow where it meets the
        // block, wide at the mouth. Tapering the other way draws an arrow.
        dl->AddTriangleFilled(ImVec2(sx + 7.5f, ty - 5.5f),
                              ImVec2(sx + 7.5f, ty + 5.5f),
                              ImVec2(sx + 2.0f, ty), pal::Glyph);
        for (int i = 0; i < waves; ++i) {
            dl->PathArcTo(ImVec2(sx + 7.0f, ty), 3.0f + 2.8f * i, -0.95f, 0.95f,
                          10);
            dl->PathStroke(pal::Glyph, 0, 1.3f);
        }
    };
    speaker(vmn.x, 0);
    speaker(vmx.x - 14.0f, 2);

    float vol = player_ ? player_->volume() : 0.0f;
    if (ImGui::IsItemActive()) {
        vol = std::clamp((ImGui::GetIO().MousePos.x - tx0) / (tx1 - tx0), 0.0f,
                         1.0f);
        if (player_) player_->setVolume(vol);
    }
    const float knobX = tx0 + (tx1 - tx0) * vol;
    const float gh = 3.5f;
    dl->AddRectFilled(ImVec2(tx0, ty - gh), ImVec2(tx1, ty + gh),
                      pal::VolumeEmpty, gh);
    if (knobX > tx0 + 1.0f)
        dl->AddRectFilled(ImVec2(tx0, ty - gh), ImVec2(knobX, ty + gh),
                          pal::VolumeFilled, gh);
    dl->AddRect(ImVec2(tx0, ty - gh), ImVec2(tx1, ty + gh), pal::ControlBorder,
                gh);
    dl->AddCircleFilled(ImVec2(knobX, ty), 6.0f, pal::ControlTop, 20);
    dl->AddCircle(ImVec2(knobX, ty), 6.0f, pal::ControlBorder, 20, 1.0f);
}

void App::drawNowPlaying(ImVec2 a, ImVec2 b) {
    const Track* track = playingTrack();
    if (!track) return;
    const Track& t = *track;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // Song titles are arbitrarily long and this well is 360px wide, so both
    // lines are fitted to the space between the LCD's own buttons rather than
    // centred on the full width and left to run over them.
    const float tx0 = a.x + kLcdTextInset, tx1 = b.x - kLcdTextInset;

    // Three rows in 40px, so they are spaced to just clear one another: a
    // 13px title, an 11px subtitle, then the progress row. The subtitle used
    // to sit low enough to overlap the elapsed and remaining times.
    addTextCenteredFit(dl, fonts_.ui, fonts_.uiSize, tx0, tx1, a.y + 10.0f,
                       pal::LcdText, t.title);
    std::string sub = t.artist;
    if (!t.album.empty()) sub += sub.empty() ? t.album : "  —  " + t.album;
    addTextCenteredFit(dl, fonts_.label, fonts_.labelSize, tx0, tx1,
                       a.y + 21.0f, pal::LcdTextDim, sub);

    const double pos = player_->position();
    const double dur = player_->duration();
    const float y = b.y - 8.0f;
    const float x0 = a.x + 46.0f, x1 = b.x - 46.0f;
    const float frac =
        dur > 0 ? float(std::clamp(pos / dur, 0.0, 1.0)) : 0.0f;
    dl->AddLine(ImVec2(x0, y), ImVec2(x1, y), pal::LcdProgressBg, 2.0f);
    dl->AddLine(ImVec2(x0, y), ImVec2(x0 + (x1 - x0) * frac, y),
                pal::LcdProgressFill, 2.0f);
    dl->AddCircleFilled(ImVec2(x0 + (x1 - x0) * frac, y), 4.5f,
                        pal::LcdProgressKnob);

    dl->AddText(fonts_.label, fonts_.labelSize, ImVec2(a.x + 8.0f, y - 6.0f),
                pal::LcdTextDim,
                formatDuration(std::uint32_t(pos * 1000)).c_str());
    const std::string rem =
        "-" + formatDuration(std::uint32_t(std::max(0.0, dur - pos) * 1000));
    const float rw = fonts_.label->CalcTextSizeA(fonts_.labelSize, FLT_MAX, 0,
                                                 rem.c_str())
                         .x;
    dl->AddText(fonts_.label, fonts_.labelSize,
                ImVec2(b.x - 8.0f - rw, y - 6.0f), pal::LcdTextDim, rem.c_str());

    // Draggable scrub region over the progress line.
    ImGui::SetCursorScreenPos(ImVec2(x0, y - 8.0f));
    ImGui::InvisibleButton("##scrub", ImVec2(std::max(1.0f, x1 - x0), 16.0f));
    if (ImGui::IsItemActive() && dur > 0) {
        const float tt = std::clamp(
            (ImGui::GetIO().MousePos.x - x0) / (x1 - x0), 0.0f, 1.0f);
        player_->seek(tt * dur);
    }
}

void App::drawToolbar() {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetWindowPos();
    const float w = ImGui::GetWindowWidth();
    const ImVec2 br(p.x + w, p.y + kToolbarHeight);
    dl->AddRectFilledMultiColor(p, br, pal::ToolbarTop, pal::ToolbarTop,
                                pal::ToolbarBottom, pal::ToolbarBottom);
    // The lit top edge and the shadowed base that give the titlebar its
    // depth; without them the gradient reads as a flat grey band.
    dl->AddLine(ImVec2(p.x, p.y + 0.5f), ImVec2(br.x, p.y + 0.5f),
                IM_COL32(255, 255, 255, 200));
    dl->AddLine(ImVec2(p.x, br.y - 1.5f), ImVec2(br.x, br.y - 1.5f),
                IM_COL32(255, 255, 255, 90));
    dl->AddLine(ImVec2(p.x, br.y - 0.5f), ImVec2(br.x, br.y - 0.5f),
                pal::ToolbarBorder);

    drawTransport();

    // Centred readout, never wider than the gap the left cluster leaves on
    // either side of centre — so it shrinks before it can collide rather than
    // overlapping the volume slider.
    const float lcdW =
        std::min(kLcdWidth, w - 2.0f * kToolbarLeftEnd - 24.0f);
    if (lcdW > 80.0f) {
        const ImVec2 a(p.x + (w - lcdW) * 0.5f,
                       p.y + (kToolbarHeight - kLcdHeight) * 0.5f);
        const ImVec2 b(a.x + lcdW, a.y + kLcdHeight);
        // Shallow well. iTunes 10 lost the tall glossy wash the earlier
        // versions had over the top half — that gloss is the single most
        // iTunes-9 thing on screen — leaving a flat face, one dark inner top
        // edge for the recess, and a highlight just below the frame.
        dl->AddRectFilled(a, b, pal::LcdBg, 4.0f);
        dl->AddRectFilledMultiColor(ImVec2(a.x + 1, a.y + 1),
                                    ImVec2(b.x - 1, b.y - 1), pal::LcdBgTop,
                                    pal::LcdBgTop, pal::LcdBgBottom,
                                    pal::LcdBgBottom);
        dl->AddLine(ImVec2(a.x + 4, a.y + 1.0f), ImVec2(b.x - 4, a.y + 1.0f),
                    IM_COL32(120, 132, 145, 110));
        dl->AddRect(a, b, pal::LcdBorder, 4.0f);
        dl->AddLine(ImVec2(a.x + 4, b.y + 0.5f), ImVec2(b.x - 4, b.y + 0.5f),
                    IM_COL32(255, 255, 255, 170));

        const bool syncing = sync_.busy();
        if (!syncing && playingTrackId_ != 0) {
            drawNowPlaying(a, b);
        } else {
            const auto& dev = watcher_.device();
            std::string line1 = dev ? dev->volumeName : "PodBox";
            std::string line2;
            if (syncing) {
                line1 = "Copying “" + sync_.currentName() + "”";
                line2 = std::to_string(std::min(sync_.batchDone() + 1,
                                                sync_.batchTotal())) +
                        " of " + std::to_string(sync_.batchTotal());
            } else if (dev && library_) {
                line2 = std::to_string(library_->tracks.size()) + " songs — " +
                        formatBytes(dev->freeBytes) + " available";
            } else if (dev) {
                line2 = formatBytes(dev->freeBytes) + " available";
            } else {
                line2 = "No iPod connected";
            }
            // "Copying <name>" carries a filename, so it needs fitting for
            // exactly the same reason the now-playing lines do.
            const float lx0 = a.x + kLcdTextInset, lx1 = b.x - kLcdTextInset;
            addTextCenteredFit(dl, fonts_.ui, fonts_.uiSize, lx0, lx1,
                               a.y + 12.0f, pal::LcdText, line1);
            addTextCenteredFit(dl, fonts_.label, fonts_.labelSize, lx0, lx1,
                               a.y + 25.0f, pal::LcdTextDim, line2);
            if (syncing && sync_.batchTotal() > 0) {
                // Thin progress bar along the bottom of the LCD.
                const float frac =
                    float(sync_.batchDone()) / float(sync_.batchTotal());
                const ImVec2 p0(a.x + 8, b.y - 7);
                const ImVec2 p1(b.x - 8, b.y - 3);
                dl->AddRectFilled(p0, p1, pal::LcdProgressBg, 2.0f);
                dl->AddRectFilled(
                    p0,
                    ImVec2(p0.x + std::max(4.0f, (p1.x - p0.x) * frac), p1.y),
                    pal::CapacityFill, 2.0f);
            }
        }
    }

    // The right cluster is laid out from the right edge inward, so a missing
    // search field does not leave the segments stranded in the middle. Each
    // control drops out when there is no longer room for it, widest last.
    float rightX = w - 16.0f;

    // Only one way of showing a track list exists, so there is no segmented
    // view switcher: three of its four cells would be decoration. It comes
    // back when Album List, Grid and Cover Flow do.
    const bool anythingToSearch = library_ || !host_.tracks().empty();
    const bool showSearch = anythingToSearch && rightX - kSearchWidth > 320.0f;
    float searchX = 0.0f;
    if (showSearch) {
        rightX -= kSearchWidth;
        searchX = rightX;
    }

    if (showSearch) {
        ImGui::SetCursorPos(ImVec2(searchX, (kToolbarHeight - 23.0f) * 0.5f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 11.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(24, 4));
        ImGui::SetNextItemWidth(kSearchWidth);
        const ImVec2 fp = ImGui::GetCursorScreenPos();
        if (ImGui::InputTextWithHint("##search", "Search", search_,
                                     sizeof(search_))) {
            visibleDirty_ = true;
            // Typing on the device pane has nothing to filter, so move to a
            // list that does rather than swallowing the keystrokes.
            if (view_ == View::Device)
                switchSource(library_ ? View::Music : View::Library);
        }
        ImGui::PopStyleVar(2);
        const float gy = fp.y + ImGui::GetItemRectSize().y * 0.5f;
        dl->AddCircle(ImVec2(fp.x + 11.0f, gy - 1.0f), 3.5f, pal::Glyph, 12,
                      1.3f);
        dl->AddLine(ImVec2(fp.x + 13.5f, gy + 1.5f),
                    ImVec2(fp.x + 16.5f, gy + 4.0f), pal::Glyph, 1.3f);
    }

    // The toolbar is the title bar now, so dragging empty space in it moves
    // the window. Decided here, at the end, rather than with an invisible
    // button behind the controls: a button submitted first claims the mouse on
    // press before the volume slider has even been submitted, which is what
    // stopped the volume working. By this point every control in the toolbar
    // has had its turn, so a hovered id means one of them wants the click.
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const bool inToolbar = mouse.x >= p.x + kTrafficLightWidth &&
                           mouse.x < br.x && mouse.y >= p.y && mouse.y < br.y;
    if (inToolbar && !ImGui::IsAnyItemHovered() && !ImGui::IsAnyItemActive() &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        dragWindowWithCurrentEvent(window_);
}

void App::drawSidebar(float height) {
    constexpr float kArtPane = 148.0f;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, v4(pal::SidebarBg));
    ImGui::BeginChild("sidebar", ImVec2(kSidebarWidth, height));

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos();
    dl->AddLine(ImVec2(wp.x + kSidebarWidth - 0.5f, wp.y),
                ImVec2(wp.x + kSidebarWidth - 0.5f, wp.y + height),
                pal::SidebarBorder);

    const bool haveLib = library_.has_value();
    const float listHeight = (haveLib && artworkPaneOpen_)
                                 ? std::max(60.0f, height - kArtPane)
                                 : height;
    ImGui::BeginChild("sidebar_list", ImVec2(kSidebarWidth, listHeight));
    dl = ImGui::GetWindowDrawList();

    auto sectionHeader = [&](const char* text) {
        ImGui::Dummy(ImVec2(0, 6));
        ImGui::SetCursorPosX(10);
        ImGui::PushFont(fonts_.labelBold);
        ImGui::TextColored(v4(pal::SidebarHeader), "%s", text);
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0, 2));
    };

    // Full-width row: transparent selectable with the text drawn manually so
    // we control the indent, like the iTunes source list.
    auto row = [&](const char* id, const std::string& text, float indent,
                   bool selected) {
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        ImGui::PushStyleColor(ImGuiCol_Header, v4(pal::Selection));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, v4(pal::Selection));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, v4(pal::Selection));
        const bool clicked = ImGui::Selectable(
            (std::string("##") + id).c_str(), selected,
            ImGuiSelectableFlags_AllowOverlap, ImVec2(0, 18));
        ImGui::PopStyleColor(3);
        if (selected) {
            // Repaint the flat fill as the gradient the source list used.
            const ImVec2 mn = ImGui::GetItemRectMin();
            const ImVec2 mx = ImGui::GetItemRectMax();
            aqua::selectionGradient(dl, mn, mx);
        }
        // Playlist and volume names are user-supplied and the sidebar is
        // 200px wide. Reserve the right margin so a long name ends in an
        // ellipsis instead of running under the eject button or off the edge.
        const float avail = kSidebarWidth - indent - kSidebarRightMargin;
        dl->AddText(fonts_.ui, fonts_.uiSize, ImVec2(pos.x + indent, pos.y + 2),
                    selected ? IM_COL32_WHITE : pal::Text,
                    truncateToWidth(fonts_.ui, fonts_.uiSize, text, avail)
                        .c_str());
        return clicked;
    };

    // The Mac's own collection, above the device — this is the library that
    // exists whether or not an iPod is plugged in.
    // Offered from both the header glyph and the row's context menu, so the
    // items live in one place rather than being written out twice.
    auto libraryActions = [&] {
        ImGui::PushStyleColor(ImGuiCol_Text, v4(pal::Text));
        ImGui::BeginDisabled(scan_.running);
        if (ImGui::MenuItem(scan_.running ? "Scanning…" : "Rescan"))
            rescanWatchFolders();
        ImGui::EndDisabled();
        if (ImGui::MenuItem("Music Folders…")) foldersOpen_ = true;
        if (ImGui::MenuItem("Import from Apple Music…")) apple_.open = true;
        ImGui::PopStyleColor();
    };

    const float libraryHeaderY = ImGui::GetCursorScreenPos().y + 6.0f;
    sectionHeader("LIBRARY");
    {
        const std::string label =
            "Music  (" + std::to_string(host_.tracks().size()) + ")";
        if (row("hostlib", label, 28.0f, view_ == View::Library)) {
            switchSource(View::Library);
        }
        if (ImGui::BeginPopupContextItem("libctx")) {
            libraryActions();
            ImGui::EndPopup();
        }
    }
    // These three used to sit under the row as a line of buttons, which
    // overflowed a 200px sidebar and wrapped into a mess. iTunes kept its
    // source list to rows alone, so they live behind a menu on the section
    // header now — reachable from the row's context menu too, since a bare
    // glyph is not much of an advertisement.
    {
        // Drawn back up on the section header, so the cursor has to be put
        // where it was afterwards — otherwise everything below this lays out
        // from the header's line and DEVICES lands on top of the Music row.
        const ImVec2 resume = ImGui::GetCursorScreenPos();
        const ImVec2 gear(ImGui::GetWindowPos().x + kSidebarWidth - 26.0f,
                          libraryHeaderY + 1.0f);
        ImGui::SetCursorScreenPos(gear);
        const bool open = ImGui::InvisibleButton("##libmenu", ImVec2(16, 14));
        ImGui::SetCursorScreenPos(resume);
        const ImU32 c =
            ImGui::IsItemHovered() ? pal::GlyphHot : pal::SidebarHeader;
        for (int i = 0; i < 3; ++i)
            dl->AddCircleFilled(ImVec2(gear.x + 3.0f + i * 5.0f, gear.y + 7.0f),
                                1.4f, c, 6);
        if (open) ImGui::OpenPopup("libmenu");
    }
    if (ImGui::BeginPopup("libmenu")) {
        libraryActions();
        ImGui::EndPopup();
    }

    sectionHeader("DEVICES");
    const auto& dev = watcher_.device();
    if (dev) {
        const ImVec2 iconPos = ImGui::GetCursorScreenPos();
        if (row("device", dev->volumeName, 28.0f, view_ == View::Device))
            switchSource(View::Device);
        const ImVec2 afterRow = ImGui::GetCursorScreenPos();
        // Eject button overlapping the right side of the device row.
        const bool devSelected = view_ == View::Device;
        ImGui::SetCursorScreenPos(
            ImVec2(iconPos.x + kSidebarWidth - 32.0f, iconPos.y));
        if (ImGui::InvisibleButton("##eject", ImVec2(26.0f, 18.0f)))
            ejectRequested_ = true;
        const ImVec2 em = ImGui::GetItemRectMin();
        const float ejx = em.x + 8.0f, ejy = em.y + 4.0f;
        const ImU32 ejCol = ImGui::IsItemHovered()
                                ? pal::GlyphOn
                                : (devSelected ? IM_COL32_WHITE
                                               : pal::Glyph);
        drawEjectGlyph(dl, ImVec2(ejx + 5.0f, ejy + 5.0f), ejCol);
        ImGui::SetCursorScreenPos(afterRow);
        drawIpodIcon(dl, ImVec2(iconPos.x + 10, iconPos.y + 1));
        if (library_) {
            if (row("music", "Music", 28.0f, view_ == View::Music)) {
                switchSource(View::Music);
            }
            // Only offered when the device actually holds some: an empty
            // Podcasts row is a dead end, and most iPods have none.
            if (devHasPodcasts_ &&
                row("podcasts", "Podcasts", 28.0f, view_ == View::Podcasts))
                switchSource(View::Podcasts);
            if (devHasAudiobooks_ &&
                row("books", "Audiobooks", 28.0f, view_ == View::Audiobooks))
                switchSource(View::Audiobooks);
        }
    } else {
        ImGui::SetCursorPosX(10);
        ImGui::PushFont(fonts_.label);
        ImGui::TextColored(v4(pal::TextDim), "No iPod connected");
        ImGui::PopFont();
    }

    if (library_) {
        sectionHeader("PLAYLISTS");
        for (int i = 0; i < int(library_->playlists.size()); ++i) {
            const bool selected =
                view_ == View::Playlist && playlistIndex_ == i;
            if (plEdit_.renameIndex == i) {
                // Inline rename field.
                ImGui::SetCursorPosX(18);
                ImGui::SetNextItemWidth(kSidebarWidth - 26.0f);
                if (plEdit_.justOpened) {
                    ImGui::SetKeyboardFocusHere();
                    plEdit_.justOpened = false;
                }
                const bool done = ImGui::InputText(
                    "##rename", plEdit_.buf, sizeof(plEdit_.buf),
                    ImGuiInputTextFlags_EnterReturnsTrue |
                        ImGuiInputTextFlags_AutoSelectAll);
                if (done || ImGui::IsItemDeactivated()) {
                    if (plEdit_.buf[0]) {
                        library_->playlists[i].name = plEdit_.buf;
                        writeDatabase();
                    }
                    plEdit_.renameIndex = -1;
                }
                continue;
            }
            const std::string id = "pl" + std::to_string(i);
            if (row(id.c_str(), library_->playlists[i].name, 20.0f, selected)) {
                switchSource(View::Playlist, i);
            }
            if (ImGui::BeginPopupContextItem(
                    ("plctx" + std::to_string(i)).c_str())) {
                ImGui::PushStyleColor(ImGuiCol_Text, v4(pal::Text));
                ImGui::BeginDisabled(!writesSupported());
                if (ImGui::MenuItem("Rename")) {
                    plEdit_.renameIndex = i;
                    plEdit_.justOpened = true;
                    std::snprintf(plEdit_.buf, sizeof(plEdit_.buf), "%s",
                                  library_->playlists[i].name.c_str());
                }
                if (ImGui::MenuItem("Delete")) plEdit_.deleteIndex = i;
                ImGui::EndDisabled();
                ImGui::PopStyleColor();
                ImGui::EndPopup();
            }
        }
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::SetCursorPosX(18);
        ImGui::PushStyleColor(ImGuiCol_Text, v4(pal::TextDim));
        ImGui::BeginDisabled(!writesSupported());
        if (ImGui::SmallButton("+ New Playlist")) createPlaylist(0);
        ImGui::EndDisabled();
        ImGui::PopStyleColor();
    }

    ImGui::EndChild();

    if (haveLib && artworkPaneOpen_) drawArtworkPane(height);

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void App::drawMainPanel(float height) {
    const float width = ImGui::GetWindowWidth() - kSidebarWidth;
    const auto& dev = watcher_.device();
    const bool trackView = showingTracks();

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        trackView ? ImVec2(0, 0) : ImVec2(18, 14));
    ImGui::BeginChild("main", ImVec2(width, height),
                      ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();

    if (view_ == View::Library && hostView_.tracks.empty()) {
        const char* msg =
            host_.watchFolders().empty()
                ? "No music folders yet — add one in the device pane"
                : "Nothing indexed yet — press Rescan in the sidebar";
        const ImVec2 avail = ImGui::GetWindowSize();
        const ImVec2 ts = ImGui::CalcTextSize(msg);
        ImGui::SetCursorPos(
            ImVec2((avail.x - ts.x) * 0.5f, (avail.y - ts.y) * 0.5f));
        ImGui::TextDisabled("%s", msg);
    } else if (!dev && view_ != View::Library) {
        const char* msg = "Connect an iPod, or pick Music under Library";
        const ImVec2 avail = ImGui::GetWindowSize();
        const ImVec2 ts = ImGui::CalcTextSize(msg);
        ImGui::SetCursorPos(
            ImVec2((avail.x - ts.x) * 0.5f, (avail.y - ts.y) * 0.5f));
        ImGui::TextDisabled("%s", msg);
    } else if (trackView) {
        // The browser reads the search-filtered set and constrains the table,
        // and both come out of one rebuild — so it has to happen before either
        // is drawn, not between them.
        if (visibleDirty_) rebuildVisible();
        if (browserApplies()) drawColumnBrowser(width);
        drawTrackTable();
    } else {
        drawDeviceView(*dev);
    }

    ImGui::EndChild();
}

void App::drawColumnBrowser(float width) {
    constexpr float kHeadH = 17.0f, kRowH = 17.0f;

    // Clamped every frame rather than only on drag, so shrinking the window
    // cannot strand the splitter past the bottom of the panel.
    const float maxH = std::max(60.0f, ImGui::GetWindowHeight() - 140.0f);
    browser_.height = std::clamp(browser_.height, 60.0f, maxH);

    ImGui::PushStyleColor(ImGuiCol_ChildBg, v4(pal::BrowserBg));
    ImGui::BeginChild("browser", ImVec2(width, browser_.height), false,
                      ImGuiWindowFlags_NoScrollbar);

    // Taken after BeginChild on purpose. A child has its own draw list which
    // is rendered over the parent's, so headers and dividers drawn into the
    // parent's list end up underneath this child's background — which is
    // exactly where the pane titles went missing.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float paneW = std::floor(width / 3.0f);

    struct Pane {
        const char* title;
        std::vector<std::string>* values;
        std::optional<std::string>* sel;
        const char* noun;
    };
    const Pane panes[3] = {
        {"Genres", &browser_.genres, &browser_.genre, "Genres"},
        {"Artists", &browser_.artists, &browser_.artist, "Artists"},
        {"Albums", &browser_.albums, &browser_.album, "Albums"},
    };

    for (int i = 0; i < 3; ++i) {
        // The last pane takes the remainder so the three always reach the edge.
        const float x0 = origin.x + paneW * i;
        const float w = (i == 2) ? (origin.x + width - x0) : paneW;

        aqua::gradientRect(dl, ImVec2(x0, origin.y),
                           ImVec2(x0 + w, origin.y + kHeadH),
                           pal::BrowserHeadTop, pal::BrowserHeadBottom,
                           pal::BrowserBorder, 0.0f, false);
        addTextCentered(dl, fonts_.labelBold, fonts_.labelSize,
                        ImVec2(x0 + w * 0.5f, origin.y + kHeadH * 0.5f),
                        pal::BrowserHeadText, panes[i].title);

        ImGui::SetCursorScreenPos(ImVec2(x0, origin.y + kHeadH));
        ImGui::PushID(i);
        ImGui::BeginChild("pane", ImVec2(w - 1.0f, browser_.height - kHeadH));
        ImDrawList* pdl = ImGui::GetWindowDrawList();

        // One row: a transparent selectable with the label drawn by hand, so
        // the indent and the ellipsis are ours. Same shape as the source list.
        int rowNo = 0;
        auto row = [&](const std::string& text, bool selected) {
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::PushStyleColor(ImGuiCol_Header, v4(pal::Selection));
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, v4(pal::Selection));
            ImGui::PushStyleColor(ImGuiCol_HeaderActive, v4(pal::Selection));
            const bool clicked =
                ImGui::Selectable("##r", selected, 0, ImVec2(0, kRowH));
            ImGui::PopStyleColor(3);
            const ImVec2 mn = ImGui::GetItemRectMin();
            const ImVec2 mx = ImGui::GetItemRectMax();
            if (selected)
                aqua::selectionGradient(pdl, mn, mx);
            else if (rowNo % 2)
                pdl->AddRectFilled(mn, mx, pal::BrowserRowAlt);
            // Truncated for the ellipsis; the clip rect stays as a backstop
            // in case the measurement and the rasteriser ever disagree.
            const std::string shown =
                truncateToWidth(fonts_.ui, fonts_.uiSize, text,
                                mx.x - p.x - 12.0f);
            pdl->PushClipRect(mn, ImVec2(mx.x - 4.0f, mx.y), true);
            pdl->AddText(fonts_.ui, fonts_.uiSize,
                         ImVec2(p.x + 6.0f, p.y + 1.0f),
                         selected ? IM_COL32_WHITE : pal::Text, shown.c_str());
            pdl->PopClipRect();
            ++rowNo;
            return clicked;
        };

        const std::string allLabel = "All (" +
                                     std::to_string(panes[i].values->size()) +
                                     " " + panes[i].noun + ")";
        ImGui::PushID("all");
        if (row(allLabel, !panes[i].sel->has_value())) {
            panes[i].sel->reset();
            visibleDirty_ = true;
        }
        ImGui::PopID();

        for (int k = 0; k < int(panes[i].values->size()); ++k) {
            const std::string& v = (*panes[i].values)[k];
            const bool selected =
                panes[i].sel->has_value() && **panes[i].sel == v;
            ImGui::PushID(k);
            // A blank artist or genre is common in ripped music and is a
            // real facet, so it gets a label rather than an empty row. Tags
            // that are nothing but whitespace look identical to the reader
            // and are treated the same way.
            const bool blank =
                v.find_first_not_of(" \t\r\n") == std::string::npos;
            if (row(blank ? "Unknown" : v, selected) && !selected) {
                *panes[i].sel = v;
                visibleDirty_ = true;
            }
            ImGui::PopID();
        }

        ImGui::EndChild();
        ImGui::PopID();

        if (i < 2)
            dl->AddLine(ImVec2(x0 + w - 0.5f, origin.y),
                        ImVec2(x0 + w - 0.5f, origin.y + browser_.height),
                        pal::BrowserBorder);
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();

    // Splitter. Session-lived: there is no UI settings file, and inventing one
    // for a divider position is not worth it. Back on the parent's draw list
    // now that the child has been closed.
    dl = ImGui::GetWindowDrawList();
    const ImVec2 sp = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##browser_split", ImVec2(width, 5.0f));
    if (ImGui::IsItemHovered() || ImGui::IsItemActive())
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    if (ImGui::IsItemActive())
        browser_.height = std::clamp(
            browser_.height + ImGui::GetIO().MouseDelta.y, 60.0f, maxH);
    dl->AddRectFilledMultiColor(sp, ImVec2(sp.x + width, sp.y + 5.0f),
                                pal::BrowserHeadTop, pal::BrowserHeadTop,
                                pal::BrowserHeadBottom, pal::BrowserHeadBottom);
    dl->AddLine(ImVec2(sp.x, sp.y + 0.5f), ImVec2(sp.x + width, sp.y + 0.5f),
                pal::BrowserBorder);
    dl->AddLine(ImVec2(sp.x, sp.y + 4.5f), ImVec2(sp.x + width, sp.y + 4.5f),
                pal::BrowserBorder);
}

void App::drawDeviceView(const IpodInfo& dev) {
    ImGui::PushFont(fonts_.uiBold);
    ImGui::TextUnformatted(dev.volumeName.c_str());
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, 4));

    if (ImGui::BeginTable("device_info", 2, ImGuiTableFlags_SizingFixedFit)) {
        auto row = [](const char* label, const std::string& value) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", label);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(value.empty() ? "—" : value.c_str());
        };
        row("Model", dev.modelName);
        row("Serial number", dev.serialNumber);
        row("Firmware", dev.firmwareVersion);
        row("Format", dev.filesystem);
        row("Location", dev.mountPoint.string());
        std::string dbInfo;
        if (library_) {
            dbInfo = std::to_string(library_->tracks.size()) + " songs";
            switch (library_->hashingScheme) {
                case kChecksumNone:
                    dbInfo += " — writable, no hash required";
                    break;
                case kChecksumHash58:
                    dbInfo += hash58Verified_
                                  ? " — hash58 verified against this iPod"
                                  : " — writes require hash58 (unverified)";
                    break;
                case kChecksumHash72:
                    dbInfo += " — read-only: writes require hash72";
                    break;
                case kChecksumHashAB:
                    dbInfo += " — read-only: writes require hashAB";
                    break;
                default:
                    dbInfo += " — read-only: unrecognised hash scheme " +
                              std::to_string(library_->hashingScheme);
                    break;
            }
        } else {
            dbInfo = libraryError_;
        }
        row("Database", dbInfo);
        ImGui::EndTable();
    }

    ImGui::Dummy(ImVec2(0, 10));
    ImGui::PushFont(fonts_.uiBold);
    ImGui::TextUnformatted("Capacity");
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, 2));
    drawCapacityBar(dev);

    ImGui::Dummy(ImVec2(0, 14));
    ImGui::PushFont(fonts_.uiBold);
    ImGui::TextUnformatted("Import format");
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, 2));
    int fmt = int(importFormat_);
    ImGui::RadioButton("Keep original format", &fmt,
                       int(ImportFormat::Original));
    ImGui::RadioButton("Convert everything to Apple Lossless (ALAC)", &fmt,
                       int(ImportFormat::Alac));
    ImGui::RadioButton(mp3EncoderAvailable()
                           ? "Convert everything to MP3 (320 kbps)"
                           : "Convert everything to AAC (256 kbps)",
                       &fmt, int(ImportFormat::Mp3));
    importFormat_ = ImportFormat(fmt);

    ImGui::PushFont(fonts_.label);
    ImGui::TextColored(v4(pal::TextDim),
                       "Drag songs onto the window to add them. FLAC is always "
                       "converted so it plays on the iPod.");
    ImGui::PopFont();

    ImGui::Dummy(ImVec2(0, 14));
    ImGui::PushFont(fonts_.uiBold);
    ImGui::TextUnformatted("Sync");
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, 2));
    ImGui::BeginDisabled(host_.tracks().empty() || sync_.busy() ||
                         !writesSupported());
    if (ImGui::Button("Sync Library to iPod…")) {
        syncUi_.open = true;
        syncUi_.dirty = true;
        syncUi_.confirmRemove = false;
    }
    ImGui::EndDisabled();
    ImGui::PushFont(fonts_.label);
    ImGui::TextColored(v4(pal::TextDim),
                       "%zu songs in your Mac library. You'll see exactly what "
                       "would change before anything is written.",
                       host_.tracks().size());
    ImGui::PopFont();

    ImGui::Dummy(ImVec2(0, 14));
    ImGui::PushFont(fonts_.uiBold);
    ImGui::TextUnformatted("Duplicates");
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, 2));
    ImGui::Checkbox("Skip songs already on this iPod", &skipDuplicates_);

    // Finding duplicates is harmless, but the sheet's only action is to remove
    // them, so there is nothing to offer on a device we cannot write to.
    const bool canScan =
        library_ && !library_->tracks.empty() && writesSupported();
    ImGui::BeginDisabled(!canScan);
    if (ImGui::Button("Find Duplicates…")) {
        dupes_.open = true;
        dupes_.dirty = true;
    }
    ImGui::EndDisabled();

    ImGui::PushFont(fonts_.label);
    ImGui::TextColored(v4(pal::TextDim),
                       "Matches on artist, title, album and length. The better "
                       "copy is kept — lossless first, then play count.");
    ImGui::PopFont();

    ImGui::Dummy(ImVec2(0, 14));
    ImGui::PushFont(fonts_.uiBold);
    ImGui::TextUnformatted("Safety");
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, 2));
    ImGui::BeginDisabled(!writesSupported());
    if (ImGui::Button("Restore Database…")) restoreOpen_ = true;
    ImGui::EndDisabled();
    ImGui::PushFont(fonts_.label);
    if (!writesSupported() && library_ &&
        library_->hashingScheme == kChecksumHash58)
        ImGui::TextColored(v4(pal::Warning),
                           "PodBox could not reproduce this iPod's checksum, "
                           "so it stays read-only. Nothing will be changed.");
    else if (!writesSupported())
        ImGui::TextColored(v4(pal::Warning),
                           "This iPod's database carries a checksum PodBox "
                           "cannot produce yet, so it is read-only here. "
                           "Nothing on the device will be changed.");
    else if (appleMusicSyncing())
        ImGui::TextColored(v4(pal::Warning),
                           "Apple Music is syncing this iPod right now — "
                           "PodBox will not write until it finishes.");
    else
        ImGui::TextColored(v4(pal::TextDim),
                           "PodBox saves the database before every change and "
                           "keeps the last five.");
    ImGui::PopFont();
}

void App::drawTrackTable() {
    const Library* shown = shownLibrary();
    if (!shown) return;
    const ImGuiTableFlags flags =
        ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable |
        ImGuiTableFlags_Reorderable | ImGuiTableFlags_ScrollY;
    if (!ImGui::BeginTable("tracks", 8, flags)) return;

    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(
        "#",
        ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort |
            ImGuiTableColumnFlags_PreferSortAscending | ImGuiTableColumnFlags_NoReorder,
        34.0f, 0);
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 3.0f, 1);
    ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 48.0f, 2);
    ImGui::TableSetupColumn("Artist", ImGuiTableColumnFlags_WidthStretch, 2.0f,
                            3);
    ImGui::TableSetupColumn("Album", ImGuiTableColumnFlags_WidthStretch, 2.0f,
                            4);
    ImGui::TableSetupColumn("Genre", ImGuiTableColumnFlags_WidthStretch, 1.2f,
                            5);
    ImGui::TableSetupColumn("Plays", ImGuiTableColumnFlags_WidthFixed, 44.0f,
                            6);
    ImGui::TableSetupColumn("Rating", ImGuiTableColumnFlags_WidthFixed, 72.0f,
                            7);
    // ImGui fills the header row flat; iTunes' were a soft gradient with a
    // hard rule beneath, so paint over it before the labels are drawn.
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        const float h = ImGui::GetFrameHeight();
        dl->AddRectFilledMultiColor(p, ImVec2(p.x + w, p.y + h),
                                    pal::HeaderTop,
                                    pal::HeaderTop,
                                    pal::HeaderBottom,
                                    pal::HeaderBottom);
        dl->AddLine(ImVec2(p.x, p.y + h - 0.5f), ImVec2(p.x + w, p.y + h - 0.5f),
                    pal::HeaderBorder);
    }
    // Submitted by hand rather than with TableHeadersRow() so the sorted
    // column's header can carry the blue iTunes 10 wore. TableHeader() still
    // provides the sort click and the right-click column menu.
    ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
    for (int c = 0; c < ImGui::TableGetColumnCount(); ++c) {
        if (!ImGui::TableSetColumnIndex(c)) continue;
        if (ImGui::TableGetColumnFlags(c) & ImGuiTableColumnFlags_IsSorted) {
            const ImVec2 cmn = ImGui::GetCursorScreenPos();
            const float cw = ImGui::GetContentRegionAvail().x;
            const float ch = ImGui::GetFrameHeight();
            ImGui::GetWindowDrawList()->AddRectFilledMultiColor(
                ImVec2(cmn.x - 4.0f, cmn.y), ImVec2(cmn.x + cw + 4.0f, cmn.y + ch),
                pal::HeaderSortTop, pal::HeaderSortTop, pal::HeaderSortBottom,
                pal::HeaderSortBottom);
        }
        ImGui::TableHeader(ImGui::TableGetColumnName(c));
    }

    if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs()) {
        if (specs->SpecsDirty) {
            if (specs->SpecsCount > 0) {
                sortCol_ = int(specs->Specs[0].ColumnUserID);
                sortAsc_ = specs->Specs[0].SortDirection !=
                           ImGuiSortDirection_Descending;
            } else {
                sortCol_ = 0;
                sortAsc_ = true;
            }
            specs->SpecsDirty = false;
            visibleDirty_ = true;
        }
    }
    if (visibleDirty_) rebuildVisible();

    // Reordering is only meaningful for a playlist shown in manual order (no
    // search filter, no column sort, no browser facet). The browser never
    // shows in a playlist, so that last clause is belt and braces — but a
    // hidden filter is exactly what would make a drag land on the wrong row.
    const bool reorderable = view_ == View::Playlist && playlistIndex_ >= 0 &&
                             sortCol_ == 0 && search_[0] == '\0' &&
                             !browser_.engaged();
    int reorderFrom = -1, reorderTo = -1;

    // iTunes highlights rows on selection only, not hover.
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, v4(pal::Selection));

    ImGuiListClipper clipper;
    clipper.Begin(int(visible_.size()));
    std::uint32_t newRatingId = 0;
    int newRating = -1;

    while (clipper.Step()) {
        for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
            const auto [origPos, ti] = visible_[r];
            const Track& t = shown->tracks[ti];
            const bool selected = isSelected(t.id);

            ImGui::TableNextRow();
            // Tint the sorted column's body too, semi-transparent so the row
            // striping still reads through it. sortCol_ is the column's
            // UserID, and the UserIDs are 0..7 in setup order.
            if (!selected)
                ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg,
                                       pal::SortColumnTint, sortCol_);
            if (selected)
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));

            ImGui::TableNextColumn();
            char lbl[32];
            std::snprintf(lbl, sizeof(lbl), "%d##%u", r + 1, t.id);
            if (ImGui::Selectable(lbl, selected,
                                  ImGuiSelectableFlags_SpanAllColumns |
                                      ImGuiSelectableFlags_AllowDoubleClick |
                                      ImGuiSelectableFlags_AllowOverlap)) {
                const ImGuiIO& io = ImGui::GetIO();
                selectRow(r, t.id, io.KeyShift, io.KeySuper || io.KeyCtrl);
            }
            if (ImGui::IsItemHovered() &&
                ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                playTrackId(t.id);

            // Drag to reorder, only within a playlist in manual order.
            if (reorderable) {
                if (ImGui::BeginDragDropSource(
                        ImGuiDragDropFlags_SourceNoPreviewTooltip)) {
                    ImGui::SetDragDropPayload("PODBOX_ROW", &origPos,
                                              sizeof(origPos));
                    ImGui::EndDragDropSource();
                }
                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* pl =
                            ImGui::AcceptDragDropPayload("PODBOX_ROW")) {
                        const int from = *static_cast<const int*>(pl->Data);
                        reorderFrom = from;
                        reorderTo = origPos;
                    }
                    ImGui::EndDragDropTarget();
                }
            }

            trackContextMenu(t);

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(t.title.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(formatDuration(t.lengthMs).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(t.artist.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(t.album.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(t.genre.c_str());
            ImGui::TableNextColumn();
            if (t.playCount) ImGui::Text("%u", t.playCount);

            ImGui::TableNextColumn();
            {
                const ImVec2 sp = ImGui::GetCursorScreenPos();
                // A real item, not a hand-rolled rectangle test: this is what
                // makes the stars respect an open sheet, window ordering and
                // hover ownership. Testing io.MousePos directly would let a
                // click on a dialog above also set a rating down here.
                ImGui::PushID(int(t.id));
                // Ratings on the Mac library go to library.tsv; on the iPod
                // they are a database write, so on a device we cannot write
                // the stars still render but do not respond.
                const bool rateable = viewingHost() || writesSupported();
                ImGui::BeginDisabled(!rateable);
                ImGui::InvisibleButton("##rate", ImVec2(70, 16));
                ImGui::EndDisabled();
                const bool hot = rateable && ImGui::IsItemHovered();
                const int picked =
                    drawStars(ImGui::GetWindowDrawList(), ImVec2(sp.x, sp.y + 3),
                              t.rating, hot, ImGui::GetIO().MousePos,
                              ImGui::IsItemClicked());
                ImGui::PopID();
                if (picked >= 0) newRatingId = t.id, newRating = picked;
            }

            if (selected) ImGui::PopStyleColor();
        }
    }
    ImGui::PopStyleColor(2);
    ImGui::EndTable();

    if (newRatingId && newRating >= 0) setTrackRating(newRatingId, newRating);

    // Apply a completed drag-reorder to the underlying playlist order.
    if (reorderable && reorderFrom >= 0 && reorderTo >= 0 &&
        reorderFrom != reorderTo) {
        auto& ids = library_->playlists[playlistIndex_].trackIds;
        if (reorderFrom < int(ids.size()) && reorderTo < int(ids.size())) {
            const std::uint32_t moved = ids[reorderFrom];
            ids.erase(ids.begin() + reorderFrom);
            ids.insert(ids.begin() + reorderTo, moved);
            visibleDirty_ = true;
            writeDatabase();
        }
    }

    handleTrackTableKeys();
}

// Cmd+I and Delete for the track table. Kept apart from the drawing
// because it is the one part of it that is not drawing.
void App::handleTrackTableKeys() {
if (!selection_.empty() && !ImGui::GetIO().WantTextInput &&
    (ImGui::GetIO().KeySuper || ImGui::GetIO().KeyCtrl) &&
    ImGui::IsKeyPressed(ImGuiKey_I))
    openGetInfo();

if (selectedTrackId_ && !deleteRequestId_ &&
    !ImGui::GetIO().WantTextInput &&
    (ImGui::IsKeyPressed(ImGuiKey_Delete) ||
     ImGui::IsKeyPressed(ImGuiKey_Backspace))) {
    // In a playlist, plain Delete removes from the playlist; the
    // "Remove from iPod" path stays in the context menu to avoid
    // accidental file deletion.
    if (!viewingHost() && !writesSupported()) {
        setStatus(
            "This iPod needs a hashed database — writes not yet supported");
    } else if (view_ == View::Playlist && playlistIndex_ >= 0) {
        auto& ids = library_->playlists[playlistIndex_].trackIds;
        int removed = 0;
        for (std::uint32_t id : selection_) {
            if (auto it = std::find(ids.begin(), ids.end(), id);
                it != ids.end()) {
                ids.erase(it);
                ++removed;
            }
        }
        if (removed > 0) {
            visibleDirty_ = true;
            if (writeDatabase())
                setStatus("Removed " + plural(removed, "song", "songs") +
                          " from the playlist");
        }
    } else if (viewingHost()) {
        // Deleting from the Mac library would mean deleting the user's
        // own files out of a folder PodBox only indexes. Not this key's
        // job, and not something to do by accident.
        setStatus("Delete removes songs from the iPod, not from your Mac");
    } else {
        deleteRequestId_ = selectedTrackId_;
    }
}
}

void App::drawCapacityBar(const IpodInfo& dev) {
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = 14.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();

    const std::uint64_t used = dev.capacityBytes - dev.freeBytes;
    const float frac =
        dev.capacityBytes ? float(double(used) / double(dev.capacityBytes))
                          : 0.0f;
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), pal::CapacityBg, 3.0f);
    const float fillW = std::clamp(w * frac, 6.0f, w);
    dl->AddRectFilled(p, ImVec2(p.x + fillW, p.y + h), pal::CapacityFill,
                      3.0f);
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), pal::CapacityBorder, 3.0f);
    ImGui::Dummy(ImVec2(w, h + 4));

    ImGui::PushFont(fonts_.label);
    ImGui::TextColored(v4(pal::TextDim), "%s used", formatBytes(used).c_str());
    const std::string freeTxt = formatBytes(dev.freeBytes) + " free";
    const float tw = ImGui::CalcTextSize(freeTxt.c_str()).x;
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - tw);
    ImGui::TextColored(v4(pal::TextDim), "%s", freeTxt.c_str());
    ImGui::PopFont();
}

void App::drawStatusBar() {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos();
    const float w = ImGui::GetWindowWidth();
    const float y0 = wp.y + ImGui::GetWindowHeight() - kStatusBarHeight;
    dl->AddRectFilledMultiColor(
        ImVec2(wp.x, y0), ImVec2(wp.x + w, y0 + kStatusBarHeight),
        pal::StatusTop, pal::StatusTop, pal::StatusBottom, pal::StatusBottom);
    dl->AddLine(ImVec2(wp.x, y0 + 0.5f), ImVec2(wp.x + w, y0 + 0.5f),
                pal::StatusBorder);

    // The icon clusters iTunes 10 put at either end. All of them do
    // something: nothing else in this app draws a control that does not.
    const float iy = y0 + kStatusBarHeight * 0.5f;
    const float statusTop = ImGui::GetWindowHeight() - kStatusBarHeight;
    float ix = 8.0f;

    // One status-bar icon: reserves its slot, reports the click, and hands
    // back where to draw and in what colour, so each icon below is its glyph
    // and nothing else.
    struct Icon {
        ImVec2 c;       // centre to draw around
        ImU32 col;
        bool clicked;
    };
    auto icon = [&](const char* id, bool enabled, bool on) -> Icon {
        ImGui::SetCursorPos(ImVec2(ix, statusTop + 4.0f));
        ImGui::PushID(id);
        ImGui::BeginDisabled(!enabled);
        const bool clicked = ImGui::InvisibleButton("##i", ImVec2(16, 16));
        ImGui::EndDisabled();
        const bool hot = ImGui::IsItemHovered();
        ImGui::PopID();
        const ImVec2 centre(wp.x + ix + 8.0f, iy);
        ix += 22.0f;
        const ImU32 col = !enabled  ? pal::GlyphDim
                          : on      ? pal::GlyphOn
                          : hot     ? pal::GlyphHot
                                    : pal::Glyph;
        return {centre, col, clicked};
    };

    // Each icon is drawn inside the same 13x13 box around its centre, with
    // the same 1.3px stroke, so the row reads as one set rather than four
    // separate doodles.
    constexpr float kStroke = 1.3f;

    // New playlist: a plus.
    {
        const Icon i = icon("newpl", library_ && writesSupported(), false);
        dl->AddLine(ImVec2(i.c.x - 5.5f, i.c.y), ImVec2(i.c.x + 5.5f, i.c.y),
                    i.col, 1.6f);
        dl->AddLine(ImVec2(i.c.x, i.c.y - 5.5f), ImVec2(i.c.x, i.c.y + 5.5f),
                    i.col, 1.6f);
        if (i.clicked) createPlaylist(0);
    }
    // Shuffle: two arrows crossing, each with its head on the stroke it
    // belongs to rather than floating beside it.
    {
        const Icon i = icon("shuffle", true, shuffle_);
        auto arrow = [&](float y0, float y1) {
            const ImVec2 from(i.c.x - 6.5f, i.c.y + y0);
            const ImVec2 to(i.c.x + 3.0f, i.c.y + y1);
            dl->AddLine(from, to, i.col, kStroke);
            // Head aligned to the stroke's direction.
            const float dx = to.x - from.x, dy = to.y - from.y;
            const float len = std::sqrt(dx * dx + dy * dy);
            const float ux = dx / len, uy = dy / len;
            const float px = -uy, py = ux;
            const ImVec2 tip(to.x + ux * 3.5f, to.y + uy * 3.5f);
            dl->AddTriangleFilled(
                tip, ImVec2(to.x + px * 2.6f, to.y + py * 2.6f),
                ImVec2(to.x - px * 2.6f, to.y - py * 2.6f), i.col);
        };
        arrow(-4.0f, 4.0f);
        arrow(4.0f, -4.0f);
        if (i.clicked) shuffle_ = !shuffle_;
    }
    // Repeat: a closed loop with the arrow head sitting on the top edge.
    {
        const Icon i = icon("repeat", true, repeat_ != Repeat::Off);
        const float r = 4.0f, hw = 3.0f;
        constexpr float kPi = 3.14159265f;
        dl->PathArcTo(ImVec2(i.c.x - hw, i.c.y), r, kPi * 0.5f, kPi * 1.5f, 12);
        dl->PathStroke(i.col, 0, kStroke);
        dl->PathArcTo(ImVec2(i.c.x + hw, i.c.y), r, -kPi * 0.5f, kPi * 0.5f,
                      12);
        dl->PathStroke(i.col, 0, kStroke);
        dl->AddLine(ImVec2(i.c.x - hw, i.c.y + r), ImVec2(i.c.x + hw, i.c.y + r),
                    i.col, kStroke);
        dl->AddLine(ImVec2(i.c.x - hw, i.c.y - r), ImVec2(i.c.x + 1.0f, i.c.y - r),
                    i.col, kStroke);
        dl->AddTriangleFilled(ImVec2(i.c.x + 1.0f, i.c.y - r - 2.6f),
                              ImVec2(i.c.x + 1.0f, i.c.y - r + 2.6f),
                              ImVec2(i.c.x + 4.5f, i.c.y - r), i.col);
        if (repeat_ == Repeat::One)
            dl->AddText(fonts_.label, fonts_.labelSize * 0.9f,
                        ImVec2(i.c.x - 1.5f, i.c.y - 4.0f), i.col, "1");
        if (i.clicked)
            repeat_ = repeat_ == Repeat::Off   ? Repeat::All
                      : repeat_ == Repeat::All ? Repeat::One
                                               : Repeat::Off;
    }
    // Show or hide the Now Playing well: a framed picture, sun and hills.
    {
        const Icon i = icon("artwork", true, artworkPaneOpen_);
        const ImVec2 tl(i.c.x - 6.5f, i.c.y - 5.5f);
        const ImVec2 lr(i.c.x + 6.5f, i.c.y + 5.5f);
        dl->AddRect(tl, lr, i.col, 1.5f, 0, kStroke);
        dl->AddCircleFilled(ImVec2(tl.x + 3.5f, tl.y + 3.2f), 1.5f, i.col, 8);
        dl->PathLineTo(ImVec2(tl.x + 1.2f, lr.y - 1.6f));
        dl->PathLineTo(ImVec2(i.c.x - 0.5f, i.c.y + 0.5f));
        dl->PathLineTo(ImVec2(i.c.x + 2.0f, lr.y - 2.6f));
        dl->PathLineTo(ImVec2(i.c.x + 4.0f, i.c.y - 0.5f));
        dl->PathLineTo(ImVec2(lr.x - 1.2f, lr.y - 1.6f));
        dl->PathStroke(i.col, 0, kStroke);
        if (i.clicked) artworkPaneOpen_ = !artworkPaneOpen_;
    }
    const float leftEdge = ix;

    // Eject, at the right end, only when there is something to eject.
    float rightEdge = w - 8.0f;
    const auto& dev = watcher_.device();
    if (dev) {
        rightEdge -= 16.0f;
        ImGui::SetCursorPos(ImVec2(rightEdge, statusTop + 4.0f));
        if (ImGui::InvisibleButton("##ejectstatus", ImVec2(16, 16)))
            ejectRequested_ = true;
        const ImU32 c = ImGui::IsItemHovered() ? pal::GlyphHot : pal::Glyph;
        const float gx = wp.x + rightEdge + 8.0f;
        drawEjectGlyph(dl, ImVec2(gx, iy), c);
    }

    // Whatever is left between the two clusters belongs to the label.
    std::string text;
    if (!statusMsg_.empty() && ImGui::GetTime() < statusMsgUntil_) {
        text = statusMsg_;
        addTextCenteredFit(dl, fonts_.label, fonts_.labelSize,
                           wp.x + leftEdge, wp.x + rightEdge, iy,
                           pal::StatusText, text);
        return;
    }
    const Library* shown = shownLibrary();
    const bool onTracks = shown && showingTracks();
    if (onTracks) {
        text = std::to_string(visible_.size()) + " songs, " +
               formatTotalDuration(visibleTotalMs_) + ", " +
               formatBytes(visibleTotalBytes_);
        // A multi-selection is worth stating: it is what the context menu and
        // Delete will act on.
        if (selection_.size() > 1)
            text += "   ·   " + std::to_string(selection_.size()) + " selected";
    } else if (dev) {
        text = formatBytes(dev->capacityBytes) + " capacity, " +
               formatBytes(dev->freeBytes) + " available";
    } else {
        text = "No iPod connected";
    }
    addTextCenteredFit(dl, fonts_.label, fonts_.labelSize, wp.x + leftEdge,
                       wp.x + rightEdge, iy, pal::StatusText, text);
}
}  // namespace podbox
