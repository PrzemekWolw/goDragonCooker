#include "app.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <thread>

#include <nlohmann/json.hpp>

#if defined(__APPLE__)
#define GL_SILENCE_DEPRECATION
#endif

#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "imgui_stdlib.h"
#include <GLFW/glfw3.h>

#if defined(_WIN32)
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <dwmapi.h>
#endif

#include "app_icon.h"

AppState app;
CookTab cookTab;
MatGenTab matGenTab;

void requestRedraw()
{
    glfwPostEmptyEvent();
}

float uiScale = 1.0f;

float px(float value)
{
    if (value == 0.0f) {
        return 0.0f;
    }
    const float sign = value < 0.0f ? -1.0f : 1.0f;
    return sign * std::max(1.0f, std::round(std::fabs(value) * uiScale));
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

static fs::path settingsPath()
{
    const fs::path directory = userConfigDir();
    return directory.empty() ? fs::path() : directory / "goDragonCooker" / "settings.json";
}

static void loadSettings()
{
    std::ifstream file(settingsPath(), std::ios::binary);
    if (!file) {
        return;
    }
    try {
        const nlohmann::json settings = nlohmann::json::parse(file);
        const std::string backend = settings.value("textureBackend", "");
        if (backend == textureBackendTexconv || backend == textureBackendCompressonator) {
            app.textureBackend = backend;
        }
        app.gpuAdapter = settings.value("texconvGPU", 0);
        app.vulkanDevice = settings.value("vulkanGPU", 0);
    } catch (const std::exception&) {
    }
}

void saveSettings()
{
    nlohmann::ordered_json settings;
    {
        std::lock_guard lock(app.mu);
        settings["textureBackend"] = app.textureBackend;
        settings["texconvGPU"] = app.gpuAdapter;
        settings["vulkanGPU"] = app.vulkanDevice;
    }
    const fs::path path = settingsPath();
    std::error_code ec;
    if (path.empty() || (fs::create_directories(path.parent_path(), ec), ec)) {
        return;
    }
    std::ofstream(path, std::ios::binary) << settings.dump(4);
}

// ---------------------------------------------------------------------------
// Widgets
// ---------------------------------------------------------------------------

bool comboStrings(const char* id, int* index, const std::vector<std::string>& items, float width)
{
    bool changed = false;
    ImGui::SetNextItemWidth(width);
    if (ImGui::BeginCombo(id, items[*index].c_str())) {
        for (int i = 0; i < int(items.size()); ++i) {
            const bool selected = i == *index;
            ImGui::PushID(i);
            if (ImGui::Selectable(items[i].c_str(), selected)) {
                changed = i != *index;
                *index = i;
            }
            ImGui::PopID();
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

void tooltip(const char* text)
{
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", text);
    }
}

// ---------------------------------------------------------------------------
// Directory picker
// ---------------------------------------------------------------------------

static struct {
    bool open = false;
    bool appearing = false;
    std::string pathStr;
    std::string* target = nullptr;
} dirPicker;

void openDirPicker(std::string* target)
{
    dirPicker.open = true;
    dirPicker.appearing = true;
    dirPicker.target = target;
    if (!target->empty()) {
        dirPicker.pathStr = *target;
    } else if (dirPicker.pathStr.empty()) {
        std::error_code ec;
        dirPicker.pathStr = u8str(fs::current_path(ec));
    }
}

static void renderDirPicker()
{
    if (!dirPicker.open) {
        return;
    }
    if (dirPicker.appearing) {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(px(650), px(500)), ImGuiCond_Always);
        ImGui::SetNextWindowFocus();
        dirPicker.appearing = false;
    }
    if (ImGui::Begin("Pick Directory", &dirPicker.open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::InputText("##picker_path", &dirPicker.pathStr);
        ImGui::SameLine();
        if (ImGui::Button("Up")) {
            const fs::path parent = u8path(dirPicker.pathStr).parent_path();
            if (!parent.empty()) {
                dirPicker.pathStr = u8str(parent);
            }
        }
        ImGui::Separator();
        ImGui::TextUnformatted("Folders");

        if (ImGui::BeginChild("##picker_list", ImVec2(0, -px(70)), ImGuiChildFlags_Borders)) {
            const fs::path path = u8path(dirPicker.pathStr);
            std::error_code ec;
            if (!fs::is_directory(path, ec)) {
                ImGui::Text("Cannot read: %s", dirPicker.pathStr.c_str());
            } else {
                std::vector<std::string> names;
                for (const auto& entry : fs::directory_iterator(path, ec)) {
                    const std::string name = u8str(entry.path().filename());
                    if (entry.is_directory(ec) && !hasPrefix(name, ".")) {
                        names.push_back(name);
                    }
                }
                std::sort(names.begin(), names.end());
                for (const std::string& name : names) {
                    if (ImGui::Selectable((name + "/").c_str())) {
                        dirPicker.pathStr = u8str(path / u8path(name));
                    }
                }
            }
        }
        ImGui::EndChild();

        if (ImGui::Button("Select")) {
            *dirPicker.target = dirPicker.pathStr;
            dirPicker.open = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            dirPicker.open = false;
        }
    }
    ImGui::End();
}

// ---------------------------------------------------------------------------
// Window setup
// ---------------------------------------------------------------------------

static float snap(float value, float scale)
{
    if (value == 0.0f) {
        return 0.0f;
    }
    const float sign = value < 0.0f ? -1.0f : 1.0f;
    return sign * std::max(1.0f, std::round(std::fabs(value) * scale));
}

static ImVec2 snap(const ImVec2& value, float scale)
{
    return ImVec2(snap(value.x, scale), snap(value.y, scale));
}

static void applyTheme(float scale)
{
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;

    colors[ImGuiCol_Text] = colText;
    colors[ImGuiCol_TextDisabled] = colCookErr;
    colors[ImGuiCol_WindowBg] = ImVec4(0.09f, 0.12f, 0.16f, 0.96f);
    colors[ImGuiCol_ChildBg] = ImVec4(0.08f, 0.10f, 0.14f, 0.25f);
    colors[ImGuiCol_PopupBg] = ImVec4(0.11f, 0.15f, 0.20f, 1.00f);
    colors[ImGuiCol_Border] = ImVec4(0.00f, 0.00f, 0.00f, 0.30f);
    colors[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.30f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.16f, 0.19f, 0.25f, 1.00f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.20f, 0.24f, 0.31f, 1.00f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.00f, 0.00f, 0.00f, 0.24f);
    colors[ImGuiCol_TitleBg] = colBar;
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.20f, 0.24f, 0.31f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.05f, 0.06f, 0.08f, 0.78f);
    colors[ImGuiCol_MenuBarBg] = ImVec4(0.15f, 0.18f, 0.24f, 0.35f);
    colors[ImGuiCol_ScrollbarBg] = ImVec4(0.07f, 0.08f, 0.12f, 1.00f);
    colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.15f, 0.18f, 0.24f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.20f, 0.24f, 0.31f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.81f, 0.40f, 0.13f, 1.00f);
    colors[ImGuiCol_CheckMark] = ImVec4(0.81f, 0.40f, 0.13f, 1.00f);
    colors[ImGuiCol_SliderGrab] = ImVec4(0.81f, 0.40f, 0.13f, 0.71f);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(1.00f, 0.40f, 0.00f, 1.00f);
    colors[ImGuiCol_Button] = ImVec4(0.15f, 0.18f, 0.24f, 1.00f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.20f, 0.24f, 0.31f, 1.00f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.81f, 0.40f, 0.12f, 0.78f);
    colors[ImGuiCol_Header] = ImVec4(0.20f, 0.24f, 0.31f, 0.63f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.20f, 0.24f, 0.31f, 0.78f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.20f, 0.24f, 0.31f, 1.00f);
    colors[ImGuiCol_Separator] = ImVec4(0.20f, 0.24f, 0.31f, 1.00f);
    colors[ImGuiCol_SeparatorHovered] = ImVec4(0.30f, 0.36f, 0.47f, 1.00f);
    colors[ImGuiCol_SeparatorActive] = ImVec4(0.81f, 0.40f, 0.13f, 1.00f);
    colors[ImGuiCol_ResizeGrip] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.56f, 0.56f, 0.58f, 1.00f);
    colors[ImGuiCol_ResizeGripActive] = ImVec4(0.06f, 0.05f, 0.07f, 1.00f);
    colors[ImGuiCol_Tab] = ImVec4(0.00f, 0.00f, 0.00f, 0.35f);
    colors[ImGuiCol_TabHovered] = ImVec4(0.26f, 0.29f, 0.39f, 1.00f);
    colors[ImGuiCol_TabSelected] = ImVec4(0.67f, 0.33f, 0.10f, 1.00f);
    colors[ImGuiCol_TabSelectedOverline] = ImVec4(0.81f, 0.40f, 0.13f, 1.00f);
    colors[ImGuiCol_TabDimmed] = ImVec4(0.00f, 0.00f, 0.00f, 0.31f);
    colors[ImGuiCol_TabDimmedSelected] = ImVec4(0.31f, 0.35f, 0.47f, 0.75f);
    colors[ImGuiCol_TabDimmedSelectedOverline] = ImVec4(0.81f, 0.40f, 0.13f, 0.50f);
    colors[ImGuiCol_TextSelectedBg] = ImVec4(1.00f, 0.47f, 0.00f, 0.43f);

    style.WindowPadding = ImVec2(10, 10);
    style.FramePadding = ImVec2(2, 1);
    style.CellPadding = ImVec2(2, 2);
    style.ItemSpacing = ImVec2(6, 4);
    style.ItemInnerSpacing = ImVec2(8, 6);
    style.IndentSpacing = 20.0f;
    style.ScrollbarSize = 15.0f;
    style.GrabMinSize = 5.0f;

    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.TabBorderSize = 0.0f;

    style.WindowRounding = 2.0f;
    style.ChildRounding = 2.0f;
    style.FrameRounding = 4.0f;
    style.PopupRounding = 0.0f;
    style.ScrollbarRounding = 8.0f;
    style.GrabRounding = 4.0f;
    style.TabRounding = 2.0f;

    // Nearest pixel. ImGui's ScaleAllSizes truncates, so a 1px padding stays 1 at 1.75x.
    style.WindowPadding = snap(style.WindowPadding, scale);
    style.WindowRounding = snap(style.WindowRounding, scale);
    style.WindowBorderSize = snap(style.WindowBorderSize, scale);
    style.WindowMinSize = snap(style.WindowMinSize, scale);
    style.WindowBorderHoverPadding = snap(style.WindowBorderHoverPadding, scale);
    style.ChildRounding = snap(style.ChildRounding, scale);
    style.ChildBorderSize = snap(style.ChildBorderSize, scale);
    style.PopupRounding = snap(style.PopupRounding, scale);
    style.PopupBorderSize = snap(style.PopupBorderSize, scale);
    style.FramePadding = snap(style.FramePadding, scale);
    style.FrameBorderSize = snap(style.FrameBorderSize, scale);
    style.FrameRounding = snap(style.FrameRounding, scale);
    style.ItemSpacing = snap(style.ItemSpacing, scale);
    style.ItemInnerSpacing = snap(style.ItemInnerSpacing, scale);
    style.CellPadding = snap(style.CellPadding, scale);
    style.TouchExtraPadding = snap(style.TouchExtraPadding, scale);
    style.IndentSpacing = snap(style.IndentSpacing, scale);
    style.ColumnsMinSpacing = snap(style.ColumnsMinSpacing, scale);
    style.ScrollbarSize = snap(style.ScrollbarSize, scale);
    style.ScrollbarRounding = snap(style.ScrollbarRounding, scale);
    style.ScrollbarPadding = snap(style.ScrollbarPadding, scale);
    style.GrabMinSize = snap(style.GrabMinSize, scale);
    style.GrabRounding = snap(style.GrabRounding, scale);
    style.LogSliderDeadzone = snap(style.LogSliderDeadzone, scale);
    style.ImageRounding = snap(style.ImageRounding, scale);
    style.ImageBorderSize = snap(style.ImageBorderSize, scale);
    style.TabRounding = snap(style.TabRounding, scale);
    style.TabBorderSize = snap(style.TabBorderSize, scale);
    style.TabMinWidthBase = snap(style.TabMinWidthBase, scale);
    style.TabMinWidthShrink = snap(style.TabMinWidthShrink, scale);
    style.TabBarBorderSize = snap(style.TabBarBorderSize, scale);
    style.TabBarOverlineSize = snap(style.TabBarOverlineSize, scale);
    style.TreeLinesSize = snap(style.TreeLinesSize, scale);
    style.TreeLinesRounding = snap(style.TreeLinesRounding, scale);
    style.MenuItemRounding = snap(style.MenuItemRounding, scale);
    style.SelectableRounding = snap(style.SelectableRounding, scale);
    style.DragDropTargetRounding = snap(style.DragDropTargetRounding, scale);
    style.DragDropTargetBorderSize = snap(style.DragDropTargetBorderSize, scale);
    style.DragDropTargetPadding = snap(style.DragDropTargetPadding, scale);
    style.ColorMarkerSize = snap(style.ColorMarkerSize, scale);
    style.InputTextCursorSize = snap(style.InputTextCursorSize, scale);
    style.SeparatorSize = snap(style.SeparatorSize, scale);
    style.SeparatorTextBorderSize = snap(style.SeparatorTextBorderSize, scale);
    style.SeparatorTextPadding = snap(style.SeparatorTextPadding, scale);
    style.DisplayWindowPadding = snap(style.DisplayWindowPadding, scale);
    style.DisplaySafeAreaPadding = snap(style.DisplaySafeAreaPadding, scale);
    style.MouseCursorScale = snap(style.MouseCursorScale, scale);

    style.FontSizeBase = 18.7f;
    style.FontScaleDpi = scale;
}

static void loadFont()
{
    const fs::path candidates[] = {
        exeDir() / "fonts" / "OpenSans-Regular.ttf",
        fs::path("app/fonts/OpenSans-Regular.ttf"),
        fs::path("fonts/OpenSans-Regular.ttf"),
    };
    for (const fs::path& path : candidates) {
        std::error_code ec;
        if (!fs::is_regular_file(path, ec)) {
            continue;
        }
        ImGui::GetIO().Fonts->AddFontFromFileTTF(u8str(path).c_str(), 18.7f);
        return;
    }
}

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

// ---------------------------------------------------------------------------
// Render loop
// ---------------------------------------------------------------------------

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
    matGenTab.settings = defaultMatGenSettings();
    loadSettings();

    if (!glfwInit()) {
        return 1;
    }
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
