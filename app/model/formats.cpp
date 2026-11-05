#include "model/formats.h"

#include <algorithm>
#include <mutex>
#include <set>
#include <sstream>

#include <assimp/Exporter.hpp>
#include <assimp/Importer.hpp>
#include <assimp/importerdesc.h>

#include "core/i18n.h"
#include "core/platform.h"
#include "core/strings.h"
#include "model/ts/ts_io.h"

namespace model {
namespace {

auto whenEq(const char* key, int value)
{
    return [key = std::string(key), value](const nlohmann::json& v) { return v.value(key, 0) == value; };
}
auto whenNe(const char* key, int value)
{
    return [key = std::string(key), value](const nlohmann::json& v) { return v.value(key, 0) != value; };
}
auto whenTrue(const char* key)
{
    return [key = std::string(key)](const nlohmann::json& v) { return v.value(key, 0) != 0; };
}

SettingDef boolSetting(const char* key, const char* label, bool def, const char* tooltip = "")
{
    SettingDef s;
    s.key = key;
    s.label = label;
    s.tooltip = tooltip;
    s.type = SettingType::Bool;
    s.defaultValue = def ? 1 : 0;
    return s;
}

SettingDef intSetting(const char* key, const char* label, int minV, int maxV, int def, const char* tooltip = "")
{
    SettingDef s;
    s.key = key;
    s.label = label;
    s.tooltip = tooltip;
    s.type = SettingType::Int;
    s.minValue = minV;
    s.maxValue = maxV;
    s.defaultValue = def;
    return s;
}

SettingDef choiceSetting(const char* key, const char* label, std::vector<std::string> choices, int def, const char* tooltip = "")
{
    SettingDef s;
    s.key = key;
    s.label = label;
    s.tooltip = tooltip;
    s.type = SettingType::Choice;
    s.choices = std::move(choices);
    s.maxValue = int(s.choices.size()) - 1;
    s.defaultValue = def;
    return s;
}

// Settings for building a TSShape from an Assimp scene (used by both TS targets).
void addTsImportSettings(std::vector<SettingDef>& s)
{
    s.push_back(choiceSetting("lod", "gdc.format.lod.label", {"gdc.format.choice.auto", "gdc.format.lod.choice.trailingNumber", "gdc.format.lod.choice.singleDetail"}, 0,
                              "gdc.format.lod.tooltip"));
    SettingDef size = intSetting("detailSize", "gdc.format.detailSize.label", 1, 100000, 2);
    size.visible = whenEq("lod", 2);
    s.push_back(size);
    s.push_back(choiceSetting("scale", "gdc.format.scale.label", {"gdc.format.choice.auto", "gdc.format.scale.choice.uniform", "gdc.format.scale.choice.aligned", "gdc.format.scale.choice.arbitrary"}, 0,
                              "gdc.format.scale.tooltip"));
    s.push_back(choiceSetting("exporter", "gdc.format.exporter.label", {"gdc.format.exporter.choice.collada", "gdc.format.exporter.choice.assimp"}, 0,
                              "gdc.format.exporter.tooltip"));
}

void addSharedAssimpSettings(std::vector<SettingDef>& s, bool pointClouds)
{
    s.push_back(boolSetting("lodNames", "gdc.format.lodNames.label", true,
                            "gdc.format.lodNames.tooltip"));
    SettingDef col = boolSetting("collision", "gdc.format.collision.label", true);
    col.visible = whenTrue("lodNames");
    s.push_back(col);
    s.push_back(boolSetting("animations", "gdc.format.animations.label", true));
    if (pointClouds) {
        s.push_back(boolSetting("pointClouds", "gdc.format.pointClouds.label", false, "gdc.format.pointClouds.tooltip"));
    }
}

struct Registry {
    std::vector<Format> list;

    Registry()
    {
        addTsFormats();
        addAssimpWriters();
        addAssimpReaders();
    }

