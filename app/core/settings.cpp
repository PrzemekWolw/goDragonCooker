#include "core/settings.h"

#include <fstream>
#include <mutex>
#include <vector>

#include "core/platform.h"

namespace {

struct Section {
    std::function<void(const SettingsJson&)> load;
    std::function<void(SettingsJson&)> save;
};

std::mutex settingsMu;
std::vector<Section> sections;
SettingsJson document = SettingsJson::object();

fs::path settingsPath()
{
    const fs::path directory = userConfigDir();
    return directory.empty() ? fs::path() : directory / "goDragonCooker" / "settings.json";
}

} // namespace

void registerSettings(std::function<void(const SettingsJson&)> load, std::function<void(SettingsJson&)> save)
{
    std::lock_guard lock(settingsMu);
    sections.push_back({std::move(load), std::move(save)});
}

void loadSettings()
{
    std::lock_guard lock(settingsMu);
    std::ifstream file(settingsPath(), std::ios::binary);
    if (!file) {
        return;
    }
    document = SettingsJson::parse(file, nullptr, false);
    if (!document.is_object()) {
        document = SettingsJson::object();
        return;
    }
    // A value of the wrong type throws; that section keeps its defaults.
    for (const Section& section : sections) {
        try {
            section.load(document);
        } catch (const std::exception&) {
        }
    }
}

void saveSettings()
{
    std::lock_guard lock(settingsMu);
    for (const Section& section : sections) {
        section.save(document);
    }
    const fs::path path = settingsPath();
    std::error_code ec;
    if (path.empty() || (fs::create_directories(path.parent_path(), ec), ec)) {
        return;
    }
    std::ofstream(path, std::ios::binary) << document.dump(4);
}
