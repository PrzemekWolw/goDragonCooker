#include "tabs/tabs.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <thread>

#include <nlohmann/json.hpp>

#include "core/i18n.h"
#include "core/platform.h"
#include "core/strings.h"
#include "core/task.h"
#include "imgui.h"
#include "imgui_stdlib.h"
#include "ui/theme.h"
#include "ui/widgets.h"

using Json = nlohmann::ordered_json;

namespace {

struct TexEntry {
    std::string suffix;
    std::string ext;
    std::string stageKey;
};

struct MatGenSettings {
    std::vector<TexEntry> suffixes;
    std::vector<TexEntry> customSuffixes;
    double detailScaleX = 0.1;
    double detailScaleY = 0.1;
    bool instanceOpacity = false;
    bool instanceEmissive = false;
    bool enableDetailMaps = false;
};

struct PatternEditBuf {
    std::string suffix;
    int extIdx = 0;
    bool dirty = false;
};

// UI-thread-only fields are plain; runs get copies.
struct MatGenTab {
    TaskState task;
    std::string directory;
    MatGenSettings settings;
    std::map<std::string, PatternEditBuf> patternBufs;
};

MatGenTab matGenTab;

} // namespace

// ---------------------------------------------------------------------------
// Material Generator
// ---------------------------------------------------------------------------

static const std::vector<TexEntry> defaultTextureEntries = {
    {"_nm", ".normal.png", "normalMap"},
    {"_b", ".color.png", "baseColorMap"},
    {"_d", ".color.png", "baseColorMap"},
    {"_n", ".normal.png", "normalMap"},
    {"_r", ".data.png", "roughnessMap"},
    {"_m", ".data.png", "metallicMap"},
    {"_ao", ".data.png", "ambientOcclusionMap"},
    {"_e", ".color.png", "emissiveMap"},
    {"_o", ".data.png", "opacityMap"},
};

// Base stage key -> detail texture to search for when the base exists.
static const std::map<std::string, TexEntry> detailPatterns = {
    {"baseColorMap", {"_dd", ".color.png", "detailMap"}},
    {"normalMap", {"_dn", ".normal.png", "detailNormalMap"}},
    {"opacityMap", {"_do", ".data.png", "opacityDetailMap"}},
    {"metallicMap", {"_dm", ".data.png", "metallicDetailMap"}},
    {"roughnessMap", {"_dr", ".data.png", "roughnessDetailMap"}},
    {"ambientOcclusionMap", {"_dao", ".data.png", "ambientOcclusionDetailMap"}},
};

static const char* detailStageKeys[] = {
    "detailMap", "detailNormalMap", "opacityDetailMap", "metallicDetailMap", "roughnessDetailMap",
    "ambientOcclusionDetailMap",
};

void matGenTabInit()
{
    matGenTab.settings.suffixes = defaultTextureEntries;
}

// ".data.png" -> ".dataR.png". Empty when the name is not a .data texture.
static std::string dataRName(const std::string& name)
{
    const std::string marker = ".data.";
    const auto pos = name.rfind(marker);
    if (pos == std::string::npos) {
        return "";
    }
    std::string out = name;
    out.insert(pos + marker.size() - 1, "R");
    return out;
}

static bool matchTex(const std::string& name, const std::vector<TexEntry>& entries, std::string& base,
                     std::string& key)
{
    const std::string lower = toLower(name);
    for (const TexEntry& entry : entries) {
        const std::string suffix = toLower(entry.suffix + entry.ext);
        std::string matched = suffix;
        if (!hasSuffix(lower, suffix)) {
            const std::string alt = dataRName(suffix);
            if (alt.empty() || !hasSuffix(lower, toLower(alt))) {
                continue;
            }
            matched = alt;
        }
        base = name.substr(0, name.size() - matched.size());
        key = entry.stageKey;
        return true;
    }
    return false;
}

using TexMap = std::map<std::string, std::string>;

