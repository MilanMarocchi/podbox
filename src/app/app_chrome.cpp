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
#include <cstdio>
#include <string>

namespace fs = std::filesystem;

namespace podbox {

void App::updateArtwork() {
    const Library* shown = shownLibrary();
    const auto* index = shownIndex();
    if (!shown || !index || selectedTrackId_ == 0) {
        artHasImage_ = false;
        artTrackId_ = 0;
        return;
    }
    if (selectedTrackId_ == artTrackId_) return;  // already current
    artTrackId_ = selectedTrackId_;
    artHasImage_ = false;

    const auto it = index->find(selectedTrackId_);
    if (it == index->end()) return;
    const fs::path file = trackFilePath(shown->tracks[it->second]);
    const ArtImage img = loadEmbeddedArtwork(file);
    if (!img.ok()) return;

    if (artTexture_ == 0) glGenTextures(1, &artTexture_);
    glBindTexture(GL_TEXTURE_2D, artTexture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, img.width, img.height, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, img.rgba.data());
    artHasImage_ = true;
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
    dl->AddRectFilled(p, ImVec2(p.x + box, p.y + box), pal::rgb(255, 255, 255),
                      2.0f);
    if (artHasImage_ && artTexture_) {
        dl->AddImage((ImTextureID)(intptr_t)artTexture_, p,
                     ImVec2(p.x + box, p.y + box));
    } else {
        // Empty "no artwork" well, like iTunes' placeholder note glyph.
        addTextCentered(dl, fonts_.label, fonts_.labelSize,
                        ImVec2(p.x + box * 0.5f, p.y + box * 0.5f),
                        pal::rgb(170, 176, 184),
                        selectedTrackId_ ? "No artwork" : "");
    }
    dl->AddRect(p, ImVec2(p.x + box, p.y + box), pal::rgb(168, 174, 182), 2.0f);
}

void App::drawTransport(float toolbarWidth) {
    (void)toolbarWidth;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float cy = kToolbarHeight * 0.5f;
    const PlaybackState st = player_ ? player_->state() : PlaybackState::Stopped;
    const bool haveList = library_ && !visible_.empty();
    const ImU32 glyph = haveList ? pal::rgb(60, 66, 74) : pal::rgb(178, 184, 190);
    constexpr float kPrevW = 26.0f, kPlayW = 30.0f, kNextW = 26.0f;
    constexpr float kGap = 4.0f, kStartX = 16.0f;
    float x = kStartX;
    bool clicked = false;

    // One bezelled capsule behind all three, the way the iTunes transport was
    // drawn — separate flat glyphs read as a debug UI.
    {
        const ImVec2 wp = ImGui::GetWindowPos();
        const float w = kPrevW + kPlayW + kNextW + kGap * 2.0f;
        const ImVec2 a(wp.x + kStartX - 5.0f, wp.y + cy - 15.0f);
        const ImVec2 b(a.x + w + 10.0f, wp.y + cy + 15.0f);
        aqua::gradientRect(dl, a, b, pal::rgb(252, 252, 253),
                           pal::rgb(214, 217, 221), pal::rgb(150, 154, 160),
                           15.0f);
        // Hairlines separating the three, as on the real control.
        for (float sx : {kStartX + kPrevW + kGap * 0.5f,
                         kStartX + kPrevW + kGap + kPlayW + kGap * 0.5f})
            dl->AddLine(ImVec2(wp.x + sx, a.y + 5.0f),
                        ImVec2(wp.x + sx, b.y - 5.0f),
                        pal::rgb(190, 194, 199));
    }

    auto button = [&](const char* id, float w) -> ImVec2 {
        ImGui::SetCursorPos(ImVec2(x, cy - 13.0f));
        ImGui::InvisibleButton(id, ImVec2(w, 26.0f));
        clicked = ImGui::IsItemClicked();
        const ImVec2 mn = ImGui::GetItemRectMin();
        const ImVec2 mx = ImGui::GetItemRectMax();
        x += w + kGap;
        return ImVec2((mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f);
    };

    // Previous.
    {
        const ImVec2 c = button("##prev", 26.0f);
        dl->AddTriangleFilled(ImVec2(c.x + 3, c.y - 5), ImVec2(c.x + 3, c.y + 5),
                              ImVec2(c.x - 3, c.y), glyph);
        dl->AddRectFilled(ImVec2(c.x - 6, c.y - 5), ImVec2(c.x - 4, c.y + 5),
                          glyph);
        if (clicked && haveList) playRelative(-1);
    }
    // Play / pause.
    {
        const ImVec2 c = button("##playpause", 30.0f);
        if (st == PlaybackState::Playing) {
            dl->AddRectFilled(ImVec2(c.x - 5, c.y - 6), ImVec2(c.x - 1, c.y + 6),
                              glyph);
            dl->AddRectFilled(ImVec2(c.x + 1, c.y - 6), ImVec2(c.x + 5, c.y + 6),
                              glyph);
        } else {
            dl->AddTriangleFilled(ImVec2(c.x - 5, c.y - 7),
                                  ImVec2(c.x - 5, c.y + 7),
                                  ImVec2(c.x + 6, c.y), glyph);
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
                playTrackId(library_->tracks[visible_[0].second].id);
        }
    }
    // Next.
    {
        const ImVec2 c = button("##next", 26.0f);
        dl->AddTriangleFilled(ImVec2(c.x - 3, c.y - 5), ImVec2(c.x - 3, c.y + 5),
                              ImVec2(c.x + 3, c.y), glyph);
        dl->AddRectFilled(ImVec2(c.x + 4, c.y - 5), ImVec2(c.x + 6, c.y + 5),
                          glyph);
        if (clicked && haveList) playRelative(+1);
    }

    // Volume slider with a small speaker glyph.
    x += 10.0f;
    ImGui::SetCursorPos(ImVec2(x, cy - 8.0f));
    ImGui::InvisibleButton("##volume", ImVec2(88.0f, 16.0f));
    const ImVec2 vmn = ImGui::GetItemRectMin();
    const ImVec2 vmx = ImGui::GetItemRectMax();
    const float ty = (vmn.y + vmx.y) * 0.5f;
    const float tx0 = vmn.x + 16.0f, tx1 = vmx.x;
    // Speaker.
    dl->AddRectFilled(ImVec2(vmn.x, ty - 3), ImVec2(vmn.x + 4, ty + 3),
                      pal::rgb(120, 126, 134));
    dl->AddTriangleFilled(ImVec2(vmn.x + 4, ty - 5), ImVec2(vmn.x + 4, ty + 5),
                          ImVec2(vmn.x + 9, ty), pal::rgb(120, 126, 134));
    float vol = player_ ? player_->volume() : 0.0f;
    if (ImGui::IsItemActive()) {
        vol = std::clamp((ImGui::GetIO().MousePos.x - tx0) / (tx1 - tx0), 0.0f,
                         1.0f);
        if (player_) player_->setVolume(vol);
    }
    dl->AddLine(ImVec2(tx0, ty), ImVec2(tx1, ty), pal::rgb(176, 182, 190), 2.0f);
    dl->AddLine(ImVec2(tx0, ty), ImVec2(tx0 + (tx1 - tx0) * vol, ty),
                pal::rgb(120, 140, 175), 2.0f);
    dl->AddCircleFilled(ImVec2(tx0 + (tx1 - tx0) * vol, ty), 5.0f,
                        pal::rgb(88, 94, 102));
}

void App::drawNowPlaying(ImVec2 a, ImVec2 b) {
    if (!library_) return;
    const auto it = trackIndexById_.find(playingTrackId_);
    if (it == trackIndexById_.end()) return;
    const Track& t = library_->tracks[it->second];
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float cx = (a.x + b.x) * 0.5f;

    addTextCentered(dl, fonts_.ui, fonts_.uiSize, ImVec2(cx, a.y + 11.0f),
                    pal::LcdText, t.title.c_str());
    std::string sub = t.artist;
    if (!t.album.empty()) sub += sub.empty() ? t.album : "  —  " + t.album;
    addTextCentered(dl, fonts_.label, fonts_.labelSize,
                    ImVec2(cx, a.y + 24.0f), pal::LcdTextDim, sub.c_str());

    const double pos = player_->position();
    const double dur = player_->duration();
    const float y = b.y - 9.0f;
    const float x0 = a.x + 46.0f, x1 = b.x - 46.0f;
    const float frac =
        dur > 0 ? float(std::clamp(pos / dur, 0.0, 1.0)) : 0.0f;
    dl->AddLine(ImVec2(x0, y), ImVec2(x1, y), pal::rgb(185, 192, 200), 2.0f);
    dl->AddLine(ImVec2(x0, y), ImVec2(x0 + (x1 - x0) * frac, y),
                pal::rgb(84, 132, 214), 2.0f);
    dl->AddCircleFilled(ImVec2(x0 + (x1 - x0) * frac, y), 4.5f,
                        pal::rgb(70, 110, 180));

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

    drawTransport(w);

    // Centered "LCD" status display, like the old iTunes readout.
    const float lcdW = std::min(kLcdWidth, w - 40.0f);
    if (lcdW > 80.0f) {
        const ImVec2 a(p.x + (w - lcdW) * 0.5f,
                       p.y + (kToolbarHeight - kLcdHeight) * 0.5f);
        const ImVec2 b(a.x + lcdW, a.y + kLcdHeight);
        // Sunken well: a soft vertical wash, a dark inner top edge for the
        // recess, and a highlight just below the frame.
        dl->AddRectFilled(a, b, pal::LcdBg, 5.0f);
        dl->AddRectFilledMultiColor(
            ImVec2(a.x + 1, a.y + 1), ImVec2(b.x - 1, a.y + kLcdHeight * 0.55f),
            IM_COL32(255, 255, 255, 150), IM_COL32(255, 255, 255, 150),
            IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 0));
        dl->AddLine(ImVec2(a.x + 4, a.y + 1.0f), ImVec2(b.x - 4, a.y + 1.0f),
                    IM_COL32(120, 132, 145, 110));
        dl->AddRect(a, b, pal::LcdBorder, 5.0f);
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
            const float cx = (a.x + b.x) * 0.5f;
            addTextCentered(dl, fonts_.ui, fonts_.uiSize,
                            ImVec2(cx, a.y + 12.0f), pal::LcdText,
                            line1.c_str());
            addTextCentered(dl, fonts_.label, fonts_.labelSize,
                            ImVec2(cx, a.y + 25.0f), pal::LcdTextDim,
                            line2.c_str());
            if (syncing && sync_.batchTotal() > 0) {
                // Thin progress bar along the bottom of the LCD.
                const float frac =
                    float(sync_.batchDone()) / float(sync_.batchTotal());
                const ImVec2 p0(a.x + 8, b.y - 7);
                const ImVec2 p1(b.x - 8, b.y - 3);
                dl->AddRectFilled(p0, p1, pal::rgb(196, 205, 212), 2.0f);
                dl->AddRectFilled(
                    p0,
                    ImVec2(p0.x + std::max(4.0f, (p1.x - p0.x) * frac), p1.y),
                    pal::CapacityFill, 2.0f);
            }
        }
    }