    void addTsFormats()
    {
        Format cdae;
        cdae.id = "cdae";
        cdae.label = "Torque3D cached shape (CDAE v31)";
        cdae.exts = {".cdae"};
        cdae.canRead = cdae.canWrite = cdae.isTs = true;
        cdae.caps = {true, true, true, true, true, true, true, true, true, true, true};
        cdae.settings.push_back(choiceSetting("compression", "gdc.format.compression.label", {"gdc.format.compression.choice.auto", "gdc.format.choice.on", "gdc.format.choice.off"}, 0,
                                              "gdc.format.compression.tooltip"));
        SettingDef level = intSetting("zstdLevel", "gdc.format.zstdLevel.label", 1, 19, 3);
        level.visible = whenNe("compression", 2);
        cdae.settings.push_back(level);
        addTsImportSettings(cdae.settings);
        cdae.exporterId = [](const nlohmann::json&) { return std::string(); };
        cdae.outputExt = [](const nlohmann::json&) { return std::string(".cdae"); };
        list.push_back(cdae);

        Format dts;
        dts.id = "dts";
        dts.label = "Torque3D shape (DTS)";
        dts.exts = {".dts"};
        dts.canRead = dts.canWrite = dts.isTs = true;
        dts.caps = cdae.caps;
        std::vector<std::string> versions;
        for (int v = ts::kMaxDtsWriteVersion; v >= ts::kMinDtsWriteVersion; --v) {
            // No real v27/v28 sample exists to check these layouts against.
            versions.push_back(v == 27 || v == 28 ? "gdc.format.dtsVersion.v" + std::to_string(v) : format("v%d", v));
        }
        SettingDef version = choiceSetting("version", "gdc.format.dtsVersion.label", versions, 0,
                                           "gdc.format.dtsVersion.tooltip");
        dts.settings.push_back(version);
        addTsImportSettings(dts.settings);
        dts.exporterId = [](const nlohmann::json&) { return std::string(); };
        dts.outputExt = [](const nlohmann::json&) { return std::string(".dts"); };
        list.push_back(dts);
    }