static Json buildMat(const std::string& baseName, const std::string& texPath, const TexMap& tex,
                     const MatGenSettings& settings)
{
    Json stage = Json::object();
    auto put = [&](const char* key) {
        const auto it = tex.find(key);
        if (it == tex.end()) {
            return false;
        }
        stage[key] = texPath + it->second;
        return true;
    };

    put("ambientOcclusionMap");
    put("baseColorMap");
    put("normalMap");
    put("roughnessMap");
    if (tex.count("metallicMap")) {
        stage["metallicFactor"] = 1;
        put("metallicMap");
    }
    put("opacityMap");
    if (tex.count("emissiveMap")) {
        stage["emissiveFactor"] = Json::array({1, 1, 1});
        stage["emissiveIntensityNits"] = 2000;
        put("emissiveMap");
    }
    if (settings.enableDetailMaps) {
        bool hasAnyDetail = false;
        for (const char* key : detailStageKeys) {
            hasAnyDetail |= put(key);
        }
        if (hasAnyDetail) {
            stage["detailScale"] = Json::array({settings.detailScaleX, settings.detailScaleY});
        }
    }
    if (settings.instanceOpacity) {
        stage["instanceOpacity"] = true;
    }
    if (settings.instanceEmissive) {
        stage["instanceEmissive"] = true;
    }

    Json def = Json::object();
    def["name"] = baseName;
    def["mapTo"] = baseName;
    def["class"] = "Material";
    def["materialTag0"] = "dragon2ng";
    def["translucentBlendOp"] = "None";
    def["version"] = 1.5;
    if (tex.count("opacityMap")) {
        def["alphaTest"] = true;
        def["alphaRef"] = 64;
    }
    def["Stages"] = Json::array();
    def["Stages"].push_back(stage);
    return def;
}

static fs::path cleanPath(const std::string& path)
{
    fs::path result = u8path(path).lexically_normal();
    if (result.filename().empty() && result.has_relative_path()) {
        result = result.parent_path();
    }
    return result;
}

// Returns the path up to and including the first "levels" component (case-insensitive), or empty.
static fs::path findLevelsRoot(const fs::path& root)
{
    fs::path result;
    for (const fs::path& part : root) {
        result /= part;
        if (toLower(u8str(part)) == "levels") {
            return result;
        }
    }
    return {};
}

static std::string computeTexPathForDirectory(const fs::path& levelsRoot, const fs::path& directory)
{
    const fs::path relative = directory.lexically_normal().lexically_relative(levelsRoot.parent_path());
    return "/" + relative.generic_u8string() + "/";
}

// Adds generated materials that don't exist yet. Returns the number added, or -1 with error set.
static int mergeMaterials(const fs::path& path, const std::map<std::string, Json>& generated, std::string& error)
{
    Json merged = Json::object();
    std::error_code ec;
    if (fs::exists(path, ec)) {
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            error = format(tr("gdc.matgen.cannotRead"), u8str(path).c_str());
            return -1;
        }
        try {
            merged = Json::parse(file);
        } catch (const std::exception& e) {
            error = format(tr("gdc.matgen.invalidExistingMaterialsFile"), e.what());
            return -1;
        }
        if (merged.is_null()) {
            merged = Json::object();
        } else if (!merged.is_object()) {
            error = tr("gdc.matgen.invalidExistingMaterialsFileNot");
            return -1;
        }
    }

    std::set<std::string> existingNames;
    for (const auto& [name, material] : merged.items()) {
        existingNames.insert(name);
        if (material.is_object() && material.contains("name") && material["name"].is_string()) {
            const std::string materialName = material["name"];
            if (!materialName.empty()) {
                existingNames.insert(materialName);
            }
        }
    }

    int added = 0;
    for (const auto& [name, material] : generated) {
        if (existingNames.insert(name).second) {
            merged[name] = material;
            ++added;
        }
    }
    if (added == 0) {
        return 0;
    }

    std::vector<std::string> keys;
    for (const auto& item : merged.items()) {
        keys.push_back(item.key());
    }
    std::sort(keys.begin(), keys.end());
    Json sorted = Json::object();
    for (const std::string& key : keys) {
        sorted[key] = std::move(merged[key]);
    }

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << sorted.dump(2, ' ', false, Json::error_handler_t::replace) << '\n';
    if (!file) {
        error = format(tr("gdc.matgen.cannotWrite"), u8str(path).c_str());
        return -1;
    }
    return added;
}

