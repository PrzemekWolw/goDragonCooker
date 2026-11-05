#pragma once

#include <functional>

#include <nlohmann/json.hpp>

using SettingsJson = nlohmann::ordered_json;

// Each area registers how it reads and writes its part of settings.json.
void registerSettings(std::function<void(const SettingsJson&)> load, std::function<void(SettingsJson&)> save);
void loadSettings();
void saveSettings();
