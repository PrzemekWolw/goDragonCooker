#include "tabs/tabs.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <map>
#include <set>
#include <thread>

#include <nlohmann/json.hpp>

#include "core/platform.h"
#include "core/settings.h"
#include "core/strings.h"
#include "core/task.h"
#include "imgui.h"
#include "imgui_stdlib.h"
#include "model/convert.h"
#include "model/ts/ts_io.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace {

// Listed first in the target dropdown; the rest follow by label.
const char* const kMainTargets[] = {"cdae", "dts", "collada", "gltf2", "fbx", "obj"};

struct ModelSettings {
    std::string directory;
    std::string outDir;
    bool mirror = false; // write into outDir mirroring the source tree
    bool overwrite = false;
    bool validate = true;
    bool verboseLog = false;
    std::set<std::string> sourceExts;
    std::string target = "cdae";
    std::map<std::string, nlohmann::json> formatSettings; // target id -> values
};

// UI-thread-only fields are plain; runs get copies.
struct ModelTab {
    TaskState task;
    ModelSettings settings;
    std::map<std::string, size_t> found; // extension -> file count from the last scan
    std::string scannedDir;
};

ModelTab modelTab;

// Writable formats with the common targets first.
std::vector<const model::Format*> targetList()
{
    std::vector<const model::Format*> list = model::writableFormats();
    auto rank = [](const model::Format* f) {
        const auto it = std::find_if(std::begin(kMainTargets), std::end(kMainTargets), [&](const char* t) { return f->id == t; });
        return size_t(it - std::begin(kMainTargets));
    };
    std::stable_sort(list.begin(), list.end(), [&](const model::Format* a, const model::Format* b) {
        const size_t ra = rank(a), rb = rank(b);
        return ra != rb ? ra < rb : toLower(a->label) < toLower(b->label);
    });
    return list;
}

bool underPath(const fs::path& p, const fs::path& root)
{
    if (root.empty()) {
        return false;
    }
    std::error_code ec;
    const fs::path rel = fs::relative(p, root, ec);
    return !ec && !rel.empty() && *rel.begin() != "..";
}

template <class F>
void walkModels(const std::string& directory, const ModelSettings& s, F&& onFile)
{
    std::error_code ec;
    const fs::path root = u8path(directory);
    const fs::path outRoot = s.mirror && !s.outDir.empty() ? u8path(s.outDir) : fs::path();
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
         it.increment(ec)) {
        if (!it->is_regular_file(ec) || underPath(it->path(), outRoot)) {
            continue;
        }
        const std::string ext = model::modelExtension(u8str(it->path()));
        if (!ext.empty() && model::readerForExt(ext)) {
            onFile(u8str(it->path()), ext);
        }
    }
}

void scan(ModelTab& t)
{
    t.found.clear();
    walkModels(t.settings.directory, t.settings, [&](const std::string&, const std::string& ext) { ++t.found[ext]; });
    t.scannedDir = t.settings.directory;
    if (t.settings.sourceExts.empty()) {
        for (const auto& [ext, n] : t.found) {
            t.settings.sourceExts.insert(ext);
        }
    }
}

struct Job {
    std::string source;
    std::string output;
    uintmax_t size = 0;
};

