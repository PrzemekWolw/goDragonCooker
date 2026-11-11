#include "ui/theme.h"

#include <algorithm>
#include <cmath>

#include "core/platform.h"

float uiScale = 1.0f;

float px(float value)
{
    if (value == 0.0f) {
        return 0.0f;
    }
    const float sign = value < 0.0f ? -1.0f : 1.0f;
    return sign * std::max(1.0f, std::round(std::fabs(value) * uiScale));
}

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

void applyTheme(float scale)
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

static fs::path findFont(const char* name)
{
    const fs::path candidates[] = {
        exeDir() / "fonts" / name,
        fs::path("app/fonts") / name,
        fs::path("fonts") / name,
    };
    for (const fs::path& path : candidates) {
        std::error_code ec;
        if (fs::is_regular_file(path, ec)) {
            return path;
        }
    }
    return {};
}

void loadFont()
{
    const fs::path text = findFont("OpenSans-Regular.ttf");
    if (text.empty()) {
        return;
    }
    ImFontAtlas* atlas = ImGui::GetIO().Fonts;
    atlas->AddFontFromFileTTF(u8str(text).c_str(), 18.7f);

    const fs::path icons = findFont("MaterialIcons-Regular.ttf");
    if (icons.empty()) {
        return;
    }
    // Material Icons sit on the baseline with no descent; shift them down to center on the text.
    ImFontConfig config;
    config.MergeMode = true;
    config.GlyphMinAdvanceX = 18.7f;
    config.GlyphOffset = ImVec2(0, 4.0f);
    atlas->AddFontFromFileTTF(u8str(icons).c_str(), 18.7f, &config);
}
