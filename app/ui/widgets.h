#pragma once

#include <functional>
#include <string>
#include <vector>

#include "imgui.h"

bool comboStrings(const char* id, int* index, const std::vector<std::string>& items, float width);
void tooltip(const char* text);
// Two-line tooltip: a title, then a description.
void tooltip(const char* title, const char* text);

// "icon text###key" for buttons that keep their translated text.
std::string iconLabel(const char* icon, const char* id, const std::string& key);
// Icon-only button; key keeps the ImGui ID stable. Pair it with a tooltip.
bool iconButton(const char* icon, const char* key);
// Drawn in color when on; dimmed and without a background when off. The caller flips the state.
bool toggleButton(const std::string& label, bool on, const ImVec4& color);

void openDirPicker(std::string* target);
void renderDirPicker();
// Path input with a browse button. True when the text was edited.
bool directoryField(const char* id, std::string& dir);

struct TaskState;
// Run/stop button and status line of a background task; `logLines` gets a snapshot of its log.
// True when run was pressed.
bool taskButton(TaskState& task, const std::string& runLabel, const std::string& stopLabel, std::vector<std::string>& logLines);

enum class LogLevel { Info, Detail, Ok, Warn, Error };

// "Output:" label plus a filter toolbar, then the scrolling log. height 0 fills the remaining space.
// levelFor picks a line's color and filter group.
void logView(const char* id, const std::vector<std::string>& lines, float height,
             const std::function<LogLevel(const std::string&)>& levelFor);
