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

void applyTheme()
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
    using S = ImGuiStyle;
    for (float S::*f : {&S::WindowRounding, &S::WindowBorderSize, &S::WindowBorderHoverPadding, &S::ChildRounding, &S::ChildBorderSize,
                        &S::PopupRounding, &S::PopupBorderSize, &S::FrameBorderSize, &S::FrameRounding, &S::IndentSpacing,
                        &S::ColumnsMinSpacing, &S::ScrollbarSize, &S::ScrollbarRounding, &S::ScrollbarPadding, &S::GrabMinSize,
                        &S::GrabRounding, &S::LogSliderDeadzone, &S::ImageRounding, &S::ImageBorderSize, &S::TabRounding,
                        &S::TabBorderSize, &S::TabMinWidthBase, &S::TabMinWidthShrink, &S::TabBarBorderSize, &S::TabBarOverlineSize,
                        &S::TreeLinesSize, &S::TreeLinesRounding, &S::MenuItemRounding, &S::SelectableRounding,
                        &S::DragDropTargetRounding, &S::DragDropTargetBorderSize, &S::DragDropTargetPadding, &S::ColorMarkerSize,
                        &S::InputTextCursorSize, &S::SeparatorSize, &S::SeparatorTextBorderSize, &S::MouseCursorScale}) {
        style.*f = px(style.*f);
    }
    for (ImVec2 S::*v : {&S::WindowPadding, &S::WindowMinSize, &S::FramePadding, &S::ItemSpacing, &S::ItemInnerSpacing, &S::CellPadding,
                         &S::TouchExtraPadding, &S::SeparatorTextPadding, &S::DisplayWindowPadding, &S::DisplaySafeAreaPadding}) {
        style.*v = ImVec2(px((style.*v).x), px((style.*v).y));
    }

    style.FontSizeBase = 18.7f;
    style.FontScaleDpi = uiScale;
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
