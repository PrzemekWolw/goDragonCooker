// gdc_i18n_test: the embedded locales cover the same IDs, and every ID a setting refers to exists.
#include <cstdio>
#include <set>

#include "core/i18n.h"
#include "model/formats.h"

int main()
{
    int missing = 0, checked = 0;
    const auto source = translationEntries(0);
    std::printf("en-US: %zu ids\n", source.size());
    for (int lang = 1; lang < int(languages().size()); ++lang) {
        if (translationEntries(lang).size() != source.size()) {
            std::printf("[%s] %zu ids, en-US has %zu\n", languages()[size_t(lang)].code, translationEntries(lang).size(), source.size());
            ++missing;
        }
        for (const auto& e : source) {
            if (!hasTranslation(e.first.c_str(), lang)) {
                std::printf("MISSING [%s] %s\n", languages()[size_t(lang)].code, e.first.c_str());
                ++missing;
            }
        }
    }
    auto check = [&](const std::string& id, const std::string& where) {
        if (id.empty() || (id[0] == 'v' && id.size() < 4)) {
            return;
        }
        ++checked;
        if (!hasTranslation(id.c_str(), 0)) {
            std::printf("UNKNOWN ID %s (%s)\n", id.c_str(), where.c_str());
            ++missing;
        }
    };
    for (const model::Format& f : model::formats()) {
        for (const model::SettingDef& s : f.settings) {
            check(s.label, f.id + "/" + s.key + " label");
            check(s.tooltip, f.id + "/" + s.key + " tooltip");
            for (const std::string& c : s.choices) {
                check(c, f.id + "/" + s.key + " choice");
            }
        }
    }
    check("gdc.common.automaticBest", "gpu");
    check("gdc.gpu.noAdapters", "gpu");
    check("gdc.gpu.testingSelected", "gpu");
    std::printf("%d setting ids checked, %d problem(s)\n", checked, missing);
    return missing ? 1 : 0;
}
