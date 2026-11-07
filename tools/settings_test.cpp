// Tests for util/settings: the file round-trip, tolerance of damaged or
// foreign files, and where a remembered window is put back.

#include "util/settings.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;
using podbox::Settings;
using podbox::WindowFrame;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("  FAIL: %s\n", what);
        ++failures;
    }
}

void roundTrip() {
    Settings s;
    s.window = {120, 80, 1280, 800};
    s.windowMaximized = true;
    s.artworkWindowOpen = true;
    s.volume = 0.25f;
    s.shuffle = true;
    s.repeat = 2;
    s.importFormat = 3;
    s.skipDuplicates = false;
    s.artworkPaneOpen = false;
    s.browserVisible = false;
    s.browserHeight = 240.0f;
    s.tableLayout =
        "[Table][0x1234ABCD,8]\nRefScale=13\nColumn 0  Width=34 Sort=0v\n";
    const Settings back = podbox::parseSettings(podbox::formatSettings(s));
    check(back == s, "every field survives format and parse");

    const fs::path dir = fs::temp_directory_path() /
                         ("podbox-settings-test-" + std::to_string(getpid()));
    const fs::path file = dir / "nested" / "settings.ini";
    check(podbox::saveSettings(file, s), "save creates missing folders");
    check(podbox::loadSettings(file) == s, "saved file loads back");
    s.volume = 0.5f;
    check(podbox::saveSettings(file, s), "save replaces an existing file");
    check(podbox::loadSettings(file).volume == 0.5f, "replacement is read");
    int leftovers = 0;
    for (const auto& e : fs::directory_iterator(file.parent_path()))
        if (e.path() != file) ++leftovers;
    check(leftovers == 0, "no temporary file is left behind");
    fs::remove_all(dir);

    check(podbox::loadSettings(dir / "missing.ini") == Settings{},
          "a missing file means defaults");
}

void tolerance() {
    const Settings s = podbox::parseSettings(
        "garbage line\n"
        "future_key=whatever\n"
        "window=10 20 -5 600\n"
        "volume=loud\n"
        "repeat=7\n"
        "import_format=-1\n"
        "shuffle=maybe\n"
        "browser_height=5\r\n"
        "skip_duplicates=0\r\n");
    check(!s.window.valid(), "a negative window size is ignored");
    check(!s.volume, "an unparsable volume is ignored");
    check(s.repeat == 0, "an out-of-range repeat mode is ignored");
    check(s.importFormat == 0, "an out-of-range import format is ignored");
    check(!s.shuffle, "a non-boolean is ignored");
    check(s.browserHeight == 60.0f, "the browser height is clamped");
    check(!s.skipDuplicates, "CRLF line endings are accepted");
    check(podbox::parseSettings("volume=3\n").volume == 1.0f,
          "volume is clamped to the player's range");
}

void tables() {
    const std::string ini =
        "[Window][Debug##Default]\nPos=60,60\nSize=400,400\n\n"
        "[Table][0xAAAA0001,8]\nColumn 0  Width=34\n\n"
        "[Docking][Data]\n\n"
        "[Table][0xAAAA0002,3]\nColumn 1  Weight=1.0\n";
    const std::string kept = podbox::tableSections(ini);
    check(kept ==
              "[Table][0xAAAA0001,8]\nColumn 0  Width=34\n"
              "[Table][0xAAAA0002,3]\nColumn 1  Weight=1.0\n",
          "only [Table] sections are kept, in order");
}

void frames() {
    const std::vector<WindowFrame> one = {{0, 25, 1440, 875}};
    const WindowFrame onScreen{100, 100, 1100, 720};
    check(podbox::fitWindowFrame(onScreen, one, 900, 560) == onScreen,
          "a frame on screen is kept exactly");

    const WindowFrame partly{1000, 300, 1100, 720};
    check(podbox::fitWindowFrame(partly, one, 900, 560) == partly,
          "a window dragged partly off screen stays where it was put");

    // Saved on an external display to the right that is now unplugged.
    const WindowFrame gone{1600, 100, 1200, 800};
    const WindowFrame moved = podbox::fitWindowFrame(gone, one, 900, 560);
    check(moved.x == 120 && moved.y == 62 && moved.width == 1200 &&
              moved.height == 800,
          "a window on a missing display is centred on the main one");

    // Saved on a big display, restored on a laptop.
    const WindowFrame big{0, 25, 2560, 1400};
    const WindowFrame shrunk = podbox::fitWindowFrame(big, one, 900, 560);
    check(shrunk == WindowFrame{0, 25, 1440, 875},
          "a window larger than the screen is shrunk to fit");

    const WindowFrame underMenu{200, 10, 1100, 720};
    check(podbox::fitWindowFrame(underMenu, one, 900, 560).y == 25,
          "the top edge is kept below the menu bar");

    const std::vector<WindowFrame> two = {{0, 25, 1440, 875},
                                          {1440, 0, 2560, 1440}};
    check(podbox::fitWindowFrame(gone, two, 900, 560) == gone,
          "a window on a second display goes back to it");

    check(!podbox::fitWindowFrame({}, one, 900, 560).valid(),
          "no saved frame leaves placement to the system");
}

}  // namespace

int main() {
    roundTrip();
    tolerance();
    tables();
    frames();
    if (failures) {
        std::printf("settings_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("settings_test: all passed\n");
    return 0;
}