void runConverter(ModelSettings s)
{
    TaskState& tab = modelTab.task;
    tab.begin("Seeking models...");
    const model::Format* target = model::formatById(s.target);
    if (!target || !target->canWrite) {
        tab.log("No target format selected.");
        tab.finish("Nothing to do");
        return;
    }
    nlohmann::json& targetSettings = s.formatSettings[target->id];
    tab.log(format("Target %s: %s", target->label.c_str(), model::describeSettings(*target, targetSettings).c_str()));

    std::vector<std::string> sources;
    walkModels(s.directory, s, [&](const std::string& path, const std::string& ext) {
        if (s.sourceExts.count(ext)) {
            sources.push_back(path);
        }
    });
    std::sort(sources.begin(), sources.end());

    const std::string ext = target->outputExt(model::withDefaults(*target, targetSettings));
    std::vector<model::OutputRequest> requests;
    for (const std::string& src : sources) {
        requests.push_back({src, target->id, ext});
    }
    const std::vector<std::string> outputs = model::planOutputPaths(requests, s.directory, s.mirror ? s.outDir : std::string());

    std::vector<Job> jobs;
    size_t skipped = 0;
    for (size_t i = 0; i < requests.size(); ++i) {
        std::error_code ec;
        if (!s.overwrite && fs::exists(u8path(outputs[i]), ec)) {
            ++skipped;
            continue;
        }
        const uintmax_t size = fs::file_size(u8path(requests[i].source), ec);
        jobs.push_back({requests[i].source, outputs[i], ec ? 0 : size});
    }
    // Largest first, so one big model does not start last and leave the other workers idle.
    std::stable_sort(jobs.begin(), jobs.end(), [](const Job& a, const Job& b) { return a.size > b.size; });
    if (skipped) {
        tab.log(format("  SKIP: %zu output(s) already exist (enable overwrite to replace)", skipped));
    }
    if (jobs.empty()) {
        tab.log("No models to convert.");
        tab.finish("Nothing to do");
        return;
    }

    const auto t0 = std::chrono::steady_clock::now();
    std::mutex reportMu;
    nlohmann::json report = nlohmann::json::array();
    size_t done = 0, ok = 0, warned = 0, failed = 0;
    const size_t total = jobs.size();
    tab.setStatus(format("Converting: 0/%zu", total));

    const unsigned workers = std::max(1u, std::min<unsigned>(std::thread::hardware_concurrency(), unsigned(total)));
    tab.log(format("Converting %zu model(s) to %s with %u workers.", total, target->label.c_str(), workers));
    runPool(
        jobs.size(), workers, tab,
        [&](size_t j) {
            const Job& job = jobs[j];
            const std::string name = u8str(u8path(job.source).filename());
            model::Model source;
            std::string error;
            std::vector<std::string> loadWarnings;
            if (!source.load(job.source, error, loadWarnings)) {
                tab.log(format("  FAIL: %s - %s", name.c_str(), error.c_str()));
                std::lock_guard lock(reportMu);
                ++failed;
                ++done;
                return std::string();
            }
            {
                model::ConvertOptions o;
                o.target = target;
                o.settings = targetSettings;
                o.validate = s.validate;
                const model::ConvertResult r = model::convertModel(source, job.output, o);
                const std::string outName = u8str(u8path(job.output).filename());
                std::vector<std::string> out;
                if (!r.ok) {
                    out.push_back(format("  FAIL: %s -> %s - %s", name.c_str(), outName.c_str(), r.error.c_str()));
                } else {
                    const model::Verdict v = r.validated ? r.report.verdict : model::Verdict::Pass;
                    const char* tag = v == model::Verdict::Fail ? "FAIL" : v == model::Verdict::Warn ? "WARN" : "OK";
                    out.push_back(format("  %s: %s -> %s%s", tag, name.c_str(), outName.c_str(),
                                         r.validated ? format(" (validation %s)", model::verdictName(v)).c_str() : ""));
                    if (s.verboseLog) {
                        for (const std::string& l : r.log) {
                            out.push_back("      " + l);
                        }
                    }
                    for (const std::string& w : loadWarnings) {
                        out.push_back("      note: " + w);
                    }
                    for (const std::string& w : r.warnings) {
                        out.push_back("      note: " + w);
                    }
                    for (const std::string& l : r.report.lines) {
                        if (s.verboseLog || hasPrefix(l, "WARN") || hasPrefix(l, "FAIL")) {
                            out.push_back("      " + l);
                        }
                    }
                }
                for (const std::string& l : out) {
                    tab.log(l);
                }
                std::lock_guard lock(reportMu);
                ++done;
                if (!r.ok || (r.validated && r.report.verdict == model::Verdict::Fail)) {
                    ++failed;
                } else if (r.validated && r.report.verdict == model::Verdict::Warn) {
                    ++warned;
                } else {
                    ++ok;
                }
                nlohmann::json entry = r.validated ? r.report.json : nlohmann::json::object();
                entry["source"] = job.source;
                entry["output"] = job.output;
                entry["target"] = o.target->id;
                entry["ok"] = r.ok;
                if (!r.ok) {
                    entry["error"] = r.error;
                }
                entry["log"] = r.log;
                entry["warnings"] = r.warnings;
                entry["lines"] = r.report.lines;
                report.push_back(entry);
                tab.setStatus(format("Converting: %zu/%zu (%zu ok, %zu warn, %zu failed)", done, total, ok, warned, failed));
            }
            return std::string();
        },
        [](size_t, const std::string&) {});

    if (s.validate) {
        const fs::path dir = s.mirror && !s.outDir.empty() ? u8path(s.outDir) : u8path(s.directory);
        const fs::path reportPath = dir / "model_validation.json";
        std::ofstream(reportPath, std::ios::binary) << report.dump(2);
        tab.log("Validation report: " + u8str(reportPath));
    }
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (tab.stop) {
        tab.log("Conversion cancelled.");
        tab.finish("Cancelled");
        return;
    }
    tab.log(format("Finished in %.2fs (%zu ok, %zu with warnings, %zu failed)", elapsed, ok, warned, failed));
    tab.finish(format("Done: %zu/%zu converted", ok + warned, total));
}

