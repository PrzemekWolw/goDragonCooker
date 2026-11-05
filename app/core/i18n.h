#pragma once

#include <string>
#include <vector>

struct LanguageInfo {
    const char* code;       // Locale folder name; stored in settings.json
    const char* nativeName; // shown in the language selector
};

// en-US comes first and is the fallback.
const std::vector<LanguageInfo>& languages();

int currentLanguage();
void setLanguage(int index);

// Picks the system language as the default and registers the "language" setting. Call before loadSettings().
void i18nInit();

// Looks a translation ID up in the current language (app/locales/<locale>/main.translation.json,
// embedded at build time). A missing ID falls back to en-US, and a string that is not an ID comes back unchanged,
// so device names and other runtime text can pass through. Safe to call from any thread.
// Translations of printf-style strings must keep the same conversions in the same order.
const char* tr(const char* id);
const char* tr(const std::string& id);

// "<translated>###key" for ImGui widgets: the identity stays fixed when the language changes.
std::string trId(const std::string& id, const std::string& key);

// True when the language has its own entry for the ID (no fallback).
bool hasTranslation(const char* id, int language);

// Every ID defined for the language, for the consistency tests.
std::vector<std::pair<std::string, std::string>> translationEntries(int language);
