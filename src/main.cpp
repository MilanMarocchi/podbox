#include "app/app.h"
#include "ui/theme.h"

#include <podbox_version.h>

#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>

#define GL_SILENCE_DEPRECATION
#include <GLFW/glfw3.h>

#include <cstdio>
#include <string>

int main() {
    glfwSetErrorCallback([](int code, const char* desc) {
        std::fprintf(stderr, "GLFW error %d: %s\n", code, desc);
    });
    if (!glfwInit()) return 1;

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);

    // Version in the title so a screenshot in a bug report identifies itself.
    const std::string title = std::string("PodBox ") + podbox::kVersion;
    GLFWwindow* window =
        glfwCreateWindow(1100, 720, title.c_str(), nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;

    float contentScale = 1.0f, ignored = 1.0f;
    glfwGetWindowContentScale(window, &contentScale, &ignored);
    const podbox::Fonts fonts = podbox::setupTheme(contentScale);

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 150");

    podbox::App app(fonts);
    glfwSetWindowUserPointer(window, &app);
    glfwSetDropCallback(window, [](GLFWwindow* w, int count,
                                   const char** paths) {
        auto* a = static_cast<podbox::App*>(glfwGetWindowUserPointer(w));
        a->onFilesDropped(std::vector<std::string>(paths, paths + count));
    });

    while (!glfwWindowShouldClose(window)) {
        // Render continuously (capped by vsync) while focused so the UI feels
        // responsive; idle on events when in the background to save power.
        // Either way we wake at least a few times a second so the device
        // watcher can notice an iPod being plugged in or removed.
        if (glfwGetWindowAttrib(window, GLFW_FOCUSED))
            glfwPollEvents();
        else
            glfwWaitEventsTimeout(0.2);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        app.frame();

        ImGui::Render();
        int fbWidth = 0, fbHeight = 0;
        glfwGetFramebufferSize(window, &fbWidth, &fbHeight);
        glViewport(0, 0, fbWidth, fbHeight);
        glClearColor(0.86f, 0.86f, 0.86f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
