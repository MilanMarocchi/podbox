#include "app/app.h"
#include "ui/macos_window.h"
#include "ui/theme.h"
#include "util/settings.h"

#include <podbox_version.h>

#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>

#define GL_SILENCE_DEPRECATION
#include <GLFW/glfw3.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

constexpr int kMinWidth = 900, kMinHeight = 560;

// Finder launches apps with PATH=/usr/bin:/bin:/usr/sbin:/sbin, which hides
// the ffmpeg/lame that MP3 and AAC conversion shell out to. Append the usual
// package-manager locations; a PATH set by a terminal launch still wins.
void addToolDirectoriesToPath() {
    std::string path = std::getenv("PATH") ? std::getenv("PATH") : "";
    const char* home = std::getenv("HOME");
    const std::string dirs[] = {
        "/opt/homebrew/bin",
        "/usr/local/bin",
        "/opt/local/bin",
        home ? std::string(home) + "/.nix-profile/bin" : std::string(),
        "/run/current-system/sw/bin",
    };
    for (const std::string& dir : dirs) {
        if (dir.empty() ||
            (":" + path + ":").find(":" + dir + ":") != std::string::npos)
            continue;
        path += path.empty() ? dir : ":" + dir;
    }
    setenv("PATH", path.c_str(), 1);
}

// Puts the window back where the last session left it, if that is still on
// a display that is connected.
void restoreWindowFrame(GLFWwindow* window, const podbox::Settings& settings) {
    std::vector<podbox::WindowFrame> areas;
    int count = 0;
    GLFWmonitor** monitors = glfwGetMonitors(&count);
    for (int i = 0; i < count; ++i) {
        podbox::WindowFrame a;
        glfwGetMonitorWorkarea(monitors[i], &a.x, &a.y, &a.width, &a.height);
        if (a.valid()) areas.push_back(a);
    }
    const podbox::WindowFrame f =
        podbox::fitWindowFrame(settings.window, areas, kMinWidth, kMinHeight);
    if (!f.valid()) return;
    glfwSetWindowPos(window, f.x, f.y);
    glfwSetWindowSize(window, f.width, f.height);
}

// Remembers the frame the window should come back at. Zoomed and full-screen
// frames are the screen's, not the user's, so only the zoom is recorded then
// and the frame from before it is kept.
void captureWindowFrame(GLFWwindow* window, podbox::Settings& settings) {
    if (glfwGetWindowAttrib(window, GLFW_ICONIFIED) ||
        podbox::windowIsFullScreen(window))
        return;
    settings.windowMaximized = glfwGetWindowAttrib(window, GLFW_MAXIMIZED);
    if (settings.windowMaximized) return;
    podbox::WindowFrame f;
    glfwGetWindowPos(window, &f.x, &f.y);
    glfwGetWindowSize(window, &f.width, &f.height);
    if (f.valid()) settings.window = f;
}

}  // namespace

int main() {
    addToolDirectoriesToPath();
    glfwSetErrorCallback([](int code, const char* desc) {
        std::fprintf(stderr, "GLFW error %d: %s\n", code, desc);
    });
    if (!glfwInit()) return 1;

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);

    const auto settingsFile = podbox::settingsPath();
    podbox::Settings settings = podbox::loadSettings(settingsFile);

    // Created hidden so it appears where it was last time, not at a default
    // spot it then jumps away from.
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    // Version in the title so a screenshot in a bug report identifies itself.
    const std::string title = std::string("PodBox ") + podbox::kVersion;
    GLFWwindow* window =
        glfwCreateWindow(1100, 720, title.c_str(), nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        return 1;
    }
    // The toolbar runs to the top of the window, with the traffic lights
    // over it — see ui/macos_window.h.
    podbox::useUnifiedTitlebar(window);

    // A floor, so the chrome cannot be squeezed into nonsense. Below this the
    // LCD would start eating the volume slider and the sidebar would leave no
    // usable room for the track table. Controls in the toolbar still drop out
    // one at a time above it as things get tight.
    glfwSetWindowSizeLimits(window, kMinWidth, kMinHeight, GLFW_DONT_CARE,
                            GLFW_DONT_CARE);
    restoreWindowFrame(window, settings);
    glfwShowWindow(window);
    if (settings.windowMaximized) glfwMaximizeWindow(window);

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // No imgui.ini: settings.ini keeps the track list's column layout, and
    // nothing else ImGui would save is worth restoring.
    io.IniFilename = nullptr;
    if (!settings.tableLayout.empty())
        ImGui::LoadIniSettingsFromMemory(settings.tableLayout.c_str(),
                                         settings.tableLayout.size());

    float contentScale = 1.0f, ignored = 1.0f;
    glfwGetWindowContentScale(window, &contentScale, &ignored);
    const podbox::Fonts fonts = podbox::setupTheme(contentScale);

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 150");

    podbox::App app(fonts);
    app.setWindow(window);
    app.restoreSettings(settings);
    // Saved as things change, not only at exit, so a crash or a forced quit
    // loses at most the last second or so.
    podbox::Settings saved = settings;
    double lastSettingsCheck = glfwGetTime();
    auto persistSettings = [&] {
        captureWindowFrame(window, settings);
        app.captureSettings(settings);
        settings.tableLayout =
            podbox::tableSections(ImGui::SaveIniSettingsToMemory());
        if (settings != saved && podbox::saveSettings(settingsFile, settings))
            saved = settings;
    };
    podbox::installMediaKeyHandler();
    glfwSetWindowUserPointer(window, &app);
    glfwSetDropCallback(window, [](GLFWwindow* w, int count,
                                   const char** paths) {
        auto* a = static_cast<podbox::App*>(glfwGetWindowUserPointer(w));
        a->onFilesDropped(std::vector<std::string>(paths, paths + count));
    });

    for (;;) {
        if (glfwWindowShouldClose(window) || app.wantsToClose()) {
            if (app.prepareToClose()) break;
            glfwSetWindowShouldClose(window, GLFW_FALSE);
        }
        // Render continuously (capped by vsync) while focused so the UI feels
        // responsive; idle on events when in the background to save power.
        // Either way we wake at least a few times a second so the device
        // watcher can notice an iPod being plugged in or removed.
        if (glfwGetWindowAttrib(window, GLFW_FOCUSED))
            glfwPollEvents();
        else
            glfwWaitEventsTimeout(0.2);

        for (;;) {
            const podbox::MediaKeyCommand command =
                podbox::takeMediaKeyCommand();
            if (command == podbox::MediaKeyCommand::None) break;
            if (command == podbox::MediaKeyCommand::Play)
                app.play();
            else if (command == podbox::MediaKeyCommand::Pause)
                app.pause();
            else
                app.togglePlayback();
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        app.frame();
        podbox::setMediaKeyPlaybackState(app.animating());

        ImGui::Render();
        int fbWidth = 0, fbHeight = 0;
        glfwGetFramebufferSize(window, &fbWidth, &fbHeight);
        glViewport(0, 0, fbWidth, fbHeight);
        glClearColor(0.86f, 0.86f, 0.86f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);

        if (glfwGetTime() - lastSettingsCheck >= 1.0) {
            lastSettingsCheck = glfwGetTime();
            persistSettings();
        }
    }
    persistSettings();
    podbox::closeArtworkWindow();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    podbox::removeMediaKeyHandler();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