    // Search box, right-aligned like iTunes.
    if (library_ && w > kLcdWidth + 2.0f * (kSearchWidth + 60.0f)) {
        ImGui::SetCursorPos(
            ImVec2(w - kSearchWidth - 16.0f, (kToolbarHeight - 23.0f) * 0.5f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 11.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10, 4));
        ImGui::SetNextItemWidth(kSearchWidth);
        if (ImGui::InputTextWithHint("##search", "Search", search_,
                                     sizeof(search_)))
            visibleDirty_ = true;
        ImGui::PopStyleVar(2);
    }
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
    const float listHeight =
        haveLib ? std::max(60.0f, height - kArtPane) : height;
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
            dl->AddRectFilledMultiColor(mn, mx, pal::rgb(116, 162, 226),
                                        pal::rgb(116, 162, 226),
                                        pal::rgb(50, 108, 200),
                                        pal::rgb(50, 108, 200));
            dl->AddLine(mn, ImVec2(mx.x, mn.y), pal::rgb(150, 186, 236));
        }
        dl->AddText(fonts_.ui, fonts_.uiSize, ImVec2(pos.x + indent, pos.y + 2),
                    selected ? IM_COL32_WHITE : pal::rgb(30, 30, 30),
                    text.c_str());
        return clicked;
    };

    // The Mac's own collection, above the device — this is the library that
    // exists whether or not an iPod is plugged in.
    sectionHeader("LIBRARY");
    {
        const std::string label =
            "Music  (" + std::to_string(host_.tracks().size()) + ")";
        if (row("hostlib", label, 28.0f, view_ == View::Library)) {
            switchSource(View::Library);
        }
    }
    ImGui::SetCursorPosX(10);
    ImGui::PushFont(fonts_.label);
    ImGui::BeginDisabled(hostScanning_);
    if (ImGui::SmallButton(hostScanning_ ? "Scanning…" : "Rescan"))
        rescanWatchFolders();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::SmallButton("Folders…")) foldersOpen_ = true;
    ImGui::SameLine();
    if (ImGui::SmallButton("Apple Music…")) appleMusicOpen_ = true;
    ImGui::PopFont();

    sectionHeader("DEVICES");
    const auto& dev = watcher_.device();
    if (dev) {
        const ImVec2 iconPos = ImGui::GetCursorScreenPos();
        if (row("device", dev->volumeName, 28.0f, view_ == View::Device))
            view_ = View::Device;
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
                                ? pal::rgb(40, 90, 200)
                                : (devSelected ? IM_COL32_WHITE
                                               : pal::rgb(90, 98, 108));
        dl->AddTriangleFilled(ImVec2(ejx, ejy + 6), ImVec2(ejx + 10, ejy + 6),
                              ImVec2(ejx + 5, ejy), ejCol);
        dl->AddRectFilled(ImVec2(ejx, ejy + 8), ImVec2(ejx + 10, ejy + 11),
                          ejCol);
        ImGui::SetCursorScreenPos(afterRow);
        drawIpodIcon(dl, ImVec2(iconPos.x + 10, iconPos.y + 1));
        if (library_) {
            if (row("music", "Music", 28.0f, view_ == View::Music)) {
                switchSource(View::Music);
            }
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
            if (renamePlaylistIndex_ == i) {
                // Inline rename field.
                ImGui::SetCursorPosX(18);
                ImGui::SetNextItemWidth(kSidebarWidth - 26.0f);
                if (renameJustOpened_) {
                    ImGui::SetKeyboardFocusHere();
                    renameJustOpened_ = false;
                }
                const bool done = ImGui::InputText(
                    "##rename", renameBuf_, sizeof(renameBuf_),
                    ImGuiInputTextFlags_EnterReturnsTrue |
                        ImGuiInputTextFlags_AutoSelectAll);
                if (done || ImGui::IsItemDeactivated()) {
                    if (renameBuf_[0]) {
                        library_->playlists[i].name = renameBuf_;
                        writeDatabase();
                    }
                    renamePlaylistIndex_ = -1;
                }
                continue;
            }
            const std::string id = "pl" + std::to_string(i);
            if (row(id.c_str(), library_->playlists[i].name, 20.0f, selected)) {
                switchSource(View::Playlist, i);
            }
            if (ImGui::BeginPopupContextItem(
                    ("plctx" + std::to_string(i)).c_str())) {
                ImGui::PushStyleColor(ImGuiCol_Text, v4(pal::rgb(30, 30, 30)));
                ImGui::BeginDisabled(!writesSupported());
                if (ImGui::MenuItem("Rename")) {
                    renamePlaylistIndex_ = i;
                    renameJustOpened_ = true;
                    std::snprintf(renameBuf_, sizeof(renameBuf_), "%s",
                                  library_->playlists[i].name.c_str());
                }
                if (ImGui::MenuItem("Delete")) deletePlaylistIndex_ = i;
                ImGui::EndDisabled();
                ImGui::PopStyleColor();
                ImGui::EndPopup();
            }
        }
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::SetCursorPosX(18);
        ImGui::PushStyleColor(ImGuiCol_Text, v4(pal::rgb(70, 80, 92)));
        ImGui::BeginDisabled(!writesSupported());
        if (ImGui::SmallButton("+ New Playlist")) createPlaylist(0);
        ImGui::EndDisabled();
        ImGui::PopStyleColor();
    }

    ImGui::EndChild();

    if (haveLib) drawArtworkPane(height);

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void App::drawMainPanel(float height) {
    const float width = ImGui::GetWindowWidth() - kSidebarWidth;
    const auto& dev = watcher_.device();
    const bool trackView =
        view_ == View::Library
            ? true
            : (dev && library_ &&
               (view_ == View::Music || view_ == View::Playlist));

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
        drawTrackTable();
    } else {
        drawDeviceView(*dev);
    }

    ImGui::EndChild();
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
                case 0: dbInfo += " — writable, no hash required"; break;
                case 1: dbInfo += " — writes require hash58"; break;
                case 2: dbInfo += " — writes require hash72"; break;
                default:
                    dbInfo += " — unknown hash scheme";
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
        syncOpen_ = true;
        syncDirty_ = true;
        syncConfirmRemove_ = false;
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
        duplicatesOpen_ = true;
        duplicatesDirty_ = true;
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
    if (!writesSupported())
        ImGui::TextColored(v4(pal::rgb(150, 90, 20)),
                           "This iPod's database carries a checksum PodBox "
                           "cannot produce yet, so it is read-only here. "
                           "Nothing on the device will be changed.");
    else if (appleMusicSyncing())
        ImGui::TextColored(v4(pal::rgb(150, 90, 20)),
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
                                    pal::rgb(250, 250, 251),
                                    pal::rgb(250, 250, 251),
                                    pal::rgb(226, 228, 232),
                                    pal::rgb(226, 228, 232));
        dl->AddLine(ImVec2(p.x, p.y + h - 0.5f), ImVec2(p.x + w, p.y + h - 0.5f),
                    pal::rgb(168, 173, 180));
    }
    ImGui::TableHeadersRow();

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

    // Reordering is only meaningful for a playlist shown in manual order
    // (no search filter, no column sort).
    const bool reorderable = view_ == View::Playlist && playlistIndex_ >= 0 &&
                             sortCol_ == 0 && search_[0] == '\0';
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

    const auto& dev = watcher_.device();
    std::string text;
    if (!statusMsg_.empty() && ImGui::GetTime() < statusMsgUntil_) {
        text = statusMsg_;
        addTextCentered(dl, fonts_.label, fonts_.labelSize,
                        ImVec2(wp.x + w * 0.5f, y0 + kStatusBarHeight * 0.5f),
                        pal::StatusText, text.c_str());
        return;
    }
    const Library* shown = shownLibrary();
    const bool onTracks = shown && (view_ == View::Library ||
                                    ((view_ == View::Music ||
                                      view_ == View::Playlist) && dev));
    if (onTracks) {
        std::uint64_t totalMs = 0, totalBytes = 0;
        for (const auto& [pos, ti] : visible_) {
            totalMs += shown->tracks[ti].lengthMs;
            totalBytes += shown->tracks[ti].sizeBytes;
        }
        text = std::to_string(visible_.size()) + " songs, " +
               formatTotalDuration(totalMs) + ", " + formatBytes(totalBytes);
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
    addTextCentered(dl, fonts_.label, fonts_.labelSize,
                    ImVec2(wp.x + w * 0.5f, y0 + kStatusBarHeight * 0.5f),
                    pal::StatusText, text.c_str());
}
}  // namespace podbox