static void runMatGen(std::string directory, MatGenSettings settings)
{
    TaskState& tab = matGenTab.task;
    tab.begin(tr("gdc.matgen.scanning"));

    const fs::path root = cleanPath(directory);
    const fs::path levelsRoot = findLevelsRoot(root);
    if (levelsRoot.empty()) {
        tab.log(format(tr("gdc.matgen.errorNoLevelsDirectoryFound"), directory.c_str()));
        tab.finish(tr("gdc.common.error"));
        return;
    }

    tab.log(format(tr("gdc.matgen.texturesRoot"), directory.c_str()));
    tab.log(format(tr("gdc.matgen.texPath"), computeTexPathForDirectory(levelsRoot, root).c_str()));

    std::vector<TexEntry> allEntries = settings.suffixes;
    allEntries.insert(allEntries.end(), settings.customSuffixes.begin(), settings.customSuffixes.end());
    if (!settings.customSuffixes.empty()) {
        tab.log(format(tr("gdc.matgen.customPatternsEntries"), settings.customSuffixes.size()));
    }
    if (settings.enableDetailMaps) {
        tab.log(format(tr("gdc.matgen.detailMapsEnabledScaleX"), settings.detailScaleX, settings.detailScaleY));
    }
    if (settings.instanceOpacity) {
        tab.log("Instance opacity: true");
    }
    if (settings.instanceEmissive) {
        tab.log("Instance emissive: true");
    }
    tab.log("---");

    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
        tab.log("ERROR: " + format(tr("gdc.common.walkErrorCannotRead"), directory.c_str()));
        tab.finish(tr("gdc.common.error"));
        return;
    }
    std::vector<fs::path> directories = {root};
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (it->is_directory(ec)) {
            directories.push_back(it->path());
        }
    }

    int totalGroups = 0;
    for (const fs::path& path : directories) {
        if (tab.stop) {
            tab.log(tr("gdc.matgen.generationCancelled"));
            tab.finish(tr("gdc.common.cancelled"));
            return;
        }

        std::vector<std::string> names;
        for (const auto& entry : fs::directory_iterator(path, ec)) {
            if (!entry.is_directory(ec)) {
                names.push_back(u8str(entry.path().filename()));
            }
        }
        std::sort(names.begin(), names.end());

        std::map<std::string, std::string> fileNames;
        for (const std::string& name : names) {
            fileNames.emplace(toLower(name), name);
        }

        std::map<std::string, TexMap> mat;
        for (const std::string& name : names) {
            std::string base, key;
            if (matchTex(name, allEntries, base, key) && !base.empty()) {
                mat[base].emplace(key, name);
            }
        }
        if (mat.empty()) {
            continue;
        }

        if (settings.enableDetailMaps) {
            for (auto& [base, textures] : mat) {
                for (const auto& [baseStageKey, detail] : detailPatterns) {
                    if (!textures.count(baseStageKey)) {
                        continue;
                    }
                    const std::string wanted = base + detail.suffix + detail.ext;
                    auto it = fileNames.find(toLower(wanted));
                    if (it == fileNames.end()) {
                        const std::string alt = dataRName(wanted);
                        it = alt.empty() ? fileNames.end() : fileNames.find(toLower(alt));
                    }
                    if (it != fileNames.end()) {
                        textures[detail.stageKey] = it->second;
                    }
                }
            }
        }

        const std::string texPath = computeTexPathForDirectory(levelsRoot, path);
        std::map<std::string, Json> output;
        for (const auto& [base, textures] : mat) {
            output[base] = buildMat(base, texPath, textures, settings);
        }

        const std::string dirName = u8str(path.filename());
        std::string error;
        const int added = mergeMaterials(path / u8path(dirName + ".materials.json"), output, error);
        if (added < 0) {
            tab.log(format(tr("gdc.matgen.errorWriteFailedIn"), dirName.c_str(), error.c_str()));
            continue;
        }

        totalGroups += added;
        if (added == 0) {
            tab.log(format(tr("gdc.matgen.unchangedExistingMaterials"), dirName.c_str(), output.size()));
        } else {
            tab.log(format(tr("gdc.matgen.materialsJsonNewMaterials"), dirName.c_str(), dirName.c_str(), added));
        }
        tab.setStatus(format(tr("gdc.matgen.generatedMaterialGroups"), totalGroups));
    }

    tab.log(format(tr("gdc.matgen.doneMaterialGroupsGenerated"), totalGroups));
    tab.finish(format(tr("gdc.matgen.doneGroups"), totalGroups));
}

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------

struct PatternInfo {
    const char* stage;
    const char* name;
    const char* description;
    const char* suffix;
    int extIdx;
};

