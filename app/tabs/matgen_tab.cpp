#include "tabs/tabs.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <thread>

#include <nlohmann/json.hpp>

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

static bool matchTex(const std::string& name, const std::vector<TexEntry>& entries, std::string& base,
                     std::string& key)
{
    const std::string lower = toLower(name);
    for (const TexEntry& entry : entries) {
        const std::string suffix = toLower(entry.suffix + entry.ext);
        if (hasSuffix(lower, suffix)) {
            base = name.substr(0, name.size() - suffix.size());
            key = entry.stageKey;
            return true;
        }
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
            error = "cannot read " + u8str(path);
            return -1;
        }
        try {
            merged = Json::parse(file);
        } catch (const std::exception& e) {
            error = std::string("invalid existing materials file: ") + e.what();
            return -1;
        }
        if (merged.is_null()) {
            merged = Json::object();
        } else if (!merged.is_object()) {
            error = "invalid existing materials file: not a JSON object";
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
        error = "cannot write " + u8str(path);
        return -1;
    }
    return added;
}

static void runMatGen(std::string directory, MatGenSettings settings)
{
    TaskState& tab = matGenTab.task;
    tab.begin("Scanning...");

    const fs::path root = cleanPath(directory);
    const fs::path levelsRoot = findLevelsRoot(root);
    if (levelsRoot.empty()) {
        tab.log("ERROR: no 'levels' directory found in: " + directory);
        tab.finish("Error");
        return;
    }

    tab.log("Textures root: " + directory);
    tab.log("Tex path:     " + computeTexPathForDirectory(levelsRoot, root));

    std::vector<TexEntry> allEntries = settings.suffixes;
    allEntries.insert(allEntries.end(), settings.customSuffixes.begin(), settings.customSuffixes.end());
    if (!settings.customSuffixes.empty()) {
        tab.log(format("Custom patterns: %zu entries", settings.customSuffixes.size()));
    }
    if (settings.enableDetailMaps) {
        tab.log(format("Detail maps:  enabled (scale %.2f x %.2f)", settings.detailScaleX, settings.detailScaleY));
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
        tab.log("Walk error: cannot read " + directory);
        tab.finish("Error");
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
            tab.log("Generation cancelled.");
            tab.finish("Cancelled");
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
                    const auto it = fileNames.find(toLower(base + detail.suffix + detail.ext));
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
            tab.log(format("  WRITE ERROR in %s: %s", dirName.c_str(), error.c_str()));
            continue;
        }

        totalGroups += added;
        if (added == 0) {
            tab.log(format("  %s/ -> unchanged (%zu existing materials)", dirName.c_str(), output.size()));
        } else {
            tab.log(format("  %s/ -> %s.materials.json (%d new materials)", dirName.c_str(), dirName.c_str(), added));
        }
        tab.setStatus(format("Generated %d material groups", totalGroups));
    }

    tab.log(format("\nDone. %d material groups generated.", totalGroups));
    tab.finish(format("Done: %d groups", totalGroups));
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
    {"baseColorMap", "Base color", "Main surface color", "_b", 0},
    {"normalMap", "Normal", "Surface normal detail", "_nm", 1},
    {"roughnessMap", "Roughness", "Surface roughness", "_r", 2},
    {"metallicMap", "Metallic", "Metal surface mask", "_m", 2},
    {"ambientOcclusionMap", "Ambient occlusion", "Ambient light occlusion", "_ao", 2},
    {"emissiveMap", "Emissive", "Self-lit surface", "_e", 0},
    {"opacityMap", "Opacity", "Transparency mask", "_o", 2},
    {"detailMap", "Detail color", "Fine color detail", "_dd", 0},
    {"detailNormalMap", "Detail normal", "Fine normal detail", "_dn", 1},
    {"opacityDetailMap", "Detail opacity", "Fine transparency detail", "_do", 2},
    {"metallicDetailMap", "Detail metallic", "Fine metallic detail", "_dm", 2},
    {"roughnessDetailMap", "Detail roughness", "Fine roughness detail", "_dr", 2},
    {"ambientOcclusionDetailMap", "Detail ambient occlusion", "Fine occlusion detail", "_dao", 2},
};

static const std::vector<std::string> allExtOpts = {
    ".color.png", ".normal.png", ".data.png", ".color.jpg", ".normal.jpg", ".data.jpg",
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
    ImGui::TextUnformatted(info.name);
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
        if (ImGui::Button(("Save##upd_" + id).c_str())) {
            const std::string suffix = trim(buf.suffix);
            if (!suffix.empty()) {
                custom[index] = {suffix, allExtOpts[buf.extIdx], stage};
                buf.dirty = false;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button(("Remove##rem_" + id).c_str())) {
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
    if (ImGui::Button(("Add pattern##add_" + stage).c_str())) {
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
    if (!ImGui::BeginTabItem("Material Generator")) {
        return;
    }
    MatGenTab& m = matGenTab;
    MatGenSettings& s = m.settings;

    ImGui::TextUnformatted("Textures directory:");
    ImGui::InputText("##matgen_directory", &m.directory);
    ImGui::SameLine();
    if (ImGui::Button("Browse")) {
        openDirPicker(&m.directory);
    }

    ImGui::Separator();

    if (ImGui::TreeNodeEx("Basic Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("Instance opacity", &s.instanceOpacity);
        tooltip("Set instanceOpacity=true on stages that have an opacity map.");
        ImGui::Checkbox("Instance emissive", &s.instanceEmissive);
        tooltip("Set instanceEmissive=true on stages that have an emissive map.");
        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Detail Maps")) {
        ImGui::Checkbox("Enable detail maps (wire up _dd, _dn, etc.)", &s.enableDetailMaps);
        tooltip("When both base and detail textures exist in a directory, auto-wire them into the output.");
        if (s.enableDetailMaps) {
            float x = float(s.detailScaleX);
            float y = float(s.detailScaleY);
            ImGui::TextUnformatted("Detail scale:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(px(120));
            if (ImGui::InputFloat("##detail_scale_x", &x, 0.01f, 0.0f, "%.2f")) {
                s.detailScaleX = x;
            }
            tooltip("UV tiling X for detail maps.");
            ImGui::SameLine();
            ImGui::TextUnformatted("x ");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(px(120));
            if (ImGui::InputFloat("##detail_scale_y", &y, 0.01f, 0.0f, "%.2f")) {
                s.detailScaleY = y;
            }
            tooltip("UV tiling Y for detail maps.");
        }
        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Texture Patterns")) {
        ImGui::PushStyleColor(ImGuiCol_Text, colTextDim);
        ImGui::TextUnformatted(
            "Built-in patterns are used automatically. Add a custom pattern for a different filename convention.");
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
        if (ImGui::Button("Stop Generator")) {
            m.task.stop = true;
        }
    } else if (ImGui::Button("Generate Materials") && !m.directory.empty()) {
        m.task.log("");
        std::thread(runMatGen, m.directory, s).detach();
    }
    ImGui::SameLine();
    ImGui::Text("Status: %s", status.empty() ? "Waiting..." : status.c_str());

    ImGui::Separator();
    ImGui::TextUnformatted("Output:");
    logView("##matgen_log", logLines, 0, [](const std::string& line) {
        const std::string lower = toLower(line);
        const bool isError = lower.find("error") != std::string::npos || lower.find("fail") != std::string::npos;
        return isError ? colCookErr : colText;
    });
    ImGui::EndTabItem();
}
