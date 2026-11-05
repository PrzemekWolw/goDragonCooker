#include "model/formats.h"

#include <algorithm>
#include <mutex>
#include <set>
#include <sstream>

#include <assimp/Exporter.hpp>
#include <assimp/Importer.hpp>
#include <assimp/importerdesc.h>

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
    s.push_back(choiceSetting("lod", "LOD detection", {"Auto", "Trailing number", "Single detail"}, 0,
                              "Auto: trailing-number LODs (body200, Collision-1) when the scene has base*/start* nodes,\n"
                              "otherwise every mesh node is one object at a single detail size."));
    SettingDef size = intSetting("detailSize", "Single detail size", 1, 100000, 2);
    size.visible = whenEq("lod", 2);
    s.push_back(size);
    s.push_back(choiceSetting("scale", "Animated scale", {"Auto", "Uniform", "Aligned", "Arbitrary"}, 0,
                              "How scale keyframes are stored. Auto picks uniform or aligned per sequence."));
    s.push_back(choiceSetting("exporter", "Exporter id", {"Collada (124)", "Assimp (233)"}, 0,
                              "Stored in the version word. The game uses a cached .cdae next to a .dae only when it is 124\n"
                              "(or the .dae is missing); 233 marks an Assimp import cache."));
}

void addSharedAssimpSettings(std::vector<SettingDef>& s, bool pointClouds)
{
    s.push_back(boolSetting("lodNames", "Export LODs and collision", true,
                            "From DTS/CDAE: every detail level becomes a node named <object><size> (body200, Colmesh-1)\n"
                            "under base01/start01, so importing it back restores the LODs. Off: highest visible detail only."));
    SettingDef col = boolSetting("collision", "Include collision meshes", true);
    col.visible = whenTrue("lodNames");
    s.push_back(col);
    s.push_back(boolSetting("animations", "Export animations", true));
    if (pointClouds) {
        s.push_back(boolSetting("pointClouds", "Export point clouds", false, "Write meshes without faces as point clouds."));
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
        cdae.settings.push_back(choiceSetting("compression", "Compression", {"Auto (> 512 KB)", "On", "Off"}, 0,
                                              "zstd-compress the body. Auto compresses bodies larger than 512 KB."));
        SettingDef level = intSetting("zstdLevel", "zstd level", 1, 19, 3);
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
            versions.push_back(format(v == 27 || v == 28 ? "v%d (unverified)" : "v%d", v));
        }
        SettingDef version = choiceSetting("version", "DTS version", versions, 0,
                                           "v29: current. v28 drops the object-name header. v27/26 also drop tangents.\n"
                                           "v25/24 also drop UV2 and vertex colors and need 16-bit indices.");
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
            f.settings.push_back(choiceSetting("container", "Container", {"Binary (.glb)", "JSON + .bin (.gltf)"}, 0));
            f.settings.push_back(boolSetting("unlimitedBones", "Unlimited bones per vertex", true,
                                             "Write JOINTS_1.. sets instead of keeping only the 4 largest weights."));
            f.settings.push_back(boolSetting("specGloss", "Specular-glossiness materials", false,
                                             "KHR_materials_pbrSpecularGlossiness instead of metal-rough."));
            f.settings.push_back(boolSetting("draco", "Draco mesh compression", false,
                                             "KHR_draco_mesh_compression. The game's Assimp reads it. Joints and weights stay lossless;\n"
                                             "points, lines and morph targets stay uncompressed."));
            auto dracoOn = whenTrue("draco");
            SettingDef level = intSetting("dracoLevel", "Draco level", 0, 10, 7, "Higher is smaller and slower to encode.");
            level.visible = dracoOn;
            f.settings.push_back(level);
            const struct {
                const char* key;
                const char* label;
                int def;
            } bits[] = {{"qPos", "Position bits", 14}, {"qNormal", "Normal bits", 10}, {"qUv", "UV bits", 12},
                        {"qColor", "Color bits", 8},   {"qGeneric", "Generic bits", 12}};
            for (const auto& b : bits) {
                SettingDef q = intSetting(b.key, b.label, 1, 30, b.def, "Quantization bits; position error is about bbox / 2^bits.");
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
            f.settings.push_back(choiceSetting("container", "Container", {"Binary (.glb)", "JSON + .bin (.gltf)"}, 0));
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
            f.settings.push_back(choiceSetting("container", "Encoding", {"Binary", "ASCII"}, 0));
            f.settings.push_back(boolSetting("transparencyFactor", "Transparency factor refers to opacity", false,
                                             "Write TransparencyFactor as opacity (some tools read it that way)."));
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
            f.settings.push_back(boolSetting("mtl", "Write .mtl material file", true));
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
            f.settings.push_back(choiceSetting("container", "Encoding", {"Binary", "ASCII"}, 0));
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
            f.settings.push_back(choiceSetting("container", "Encoding", {"Binary", "ASCII"}, 0));
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
            f.settings.push_back(boolSetting("x64", "64-bit floats", false));
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
                f.exts.push_back("." + toLower(e));
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
            text = value >= 0 && value < int(s.choices.size()) ? s.choices[value] : std::to_string(value);
            break;
        }
        if (!out.empty()) {
            out += ", ";
        }
        out += s.label + " " + text;
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