    void addAssimpWriters()
    {
        Assimp::Exporter exporter;
        std::set<std::string> ids;
        for (size_t i = 0; i < exporter.GetExportFormatCount(); ++i) {
            ids.insert(exporter.GetExportFormatDescription(i)->id);
        }
        std::set<std::string> folded;
        auto fold = [&](Format f, std::vector<std::string> members) {
            members.erase(std::remove_if(members.begin(), members.end(), [&](const std::string& m) { return !ids.count(m); }),
                          members.end());
            if (members.empty()) {
                return;
            }
            folded.insert(members.begin(), members.end());
            f.canWrite = true;
            list.push_back(std::move(f));
        };

        {
            Format f;
            f.id = "collada";
            f.label = "Collada (.dae)";
            f.exts = {".dae"};
            f.caps.uv2 = f.caps.colors = f.caps.skins = f.caps.animations = true;
            addSharedAssimpSettings(f.settings, true);
            f.exporterId = [](const nlohmann::json&) { return std::string("collada"); };
            f.outputExt = [](const nlohmann::json&) { return std::string(".dae"); };
            fold(f, {"collada"});
        }
        {
            Format f;
            f.id = "gltf2";
            f.label = "glTF 2.0 (.glb / .gltf)";
            f.exts = {".glb", ".gltf"};
            f.caps.uv2 = f.caps.colors = f.caps.tangents = f.caps.skins = f.caps.animations = f.caps.morphs = true;
            f.settings.push_back(choiceSetting("container", "gdc.format.container.label", {"gdc.format.container.choice.glb", "gdc.format.container.choice.gltf"}, 0));
            f.settings.push_back(boolSetting("unlimitedBones", "gdc.format.unlimitedBones.label", true,
                                             "gdc.format.unlimitedBones.tooltip"));
            f.settings.push_back(boolSetting("specGloss", "gdc.format.specGloss.label", false,
                                             "gdc.format.specGloss.tooltip"));
            f.settings.push_back(boolSetting("draco", "gdc.format.draco.label", false,
                                             "gdc.format.draco.tooltip"));
            auto dracoOn = whenTrue("draco");
            SettingDef level = intSetting("dracoLevel", "gdc.format.dracoLevel.label", 0, 10, 7,
                                          "gdc.format.dracoLevel.tooltip");
            level.visible = dracoOn;
            f.settings.push_back(level);
            const struct {
                const char* key;
                const char* label;
                int def;
                const char* tip;
            } bits[] = {
                {"qPos", "gdc.format.qPos.label", 14,
                 "gdc.format.qPos.tooltip"},
                {"qNormal", "gdc.format.qNormal.label", 10,
                 "gdc.format.qNormal.tooltip"},
                {"qUv", "gdc.format.qUv.label", 12,
                 "gdc.format.qUv.tooltip"},
                {"qColor", "gdc.format.qColor.label", 8,
                 "gdc.format.qColor.tooltip"},
                {"qGeneric", "gdc.format.qGeneric.label", 12,
                 "gdc.format.qGeneric.tooltip"}};
            for (const auto& b : bits) {
                SettingDef q = intSetting(b.key, b.label, 1, 30, b.def, b.tip);
                q.visible = dracoOn;
                f.settings.push_back(q);
            }
            addSharedAssimpSettings(f.settings, true);
            f.exporterId = [](const nlohmann::json& v) { return std::string(v.value("container", 0) == 1 ? "gltf2" : "glb2"); };
            f.outputExt = [](const nlohmann::json& v) { return std::string(v.value("container", 0) == 1 ? ".gltf" : ".glb"); };
            fold(f, {"gltf2", "glb2"});
        }
        {
            Format f;
            f.id = "gltf1";
            f.label = "glTF 1.0 (.glb / .gltf)";
            f.exts = {".glb", ".gltf"};
            f.settings.push_back(choiceSetting("container", "gdc.format.container.label", {"gdc.format.container.choice.glb", "gdc.format.container.choice.gltf"}, 0));
            addSharedAssimpSettings(f.settings, false);
            f.exporterId = [](const nlohmann::json& v) { return std::string(v.value("container", 0) == 1 ? "gltf" : "glb"); };
            f.outputExt = [](const nlohmann::json& v) { return std::string(v.value("container", 0) == 1 ? ".gltf" : ".glb"); };
            fold(f, {"gltf", "glb"});
        }
        {
            Format f;
            f.id = "fbx";
            f.label = "Autodesk FBX (.fbx)";
            f.exts = {".fbx"};
            f.caps.uv2 = f.caps.colors = f.caps.skins = f.caps.animations = true;
            f.settings.push_back(choiceSetting("container", "gdc.format.encoding.label", {"gdc.format.encoding.choice.binary", "gdc.format.encoding.choice.ascii"}, 0));
            f.settings.push_back(boolSetting("transparencyFactor", "gdc.format.transparencyFactor.label", false,
                                             "gdc.format.transparencyFactor.tooltip"));
            addSharedAssimpSettings(f.settings, false);
            f.exporterId = [](const nlohmann::json& v) { return std::string(v.value("container", 0) == 1 ? "fbxa" : "fbx"); };
            f.outputExt = [](const nlohmann::json&) { return std::string(".fbx"); };
            fold(f, {"fbx", "fbxa"});
        }
        {
            Format f;
            f.id = "obj";
            f.label = "Wavefront OBJ (.obj)";
            f.exts = {".obj"};
            f.caps.hierarchy = false;
            f.settings.push_back(boolSetting("mtl", "gdc.format.mtl.label", true));
            addSharedAssimpSettings(f.settings, true);
            f.exporterId = [](const nlohmann::json& v) { return std::string(v.value("mtl", 1) ? "obj" : "objnomtl"); };
            f.outputExt = [](const nlohmann::json&) { return std::string(".obj"); };
            fold(f, {"obj", "objnomtl"});
        }
        {
            Format f;
            f.id = "stl";
            f.label = "Stereolithography (.stl)";
            f.exts = {".stl"};
            f.caps = {false, false, false, false, false, false, false, false, false, false, false};
            f.settings.push_back(choiceSetting("container", "gdc.format.encoding.label", {"gdc.format.encoding.choice.binary", "gdc.format.encoding.choice.ascii"}, 0));
            addSharedAssimpSettings(f.settings, true);
            f.exporterId = [](const nlohmann::json& v) { return std::string(v.value("container", 0) == 1 ? "stl" : "stlb"); };
            f.outputExt = [](const nlohmann::json&) { return std::string(".stl"); };
            fold(f, {"stl", "stlb"});
        }
        {
            Format f;
            f.id = "ply";
            f.label = "Stanford PLY (.ply)";
            f.exts = {".ply"};
            f.caps.hierarchy = f.caps.materials = false;
            f.caps.colors = true;
            f.settings.push_back(choiceSetting("container", "gdc.format.encoding.label", {"gdc.format.encoding.choice.binary", "gdc.format.encoding.choice.ascii"}, 0));
            addSharedAssimpSettings(f.settings, true);
            f.exporterId = [](const nlohmann::json& v) { return std::string(v.value("container", 0) == 1 ? "ply" : "plyb"); };
            f.outputExt = [](const nlohmann::json&) { return std::string(".ply"); };
            fold(f, {"ply", "plyb"});
        }
        {
            Format f;
            f.id = "x";
            f.label = "DirectX X (.x)";
            f.exts = {".x"};
            f.caps.colors = true;
            f.settings.push_back(boolSetting("x64", "gdc.format.x64.label", false));
            addSharedAssimpSettings(f.settings, true);
            f.exporterId = [](const nlohmann::json&) { return std::string("x"); };
            f.outputExt = [](const nlohmann::json&) { return std::string(".x"); };
            fold(f, {"x"});
        }

        // Everything else Assimp can write, with no format-specific settings.
        for (size_t i = 0; i < exporter.GetExportFormatCount(); ++i) {
            const aiExportFormatDesc* d = exporter.GetExportFormatDescription(i);
            if (folded.count(d->id)) {
                continue;
            }
            Format f;
            f.id = d->id;
            f.label = format("%s (.%s)", d->description, d->fileExtension);
            f.exts = {"." + toLower(d->fileExtension)};
            f.canWrite = true;
            const std::string id = d->id;
            if (id == "assbin" || id == "assjson" || id == "assxml") {
                f.caps.uv2 = f.caps.colors = f.caps.tangents = f.caps.skins = f.caps.animations = f.caps.morphs = true;
            } else if (id == "3ds") {
                f.caps.normals = false;
            } else if (id == "3mf" || id == "stp" || id == "pbrt") {
                f.caps.uv = id == "pbrt";
                f.caps.normals = id == "pbrt";
            }
            addSharedAssimpSettings(f.settings, true);
            const std::string ext = "." + std::string(d->fileExtension);
            f.exporterId = [id](const nlohmann::json&) { return id; };
            f.outputExt = [ext](const nlohmann::json&) { return ext; };
            list.push_back(std::move(f));
        }
    }

