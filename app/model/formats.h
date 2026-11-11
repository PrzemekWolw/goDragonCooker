#pragma once

#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

// Model formats the converter can read and write: the TSShape formats (DTS, CDAE) plus every
// Assimp importer and exporter. Assimp exporter variants (gltf/glb, binary/ascii, obj with or
// without mtl) are folded into one target with a container setting.
namespace model {

enum class SettingType { Bool, Int, Choice };

struct SettingDef {
    std::string key;
    std::string label;
    std::string tooltip;
    SettingType type = SettingType::Bool;
    int minValue = 0;
    int maxValue = 1;
    std::vector<std::string> choices;
    int defaultValue = 0;
    // Shown only when this returns true for the current values (nullptr = always).
    std::function<bool(const nlohmann::json&)> visible;
};

// What survives a write to this format; validation only compares what both sides keep.
struct Capabilities {
    bool hierarchy = true; // the node tree (parents and node transforms)
    bool materials = true;
    bool normals = true;
    bool uv = true;
    bool uv2 = false;
    bool colors = false;
    bool tangents = false;
    bool skins = false;
    bool animations = false;
    bool morphs = false;
    bool lods = false; // detail levels and collision as real TSShape details
    bool meshNames = true; // the name of every mesh node
    bool poses = true; // node transforms; false when the writer bakes them into the vertices
};

struct Format {
    std::string id;
    std::string label;
    std::vector<std::string> exts; // lowercase, with dot
    bool canRead = false;
    bool canWrite = false;
    bool isTs = false;
    Capabilities caps;
    std::vector<SettingDef> settings;
    // Assimp exporter id for a given settings object (TS formats return "").
    std::function<std::string(const nlohmann::json&)> exporterId;
    // Output extension for a given settings object.
    std::function<std::string(const nlohmann::json&)> outputExt;
    // The written mesh node name for a mesh node read back from this format (name, parent node name), when the
    // writer decorates it.
    std::function<std::string(const std::string&, const std::string&)> meshNameOf;
};

const std::vector<Format>& formats();
const Format* formatById(const std::string& id);
// Lowercase extension with dot; ".xml"-style ambiguous extensions resolve to the first reader.
const Format* readerForExt(const std::string& ext);
std::vector<std::string> readableExtensions();
std::vector<const Format*> writableFormats();

int settingValue(const Format& f, const nlohmann::json& values, const std::string& key);
bool settingBool(const Format& f, const nlohmann::json& values, const std::string& key);
// Defaults filled in for every setting of the format.
nlohmann::json withDefaults(const Format& f, const nlohmann::json& values);
// "draco on, level 7, ..." for the conversion log.
std::string describeSettings(const Format& f, const nlohmann::json& values);

// Extension of a path including multi-part ones like ".mesh.xml", lowercase.
std::string modelExtension(const std::string& path);

} // namespace model
