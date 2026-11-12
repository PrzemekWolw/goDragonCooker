#include "core/i18n.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <deque>
#include <string_view>
#include <unordered_map>

#if defined(_WIN32)
#include <windows.h>
#endif

#include <nlohmann/json.hpp>

#include "core/settings.h"
#include "locales_data.h"

namespace {

// Order matches kLocaleData in the generated locales_data.h.
const std::vector<LanguageInfo> kLanguages = {
    {"en-US", "English"}, {"pl_PL", "Polski"}, {"es_ES", "Espa\xC3\xB1" "ol"}, {"de_DE", "Deutsch"}, {"it_IT", "Italiano"},
};

struct Locale {
    std::deque<std::pair<std::string, std::string>> entries; // deque: the views below stay valid
    std::unordered_map<std::string_view, const std::string*> index;
};

std::atomic<int> language{0};

const std::vector<Locale>& locales()
{
    static const std::vector<Locale> all = [] {
        std::vector<Locale> out(kLanguages.size());
        for (size_t i = 0; i < kLanguages.size() && i < kLocaleDataCount; ++i) {
            const auto json = nlohmann::json::parse(kLocaleData[i].data, kLocaleData[i].data + kLocaleData[i].size, nullptr, false);
            if (!json.is_object()) {
                continue;
            }
            Locale& l = out[i];
            for (auto it = json.begin(); it != json.end(); ++it) {
                if (it.value().is_string()) {
                    l.entries.emplace_back(it.key(), it.value().get<std::string>());
                }
            }
            for (const auto& e : l.entries) {
                l.index.emplace(e.first, &e.second);
            }
        }
        return out;
    }();
    return all;
}

const std::string* find(const Locale& l, const char* id)
{
    const auto it = l.index.find(id);
    return it == l.index.end() ? nullptr : it->second;
}

int languageFromCode(const std::string& code)
{
    for (size_t i = 0; i < kLanguages.size(); ++i) {
        if (code == kLanguages[i].code) {
            return int(i);
        }
    }
    for (size_t i = 0; i < kLanguages.size(); ++i) { // "pl", "de-AT", "es_MX.UTF-8": match on the language part
        if (code.compare(0, 2, kLanguages[i].code, 2) == 0) {
            return int(i);
        }
    }
    return -1;
}

int systemLanguage()
{
#if defined(_WIN32)
    wchar_t name[LOCALE_NAME_MAX_LENGTH] = {};
    if (GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH) > 0 && name[0] < 128 && name[1] < 128) {
        const std::string code = {char(name[0]), char(name[1])};
        return std::max(0, languageFromCode(code));
    }
#else
    for (const char* var : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
        const char* value = std::getenv(var);
        if (value && *value) {
            return std::max(0, languageFromCode(value));
        }
    }
#endif
    return 0;
}

} // namespace

const std::vector<LanguageInfo>& languages() { return kLanguages; }

int currentLanguage() { return language.load(); }

void setLanguage(int index)
{
    if (index >= 0 && index < int(kLanguages.size())) {
        language = index;
    }
}

void i18nInit()
{
    language = systemLanguage();
    registerSettings(
        [](const SettingsJson& j) {
            if (const int found = languageFromCode(j.value("language", "")); found >= 0) {
                language = found;
            }
        },
        [](SettingsJson& j) { j["language"] = kLanguages[size_t(language.load())].code; });
}

const char* tr(const char* id)
{
    if (!id) {
        return id;
    }
    const auto& all = locales();
    const int lang = language.load();
    if (const std::string* s = find(all[size_t(lang)], id); s && !s->empty()) {
        return s->c_str();
    }
    if (lang != 0) {
        if (const std::string* s = find(all[0], id)) {
            return s->c_str();
        }
    }
    return id;
}

const char* tr(const std::string& id) { return tr(id.c_str()); }

std::string trId(const std::string& id, const std::string& key) { return std::string(tr(id)) + "###" + key; }

bool hasTranslation(const char* id, int lang)
{
    const auto& all = locales();
    if (lang < 0 || lang >= int(all.size())) {
        return false;
    }
    const std::string* s = find(all[size_t(lang)], id);
    return s && !s->empty();
}

std::vector<std::pair<std::string, std::string>> translationEntries(int lang)
{
    const auto& all = locales();
    if (lang < 0 || lang >= int(all.size())) {
        return {};
    }
    return {all[size_t(lang)].entries.begin(), all[size_t(lang)].entries.end()};
}
