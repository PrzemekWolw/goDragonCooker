#include <cstdint>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#if defined(__APPLE__)
#define GL_SILENCE_DEPRECATION
#endif

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>

#if defined(_WIN32)
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <dwmapi.h>
#endif

#include "app_icon.h"
#include "core/settings.h"
#include "core/task.h"
#include "gpu/gpu.h"
#include "tabs/tabs.h"
#include "ui/theme.h"
#include "ui/widgets.h"

static void setAppIcon(GLFWwindow* window)
{
#if !defined(__APPLE__)
    auto u16 = [](const unsigned char* p) { return int(p[0] | (p[1] << 8)); };
    auto u32 = [](const unsigned char* p) { return int(uint32_t(p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24))); };

    const unsigned char* data = appIcon;
    const int size = int(sizeof(appIcon));
    if (size < 22 || u16(data) != 0 || u16(data + 2) != 1 || u16(data + 4) < 1) {
        return;
    }
    const int imageSize = u32(data + 14);
    const int offset = u32(data + 18);
    if (offset < 0 || imageSize < 40 || offset + imageSize > size) {
        return;
    }

    // Single-image ICO holding a BMP (BITMAPINFOHEADER + pixels + AND mask).
    const unsigned char* bmp = data + offset;
    const int headerSize = u32(bmp);
    const int width = u32(bmp + 4);
    const int height = u32(bmp + 8) / 2;
    const int bits = u16(bmp + 14);
    if (headerSize < 40 || width <= 0 || height <= 0 || (bits != 24 && bits != 32)) {
        return;
    }
    const int rowSize = ((width * bits + 31) / 32) * 4;
    const int maskRowSize = ((width + 31) / 32) * 4;
    const int pixelOffset = headerSize;
    const int maskOffset = pixelOffset + rowSize * height;
    if (maskOffset + maskRowSize * height > imageSize) {
        return;
    }

    bool hasAlpha = false;
    for (int i = 0; bits == 32 && i < width * height && !hasAlpha; ++i) {
        hasAlpha = bmp[pixelOffset + (i / width) * rowSize + (i % width) * 4 + 3] != 0;
    }

    std::vector<unsigned char> pixels(size_t(width) * height * 4);
    for (int y = 0; y < height; ++y) {
        const unsigned char* src = bmp + pixelOffset + (height - 1 - y) * rowSize;
        const unsigned char* mask = bmp + maskOffset + (height - 1 - y) * maskRowSize;
        for (int x = 0; x < width; ++x) {
            const unsigned char* p = src + x * (bits / 8);
            unsigned char* out = &pixels[(size_t(y) * width + x) * 4];
            out[0] = p[2];
            out[1] = p[1];
            out[2] = p[0];
            out[3] = (bits == 32 && hasAlpha) ? p[3] : 255;
            if (mask[x / 8] & (0x80 >> (x & 7))) {
                out[3] = 0;
            }
        }
    }
    GLFWimage image = {width, height, pixels.data()};
    glfwSetWindowIcon(window, 1, &image);
#else
    (void)window;
#endif
}

static void setTitleBarColor(GLFWwindow* window)
{
#if defined(_WIN32)
    const HWND hwnd = glfwGetWin32Window(window);
    auto setColor = [&](DWORD attribute, const ImVec4& color) {
        const COLORREF value = RGB(int(color.x * 255), int(color.y * 255), int(color.z * 255));
        DwmSetWindowAttribute(hwnd, attribute, &value, sizeof(value));
    };
    setColor(35, colBar);  // DWMWA_CAPTION_COLOR
    setColor(36, colText); // DWMWA_TEXT_COLOR
#else
    (void)window;
#endif
}

static void renderUI()
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::Begin("##main", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleVar();

    std::string texconvWarn;
    bool warnShown;
    {
        std::lock_guard lock(app.mu);
        warnShown = app.warnShown;
        texconvWarn = app.texconvWarn;
    }
    if (warnShown) {
        ImGui::OpenPopup("texconv_warning");
    }
    if (ImGui::BeginPopupModal("texconv_warning", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Warning: %s", texconvWarn.c_str());
        ImGui::Dummy(ImVec2(0, px(10)));
        if (ImGui::Button("OK")) {
            {
                std::lock_guard lock(app.mu);
                app.warnShown = false;
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::Separator();
    if (ImGui::BeginTabBar("##tabs")) {
        cookTabUI();
        matGenTabUI();
        ImGui::EndTabBar();
    }
    ImGui::End();

    renderDirPicker();
}

int main()
{
    registerGpuSettings();
    cookTabInit();
    matGenTabInit();
    loadSettings();

    if (!glfwInit()) {
        return 1;
    }
    setRedrawCallback([] { glfwPostEmptyEvent(); });
#if defined(__APPLE__)
    const char* glslVersion = "#version 150";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#else
    const char* glslVersion = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#endif

    const std::string title = std::string("Materials Tools (GoDragonPBR) v") + APP_VERSION;
    GLFWwindow* window = glfwCreateWindow(1280, 900, title.c_str(), nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    setAppIcon(window);
    setTitleBarColor(window);

    uiScale = 1.0f;
#if !defined(__APPLE__)
    glfwGetWindowContentScale(window, &uiScale, nullptr);
#endif
    if (uiScale < 1.0f) {
        uiScale = 1.0f;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    applyTheme(uiScale);
    loadFont();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glslVersion);

    std::thread(runStartupGPUCheck).detach();

    while (!glfwWindowShouldClose(window)) {
        glfwWaitEventsTimeout(0.1);
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        renderUI();
        ImGui::Render();

        int width, height;
        glfwGetFramebufferSize(window, &width, &height);
        glViewport(0, 0, width, height);
        glClearColor(colBg.x, colBg.y, colBg.z, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    // Background cook/GPU-check threads may still be running; skip static destructors.
    std::_Exit(0);
}