// Draws the settings of one target; returns true when a value changed.
bool formatSettingsUI(const model::Format& f, nlohmann::json& values)
{
    bool changed = false;
    values = model::withDefaults(f, values);
    for (const model::SettingDef& s : f.settings) {
        if (s.visible && !s.visible(values)) {
            continue;
        }
        const std::string id = s.label + "##" + f.id + "_" + s.key;
        int v = values[s.key].get<int>();
        switch (s.type) {
        case model::SettingType::Bool: {
            bool b = v != 0;
            if (ImGui::Checkbox(id.c_str(), &b)) {
                v = b ? 1 : 0;
                changed = true;
            }
            break;
        }
        case model::SettingType::Int:
            ImGui::SetNextItemWidth(px(200));
            if (s.maxValue - s.minValue <= 32 ? ImGui::SliderInt(id.c_str(), &v, s.minValue, s.maxValue)
                                              : ImGui::InputInt(id.c_str(), &v)) {
                v = std::clamp(v, s.minValue, s.maxValue);
                changed = true;
            }
            break;
        case model::SettingType::Choice:
            ImGui::TextUnformatted(s.label.c_str());
            ImGui::SameLine();
            if (comboStrings(("##" + f.id + "_" + s.key).c_str(), &v, s.choices, px(240))) {
                changed = true;
            }
            break;
        }
        if (!s.tooltip.empty()) {
            tooltip(s.tooltip.c_str());
        }
        values[s.key] = v;
    }
    if (f.id == "dts") {
        const int version = ts::kMaxDtsWriteVersion - values.value("version", 0);
        for (const std::string& loss : ts::dtsVersionLosses(version)) {
            ImGui::TextColored(colTextDim, "v%d: %s", version, loss.c_str());
        }
    }
    return changed;
}

ImVec4 modelLogColor(const std::string& line)
{
    if (hasPrefix(line, "  OK:")) {
        return colCookOk;
    }
    if (hasPrefix(line, "  FAIL:") || hasPrefix(line, "      FAIL")) {
        return colCookErr;
    }
    if (hasPrefix(line, "  WARN:") || hasPrefix(line, "      WARN")) {
        return colWarn;
    }
    if (hasPrefix(line, "  SKIP:") || hasPrefix(line, "      ")) {
        return colTextDim;
    }
    return colText;
}

} // namespace

void modelTabInit()
{
    registerSettings(
        [](const SettingsJson& j) {
            if (!j.contains("modelConverter") || !j["modelConverter"].is_object()) {
                return;
            }
            const SettingsJson& m = j["modelConverter"];
            ModelSettings& s = modelTab.settings;
            s.directory = m.value("directory", "");
            s.outDir = m.value("outDir", "");
            s.mirror = m.value("mirror", false);
            s.overwrite = m.value("overwrite", false);
            s.validate = m.value("validate", true);
            s.verboseLog = m.value("verboseLog", false);
            if (m.contains("sourceExts") && m["sourceExts"].is_array()) {
                for (const auto& e : m["sourceExts"]) {
                    if (e.is_string()) {
                        s.sourceExts.insert(e.get<std::string>());
                    }
                }
            }
            if (m.contains("target") && m["target"].is_string()) {
                s.target = m["target"].get<std::string>();
            } else if (m.contains("targets") && m["targets"].is_array() && !m["targets"].empty() &&
                       m["targets"][0].is_string()) {
                s.target = m["targets"][0].get<std::string>();
            }
            if (m.contains("formatSettings") && m["formatSettings"].is_object()) {
                for (const auto& [id, v] : m["formatSettings"].items()) {
                    s.formatSettings[id] = nlohmann::json::parse(v.dump());
                }
            }
        },
        [](SettingsJson& j) {
            const ModelSettings& s = modelTab.settings;
            SettingsJson m = SettingsJson::object();
            m["directory"] = s.directory;
            m["outDir"] = s.outDir;
            m["mirror"] = s.mirror;
            m["overwrite"] = s.overwrite;
            m["validate"] = s.validate;
            m["verboseLog"] = s.verboseLog;
            m["sourceExts"] = std::vector<std::string>(s.sourceExts.begin(), s.sourceExts.end());
            m["target"] = s.target;
            SettingsJson fs = SettingsJson::object();
            for (const auto& [id, v] : s.formatSettings) {
                fs[id] = SettingsJson::parse(v.dump());
            }
            m["formatSettings"] = fs;
            j["modelConverter"] = m;
        });
}

