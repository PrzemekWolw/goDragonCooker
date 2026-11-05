#include "ui/widgets.h"

#include <algorithm>

#include "core/i18n.h"
#include "core/platform.h"
#include "core/strings.h"
#include "imgui_stdlib.h"
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
        if (ImGui::Button(trId("gdc.common.up", "picker_up").c_str())) {
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
        }
        ImGui::SameLine();
        if (ImGui::Button(trId("gdc.common.cancel", "picker_cancel").c_str())) {
            dirPicker.open = false;
        }
    }
    ImGui::End();
}

void logView(const char* id, const std::vector<std::string>& lines, float height,
             const std::function<ImVec4(const std::string&)>& colorFor)
{
    if (ImGui::BeginChild(id, ImVec2(0, height), ImGuiChildFlags_Borders)) {
        for (const std::string& line : lines) {
            ImGui::PushStyleColor(ImGuiCol_Text, colorFor(line));
            ImGui::TextUnformatted(line.c_str());
            ImGui::PopStyleColor();
        }
    }
    ImGui::EndChild();
}
