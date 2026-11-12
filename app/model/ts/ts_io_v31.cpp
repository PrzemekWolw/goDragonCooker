// CDAE v31: i32 version, u32 header size, MessagePack header map, then a MessagePack body
// (optionally one zstd frame). Vectors are packed as (u32 count, i32 element size, bin bytes).
#include <algorithm>
#include <cstring>
#include <fstream>

#include <msgpack.hpp>
#include <zstd.h>

#include "core/platform.h"
#include "core/strings.h"
#include "model/ts/ts_io.h"

namespace ts {
namespace {

using Packer = msgpack::packer<msgpack::sbuffer>;

template <class T>
void packVector(Packer& p, const std::vector<T>& v)
{
    p.pack_uint32(uint32_t(v.size()));
    p.pack_int32(int32_t(sizeof(T)));
    const uint32_t bytes = uint32_t(v.size() * sizeof(T));
    p.pack_bin(bytes);
    p.pack_bin_body(reinterpret_cast<const char*>(v.data()), bytes);
}

void packPoint(Packer& p, const Vec3& v)
{
    p.pack_array(3);
    p.pack_float(v.x);
    p.pack_float(v.y);
    p.pack_float(v.z);
}

void packBox(Packer& p, const Box& b)
{
    p.pack_array(6);
    p.pack_float(b.min.x);
    p.pack_float(b.min.y);
    p.pack_float(b.min.z);
    p.pack_float(b.max.x);
    p.pack_float(b.max.y);
    p.pack_float(b.max.z);
}

void packSet(Packer& p, const IntegerSet& s)
{
    const int used = std::min(s.usedWords(), 64);
    p.pack_array(2);
    p.pack_int32(used);
    p.pack_array(uint32_t(used));
    for (int i = 0; i < used; ++i) {
        p.pack_uint32(s.words[i]);
    }
}

class Unpacker {
public:
    Unpacker(const char* data, size_t size) : data(data), size(size) {}

    bool next(msgpack::object_handle& oh)
    {
        if (!err.empty()) {
            return false;
        }
        try {
            oh = msgpack::unpack(data, size, offset);
        } catch (const std::exception& e) {
            err = std::string("MessagePack parse error: ") + e.what();
            return false;
        }
        return true;
    }
    // msgpack's as<T>() rejects a value of the wrong type or range.
    template <class T>
    T scalar(const char* what, const char* kind)
    {
        msgpack::object_handle oh;
        if (!next(oh)) {
            return T{};
        }
        try {
            return oh.get().as<T>();
        } catch (const std::exception&) {
            fail(format("expected %s for %s", kind, what));
            return T{};
        }
    }
    uint32_t u32(const char* what) { return scalar<uint32_t>(what, "unsigned int"); }
    int32_t i32(const char* what) { return scalar<int32_t>(what, "int"); }
    float f32(const char* what) { return scalar<float>(what, "float"); }
    std::string str(const char* what) { return scalar<std::string>(what, "string"); }
    std::vector<float> floats(size_t n, const char* what)
    {
        std::vector<float> out(n, 0.0f);
        msgpack::object_handle oh;
        if (!next(oh)) {
            return out;
        }
        const msgpack::object& o = oh.get();
        if (o.type != msgpack::type::ARRAY || o.via.array.size != n) {
            fail(format("expected %zu-float array for %s", n, what));
            return out;
        }
        try {
            for (size_t i = 0; i < n; ++i) {
                out[i] = o.via.array.ptr[i].as<float>();
            }
        } catch (const std::exception&) {
            fail(format("bad float in %s", what));
        }
        return out;
    }
    Vec3 point(const char* what)
    {
        const std::vector<float> f = floats(3, what);
        return {f[0], f[1], f[2]};
    }
    Box box(const char* what)
    {
        const std::vector<float> f = floats(6, what);
        return {{f[0], f[1], f[2]}, {f[3], f[4], f[5]}};
    }
    IntegerSet set(const char* what)
    {
        IntegerSet s;
        msgpack::object_handle oh;
        if (!next(oh)) {
            return s;
        }
        const msgpack::object& o = oh.get();
        try {
            if (o.type != msgpack::type::ARRAY || o.via.array.size != 2) {
                throw std::runtime_error("shape");
            }
            const int used = o.via.array.ptr[0].as<int>();
            const msgpack::object& arr = o.via.array.ptr[1];
            if (used < 0 || used > 64 || arr.type != msgpack::type::ARRAY || int(arr.via.array.size) != used) {
                throw std::runtime_error("size");
            }
            s.words.resize(size_t(used));
            for (int i = 0; i < used; ++i) {
                s.words[i] = arr.via.array.ptr[i].as<uint32_t>();
            }
        } catch (const std::exception&) {
            fail(format("bad integer set %s", what));
        }
        return s;
    }
    template <class T>
    void vec(std::vector<T>& out, const char* what)
    {
        const uint32_t count = u32(what);
        const int32_t elem = i32(what);
        msgpack::object_handle oh;
        if (!next(oh)) {
            return;
        }
        if (elem != int32_t(sizeof(T))) {
            fail(format("%s: element size %d, expected %zu", what, elem, sizeof(T)));
            return;
        }
        const msgpack::object& o = oh.get();
        if (o.type != msgpack::type::BIN || size_t(o.via.bin.size) != size_t(count) * sizeof(T)) {
            fail(format("%s: bad binary block", what));
            return;
        }
        out.resize(count);
        if (count) {
            std::memcpy(out.data(), o.via.bin.ptr, o.via.bin.size);
        }
    }