static const PatternInfo patterns[] = {
    {"baseColorMap", "gdc.matgen.pattern.baseColorMap.name", "gdc.matgen.pattern.baseColorMap.description", "_b", 0},
    {"normalMap", "gdc.matgen.pattern.normalMap.name", "gdc.matgen.pattern.normalMap.description", "_nm", 1},
    {"roughnessMap", "gdc.matgen.pattern.roughnessMap.name", "gdc.matgen.pattern.roughnessMap.description", "_r", 2},
    {"metallicMap", "gdc.matgen.pattern.metallicMap.name", "gdc.matgen.pattern.metallicMap.description", "_m", 2},
    {"ambientOcclusionMap", "gdc.matgen.pattern.ambientOcclusionMap.name", "gdc.matgen.pattern.ambientOcclusionMap.description", "_ao", 2},
    {"emissiveMap", "gdc.matgen.pattern.emissiveMap.name", "gdc.matgen.pattern.emissiveMap.description", "_e", 0},
    {"opacityMap", "gdc.matgen.pattern.opacityMap.name", "gdc.matgen.pattern.opacityMap.description", "_o", 2},
    {"detailMap", "gdc.matgen.pattern.detailMap.name", "gdc.matgen.pattern.detailMap.description", "_dd", 0},
    {"detailNormalMap", "gdc.matgen.pattern.detailNormalMap.name", "gdc.matgen.pattern.detailNormalMap.description", "_dn", 1},
    {"opacityDetailMap", "gdc.matgen.pattern.opacityDetailMap.name", "gdc.matgen.pattern.opacityDetailMap.description", "_do", 2},
    {"metallicDetailMap", "gdc.matgen.pattern.metallicDetailMap.name", "gdc.matgen.pattern.metallicDetailMap.description", "_dm", 2},
    {"roughnessDetailMap", "gdc.matgen.pattern.roughnessDetailMap.name", "gdc.matgen.pattern.roughnessDetailMap.description", "_dr", 2},
    {"ambientOcclusionDetailMap", "gdc.matgen.pattern.ambientOcclusionDetailMap.name", "gdc.matgen.pattern.ambientOcclusionDetailMap.description", "_dao", 2},
};

static const std::vector<std::string> allExtOpts = {
    ".color.png", ".normal.png", ".data.png", ".dataR.png",
    ".color.jpg", ".normal.jpg", ".data.jpg", ".dataR.jpg",
};

static int extIndex(const std::string& ext)
{
    const auto it = std::find(allExtOpts.begin(), allExtOpts.end(), ext);
    return it == allExtOpts.end() ? 0 : int(it - allExtOpts.begin());
}

static std::string patternBufferKey(const std::string& stage, size_t index)
{
    return stage + "#" + std::to_string(index);
}

static void patternEditRow(const std::string& id, PatternEditBuf& buf)
{
    ImGui::SetNextItemWidth(px(75));
    buf.dirty |= ImGui::InputText(("##suf_" + id).c_str(), &buf.suffix);
    ImGui::SameLine();
    buf.dirty |= comboStrings(("##pat_" + id).c_str(), &buf.extIdx, allExtOpts, px(110));
    ImGui::SameLine();
}

static void renderPatternRow(const PatternInfo& info)
{
    MatGenTab& m = matGenTab;
    const std::string stage = info.stage;
    auto& custom = m.settings.customSuffixes;

    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Text, colTextDim);
    ImGui::TextUnformatted(tr(info.name));
    ImGui::PopStyleColor();

    for (size_t index = 0; index < custom.size(); ++index) {
        if (custom[index].stageKey != stage) {
            continue;
        }
        const std::string key = patternBufferKey(stage, index);
        const bool created = !m.patternBufs.count(key);
        PatternEditBuf& buf = m.patternBufs[key];
        if (created || !buf.dirty) {
            buf.suffix = custom[index].suffix;
            buf.extIdx = extIndex(custom[index].ext);
        }
        const std::string id = stage + "_" + std::to_string(index);
        patternEditRow(id, buf);
        if (ImGui::Button(trId("gdc.matgen.save", "upd_" + id).c_str())) {
            const std::string suffix = trim(buf.suffix);
            if (!suffix.empty()) {
                custom[index] = {suffix, allExtOpts[buf.extIdx], stage};
                buf.dirty = false;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button(trId("gdc.matgen.remove", "rem_" + id).c_str())) {
            custom.erase(custom.begin() + std::ptrdiff_t(index));
            for (auto it = m.patternBufs.begin(); it != m.patternBufs.end();) {
                it = hasPrefix(it->first, stage + "#") ? m.patternBufs.erase(it) : std::next(it);
            }
            break;
        }
    }

    auto [it, created] = m.patternBufs.try_emplace(stage);
    PatternEditBuf& draft = it->second;
    if (created) {
        draft.suffix = info.suffix;
        draft.extIdx = info.extIdx;
    }
    patternEditRow(stage + "_ext", draft);
    if (ImGui::Button(trId("gdc.matgen.addPattern", "add_" + stage).c_str())) {
        const std::string suffix = trim(draft.suffix);
        if (!suffix.empty()) {
            custom.push_back({suffix, allExtOpts[draft.extIdx], stage});
            draft.dirty = false;
        }
    }
    tooltip(info.description);
}

