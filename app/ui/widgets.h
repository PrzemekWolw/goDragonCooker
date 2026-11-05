#pragma once

#include <functional>
#include <string>
#include <vector>

#include "imgui.h"

bool comboStrings(const char* id, int* index, const std::vector<std::string>& items, float width);
void tooltip(const char* text);

void openDirPicker(std::string* target);
void renderDirPicker();

// Scrolling log. colorFor returns the text color of a line.
void logView(const char* id, const std::vector<std::string>& lines, float height,
             const std::function<ImVec4(const std::string&)>& colorFor);