    void addAssimpReaders()
    {
        Assimp::Importer importer;
        for (size_t i = 0; i < importer.GetImporterCount(); ++i) {
            const aiImporterDesc* d = importer.GetImporterInfo(i);
            if (!d) {
                continue;
            }
            Format f;
            f.id = "in:" + std::string(d->mName);
            f.label = d->mName;
            std::istringstream exts(d->mFileExtensions);
            std::string e;
            while (exts >> e) {
                // Assimp's Terragen reader would claim BeamNG's own .ter terrain files, which are not Terragen.
                // Its .blend reader only knows old Blender versions, and the game never loads .blend anyway.
                if (toLower(e) != "ter" && toLower(e) != "blend") {
                    f.exts.push_back("." + toLower(e));
                }
            }
            if (f.exts.empty()) {
                continue;
            }
            f.canRead = true;
            list.push_back(std::move(f));
        }
    }
};

const Registry& registry()
{
    static Registry r;
    return r;
}

} // namespace

const std::vector<Format>& formats() { return registry().list; }

const Format* formatById(const std::string& id)
{
    for (const Format& f : formats()) {
        if (f.id == id) {
            return &f;
        }
    }
    return nullptr;
}

const Format* readerForExt(const std::string& ext)
{
    const std::string e = toLower(ext);
    for (const Format& f : formats()) {
        if (f.canRead && std::find(f.exts.begin(), f.exts.end(), e) != f.exts.end()) {
            return &f;
        }
    }
    return nullptr;
}

std::vector<std::string> readableExtensions()
{
    std::set<std::string> out;
    for (const Format& f : formats()) {
        if (f.canRead) {
            out.insert(f.exts.begin(), f.exts.end());
        }
    }
    return {out.begin(), out.end()};
}

std::vector<const Format*> writableFormats()
{
    std::vector<const Format*> out;
    for (const Format& f : formats()) {
        if (f.canWrite) {
            out.push_back(&f);
        }
    }
    return out;
}

int settingValue(const Format& f, const nlohmann::json& values, const std::string& key)
{
    for (const SettingDef& s : f.settings) {
        if (s.key == key) {
            int v = s.defaultValue;
            if (values.is_object() && values.contains(key) && values[key].is_number()) {
                v = values[key].get<int>();
            }
            return std::clamp(v, s.minValue, s.maxValue);
        }
    }
    return 0;
}

bool settingBool(const Format& f, const nlohmann::json& values, const std::string& key) { return settingValue(f, values, key) != 0; }

nlohmann::json withDefaults(const Format& f, const nlohmann::json& values)
{
    nlohmann::json out = nlohmann::json::object();
    for (const SettingDef& s : f.settings) {
        out[s.key] = settingValue(f, values, s.key);
    }
    return out;
}

std::string describeSettings(const Format& f, const nlohmann::json& values)
{
    const nlohmann::json v = withDefaults(f, values);
    std::string out;
    for (const SettingDef& s : f.settings) {
        if (s.visible && !s.visible(v)) {
            continue;
        }
        const int value = v[s.key].get<int>();
        std::string text;
        switch (s.type) {
        case SettingType::Bool:
            text = value ? "on" : "off";
            break;
        case SettingType::Int:
            text = std::to_string(value);
            break;
        case SettingType::Choice:
            text = value >= 0 && value < int(s.choices.size()) ? tr(s.choices[value]) : std::to_string(value);
            break;
        }
        if (!out.empty()) {
            out += ", ";
        }
        out += std::string(tr(s.label)) + " " + text;
    }
    return out.empty() ? "no extra settings" : out;
}

std::string modelExtension(const std::string& path)
{
    const std::string name = toLower(fs::u8path(path).filename().u8string());
    if (hasSuffix(name, ".mesh.xml")) {
        return ".mesh.xml";
    }
    const size_t dot = name.rfind('.');
    return dot == std::string::npos ? std::string() : name.substr(dot);
}

} // namespace model
