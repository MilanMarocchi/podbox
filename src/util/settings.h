#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace podbox {

// A window's position and size in screen coordinates, as GLFW reports them.
struct WindowFrame {
    int x = 0, y = 0, width = 0, height = 0;
    bool valid() const { return width > 0 && height > 0; }
    bool operator==(const WindowFrame&) const = default;
};

// Everything PodBox remembers between launches that is not library data:
// where the window was, and the choices the user made in the UI. Defaults
// match a first launch, so a missing or damaged file just means defaults.
struct Settings {
    WindowFrame window;  // the un-zoomed, non-full-screen frame
    bool windowMaximized = false;
    bool artworkWindowOpen = false;

    std::optional<float> volume;  // unset leaves the player's own default
    bool shuffle = false;
    int repeat = 0;  // App::Repeat
    int importFormat = 0;  // ImportFormat
    bool skipDuplicates = true;
    bool artworkPaneOpen = true;
    bool browserVisible = true;
    float browserHeight = 170.0f;

    // ImGui's [Table] ini sections: column widths, order, visibility and the
    // sort column of the track list.
    std::string tableLayout;

    bool operator==(const Settings&) const = default;
};

// ~/Library/Application Support/PodBox/settings.ini
std::filesystem::path settingsPath();

// Unknown keys and unparsable values are skipped, so a file from a newer or
// older PodBox loads whatever it can.
Settings parseSettings(const std::string& text);
std::string formatSettings(const Settings& settings);

Settings loadSettings(const std::filesystem::path& file);
// Written to a temporary file and renamed over the old one.
bool saveSettings(const std::filesystem::path& file, const Settings& settings);

// Keeps only the [Table][...] sections of an ImGui ini dump. Window
// positions are left out: the main window is laid out by hand every frame,
// and dialogs centre themselves.
std::string tableSections(const std::string& imguiIni);

// Where to put a saved window given the monitors' usable areas now, or an
// invalid frame to let the system choose. A display that has gone away, or a
// smaller one, must never leave the window off screen or larger than the
// screen. The saved frame is kept as is when its top edge, where the
// toolbar and traffic lights are, is still reachable.
WindowFrame fitWindowFrame(const WindowFrame& saved,
                           const std::vector<WindowFrame>& workAreas,
                           int minWidth, int minHeight);

}  // namespace podbox
