#include "ui/widgets.h"

#include <algorithm>
#include <unordered_map>

#include "core/i18n.h"
#include "core/platform.h"
#include "core/settings.h"
#include "core/strings.h"
#include "imgui_stdlib.h"
#include "ui/icons.h"
#include "ui/theme.h"

bool comboStrings(const char* id, int* index, const std::vector<std::string>& items, float width)
{
    bool changed = false;
    ImGui::SetNextItemWidth(width);
    if (ImGui::BeginCombo(id, tr(items[*index]))) {
        for (int i = 0; i < int(items.size()); ++i) {
            const bool selected = i == *index;
            ImGui::PushID(i);
            if (ImGui::Selectable(tr(items[i]), selected)) {
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
        ImGui::SetTooltip("%s", tr(text));
    }
}

void tooltip(const char* title, const char* text)
{
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s\n%s", tr(title), tr(text));
    }
}

std::string iconLabel(const char* icon, const char* id, const std::string& key)
{
    return format("%s %s###%s", icon, tr(id), key.c_str());
}

bool iconButton(const char* icon, const char* key)
{
    return ImGui::Button(format("%s###%s", icon, key).c_str());
}

bool toggleButton(const std::string& label, bool on, const ImVec4& color)
{
    ImGui::PushStyleColor(ImGuiCol_Text, on ? color : colTextDim);
    ImGui::PushStyleColor(ImGuiCol_Button, on ? ImGui::GetStyleColorVec4(ImGuiCol_Button) : ImVec4(0, 0, 0, 0));
    const bool clicked = ImGui::Button(label.c_str());
    ImGui::PopStyleColor(2);
    return clicked;
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

void renderDirPicker()
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
    if (ImGui::Begin(trId("gdc.common.pickDirectory", "pick_directory").c_str(), &dirPicker.open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::InputText("##picker_path", &dirPicker.pathStr);
        ImGui::SameLine();
        const bool up = iconButton(ICON_ARROW_UPWARD, "picker_up");
        tooltip("gdc.common.up");
        if (up) {
            const fs::path parent = u8path(dirPicker.pathStr).parent_path();
            if (!parent.empty()) {
                dirPicker.pathStr = u8str(parent);
            }
        }
        ImGui::Separator();
        ImGui::TextUnformatted(tr("gdc.common.folders"));

        if (ImGui::BeginChild("##picker_list", ImVec2(0, -px(70)), ImGuiChildFlags_Borders)) {
            const fs::path path = u8path(dirPicker.pathStr);
            std::error_code ec;
            if (!fs::is_directory(path, ec)) {
                ImGui::Text(tr("gdc.common.cannotRead"), dirPicker.pathStr.c_str());
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

        if (ImGui::Button(trId("gdc.common.select", "picker_select").c_str())) {
            *dirPicker.target = dirPicker.pathStr;
            dirPicker.open = false;
            saveSettings();
        }
        ImGui::SameLine();
        if (ImGui::Button(trId("gdc.common.cancel", "picker_cancel").c_str())) {
            dirPicker.open = false;
        }
    }
    ImGui::End();
}

// ---------------------------------------------------------------------------
// Log view
// ---------------------------------------------------------------------------

namespace {

enum LogGroup { kErrors, kWarnings, kOk, kOther, kGroupCount };

struct LogViewState {
    std::string filter;
    bool show[kGroupCount] = {true, true, true, true};
    bool autoScroll = true;
    bool scrollToEnd = false;
    bool expanded = false;
    bool focusExpanded = false;
};

struct LogRow {
    const std::string* text;
    LogLevel level;
};

std::unordered_map<std::string, LogViewState> logViews;

LogGroup logGroup(LogLevel level)
{
    switch (level) {
    case LogLevel::Error:
        return kErrors;
    case LogLevel::Warn:
        return kWarnings;
    case LogLevel::Ok:
        return kOk;
    default:
        return kOther;
    }
}

ImVec4 logColor(LogLevel level)
{
    switch (level) {
    case LogLevel::Error:
        return colCookErr;
    case LogLevel::Warn:
        return colWarn;
    case LogLevel::Ok:
        return colCookOk;
    case LogLevel::Detail:
        return colTextDim;
    default:
        return colText;
    }
}

void logToolbar(LogViewState& st, const size_t (&counts)[kGroupCount], const std::vector<LogRow>& rows)
{
    static const char* const groupIcons[kGroupCount] = {ICON_ERROR, ICON_WARNING, ICON_CHECK_CIRCLE, ICON_INFO};
    static const char* const groupNames[kGroupCount] = {"gdc.log.errors", "gdc.log.warnings", "gdc.log.ok",
                                                        "gdc.log.other"};
    static const ImVec4 groupColors[kGroupCount] = {colCookErr, colWarn, colCookOk, colText};

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(tr("gdc.common.output"));

    for (int g = 0; g < kGroupCount; ++g) {
        ImGui::SameLine();
        if (toggleButton(format("%s %zu###group%d", groupIcons[g], counts[g], g), st.show[g], groupColors[g])) {
            st.show[g] = !st.show[g];
            st.scrollToEnd = st.autoScroll;
        }
        tooltip(groupNames[g], "gdc.log.groups.tooltip");
    }

    ImGui::SameLine();
    if (toggleButton(ICON_VERTICAL_ALIGN_BOTTOM "###autoscroll", st.autoScroll, colText)) {
        st.autoScroll = !st.autoScroll;
        st.scrollToEnd = st.autoScroll;
    }
    tooltip("gdc.log.autoScroll", "gdc.log.autoScroll.tooltip");

    ImGui::SameLine();
    if (iconButton(ICON_CONTENT_COPY, "copy")) {
        std::string text;
        for (const LogRow& row : rows) {
            text += *row.text;
            text += '\n';
        }
        ImGui::SetClipboardText(text.c_str());
    }
    tooltip("gdc.log.copy", "gdc.log.copy.tooltip");

    const char* expandIcon = st.expanded ? ICON_FULLSCREEN_EXIT : ICON_FULLSCREEN;
    const float expandWidth = ImGui::CalcTextSize(expandIcon).x + ImGui::GetStyle().FramePadding.x * 2;
    ImGui::SameLine();
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    ImGui::SetNextItemWidth(std::max(px(120), ImGui::GetContentRegionAvail().x - expandWidth - spacing));
    if (ImGui::InputTextWithHint("##filter", tr("gdc.log.filter"), &st.filter)) {
        st.scrollToEnd = st.autoScroll;
    }
    tooltip("gdc.log.filter.tooltip");

    ImGui::SameLine();
    if (iconButton(expandIcon, "expand")) {
        st.expanded = !st.expanded;
        st.focusExpanded = st.expanded;
    }
    if (st.expanded) {
        tooltip("gdc.log.collapse", "gdc.log.collapse.tooltip");
    } else {
        tooltip("gdc.log.expand", "gdc.log.expand.tooltip");
    }
}

void logLines(LogViewState& st, const std::vector<LogRow>& rows, float height)
{
    if (ImGui::BeginChild("##lines", ImVec2(0, height), ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_HorizontalScrollbar)) {
        for (const LogRow& row : rows) {
            ImGui::PushStyleColor(ImGuiCol_Text, logColor(row.level));
            ImGui::TextUnformatted(row.text->c_str());
            ImGui::PopStyleColor();
        }
        // ScrollMaxY is still last frame's, so being at the end means new lines arrived below the view.
        if (st.scrollToEnd || (st.autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f)) {
            ImGui::SetScrollHereY(1.0f);
        }
    }
    ImGui::EndChild();
}

} // namespace

void logView(const char* id, const std::vector<std::string>& lines, float height,
             const std::function<LogLevel(const std::string&)>& levelFor)
{
    LogViewState& st = logViews[id];
    const std::string needle = toLower(st.filter);
    size_t counts[kGroupCount] = {};
    std::vector<LogRow> rows;
    rows.reserve(lines.size());
    for (const std::string& line : lines) {
        if (!needle.empty() && toLower(line).find(needle) == std::string::npos) {
            continue;
        }
        const LogLevel level = levelFor(line);
        const LogGroup group = logGroup(level);
        ++counts[group];
        if (st.show[group]) {
            rows.push_back({&line, level});
        }
    }

    ImGui::PushID(id);
    logToolbar(st, counts, rows);
    logLines(st, rows, height);
    ImGui::PopID();

    if (st.expanded) {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        if (st.focusExpanded) {
            ImGui::SetNextWindowFocus();
            st.focusExpanded = false;
        }
        const std::string window = std::string("##log_expanded") + id;
        if (ImGui::Begin(window.c_str(), nullptr,
                         ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings)) {
            logToolbar(st, counts, rows);
            logLines(st, rows, 0);
            if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::IsAnyItemActive() &&
                ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                st.expanded = false;
            }
        }
        ImGui::End();
    }
    st.scrollToEnd = false;
}
