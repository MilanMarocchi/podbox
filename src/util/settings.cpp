#include "util/settings.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <system_error>
#include <unistd.h>

namespace fs = std::filesystem;

namespace podbox {
namespace {

bool parseInt(const std::string& s, int* out) {
    if (s.empty()) return false;
    char* end = nullptr;
    errno = 0;
    const long v = std::strtol(s.c_str(), &end, 10);
    if (errno || *end || v < -1000000 || v > 1000000) return false;
    *out = int(v);
    return true;
}

bool parseFloat(const std::string& s, float* out) {
    if (s.empty()) return false;
    char* end = nullptr;
    errno = 0;
    const float v = std::strtof(s.c_str(), &end);
    if (errno || *end || !(v == v)) return false;
    *out = v;
    return true;
}

bool parseBool(const std::string& s, bool* out) {
    if (s == "1" || s == "true") { *out = true; return true; }
    if (s == "0" || s == "false") { *out = false; return true; }
    return false;
}

std::string formatFloat(float v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.3f", double(v));
    return buf;
}

}  // namespace

fs::path settingsPath() {
    const char* home = std::getenv("HOME");
    return fs::path(home ? home : ".") / "Library" / "Application Support" /
           "PodBox" / "settings.ini";
}

Settings parseSettings(const std::string& text) {
    Settings s;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::size_t eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos)
            continue;
        const std::string key = line.substr(0, eq);
        const std::string value = line.substr(eq + 1);
        int i = 0;
        float f = 0;
        bool b = false;
        if (key == "window") {
            WindowFrame w;
            if (std::sscanf(value.c_str(), "%d %d %d %d", &w.x, &w.y,
                            &w.width, &w.height) == 4 &&
                w.valid() && w.width < 100000 && w.height < 100000)
                s.window = w;
        } else if (key == "window_maximized" && parseBool(value, &b)) {
            s.windowMaximized = b;
        } else if (key == "artwork_window" && parseBool(value, &b)) {
            s.artworkWindowOpen = b;
        } else if (key == "volume" && parseFloat(value, &f)) {
            s.volume = std::clamp(f, 0.0f, 1.0f);
        } else if (key == "shuffle" && parseBool(value, &b)) {
            s.shuffle = b;
        } else if (key == "repeat" && parseInt(value, &i) && i >= 0 && i <= 2) {
            s.repeat = i;
        } else if (key == "import_format" && parseInt(value, &i) && i >= 0 &&
                   i <= 3) {
            s.importFormat = i;
        } else if (key == "skip_duplicates" && parseBool(value, &b)) {
            s.skipDuplicates = b;
        } else if (key == "artwork_pane" && parseBool(value, &b)) {
            s.artworkPaneOpen = b;
        } else if (key == "browser_visible" && parseBool(value, &b)) {
            s.browserVisible = b;
        } else if (key == "grid_layout" && parseBool(value, &b)) {
            s.gridLayout = b;
        } else if (key == "browser_height" && parseFloat(value, &f)) {
            s.browserHeight = std::clamp(f, 60.0f, 2000.0f);
        } else if (key == "table") {
            // One ImGui ini line per entry, in order.
            s.tableLayout += value;
            s.tableLayout += '\n';
        }
    }
    return s;
}

std::string formatSettings(const Settings& s) {
    std::ostringstream out;
    out << "# PodBox settings. Rewritten on exit; edit only while PodBox is "
           "closed.\n";
    if (s.window.valid())
        out << "window=" << s.window.x << ' ' << s.window.y << ' '
            << s.window.width << ' ' << s.window.height << '\n';
    out << "window_maximized=" << s.windowMaximized << '\n'
        << "artwork_window=" << s.artworkWindowOpen << '\n';
    if (s.volume) out << "volume=" << formatFloat(*s.volume) << '\n';
    out << "shuffle=" << s.shuffle << '\n'
        << "repeat=" << s.repeat << '\n'
        << "import_format=" << s.importFormat << '\n'
        << "skip_duplicates=" << s.skipDuplicates << '\n'
        << "artwork_pane=" << s.artworkPaneOpen << '\n'
        << "browser_visible=" << s.browserVisible << '\n'
        << "grid_layout=" << s.gridLayout << '\n'
        << "browser_height=" << formatFloat(s.browserHeight) << '\n';
    std::istringstream table(s.tableLayout);
    std::string line;
    while (std::getline(table, line))
        if (!line.empty()) out << "table=" << line << '\n';
    return out.str();
}

Settings loadSettings(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return {};
    std::ostringstream text;
    text << in.rdbuf();
    return parseSettings(text.str());
}

bool saveSettings(const fs::path& file, const Settings& settings) {
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    const fs::path tmp = file.string() + ".tmp-" + std::to_string(getpid());
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << formatSettings(settings);
        out.flush();
        if (!out) {
            out.close();
            fs::remove(tmp, ec);
            return false;
        }
    }
    fs::rename(tmp, file, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

std::string tableSections(const std::string& imguiIni) {
    std::istringstream in(imguiIni);
    std::string line, out;
    bool keep = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty() && line[0] == '[') keep = line.rfind("[Table]", 0) == 0;
        if (keep && !line.empty()) out += line + '\n';
    }
    return out;
}

WindowFrame fitWindowFrame(const WindowFrame& saved,
                           const std::vector<WindowFrame>& workAreas,
                           int minWidth, int minHeight) {
    if (!saved.valid() || workAreas.empty()) return {};
    // The strip the user grabs to move the window.
    constexpr int kGrab = 32;
    const WindowFrame* best = nullptr;
    long bestOverlap = 0;
    for (const WindowFrame& a : workAreas) {
        const int ox = std::min(saved.x + saved.width, a.x + a.width) -
                       std::max(saved.x, a.x);
        const int oy = std::min(saved.y + kGrab, a.y + a.height) -
                       std::max(saved.y, a.y);
        if (ox < 64 || oy < kGrab / 2) continue;
        if (long(ox) * oy > bestOverlap) {
            bestOverlap = long(ox) * oy;
            best = &a;
        }
    }

    WindowFrame f = saved;
    const WindowFrame& area = best ? *best : workAreas.front();
    f.width = std::max(std::min(f.width, area.width), minWidth);
    f.height = std::max(std::min(f.height, area.height), minHeight);
    if (!best) {
        // Its display is gone: centre it on the main one instead.
        f.x = area.x + (area.width - f.width) / 2;
        f.y = area.y + (area.height - f.height) / 2;
        return f;
    }
    // Partly off screen is a choice the user can make, but the top edge must
    // stay below the menu bar, and a window that had to shrink is moved back
    // inside.
    f.y = std::max(f.y, area.y);
    if (f.width != saved.width)
        f.x = std::clamp(f.x, area.x, area.x + area.width - f.width);
    if (f.height != saved.height)
        f.y = std::clamp(f.y, area.y, area.y + area.height - f.height);
    return f;
}

}  // namespace podbox
