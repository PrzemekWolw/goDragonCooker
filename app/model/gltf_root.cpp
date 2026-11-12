#include "model/gltf_root.h"

#include <algorithm>
#include <cstring>

#include "core/platform.h"
#include "core/strings.h"
#include "model/model.h"
#include "model/ts/ts_io.h"

namespace model {
namespace {

using json = nlohmann::json;

constexpr uint32_t kGlbMagic = 0x46546C67; // "glTF"
constexpr uint32_t kChunkJson = 0x4E4F534A;
constexpr uint32_t kChunkBin = 0x004E4942;

uint32_t readU32(const uint8_t* p)
{
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

void appendU32(std::vector<uint8_t>& out, uint32_t v)
{
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
    out.insert(out.end(), p, p + 4);
}

bool nameStartsWith(const json& node, const char* prefix)
{
    const json name = node.value("name", json());
    return name.is_string() && hasPrefixI(name.get<std::string>(), prefix);
}

// A base* node with a start* child is where the game reads detail levels; it is a node, not a container.
bool isBaseStart(const json& g, const json& node)
{
    if (!nameStartsWith(node, "base")) {
        return false;
    }
    for (const json& child : node.value("children", json::array())) {
        if (nameStartsWith(g["nodes"][child.get<size_t>()], "start")) {
            return true;
        }
    }
    return false;
}

bool isEmptyRoot(const json& g)
{
    const json& scenes = g.value("scenes", json::array());
    if (!g.contains("nodes") || scenes.empty() || scenes[0].value("nodes", json::array()) != json::array({0})) {
        return false;
    }
    const json& root = g["nodes"][0];
    if (isBaseStart(g, root)) {
        return false;
    }
    for (const char* key : {"mesh", "skin", "camera", "matrix", "translation", "rotation", "scale", "weights"}) {
        if (root.contains(key)) {
            return false;
        }
    }
    // Nothing else may point at the root node.
    for (const json& s : g.value("skins", json::array())) {
        const json joints = s.value("joints", json::array());
        if (s.value("skeleton", size_t(1)) == 0 || std::find(joints.begin(), joints.end(), 0) != joints.end()) {
            return false;
        }
    }
    for (const json& a : g.value("animations", json::array())) {
        for (const json& c : a.value("channels", json::array())) {
            if (c.value("target", json::object()).value("node", size_t(1)) == 0) {
                return false;
            }
        }
    }
    return root.contains("children");
}

void dropRoot(json& g)
{
    json children = g["nodes"][0]["children"];
    g["nodes"].erase(0);
    const auto shift = [](json& indices) {
        for (json& i : indices) {
            i = i.get<size_t>() - 1;
        }
    };
    for (json& n : g["nodes"]) {
        if (n.contains("children")) {
            shift(n["children"]);
        }
    }
    if (g.contains("skins")) {
        for (json& s : g["skins"]) {
            if (s.contains("skeleton")) {
                s["skeleton"] = s["skeleton"].get<size_t>() - 1;
            }
            shift(s["joints"]);
        }
    }
    if (g.contains("animations")) {
        for (json& a : g["animations"]) {
            for (json& c : a["channels"]) {
                c["target"]["node"] = c["target"]["node"].get<size_t>() - 1;
            }
        }
    }
    shift(children);
    g["scenes"][0]["nodes"] = children;
}

// The exporter's id has to be unique, so a node that shares a name with a material or a mesh is written
// "name_node". glTF names do not have to be unique, and the game addresses the mesh by the node name.
bool restoreNodeNames(json& g)
{
    if (!g.contains("nodes") || !g["nodes"].is_array()) {
        return false;
    }
    bool changed = false;
    for (json& node : g["nodes"]) {
        if (!node.is_object()) {
            continue;
        }
        const json extras = node.value("extras", json::object());
        if (!extras.is_object() || !extras.contains(kOriginalNameKey) || !extras[kOriginalNameKey].is_string()) {
            continue;
        }
        const std::string original = extras[kOriginalNameKey].get<std::string>();
        if (original.empty() || node.value("name", std::string()) == original) {
            continue;
        }
        node["name"] = original;
        changed = true;
    }
    return changed;
}

} // namespace

bool readGltfFile(const std::string& path, GltfFile& file, std::string& error)
{
    std::vector<uint8_t> bytes;
    if (!readFileBytes(path, bytes, error)) {
        return false;
    }
    std::string text;
    file.glb = bytes.size() >= 12 && readU32(bytes.data()) == kGlbMagic;
    if (file.glb) {
        for (size_t p = 12; p + 8 <= bytes.size();) {
            const uint32_t len = readU32(&bytes[p]);
            const uint32_t type = readU32(&bytes[p + 4]);
            if (p + 8 + len > bytes.size()) {
                error = "truncated GLB chunk";
                return false;
            }
            if (type == kChunkJson) {
                text.assign(reinterpret_cast<const char*>(&bytes[p + 8]), len);
            } else if (type == kChunkBin && file.bin.empty()) {
                file.bin.assign(bytes.begin() + p + 8, bytes.begin() + p + 8 + len);
            }
            p += 8 + len;
        }
    } else {
        text.assign(bytes.begin(), bytes.end());
    }
    file.json = json::parse(text, nullptr, false);
    if (file.json.is_discarded() || !file.json.is_object()) {
        error = "invalid glTF JSON";
        return false;
    }
    return true;
}

bool writeGltfFile(const std::string& path, const GltfFile& file, std::string& error)
{
    std::string text = file.json.dump();
    if (!file.glb) {
        return writeFileBytes(path, std::vector<uint8_t>(text.begin(), text.end()), error);
    }
    while (text.size() % 4) {
        text.push_back(' ');
    }
    const size_t binChunk = file.bin.empty() ? 0 : 8 + file.bin.size();
    std::vector<uint8_t> out;
    appendU32(out, kGlbMagic);
    appendU32(out, 2);
    appendU32(out, uint32_t(12 + 8 + text.size() + binChunk));
    appendU32(out, uint32_t(text.size()));
    appendU32(out, kChunkJson);
    out.insert(out.end(), text.begin(), text.end());
    if (binChunk) {
        appendU32(out, uint32_t(file.bin.size()));
        appendU32(out, kChunkBin);
        out.insert(out.end(), file.bin.begin(), file.bin.end());
    }
    return writeFileBytes(path, out, error);
}

bool gltfDropEmptyRoot(const std::string& path, std::string& error)
{
    GltfFile file;
    if (!readGltfFile(path, file, error)) {
        return false;
    }
    const bool dropped = isEmptyRoot(file.json);
    if (dropped) {
        dropRoot(file.json);
    }
    const bool renamed = restoreNodeNames(file.json);
    return (!dropped && !renamed) || writeGltfFile(path, file, error);
}

} // namespace model