    void fail(const std::string& e)
    {
        if (err.empty()) {
            err = e;
        }
    }
    bool failed() const { return !err.empty(); }
    bool atEnd() const { return offset == size; }
    size_t consumed() const { return offset; }
    const std::string& error() const { return err; }

private:
    const char* data;
    size_t size;
    size_t offset = 0;
    std::string err;
};

} // namespace

bool readCdae(const std::vector<uint8_t>& data, Shape& shape, std::string& error)
{
    shape = Shape{};
    if (data.size() < 8) {
        error = "file too small";
        return false;
    }
    uint32_t word, headerSize;
    std::memcpy(&word, data.data(), 4);
    std::memcpy(&headerSize, data.data() + 4, 4);
    shape.readVersion = int(word & 0xFF);
    shape.exporterVersion = int(word >> 16);
    if (shape.readVersion != 31) {
        error = format("unsupported version %d", shape.readVersion);
        return false;
    }
    if (headerSize > 10'000'000 || 8 + size_t(headerSize) > data.size()) {
        error = format("unreasonable header size %u", headerSize);
        return false;
    }

    bool compressed = false;
    uint64_t bodySize = 0;
    {
        Unpacker hdr(reinterpret_cast<const char*>(data.data() + 8), headerSize);
        msgpack::object_handle oh;
        if (!hdr.next(oh) || oh.get().type != msgpack::type::MAP) {
            error = hdr.failed() ? hdr.error() : "header is not a map";
            return false;
        }
        const msgpack::object_map& map = oh.get().via.map;
        for (uint32_t i = 0; i < map.size; ++i) {
            const msgpack::object& k = map.ptr[i].key;
            const msgpack::object& v = map.ptr[i].val;
            if (k.type != msgpack::type::STR) {
                error = "non-string header key";
                return false;
            }
            const std::string key = k.as<std::string>();
            if (key == "compression" && v.type == msgpack::type::BOOLEAN) {
                compressed = v.as<bool>();
            } else if (key == "bodysize" && v.type == msgpack::type::POSITIVE_INTEGER) {
                bodySize = v.as<uint64_t>();
            }
        }
        if (!hdr.atEnd()) {
            error = "leftover bytes in shape header";
            return false;
        }
    }
    const size_t bodyStart = 8 + size_t(headerSize);
    if (bodySize == 0 || bodyStart + bodySize > data.size()) {
        error = format("body truncated: header says %llu bytes, file has %zu", (unsigned long long)bodySize,
                       data.size() - std::min(data.size(), bodyStart));
        return false;
    }

    std::vector<char> raw;
    const char* body = reinterpret_cast<const char*>(data.data() + bodyStart);
    size_t size = size_t(bodySize);
    if (compressed) {
        const unsigned long long frame = ZSTD_getFrameContentSize(body, size);
        if (frame == ZSTD_CONTENTSIZE_ERROR || frame == ZSTD_CONTENTSIZE_UNKNOWN || frame > (1ull << 33)) {
            error = "zstd: unknown or invalid content size";
            return false;
        }
        raw.resize(size_t(frame));
        const size_t got = ZSTD_decompress(raw.data(), raw.size(), body, size);
        if (ZSTD_isError(got) || got != raw.size()) {
            error = std::string("zstd: ") + (ZSTD_isError(got) ? ZSTD_getErrorName(got) : "size mismatch");
            return false;
        }
        body = raw.data();
        size = raw.size();
    }

    Unpacker u(body, size);
    shape.smallestVisibleSize = u.f32("smallestVisibleSize");
    shape.smallestVisibleDL = u.i32("smallestVisibleDL");
    shape.radius = u.f32("radius");
    shape.tubeRadius = u.f32("tubeRadius");
    shape.center = u.point("center");
    shape.bounds = u.box("bounds");
    u.vec(shape.nodes, "nodes");
    u.vec(shape.objects, "objects");
    u.vec(shape.subShapeFirstNode, "subShapeFirstNode");
    u.vec(shape.subShapeFirstObject, "subShapeFirstObject");
    u.vec(shape.subShapeNumNodes, "subShapeNumNodes");
    u.vec(shape.subShapeNumObjects, "subShapeNumObjects");
    u.vec(shape.defaultRotations, "defaultRotations");
    u.vec(shape.defaultTranslations, "defaultTranslations");
    u.vec(shape.nodeRotations, "nodeRotations");
    u.vec(shape.nodeTranslations, "nodeTranslations");
    u.vec(shape.nodeUniformScales, "nodeUniformScales");
    u.vec(shape.nodeAlignedScales, "nodeAlignedScales");
    u.vec(shape.nodeArbitraryScaleFactors, "nodeArbitraryScaleFactors");
    u.vec(shape.nodeArbitraryScaleRots, "nodeArbitraryScaleRots");
    u.vec(shape.groundTranslations, "groundTranslations");
    u.vec(shape.groundRotations, "groundRotations");
    u.vec(shape.objectStates, "objectStates");
    u.vec(shape.triggers, "triggers");
    u.vec(shape.details, "details");
    for (Detail& det : shape.details) {
        if (det.averageError == 0 || det.averageError > 10000 || det.maxError == 0 || det.maxError > 10000) {
            det.averageError = det.maxError = -1.0f;
        }
    }

    const uint32_t numNames = u.u32("names");
    if (numNames > (1u << 20)) {
        u.fail("bad name count");
    }
    for (uint32_t i = 0; i < numNames && !u.failed(); ++i) {
        shape.names.push_back(u.str("name"));
    }

    const uint32_t numMeshes = u.u32("meshes");
    if (numMeshes > (1u << 22)) {
        u.fail("bad mesh count");
    }
    for (uint32_t i = 0; i < numMeshes && !u.failed(); ++i) {
        Mesh m;
        const uint32_t type = u.u32("mesh type");
        if (type == uint32_t(MeshType::Null)) {
            shape.meshes.push_back(std::move(m));
            continue;
        }
        if (type != uint32_t(MeshType::Standard) && type != uint32_t(MeshType::Skin)) {
            u.fail(format("unknown mesh type %u", type));
            break;
        }
        m.type = MeshType(type);
        m.numFrames = u.i32("numFrames");
        m.numMatFrames = u.i32("numMatFrames");
        m.parentMesh = u.i32("parentMesh");
        m.bounds = u.box("mesh bounds");
        m.center = u.point("mesh center");
        m.radius = u.f32("mesh radius");
        u.vec(m.verts, "verts");
        u.vec(m.tverts, "tverts");
        u.vec(m.tverts2, "tverts2");
        u.vec(m.colors, "colors");
        u.vec(m.norms, "norms");
        u.vec(m.encodedNorms, "encodedNorms");
        u.vec(m.primitives, "primitives");
        u.vec(m.indices, "indices");
        u.vec(m.tangents, "tangents");
        m.vertsPerFrame = u.i32("vertsPerFrame");
        m.flags = u.u32("flags");
        if (m.isSkin()) {
            if (m.parentMesh < 0) {
                u.vec(m.initialVerts, "initialVerts");
                u.vec(m.initialNorms, "initialNorms");
            }
            u.vec(m.initialTransforms, "initialTransforms");
            if (m.parentMesh < 0) {
                u.vec(m.vertexIndex, "vertexIndex");
                u.vec(m.boneIndex, "boneIndex");
                u.vec(m.weight, "weight");
                u.vec(m.nodeIndex, "nodeIndex");
            }
        }
        shape.meshes.push_back(std::move(m));
    }

    const uint32_t numSeq = u.u32("sequences");
    if (numSeq > 65536) {
        u.fail("bad sequence count");
    }
    for (uint32_t i = 0; i < numSeq && !u.failed(); ++i) {
        Sequence q;
        q.nameIndex = u.i32("seq.nameIndex");
        q.flags = u.u32("seq.flags");
        q.numKeyframes = u.i32("seq.numKeyframes");
        q.duration = u.f32("seq.duration");
        q.priority = u.i32("seq.priority");
        q.firstGroundFrame = u.i32("seq.firstGroundFrame");
        q.numGroundFrames = u.i32("seq.numGroundFrames");
        q.baseRotation = u.i32("seq.baseRotation");
        q.baseTranslation = u.i32("seq.baseTranslation");
        q.baseScale = u.i32("seq.baseScale");
        q.baseObjectState = u.i32("seq.baseObjectState");
        q.baseDecalState = u.i32("seq.baseDecalState");
        q.firstTrigger = u.i32("seq.firstTrigger");
        q.numTriggers = u.i32("seq.numTriggers");
        q.toolBegin = u.f32("seq.toolBegin");
        q.rotationMatters = u.set("rotationMatters");
        q.translationMatters = u.set("translationMatters");
        q.scaleMatters = u.set("scaleMatters");
        q.visMatters = u.set("visMatters");
        q.frameMatters = u.set("frameMatters");
        q.matFrameMatters = u.set("matFrameMatters");
        shape.sequences.push_back(std::move(q));
    }

    if (!u.failed() && !u.atEnd()) {
        const uint32_t numMats = u.u32("materials");
        if (numMats > (1u << 20)) {
            u.fail("bad material count");
        }
        for (uint32_t i = 0; i < numMats && !u.failed(); ++i) {
            Material m;
            m.name = u.str("material name");
            m.flags = u.u32("material flags");
            m.reflectanceMap = u.u32("reflectanceMap");
            m.bumpMap = u.u32("bumpMap");
            m.detailMap = u.u32("detailMap");
            m.detailScale = u.f32("detailScale");
            m.reflectionAmount = u.f32("reflectionAmount");
            shape.materials.push_back(std::move(m));
        }
    }
    if (u.failed()) {
        error = u.error();
        return false;
    }
    if (!u.atEnd()) {
        error = format("unexpected trailing body data (%zu of %zu bytes parsed)", u.consumed(), size);
        return false;
    }

    // Child skins carry no geometry in v31; borrow the parent's so the shape is self-contained.
    for (Mesh& m : shape.meshes) {
        if (m.isSkin() && m.parentMesh >= 0 && m.parentMesh < int(shape.meshes.size())) {
            const Mesh& p = shape.meshes[m.parentMesh];
            m.initialVerts = p.initialVerts;
            m.initialNorms = p.initialNorms;
            m.vertexIndex = p.vertexIndex;
            m.boneIndex = p.boneIndex;
            m.weight = p.weight;
            m.nodeIndex = p.nodeIndex;
        }
    }
    for (Mesh& m : shape.meshes) {
        m.parentMesh = -1;
    }
    shape.rebuildLinks();
    shape.updateSmallestVisible();
    return true;
}

bool writeCdae(const Shape& shapeIn, const CdaeWriteOptions& options, std::vector<uint8_t>& out, std::string& error, bool* compressedOut)
{
    Shape compacted;
    const Shape* src = &shapeIn;
    if (!shapeIn.meshesCompact()) {
        compacted = shapeIn;
        compacted.compactMeshes();
        src = &compacted;
    }
    const Shape& shape = *src;
    msgpack::sbuffer body(1 << 16);
    {
        Packer p(body);
        p.pack_float(shape.smallestVisibleSize);
        p.pack_int32(shape.smallestVisibleDL);
        p.pack_float(shape.radius);
        p.pack_float(shape.tubeRadius);
        packPoint(p, shape.center);
        packBox(p, shape.bounds);
        packVector(p, shape.nodes);
        packVector(p, shape.objects);
        packVector(p, shape.subShapeFirstNode);
        packVector(p, shape.subShapeFirstObject);
        packVector(p, shape.subShapeNumNodes);
        packVector(p, shape.subShapeNumObjects);
        packVector(p, shape.defaultRotations);
        packVector(p, shape.defaultTranslations);
        packVector(p, shape.nodeRotations);
        packVector(p, shape.nodeTranslations);
        packVector(p, shape.nodeUniformScales);
        packVector(p, shape.nodeAlignedScales);
        packVector(p, shape.nodeArbitraryScaleFactors);
        packVector(p, shape.nodeArbitraryScaleRots);
        packVector(p, shape.groundTranslations);
        packVector(p, shape.groundRotations);
        packVector(p, shape.objectStates);
        packVector(p, shape.triggers);
        packVector(p, shape.details);

        p.pack_uint32(uint32_t(shape.names.size()));
        for (const std::string& n : shape.names) {
            p.pack(n);
        }

        // Meshes are emitted in object order; the reader assigns them sequentially.
        p.pack_uint32(uint32_t(shape.meshes.size()));
        size_t written = 0;
        for (const Object& o : shape.objects) {
            for (int j = 0; j < o.numMeshes; ++j) {
                const int idx = o.startMeshIndex + j;
                const Mesh* m = idx >= 0 && idx < int(shape.meshes.size()) ? &shape.meshes[idx] : nullptr;
                ++written;
                if (!m || m->isNull() || m->type == MeshType::Decal) {
                    p.pack_uint32(uint32_t(MeshType::Null));
                    continue;
                }
                const MeshType type = m->type == MeshType::Sorted ? MeshType::Standard : m->type;
                p.pack_uint32(uint32_t(type));
                p.pack_int32(m->numFrames);
                p.pack_int32(m->numMatFrames);
                p.pack_int32(-1);
                packBox(p, m->bounds);
                packPoint(p, m->center);
                p.pack_float(m->radius);
                const std::vector<Vec3>& verts = type == MeshType::Skin && m->verts.empty() ? m->initialVerts : m->verts;
                const std::vector<Vec3>& norms = type == MeshType::Skin && m->norms.empty() ? m->initialNorms : m->norms;
                packVector(p, verts);
                packVector(p, m->tverts);
                packVector(p, m->tverts2);
                packVector(p, m->colors);
                packVector(p, norms);
                packVector(p, std::vector<uint8_t>{});
                packVector(p, m->primitives);
                packVector(p, m->indices);
                packVector(p, m->tangents);
                p.pack_int32(m->vertsPerFrame);
                p.pack_uint32(m->flags & ~uint32_t(UseEncodedNormals));
                if (type == MeshType::Skin) {
                    packVector(p, m->initialVerts.empty() ? m->verts : m->initialVerts);
                    packVector(p, m->initialNorms.empty() ? m->norms : m->initialNorms);
                    packVector(p, m->initialTransforms);
                    packVector(p, m->vertexIndex);
                    packVector(p, m->boneIndex);
                    packVector(p, m->weight);
                    packVector(p, m->nodeIndex);
                }
            }
        }
        if (written != shape.meshes.size()) {
            error = format("objects reference %zu meshes but the shape has %zu (compact the mesh list first)", written, shape.meshes.size());
            return false;
        }

        p.pack_uint32(uint32_t(shape.sequences.size()));
        for (const Sequence& q : shape.sequences) {
            p.pack_int32(q.nameIndex);
            p.pack_uint32(q.flags);
            p.pack_int32(q.numKeyframes);
            p.pack_float(q.duration);
            p.pack_int32(q.priority);
            p.pack_int32(q.firstGroundFrame);
            p.pack_int32(q.numGroundFrames);
            p.pack_int32(q.baseRotation);
            p.pack_int32(q.baseTranslation);
            p.pack_int32(q.baseScale);
            p.pack_int32(q.baseObjectState);
            p.pack_int32(q.baseDecalState);
            p.pack_int32(q.firstTrigger);
            p.pack_int32(q.numTriggers);
            p.pack_float(q.toolBegin);
            packSet(p, q.rotationMatters);
            packSet(p, q.translationMatters);
            packSet(p, q.scaleMatters);
            packSet(p, q.visMatters);
            packSet(p, q.frameMatters);
            packSet(p, q.matFrameMatters);
        }

        p.pack_uint32(uint32_t(shape.materials.size()));
        for (const Material& m : shape.materials) {
            p.pack(m.name);
            p.pack_uint32(m.flags);
            p.pack_uint32(m.reflectanceMap);
            p.pack_uint32(m.bumpMap);
            p.pack_uint32(m.detailMap);
            p.pack_float(m.detailScale);
            p.pack_float(m.reflectionAmount);
        }
    }

    bool compress = options.compression == CdaeCompression::On ||
                    (options.compression == CdaeCompression::Auto && body.size() > kCdaeAutoCompressBytes);
    std::vector<char> packed;
    if (compress) {
        const int level = std::clamp(options.zstdLevel, ZSTD_minCLevel(), ZSTD_maxCLevel());
        packed.resize(ZSTD_compressBound(body.size()));
        const size_t n = ZSTD_compress(packed.data(), packed.size(), body.data(), body.size(), level);
        if (ZSTD_isError(n)) {
            error = std::string("zstd: ") + ZSTD_getErrorName(n);
            return false;
        }
        packed.resize(n);
    }
    const char* bodyData = compress ? packed.data() : body.data();
    const size_t bodySize = compress ? packed.size() : body.size();

    msgpack::sbuffer header;
    {
        Packer p(header);
        p.pack_map(4);
        p.pack(std::string("info"));
        p.pack(std::string("\r\n\r\n\r\nWelcome! This is a binary file :D\r\nPlease read the docs at "
                           "https://go.beamng.com/shapeMessagepackFileformat\r\n\r\n\r\n"));
        p.pack(std::string("compression"));
        p.pack(compress);
        p.pack(std::string("bodysize"));
        p.pack(uint64_t(bodySize));
        p.pack(std::string("objectNames"));
        p.pack_array(uint32_t(shape.objects.size()));
        for (const Object& o : shape.objects) {
            p.pack(o.nameIndex >= 0 && o.nameIndex < int(shape.names.size()) ? shape.names[o.nameIndex] : std::string());
        }
    }

    out.clear();
    out.reserve(8 + header.size() + bodySize);
    const uint32_t word = 31u | (uint32_t(shape.exporterVersion) << 16);
    const uint32_t headerSize = uint32_t(header.size());
    out.insert(out.end(), reinterpret_cast<const uint8_t*>(&word), reinterpret_cast<const uint8_t*>(&word) + 4);
    out.insert(out.end(), reinterpret_cast<const uint8_t*>(&headerSize), reinterpret_cast<const uint8_t*>(&headerSize) + 4);
    out.insert(out.end(), reinterpret_cast<const uint8_t*>(header.data()), reinterpret_cast<const uint8_t*>(header.data()) + header.size());
    out.insert(out.end(), reinterpret_cast<const uint8_t*>(bodyData), reinterpret_cast<const uint8_t*>(bodyData) + bodySize);
    if (compressedOut) {
        *compressedOut = compress;
    }
    return true;
}

int peekVersion(const std::vector<uint8_t>& data, int* exporter)
{
    if (data.size() < 4) {
        return -1;
    }
    uint32_t word;
    std::memcpy(&word, data.data(), 4);
    if (exporter) {
        *exporter = int(word >> 16);
    }
    return int(word & 0xFF);
}

bool readShape(const std::vector<uint8_t>& data, Shape& shape, std::string& error, std::vector<std::string>* warnings)
{
    const int v = peekVersion(data);
    if (v < 0) {
        error = "file too small";
        return false;
    }
    if (v == 31) {
        return readCdae(data, shape, error);
    }
    if (v >= kMinReadVersion && v <= 29) {
        return readDts(data, shape, error, warnings);
    }
    error = format("unsupported version %d", v);
    return false;
}

bool readShapeFile(const std::string& path, Shape& shape, std::string& error, std::vector<std::string>* warnings)
{
    std::vector<uint8_t> data;
    return ::readFileBytes(path, data, error) && readShape(data, shape, error, warnings);
}

} // namespace ts
