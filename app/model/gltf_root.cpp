#include "model/gltf_root.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/platform.h"
#include "model/ts/ts_io.h"

namespace model {
namespace {

using json = nlohmann::json;

constexpr uint32_t kGlbMagic = 0x46546C67; // "glTF"
constexpr uint32_t kChunkJson = 0x4E4F534A;

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

bool startsWithCi(const json& name, const std::string& prefix)
{
    const std::string s = name.is_string() ? name.get<std::string>() : std::string();
    return s.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), s.begin(), [](char a, char b) { return a == std::tolower(static_cast<unsigned char>(b)); });
}

// A base* node with a start* child is where the game reads detail levels; it is a node, not a container.
bool isBaseStart(const json& g, const json& node)
{
    if (!startsWithCi(node.value("name", json()), "base")) {
        return false;
    }
    for (const json& child : node.value("children", json::array())) {
        if (startsWithCi(g["nodes"][child.get<size_t>()].value("name", json()), "start")) {
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

} // namespace

bool gltfDropEmptyRoot(const std::string& path, std::string& error)
{
    std::vector<uint8_t> bytes;
    if (!ts::readFileBytes(path, bytes, error)) {
        return false;
    }
    const bool glb = bytes.size() >= 12 && readU32(bytes.data()) == kGlbMagic;
    std::vector<uint8_t> tail; // chunks after the JSON one
    std::string text;
    if (glb) {
        if (bytes.size() < 20 || readU32(&bytes[16]) != kChunkJson || 20 + readU32(&bytes[12]) > bytes.size()) {
            error = "invalid GLB";
            return false;
        }
        const size_t len = readU32(&bytes[12]);
        text.assign(reinterpret_cast<const char*>(&bytes[20]), len);
        tail.assign(bytes.begin() + 20 + len, bytes.end());
    } else {
        text.assign(bytes.begin(), bytes.end());
    }
    json g = json::parse(text, nullptr, false);
    if (g.is_discarded() || !g.is_object()) {
        error = "invalid glTF JSON";
        return false;
    }
    if (!isEmptyRoot(g)) {
        return true;
    }
    dropRoot(g);
    text = g.dump();
    if (!glb) {
        return ts::writeFileBytes(path, std::vector<uint8_t>(text.begin(), text.end()), error);
    }
    while (text.size() % 4) {
        text.push_back(' ');
    }
    std::vector<uint8_t> out;
    appendU32(out, kGlbMagic);
    appendU32(out, 2);
    appendU32(out, uint32_t(12 + 8 + text.size() + tail.size()));
    appendU32(out, uint32_t(text.size()));
    appendU32(out, kChunkJson);
    out.insert(out.end(), text.begin(), text.end());
    out.insert(out.end(), tail.begin(), tail.end());
    return ts::writeFileBytes(path, out, error);
}

} // namespace model