void matGenTabUI()
{
    if (!ImGui::BeginTabItem(trId("gdc.matgen.materialGenerator", "tab_matgen").c_str())) {
        return;
    }
    MatGenTab& m = matGenTab;
    MatGenSettings& s = m.settings;

    ImGui::TextUnformatted(tr("gdc.common.texturesDirectory"));
    ImGui::InputText("##matgen_directory", &m.directory);
    ImGui::SameLine();
    if (ImGui::Button(trId("gdc.common.browse", "matgen_browse").c_str())) {
        openDirPicker(&m.directory);
    }

    ImGui::Separator();

    if (ImGui::TreeNodeEx(trId("gdc.matgen.basicSettings", "matgen_basic").c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox(trId("gdc.matgen.instanceOpacity", "matgen_inst_opacity").c_str(), &s.instanceOpacity);
        tooltip("gdc.matgen.instanceOpacity.tooltip");
        ImGui::Checkbox(trId("gdc.matgen.instanceEmissive", "matgen_inst_emissive").c_str(), &s.instanceEmissive);
        tooltip("gdc.matgen.instanceEmissive.tooltip");
        ImGui::TreePop();
    }

    if (ImGui::TreeNode(trId("gdc.matgen.detailMaps", "matgen_detail").c_str())) {
        ImGui::Checkbox(trId("gdc.matgen.enableDetailMapsWireUp", "matgen_detail_on").c_str(),
                        &s.enableDetailMaps);
        tooltip("gdc.matgen.enableDetailMapsWireUp.tooltip");
        if (s.enableDetailMaps) {
            float x = float(s.detailScaleX);
            float y = float(s.detailScaleY);
            ImGui::TextUnformatted(tr("gdc.matgen.detailScale"));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(px(120));
            if (ImGui::InputFloat("##detail_scale_x", &x, 0.01f, 0.0f, "%.2f")) {
                s.detailScaleX = x;
            }
            tooltip("gdc.matgen.uvTilingXForDetail");
            ImGui::SameLine();
            ImGui::TextUnformatted("x ");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(px(120));
            if (ImGui::InputFloat("##detail_scale_y", &y, 0.01f, 0.0f, "%.2f")) {
                s.detailScaleY = y;
            }
            tooltip("gdc.matgen.uvTilingYForDetail");
        }
        ImGui::TreePop();
    }

    if (ImGui::TreeNode(trId("gdc.matgen.texturePatterns", "matgen_patterns").c_str())) {
        ImGui::PushStyleColor(ImGuiCol_Text, colTextDim);
        ImGui::TextUnformatted(tr(
            "gdc.matgen.builtInPatternsAreUsed"));
        ImGui::PopStyleColor();
        if (ImGui::BeginChild("##patterns", ImVec2(0, 390), ImGuiChildFlags_Borders)) {
            for (const PatternInfo& info : patterns) {
                renderPatternRow(info);
            }
        }
        ImGui::EndChild();
        ImGui::TreePop();
    }

    ImGui::Separator();

    std::string status;
    bool running;
    std::vector<std::string> logLines;
    {
        std::lock_guard lock(m.task.mu);
        status = m.task.status;
        running = m.task.running;
        logLines = m.task.logLines;
    }
    if (running) {
        if (ImGui::Button(trId("gdc.matgen.stopGenerator", "matgen_run").c_str())) {
            m.task.stop = true;
        }
    } else if (ImGui::Button(trId("gdc.matgen.generateMaterials", "matgen_run").c_str()) && !m.directory.empty()) {
        m.task.log("");
        std::thread(runMatGen, m.directory, s).detach();
    }
    ImGui::SameLine();
    ImGui::Text(tr("gdc.common.status"), status.empty() ? tr("gdc.common.waiting") : status.c_str());

    ImGui::Separator();
    ImGui::TextUnformatted(tr("gdc.common.output"));
    logView("##matgen_log", logLines, 0, [](const std::string& line) {
        const std::string lower = toLower(line);
        const bool isError = lower.find("error") != std::string::npos || lower.find("fail") != std::string::npos;
        return isError ? colCookErr : colText;
    });
    ImGui::EndTabItem();
}
