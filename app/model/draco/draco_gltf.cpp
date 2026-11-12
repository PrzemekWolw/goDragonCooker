#include "model/draco/draco_gltf.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>

#include <nlohmann/json.hpp>

#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include <draco/compression/decode.h>
#include <draco/compression/expert_encode.h>
#include <draco/mesh/mesh.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include "core/platform.h"
#include "core/strings.h"
#include "model/gltf_root.h"
#include "model/ts/ts_io.h"

namespace model {
namespace {

using json = nlohmann::json;

constexpr const char* kExt = "KHR_draco_mesh_compression";

int componentSize(int type)
{
    switch (type) {
    case 5120:
    case 5121:
        return 1;
    case 5122:
    case 5123:
        return 2;
    case 5125:
    case 5126:
        return 4;
    default:
        return 0;
    }
}

draco::DataType dracoType(int type)
{
    switch (type) {
    case 5120:
        return draco::DT_INT8;
    case 5121:
        return draco::DT_UINT8;
    case 5122:
        return draco::DT_INT16;
    case 5123:
        return draco::DT_UINT16;
    case 5125:
        return draco::DT_UINT32;
    default:
        return draco::DT_FLOAT32;
    }
}

int numComponents(const std::string& type)
{
    if (type == "SCALAR") {
        return 1;
    }
    if (type == "VEC2") {
        return 2;
    }
    if (type == "VEC3") {
        return 3;
    }
    if (type == "VEC4") {
        return 4;
    }
    return 0;
}

struct Document {
    GltfFile file;
    json& gltf = file.json;
    std::vector<std::vector<uint8_t>> buffers;
    fs::path dir;
};

bool load(const std::string& path, Document& doc, std::string& error)
{
    if (!readGltfFile(path, doc.file, error)) {
        return false;
    }
    doc.dir = u8path(path).parent_path();
    for (const json& b : doc.gltf.value("buffers", json::array())) {
        if (!b.contains("uri")) {
            doc.buffers.push_back(doc.file.bin);
            continue;
        }
        const std::string uri = b["uri"].get<std::string>();
        if (hasPrefix(uri, "data:")) {
            error = "embedded data: URIs are not supported";
            return false;
        }
        std::vector<uint8_t> data;
        if (!readFileBytes(u8str(doc.dir / u8path(uri)), data, error)) {
            return false;
        }
        doc.buffers.push_back(std::move(data));
    }
    return true;
}

// Bytes of one accessor, tightly packed.
bool accessorBytes(const Document& doc, const json& acc, std::vector<uint8_t>& out, size_t& elemSize)
{
    const int cs = componentSize(acc.value("componentType", 0));
    const int nc = numComponents(acc.value("type", std::string()));
    if (!cs || !nc || !acc.contains("bufferView") || acc.contains("sparse")) {
        return false;
    }
    elemSize = size_t(cs) * nc;
    const json& view = doc.gltf["bufferViews"][acc["bufferView"].get<size_t>()];
    const size_t buffer = view.value("buffer", size_t(0));
    if (buffer >= doc.buffers.size()) {
        return false;
    }
    const std::vector<uint8_t>& data = doc.buffers[buffer];
    const size_t stride = view.value("byteStride", elemSize);
    const size_t base = view.value("byteOffset", size_t(0)) + acc.value("byteOffset", size_t(0));
    const size_t count = acc.value("count", size_t(0));
    if (count && base + stride * (count - 1) + elemSize > data.size()) {
        return false;
    }
    out.resize(count * elemSize);
    for (size_t i = 0; i < count; ++i) {
        std::memcpy(&out[i * elemSize], &data[base + i * stride], elemSize);
    }
    return true;
}

struct Encoded {
    std::vector<uint8_t> data;
    json attributes;
    size_t points = 0;
    size_t faces = 0;
};

bool encodePrimitive(const Document& doc, const json& prim, const DracoOptions& o, Encoded& enc)
{
    const json& accessors = doc.gltf["accessors"];
    const json& attrs = prim["attributes"];
    if (!attrs.contains("POSITION")) {
        return false;
    }
    const size_t numPoints = accessors[attrs["POSITION"].get<size_t>()].value("count", size_t(0));
    if (numPoints == 0) {
        return false;
    }
    draco::Mesh mesh;
    mesh.set_num_points(uint32_t(numPoints));

    std::vector<std::pair<int, int>> quant; // attribute id, bits (0 = lossless)
    std::vector<std::pair<std::string, int>> ids;
    for (auto it = attrs.begin(); it != attrs.end(); ++it) {
        const std::string name = it.key();
        const json& acc = accessors[it.value().get<size_t>()];
        if (acc.value("count", size_t(0)) != numPoints) {
            return false;
        }
        std::vector<uint8_t> bytes;
        size_t elemSize = 0;
        if (!accessorBytes(doc, acc, bytes, elemSize)) {
            return false;
        }
        const int ct = acc.value("componentType", 0);
        draco::GeometryAttribute::Type type = draco::GeometryAttribute::GENERIC;
        int bits = 0;
        if (name == "POSITION") {
            type = draco::GeometryAttribute::POSITION;
        } else if (name == "NORMAL") {
            type = draco::GeometryAttribute::NORMAL;
            bits = o.qNormal;
        } else if (hasPrefix(name, "TEXCOORD_")) {
            type = draco::GeometryAttribute::TEX_COORD;
            bits = o.qUv;
        } else if (hasPrefix(name, "COLOR_")) {
            type = draco::GeometryAttribute::COLOR;
            bits = o.qColor;
        } else if (name == "TANGENT") {
            bits = o.qGeneric;
        }
        if (ct != 5126) {
            bits = 0; // integer data is never quantized
        }
        draco::GeometryAttribute ga;
        ga.Init(type, nullptr, uint8_t(numComponents(acc.value("type", std::string()))), dracoType(ct), acc.value("normalized", false),
                int64_t(elemSize), 0);
        const int id = mesh.AddAttribute(ga, true, uint32_t(numPoints));
        if (id < 0) {
            return false;
        }
        draco::PointAttribute* pa = mesh.attribute(id);
        for (size_t i = 0; i < numPoints; ++i) {
            pa->SetAttributeValue(draco::AttributeValueIndex(uint32_t(i)), &bytes[i * elemSize]);
        }
        quant.push_back({id, bits});
        ids.push_back({name, id});
    }

    std::vector<uint32_t> indices;
    if (prim.contains("indices")) {
        const json& acc = accessors[prim["indices"].get<size_t>()];
        std::vector<uint8_t> bytes;
        size_t elemSize = 0;
        if (!accessorBytes(doc, acc, bytes, elemSize) || numComponents(acc.value("type", std::string())) != 1) {
            return false;
        }
        indices.resize(bytes.size() / elemSize);
        for (size_t i = 0; i < indices.size(); ++i) {
            const uint8_t* p = &bytes[i * elemSize];
            indices[i] = elemSize == 1 ? *p : elemSize == 2 ? uint32_t(p[0] | (p[1] << 8)) : uint32_t(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
        }
    } else {
        indices.resize(numPoints);
        for (size_t i = 0; i < numPoints; ++i) {
            indices[i] = uint32_t(i);
        }
    }
    if (indices.size() < 3 || indices.size() % 3) {
        return false;
    }
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        draco::Mesh::Face f;
        for (int k = 0; k < 3; ++k) {
            if (indices[i + k] >= numPoints) {
                return false;
            }
            f[k] = draco::PointIndex(indices[i + k]);
        }
        mesh.AddFace(f);
    }

    draco::ExpertEncoder encoder(mesh);
    const int speed = 10 - std::clamp(o.level, 0, 10);
    encoder.SetSpeedOptions(speed, speed);
    encoder.SetEncodingMethod(draco::MESH_EDGEBREAKER_ENCODING);
    for (const auto& [id, bits] : quant) {
        if (bits <= 0) {
            continue;
        }
        encoder.SetAttributeQuantization(id, bits);
    }
    draco::EncoderBuffer buffer;
    if (!encoder.EncodeToBuffer(&buffer).ok()) {
        return false;
    }
    enc.data.assign(buffer.data(), buffer.data() + buffer.size());

    // Edgebreaker may split non-manifold vertices; accessor counts must match the decoded mesh.
    draco::DecoderBuffer db;
    db.Init(buffer.data(), buffer.size());
    draco::Decoder decoder;
    auto decoded = decoder.DecodeMeshFromBuffer(&db);
    if (!decoded.ok() || !decoded.value()) {
        return false;
    }
    enc.points = decoded.value()->num_points();
    enc.faces = decoded.value()->num_faces();
    enc.attributes = json::object();
    for (const auto& [name, id] : ids) {
        enc.attributes[name] = mesh.attribute(id)->unique_id();
    }
    return true;
}

void markUsed(json& list, const char* key)
{
    if (!list.is_array()) {
        list = json::array();
    }
    for (const json& v : list) {
        if (v == key) {
            return;
        }
    }
    list.push_back(key);
}

} // namespace

bool dracoCompressGltf(const std::string& path, const DracoOptions& options, std::string& error, std::string& info)
{
    Document doc;
    if (!load(path, doc, error)) {
        return false;
    }
    json& g = doc.gltf;
    if (!g.contains("meshes") || !g.contains("accessors")) {
        info = "no meshes";
        return true;
    }

    // Accessors used by more than one primitive (or by anything else) cannot be redirected.
    std::map<size_t, int> uses;
    for (const json& m : g["meshes"]) {
        const json prims = m.value("primitives", json::array());
        for (const json& p : prims) {
            const json attrs = p.value("attributes", json::object());
            for (const auto& a : attrs.items()) {
                ++uses[a.value().get<size_t>()];
            }
            if (p.contains("indices")) {
                ++uses[p["indices"].get<size_t>()];
            }
        }
    }
    const json skins = g.value("skins", json::array());
    for (const json& s : skins) {
        if (s.contains("inverseBindMatrices")) {
            uses[s["inverseBindMatrices"].get<size_t>()] += 2;
        }
    }
    const json animations = g.value("animations", json::array());
    for (const json& a : animations) {
        const json samplers = a.value("samplers", json::array());
        for (const json& s : samplers) {
            uses[s.value("input", size_t(0))] += 2;
            uses[s.value("output", size_t(0))] += 2;
        }
    }

    std::vector<std::vector<uint8_t>> blobs;
    size_t total = 0, done = 0;
    for (json& m : g["meshes"]) {
        if (!m.contains("primitives")) {
            continue;
        }
        total += m["primitives"].size();
        // Lines and points stay raw; Draco only encodes triangles.
        for (json& p : m["primitives"]) {
            if (p.value("mode", 4) != 4) {
                continue;
            }
            bool ok = !p.contains("targets") && !(p.contains("extensions") && p["extensions"].contains(kExt));
            for (const auto& a : p["attributes"].items()) {
                ok = ok && uses[a.value().get<size_t>()] == 1;
            }
            if (p.contains("indices")) {
                ok = ok && uses[p["indices"].get<size_t>()] == 1;
            }
            Encoded enc;
            if (!ok || !encodePrimitive(doc, p, options, enc)) {
                continue;
            }
            for (const auto& a : p["attributes"].items()) {
                json& acc = g["accessors"][a.value().get<size_t>()];
                acc.erase("bufferView");
                acc.erase("byteOffset");
                acc["count"] = enc.points;
            }
            if (p.contains("indices")) {
                json& acc = g["accessors"][p["indices"].get<size_t>()];
                acc.erase("bufferView");
                acc.erase("byteOffset");
                acc["count"] = enc.faces * 3;
                const int ct = acc.value("componentType", 5125);
                acc["componentType"] = enc.points > 65535 ? 5125 : enc.points > 255 && ct == 5121 ? 5123 : ct;
            } else {
                json acc = {{"componentType", enc.points > 65535 ? 5125 : 5123}, {"count", enc.faces * 3}, {"type", "SCALAR"}};
                g["accessors"].push_back(acc);
                p["indices"] = g["accessors"].size() - 1;
            }
            p["extensions"][kExt] = {{"bufferView", size_t(-1)}, {"attributes", enc.attributes}};
            p["extensions"][kExt]["blob"] = blobs.size();
            blobs.push_back(std::move(enc.data));
            ++done;
        }
    }
    info = format("%zu of %zu primitives compressed", done, total);
    if (!done) {
        return true;
    }
    markUsed(g["extensionsUsed"], kExt);
    markUsed(g["extensionsRequired"], kExt);

    // Rebuild a single buffer from the views still referenced, then the Draco blobs.
    std::vector<uint8_t> bin;
    auto align = [&] {
        while (bin.size() % 4) {
            bin.push_back(0);
        }
    };
    json oldViews = g.value("bufferViews", json::array());
    json newViews = json::array();
    std::map<size_t, size_t> remap;
    auto keep = [&](json& holder) {
        if (!holder.contains("bufferView")) {
            return;
        }
        const size_t old = holder["bufferView"].get<size_t>();
        auto it = remap.find(old);
        if (it == remap.end()) {
            json v = oldViews[old];
            const std::vector<uint8_t>& src = doc.buffers[v.value("buffer", size_t(0))];
            const size_t off = v.value("byteOffset", size_t(0));
            const size_t len = v.value("byteLength", size_t(0));
            align();
            v["buffer"] = 0;
            v["byteOffset"] = bin.size();
            bin.insert(bin.end(), src.begin() + off, src.begin() + off + len);
            newViews.push_back(v);
            it = remap.emplace(old, newViews.size() - 1).first;
        }
        holder["bufferView"] = it->second;
    };
    for (json& acc : g["accessors"]) {
        keep(acc);
        if (acc.contains("sparse")) {
            keep(acc["sparse"]["indices"]);
            keep(acc["sparse"]["values"]);
        }
    }
    if (g.contains("images")) {
        for (json& img : g["images"]) {
            keep(img);
        }
    }
    for (json& m : g["meshes"]) {
        for (json& p : m["primitives"]) {
            if (!p.contains("extensions") || !p["extensions"].contains(kExt) || !p["extensions"][kExt].contains("blob")) {
                continue;
            }
            json& ext = p["extensions"][kExt];
            const std::vector<uint8_t>& blob = blobs[ext["blob"].get<size_t>()];
            align();
            newViews.push_back({{"buffer", 0}, {"byteOffset", bin.size()}, {"byteLength", blob.size()}});
            bin.insert(bin.end(), blob.begin(), blob.end());
            ext.erase("blob");
            ext["bufferView"] = newViews.size() - 1;
        }
    }
    align();
    g["bufferViews"] = newViews;

    json buffer = {{"byteLength", bin.size()}};
    if (!doc.file.glb) {
        const json& buffers = g["buffers"];
        const std::string binUri = !buffers.empty() && buffers[0].contains("uri") ? buffers[0]["uri"].get<std::string>()
                                                                                   : u8str(u8path(path).stem()) + ".bin";
        buffer["uri"] = binUri;
        if (!writeFileBytes(u8str(doc.dir / u8path(binUri)), bin, error)) {
            return false;
        }
    }
    g["buffers"] = json::array({buffer});
    doc.file.bin = std::move(bin);
    return writeGltfFile(path, doc.file, error);
}

} // namespace model