void modelTabUI()
{
    if (!ImGui::BeginTabItem("Model Converter")) {
        return;
    }
    ModelTab& t = modelTab;
    ModelSettings& s = t.settings;
    bool dirty = false;

    ImGui::TextUnformatted("Models directory:");
    dirty |= ImGui::InputText("##model_directory", &s.directory);
    ImGui::SameLine();
    if (ImGui::Button("Browse##model_dir")) {
        openDirPicker(&s.directory);
    }
    ImGui::SameLine();
    if (ImGui::Button("Scan") && !s.directory.empty()) {
        scan(t);
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Convert from:");
    if (t.found.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(colTextDim, "press Scan to list the model formats in the directory");
    } else {
        int column = 0;
        for (const auto& [ext, count] : t.found) {
            bool on = s.sourceExts.count(ext) > 0;
            if (column++ % 6) {
                ImGui::SameLine();
            }
            if (ImGui::Checkbox(format("%s (%zu)##src%s", ext.c_str(), count, ext.c_str()).c_str(), &on)) {
                on ? (void)s.sourceExts.insert(ext) : (void)s.sourceExts.erase(ext);
                dirty = true;
            }
        }
    }

    ImGui::Separator();
    static const std::vector<const model::Format*> targets = targetList();
    std::vector<std::string> labels;
    int selected = 0;
    for (size_t i = 0; i < targets.size(); ++i) {
        labels.push_back(targets[i]->label);
        if (targets[i]->id == s.target) {
            selected = int(i);
        }
    }
    ImGui::TextUnformatted("Convert to:");
    ImGui::SameLine();
    if (!targets.empty() && comboStrings("##model_target", &selected, labels, px(360))) {
        dirty = true;
    }
    tooltip("CDAE, DTS and every format the game's Assimp can export.");
    const model::Format* target = targets.empty() ? nullptr : targets[size_t(selected)];
    if (target && s.target != target->id) {
        s.target = target->id;
        dirty = true;
    }
    if (target) {
        ImGui::Indent();
        if (target->settings.empty()) {
            ImGui::TextColored(colTextDim, "No extra settings.");
        } else {
            dirty |= formatSettingsUI(*target, s.formatSettings[target->id]);
        }
        ImGui::Unindent();
    }

    ImGui::Separator();
    int mode = s.mirror ? 1 : 0;
    dirty |= ImGui::RadioButton("Write beside the source", &mode, 0);
    ImGui::SameLine();
    dirty |= ImGui::RadioButton("Write into folder (mirrors the source tree)", &mode, 1);
    s.mirror = mode == 1;
    if (s.mirror) {
        dirty |= ImGui::InputText("##model_outdir", &s.outDir);
        ImGui::SameLine();
        if (ImGui::Button("Browse##model_out")) {
            openDirPicker(&s.outDir);
        }
    }
    dirty |= ImGui::Checkbox("Overwrite existing files", &s.overwrite);
    tooltip("Off: outputs that already exist are skipped. When two sources share a name (a.fbx, a.dae),\n"
            "the source extension is kept in the output name (a.fbx.glb).");
    ImGui::SameLine();
    dirty |= ImGui::Checkbox("Validate against source", &s.validate);
    tooltip("Re-reads every written file and compares geometry, normals, sharp edges, UVs, colors, tangents,\n"
            "materials, skin weights and sampled animation with the source. Writes model_validation.json.");
    ImGui::SameLine();
    dirty |= ImGui::Checkbox("Verbose log", &s.verboseLog);
    if (dirty) {
        saveSettings();
    }

    ImGui::Separator();
    std::string status;
    bool running;
    std::vector<std::string> logLines;
    {
        std::lock_guard lock(t.task.mu);
        status = t.task.status;
        running = t.task.running;
        logLines = t.task.logLines;
    }
    if (running) {
        if (ImGui::Button("Stop Converter")) {
            t.task.stop = true;
        }
    } else if (ImGui::Button("Convert Models") && !s.directory.empty()) {
        if (t.found.empty() || t.scannedDir != s.directory) {
            scan(t);
        }
        t.task.log("");
        std::thread(runConverter, s).detach();
    }
    ImGui::Text("Status: %s", status.empty() ? "Waiting..." : status.c_str());

    ImGui::Separator();
    ImGui::TextUnformatted("Output:");
    logView("##model_log", logLines, 280, modelLogColor);
    ImGui::EndTabItem();
}
